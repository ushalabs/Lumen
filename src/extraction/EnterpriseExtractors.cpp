#include "extraction/DocumentExtractor.h"
#include "extraction/DeflateCodec.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <propkey.h>
#include <shobjidl.h>
#endif

namespace lumen::extraction {

namespace {

uint16_t readLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readBe16(const uint8_t* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

uint32_t readBe32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(readBe32(p)) << 32) | static_cast<uint64_t>(readBe32(p + 4));
}

void appendLe16(QByteArray* out, uint16_t v) {
    out->append(static_cast<char>(v & 0xFF));
    out->append(static_cast<char>((v >> 8) & 0xFF));
}

void appendLe32(QByteArray* out, uint32_t v) {
    out->append(static_cast<char>(v & 0xFF));
    out->append(static_cast<char>((v >> 8) & 0xFF));
    out->append(static_cast<char>((v >> 16) & 0xFF));
    out->append(static_cast<char>((v >> 24) & 0xFF));
}

QSet<QString> listToSet(const QStringList& items) {
    return QSet<QString>(items.cbegin(), items.cend());
}

bool decodeTextBytesSafely(const QByteArray& rawBytes, QString* outText, QString* outError) {
    if (rawBytes.isEmpty()) {
        if (outError) {
            *outError = QStringLiteral("File is empty.");
        }
        return false;
    }

    const uint8_t b0 = static_cast<uint8_t>(rawBytes[0]);
    const uint8_t b1 = (rawBytes.size() > 1) ? static_cast<uint8_t>(rawBytes[1]) : 0;
    const uint8_t b2 = (rawBytes.size() > 2) ? static_cast<uint8_t>(rawBytes[2]) : 0;

    QString decoded;
    if (rawBytes.size() >= 2 && ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF))) {
        const bool littleEndian = (b0 == 0xFF && b1 == 0xFE);
        QString utf16;
        utf16.reserve(rawBytes.size() / 2);
        for (qsizetype i = 2; i + 1 < rawBytes.size(); i += 2) {
            const uint8_t lo = static_cast<uint8_t>(rawBytes[i]);
            const uint8_t hi = static_cast<uint8_t>(rawBytes[i + 1]);
            const uint16_t codeUnit = littleEndian
                                          ? (static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8))
                                          : ((static_cast<uint16_t>(lo) << 8) | static_cast<uint16_t>(hi));
            if (codeUnit == 0) {
                if (outError) {
                    *outError = QStringLiteral("Binary NUL detected inside UTF-16 file.");
                }
                return false;
            }
            utf16.append(QChar(codeUnit));
        }
        decoded = std::move(utf16);
    } else {
        const qsizetype sampleLen = std::min<qsizetype>(rawBytes.size(), 8192);
        for (qsizetype i = 0; i < sampleLen; ++i) {
            if (rawBytes[i] == '\0') {
                if (outError) {
                    *outError = QStringLiteral("Binary content detected (NUL bytes present).");
                }
                return false;
            }
        }
        const qsizetype startOffset = (rawBytes.size() >= 3 && b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) ? 3 : 0;
        decoded = QString::fromUtf8(rawBytes.constData() + startOffset, rawBytes.size() - startOffset);
    }

    *outText = decoded;
    return true;
}

bool readZipArchiveEntries(const QByteArray& zipBytes,
                           const std::function<bool(const QString&)>& wantEntry,
                           QHash<QString, QByteArray>* outEntries,
                           QString* outError) {
    outEntries->clear();
    if (zipBytes.size() < 22) {
        if (outError) {
            *outError = QStringLiteral("ZIP archive too small.");
        }
        return false;
    }

    const auto* data = reinterpret_cast<const uint8_t*>(zipBytes.constData());
    const size_t size = static_cast<size_t>(zipBytes.size());

    size_t eocdPos = size;
    const size_t minSearch = (size > 65557) ? (size - 65557) : 0;
    for (size_t i = size - 22;; --i) {
        if (data[i] == 0x50 && data[i + 1] == 0x4B && data[i + 2] == 0x05 && data[i + 3] == 0x06) {
            eocdPos = i;
            break;
        }
        if (i == minSearch) {
            break;
        }
    }

    if (eocdPos >= size) {
        if (outError) {
            *outError = QStringLiteral("Missing ZIP End of Central Directory.");
        }
        return false;
    }

    const uint16_t entryCount = readLe16(data + eocdPos + 10);
    const uint32_t cdSize = readLe32(data + eocdPos + 12);
    const uint32_t cdOffset = readLe32(data + eocdPos + 16);

    if (static_cast<uint64_t>(cdOffset) + static_cast<uint64_t>(cdSize) > size || entryCount > 8192) {
        if (outError) {
            *outError = QStringLiteral("Invalid ZIP Central Directory geometry.");
        }
        return false;
    }

    size_t cursor = cdOffset;
    const size_t cdEnd = static_cast<size_t>(cdOffset) + static_cast<size_t>(cdSize);
    for (uint16_t idx = 0; idx < entryCount && cursor + 46 <= cdEnd; ++idx) {
        if (readLe32(data + cursor) != 0x02014B50u) {
            break;
        }
        const uint16_t method = readLe16(data + cursor + 10);
        const uint32_t compSize = readLe32(data + cursor + 20);
        const uint32_t uncompSize = readLe32(data + cursor + 24);
        const uint16_t nameLen = readLe16(data + cursor + 28);
        const uint16_t extraLen = readLe16(data + cursor + 30);
        const uint16_t commentLen = readLe16(data + cursor + 32);
        const uint32_t lfhOffset = readLe32(data + cursor + 42);

        const size_t entryStep = 46ULL + nameLen + extraLen + commentLen;
        if (cursor + entryStep > cdEnd) {
            break;
        }

        const QString entryName = QString::fromUtf8(
            reinterpret_cast<const char*>(data + cursor + 46), nameLen);
        cursor += entryStep;

        if (!wantEntry(entryName)) {
            continue;
        }
        if (uncompSize > DocumentExtractor::kMaxDecompressedStreamBytes) {
            if (outError) {
                *outError = QStringLiteral("ZIP entry exceeds decompressed size cap.");
            }
            return false;
        }
        if (method == 8 && compSize > 0 && uncompSize > static_cast<uint64_t>(compSize) * 120ULL) {
            if (outError) {
                *outError = QStringLiteral("Rejected suspicious ZIP compression ratio.");
            }
            return false;
        }
        if (static_cast<uint64_t>(lfhOffset) + 30ULL > size ||
            readLe32(data + lfhOffset) != 0x04034B50u) {
            continue;
        }

        const uint16_t lfhNameLen = readLe16(data + lfhOffset + 26);
        const uint16_t lfhExtraLen = readLe16(data + lfhOffset + 28);
        const size_t payloadOffset = static_cast<size_t>(lfhOffset) + 30ULL + lfhNameLen + lfhExtraLen;
        if (payloadOffset + compSize > size) {
            continue;
        }

        QByteArray decoded;
        if (method == 0) {
            decoded = QByteArray(reinterpret_cast<const char*>(data + payloadOffset),
                                 static_cast<qsizetype>(compSize));
        } else if (method == 8) {
            if (!DeflateCodec::inflateRaw(data + payloadOffset,
                                          compSize,
                                          &decoded,
                                          DocumentExtractor::kMaxDecompressedStreamBytes)) {
                if (outError) {
                    *outError = QStringLiteral("Deflate decompression failed for %1.").arg(entryName);
                }
                return false;
            }
        } else {
            continue;
        }

        outEntries->insert(entryName, std::move(decoded));
    }
    return true;
}

QString normalizeZipTargetPath(const QString& target) {
    QString clean = target;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (clean.startsWith(QLatin1Char('/'))) {
        clean.remove(0, 1);
    }
    if (!clean.startsWith(QStringLiteral("xl/"))) {
        clean.prepend(QStringLiteral("xl/"));
    }
    return clean;
}

QString escapeXmlText(const QString& s) {
    QString out = s;
    out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return out;
}

QString columnIndexToLetters(int zeroBasedCol) {
    QString letters;
    int col = zeroBasedCol + 1;
    while (col > 0) {
        const int rem = (col - 1) % 26;
        letters.prepend(QChar('A' + rem));
        col = (col - 1) / 26;
    }
    return letters;
}

void writeStoredZipEntry(QByteArray* archive,
                         QByteArray* centralDirectory,
                         uint16_t* entryCount,
                         const QString& entryPath,
                         const QByteArray& payload) {
    const QByteArray nameBytes = entryPath.toUtf8();
    const uint32_t uncompSize = static_cast<uint32_t>(payload.size());
    const uint32_t lfhOffset = static_cast<uint32_t>(archive->size());

    appendLe32(archive, 0x04034B50u);
    appendLe16(archive, 20);
    appendLe16(archive, 0);
    appendLe16(archive, 0);
    appendLe16(archive, 0);
    appendLe16(archive, 0);
    appendLe32(archive, 0);
    appendLe32(archive, uncompSize);
    appendLe32(archive, uncompSize);
    appendLe16(archive, static_cast<uint16_t>(nameBytes.size()));
    appendLe16(archive, 0);
    archive->append(nameBytes);
    archive->append(payload);

    appendLe32(centralDirectory, 0x02014B50u);
    appendLe16(centralDirectory, 20);
    appendLe16(centralDirectory, 20);
    appendLe16(centralDirectory, 0);
    appendLe16(centralDirectory, 0);
    appendLe16(centralDirectory, 0);
    appendLe16(centralDirectory, 0);
    appendLe32(centralDirectory, 0);
    appendLe32(centralDirectory, uncompSize);
    appendLe32(centralDirectory, uncompSize);
    appendLe16(centralDirectory, static_cast<uint16_t>(nameBytes.size()));
    appendLe16(centralDirectory, 0);
    appendLe16(centralDirectory, 0);
    appendLe16(centralDirectory, 0);
    appendLe16(centralDirectory, 0);
    appendLe32(centralDirectory, 0);
    appendLe32(centralDirectory, lfhOffset);
    centralDirectory->append(nameBytes);

    ++(*entryCount);
}

QStringList parseDelimitedLine(const QString& line, QChar delimiter) {
    QStringList fields;
    QString current;
    bool inQuotes = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('"')) {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                current.append(QLatin1Char('"'));
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (c == delimiter && !inQuotes) {
            fields.append(current.trimmed());
            current.clear();
        } else {
            current.append(c);
        }
    }
    fields.append(current.trimmed());
    return fields;
}

QString readableCodeLanguageName(const QString& extLower) {
    static const QHash<QString, QString> kLangMap = {
        {QStringLiteral("cpp"), QStringLiteral("C++ Source")},
        {QStringLiteral("cc"), QStringLiteral("C++ Source")},
        {QStringLiteral("cxx"), QStringLiteral("C++ Source")},
        {QStringLiteral("c"), QStringLiteral("C Source")},
        {QStringLiteral("h"), QStringLiteral("C/C++ Header")},
        {QStringLiteral("hpp"), QStringLiteral("C++ Header")},
        {QStringLiteral("hh"), QStringLiteral("C++ Header")},
        {QStringLiteral("hxx"), QStringLiteral("C++ Header")},
        {QStringLiteral("py"), QStringLiteral("Python Script")},
        {QStringLiteral("pyw"), QStringLiteral("Python Script")},
        {QStringLiteral("js"), QStringLiteral("JavaScript")},
        {QStringLiteral("mjs"), QStringLiteral("JavaScript Module")},
        {QStringLiteral("cjs"), QStringLiteral("JavaScript")},
        {QStringLiteral("jsx"), QStringLiteral("React JSX")},
        {QStringLiteral("ts"), QStringLiteral("TypeScript")},
        {QStringLiteral("tsx"), QStringLiteral("React TSX")},
        {QStringLiteral("rs"), QStringLiteral("Rust")},
        {QStringLiteral("go"), QStringLiteral("Go")},
        {QStringLiteral("java"), QStringLiteral("Java")},
        {QStringLiteral("kt"), QStringLiteral("Kotlin")},
        {QStringLiteral("kts"), QStringLiteral("Kotlin Script")},
        {QStringLiteral("cs"), QStringLiteral("C#")},
        {QStringLiteral("fs"), QStringLiteral("F#")},
        {QStringLiteral("vb"), QStringLiteral("Visual Basic")},
        {QStringLiteral("swift"), QStringLiteral("Swift")},
        {QStringLiteral("dart"), QStringLiteral("Dart")},
        {QStringLiteral("rb"), QStringLiteral("Ruby")},
        {QStringLiteral("php"), QStringLiteral("PHP")},
        {QStringLiteral("lua"), QStringLiteral("Lua")},
        {QStringLiteral("pl"), QStringLiteral("Perl")},
        {QStringLiteral("r"), QStringLiteral("R Script")},
        {QStringLiteral("scala"), QStringLiteral("Scala")},
        {QStringLiteral("sql"), QStringLiteral("SQL Query")},
        {QStringLiteral("sh"), QStringLiteral("Shell Script")},
        {QStringLiteral("bash"), QStringLiteral("Bash Script")},
        {QStringLiteral("zsh"), QStringLiteral("Zsh Script")},
        {QStringLiteral("ps1"), QStringLiteral("PowerShell Script")},
        {QStringLiteral("psm1"), QStringLiteral("PowerShell Module")},
        {QStringLiteral("bat"), QStringLiteral("Windows Batch")},
        {QStringLiteral("cmd"), QStringLiteral("Windows Command Script")},
        {QStringLiteral("json"), QStringLiteral("JSON Configuration")},
        {QStringLiteral("jsonc"), QStringLiteral("JSON Configuration")},
        {QStringLiteral("yaml"), QStringLiteral("YAML Configuration")},
        {QStringLiteral("yml"), QStringLiteral("YAML Configuration")},
        {QStringLiteral("toml"), QStringLiteral("TOML Configuration")},
        {QStringLiteral("ini"), QStringLiteral("INI Configuration")},
        {QStringLiteral("cfg"), QStringLiteral("Configuration File")},
        {QStringLiteral("conf"), QStringLiteral("Configuration File")},
        {QStringLiteral("xml"), QStringLiteral("XML Document")},
        {QStringLiteral("html"), QStringLiteral("HTML Document")},
        {QStringLiteral("htm"), QStringLiteral("HTML Document")},
        {QStringLiteral("css"), QStringLiteral("CSS Stylesheet")},
        {QStringLiteral("scss"), QStringLiteral("SCSS Stylesheet")},
        {QStringLiteral("sass"), QStringLiteral("Sass Stylesheet")},
        {QStringLiteral("less"), QStringLiteral("Less Stylesheet")},
        {QStringLiteral("vue"), QStringLiteral("Vue Component")},
        {QStringLiteral("svelte"), QStringLiteral("Svelte Component")},
        {QStringLiteral("cmake"), QStringLiteral("CMake Script")},
        {QStringLiteral("gradle"), QStringLiteral("Gradle Build")},
        {QStringLiteral("proto"), QStringLiteral("Protocol Buffers")},
        {QStringLiteral("graphql"), QStringLiteral("GraphQL Schema")},
        {QStringLiteral("gql"), QStringLiteral("GraphQL Schema")}
    };
    const auto it = kLangMap.constFind(extLower);
    return (it != kLangMap.constEnd()) ? it.value() : QStringLiteral("Source Code");
}

QStringList splitIdentifierWords(const QString& identifier) {
    QStringList words;
    QString current;
    for (qsizetype i = 0; i < identifier.size(); ++i) {
        const QChar c = identifier.at(i);
        if (c == QLatin1Char('_') || c == QLatin1Char('-')) {
            if (!current.isEmpty()) {
                words.append(current.toLower());
                current.clear();
            }
            continue;
        }
        if (c.isUpper() && !current.isEmpty()) {
            const QChar prev = current.back();
            const bool nextLower = (i + 1 < identifier.size() && identifier.at(i + 1).isLower());
            if (prev.isLower() || (prev.isUpper() && nextLower)) {
                words.append(current.toLower());
                current.clear();
            }
        }
        if (c.isLetter()) {
            current.append(c);
        } else if (!current.isEmpty()) {
            words.append(current.toLower());
            current.clear();
        }
    }
    if (!current.isEmpty()) {
        words.append(current.toLower());
    }
    return words;
}

QString buildCodeTopicSummary(const QString& fileName, const QString& codeText) {
    static const QSet<QString> kStopWords = {
        QStringLiteral("int"),      QStringLiteral("void"),     QStringLiteral("char"),     QStringLiteral("bool"),
        QStringLiteral("auto"),     QStringLiteral("const"),    QStringLiteral("static"),   QStringLiteral("inline"),
        QStringLiteral("return"),   QStringLiteral("public"),   QStringLiteral("private"),  QStringLiteral("protected"),
        QStringLiteral("class"),    QStringLiteral("struct"),   QStringLiteral("enum"),     QStringLiteral("namespace"),
        QStringLiteral("using"),    QStringLiteral("include"),  QStringLiteral("import"),   QStringLiteral("from"),
        QStringLiteral("export"),   QStringLiteral("default"),  QStringLiteral("function"), QStringLiteral("def"),
        QStringLiteral("var"),      QStringLiteral("let"),      QStringLiteral("true"),     QStringLiteral("false"),
        QStringLiteral("null"),     QStringLiteral("nullptr"),  QStringLiteral("none"),     QStringLiteral("self"),
        QStringLiteral("this"),     QStringLiteral("std"),      QStringLiteral("qstring"),  QStringLiteral("string"),
        QStringLiteral("for"),      QStringLiteral("while"),    QStringLiteral("else"),     QStringLiteral("elif"),
        QStringLiteral("case"),     QStringLiteral("break"),    QStringLiteral("continue"), QStringLiteral("switch"),
        QStringLiteral("try"),      QStringLiteral("catch"),    QStringLiteral("throw"),    QStringLiteral("async"),
        QStringLiteral("await"),    QStringLiteral("override"), QStringLiteral("virtual"),  QStringLiteral("final"),
        QStringLiteral("typename"), QStringLiteral("template"), QStringLiteral("sizeof"),   QStringLiteral("the"),
        QStringLiteral("and"),      QStringLiteral("with"),     QStringLiteral("for")
    };

    QStringList orderedTopics;
    QSet<QString> seenTopics;
    auto pushWord = [&](const QString& rawWord) {
        if (rawWord.size() < 3 || orderedTopics.size() >= 40) {
            return;
        }
        if (kStopWords.contains(rawWord) || seenTopics.contains(rawWord)) {
            return;
        }
        seenTopics.insert(rawWord);
        orderedTopics.append(rawWord);
    };

    const QString stem = QFileInfo(fileName).completeBaseName();
    for (const QString& w : splitIdentifierWords(stem)) {
        pushWord(w);
    }

    static const QRegularExpression kIdentRe(QStringLiteral("[A-Za-z_][A-Za-z0-9_]{2,48}"));
    const QString sample = codeText.left(12000);
    auto it = kIdentRe.globalMatch(sample);
    while (it.hasNext() && orderedTopics.size() < 40) {
        const QString ident = it.next().captured(0);
        for (const QString& w : splitIdentifierWords(ident)) {
            pushWord(w);
        }
    }

    return orderedTopics.join(QLatin1Char(' '));
}

struct MediaMetadata {
    int width = 0;
    int height = 0;
    int durationSeconds = 0;
    double fps = 0.0;
    int bitrateKbps = 0;
    int sampleRateHz = 0;
    int channels = 0;
    QString codec;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString cameraMake;
    QString cameraModel;
    QString comment;
    QString keywords;
    QString dateTaken;
    QString extraText;
};

#ifdef _WIN32
class ScopedComInit {
public:
    ScopedComInit() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        m_shouldUninit = (hr == S_OK || hr == S_FALSE);
    }
    ~ScopedComInit() {
        if (m_shouldUninit) {
            ::CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

private:
    bool m_shouldUninit = false;
};

QString propVariantToString(const PROPVARIANT& pv) {
    if (pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
        return QString::fromWCharArray(pv.pwszVal).trimmed();
    }
    if (pv.vt == VT_BSTR && pv.bstrVal != nullptr) {
        return QString::fromWCharArray(pv.bstrVal).trimmed();
    }
    if (pv.vt == (VT_VECTOR | VT_LPWSTR) && pv.calpwstr.pElems != nullptr) {
        QStringList items;
        for (ULONG i = 0; i < pv.calpwstr.cElems; ++i) {
            if (pv.calpwstr.pElems[i] != nullptr) {
                const QString part = QString::fromWCharArray(pv.calpwstr.pElems[i]).trimmed();
                if (!part.isEmpty()) {
                    items.append(part);
                }
            }
        }
        return items.join(QStringLiteral(", "));
    }
    return QString();
}

uint64_t propVariantToUInt64(const PROPVARIANT& pv) {
    switch (pv.vt) {
        case VT_UI4:
            return pv.ulVal;
        case VT_I4:
            return pv.lVal > 0 ? static_cast<uint64_t>(pv.lVal) : 0ULL;
        case VT_UI8:
            return pv.uhVal.QuadPart;
        case VT_I8:
            return pv.hVal.QuadPart > 0 ? static_cast<uint64_t>(pv.hVal.QuadPart) : 0ULL;
        case VT_UI2:
            return pv.uiVal;
        case VT_R8:
            return pv.dblVal > 0.0 ? static_cast<uint64_t>(pv.dblVal) : 0ULL;
        default:
            return 0ULL;
    }
}

void queryWindowsPropertyStore(const QString& filePath, MediaMetadata* meta) {
    using SHGetPropertyStoreFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, IBindCtx*, GETPROPERTYSTOREFLAGS, REFIID, void**);
    static SHGetPropertyStoreFn s_getPropStore = []() -> SHGetPropertyStoreFn {
        HMODULE shell32 = ::GetModuleHandleW(L"shell32.dll");
        if (shell32 == nullptr) {
            shell32 = ::LoadLibraryW(L"shell32.dll");
        }
        return shell32 ? reinterpret_cast<SHGetPropertyStoreFn>(
                             ::GetProcAddress(shell32, "SHGetPropertyStoreFromParsingName"))
                       : nullptr;
    }();

    if (s_getPropStore == nullptr) {
        return;
    }

    ScopedComInit comInit;
    IPropertyStore* store = nullptr;
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const std::wstring wpath = nativePath.toStdWString();
    HRESULT hr = s_getPropStore(wpath.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store));
    if (FAILED(hr) || store == nullptr) {
        return;
    }

    auto readStr = [&](const PROPERTYKEY& key) -> QString {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        QString val;
        if (SUCCEEDED(store->GetValue(key, &pv))) {
            val = propVariantToString(pv);
        }
        PropVariantClear(&pv);
        return val;
    };
    auto readU64 = [&](const PROPERTYKEY& key) -> uint64_t {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        uint64_t val = 0;
        if (SUCCEEDED(store->GetValue(key, &pv))) {
            val = propVariantToUInt64(pv);
        }
        PropVariantClear(&pv);
        return val;
    };

    const uint64_t vidW = readU64(PKEY_Video_FrameWidth);
    const uint64_t vidH = readU64(PKEY_Video_FrameHeight);
    if (vidW > 0 && vidH > 0) {
        meta->width = static_cast<int>(vidW);
        meta->height = static_cast<int>(vidH);
    } else {
        const uint64_t imgW = readU64(PKEY_Image_HorizontalSize);
        const uint64_t imgH = readU64(PKEY_Image_VerticalSize);
        if (imgW > 0 && imgH > 0) {
            meta->width = static_cast<int>(imgW);
            meta->height = static_cast<int>(imgH);
        }
    }

    const uint64_t dur100ns = readU64(PKEY_Media_Duration);
    if (dur100ns > 0) {
        meta->durationSeconds = static_cast<int>(dur100ns / 10000000ULL);
    }

    const uint64_t milliFps = readU64(PKEY_Video_FrameRate);
    if (milliFps > 0) {
        meta->fps = static_cast<double>(milliFps) / 1000.0;
    }

    const uint64_t totalBitrate = readU64(PKEY_Video_TotalBitrate);
    const uint64_t audioBitrate = readU64(PKEY_Audio_EncodingBitrate);
    const uint64_t bestBitrate = (totalBitrate > 0) ? totalBitrate : audioBitrate;
    if (bestBitrate > 0) {
        meta->bitrateKbps = static_cast<int>(bestBitrate / 1000ULL);
    }

    const uint64_t sampleRate = readU64(PKEY_Audio_SampleRate);
    if (sampleRate > 0) {
        meta->sampleRateHz = static_cast<int>(sampleRate);
    }
    const uint64_t channels = readU64(PKEY_Audio_ChannelCount);
    if (channels > 0) {
        meta->channels = static_cast<int>(channels);
    }

    meta->title = readStr(PKEY_Title);
    meta->comment = readStr(PKEY_Comment);
    meta->keywords = readStr(PKEY_Keywords);
    meta->artist = readStr(PKEY_Music_Artist);
    if (meta->artist.isEmpty()) {
        meta->artist = readStr(PKEY_Author);
    }
    meta->album = readStr(PKEY_Music_AlbumTitle);
    meta->genre = readStr(PKEY_Music_Genre);
    meta->cameraMake = readStr(PKEY_Photo_CameraManufacturer);
    meta->cameraModel = readStr(PKEY_Photo_CameraModel);

    store->Release();
}
#endif

QString friendlyFourCcCodec(const QString& fourCc) {
    const QString lower = fourCc.toLower();
    if (lower == QStringLiteral("avc1") || lower == QStringLiteral("h264") || lower == QStringLiteral("x264")) {
        return QStringLiteral("H.264 AVC");
    }
    if (lower == QStringLiteral("hvc1") || lower == QStringLiteral("hev1") || lower == QStringLiteral("hevc")) {
        return QStringLiteral("H.265 HEVC");
    }
    if (lower == QStringLiteral("av01")) {
        return QStringLiteral("AV1");
    }
    if (lower == QStringLiteral("vp09") || lower == QStringLiteral("vp90")) {
        return QStringLiteral("VP9");
    }
    if (lower == QStringLiteral("vp08") || lower == QStringLiteral("vp80")) {
        return QStringLiteral("VP8");
    }
    if (lower == QStringLiteral("mp4v")) {
        return QStringLiteral("MPEG-4");
    }
    if (lower == QStringLiteral("mp4a")) {
        return QStringLiteral("AAC Audio");
    }
    return fourCc.trimmed().toUpper();
}

void parseMp4MoovContainer(const QByteArray& moov, MediaMetadata* meta) {
    const auto* base = reinterpret_cast<const uint8_t*>(moov.constData());
    const size_t n = static_cast<size_t>(moov.size());

    std::function<void(size_t, size_t)> walkBoxes = [&](size_t offset, size_t end) {
        while (offset + 8 <= end) {
            uint64_t boxSize = readBe32(base + offset);
            const QByteArray boxType(reinterpret_cast<const char*>(base + offset + 4), 4);
            size_t headerSize = 8;
            if (boxSize == 1) {
                if (offset + 16 > end) {
                    break;
                }
                boxSize = readBe64(base + offset + 8);
                headerSize = 16;
            } else if (boxSize == 0) {
                boxSize = end - offset;
            }
            if (boxSize < headerSize || offset + boxSize > end) {
                break;
            }

            const size_t payloadStart = offset + headerSize;
            const size_t payloadEnd = offset + static_cast<size_t>(boxSize);
            const size_t payloadLen = payloadEnd - payloadStart;

            if (boxType == "mvhd" && payloadLen >= 20 && meta->durationSeconds <= 0) {
                const uint8_t version = base[payloadStart];
                if (version == 0 && payloadLen >= 20) {
                    const uint32_t timescale = readBe32(base + payloadStart + 12);
                    const uint32_t duration = readBe32(base + payloadStart + 16);
                    if (timescale > 0) {
                        meta->durationSeconds = static_cast<int>(duration / timescale);
                    }
                } else if (version == 1 && payloadLen >= 32) {
                    const uint32_t timescale = readBe32(base + payloadStart + 20);
                    const uint64_t duration = readBe64(base + payloadStart + 24);
                    if (timescale > 0) {
                        meta->durationSeconds = static_cast<int>(duration / timescale);
                    }
                }
            } else if (boxType == "tkhd" && payloadLen >= 84) {
                const uint8_t version = base[payloadStart];
                const size_t dimOff = (version == 1) ? 88 : 76;
                if (payloadLen >= dimOff + 8) {
                    const uint32_t w = readBe32(base + payloadStart + dimOff) >> 16;
                    const uint32_t h = readBe32(base + payloadStart + dimOff + 4) >> 16;
                    if (w > 0 && h > 0 && (meta->width <= 0 || meta->height <= 0)) {
                        meta->width = static_cast<int>(w);
                        meta->height = static_cast<int>(h);
                    }
                }
            } else if (boxType == "stsd" && payloadLen >= 16) {
                const QString fourCc = QString::fromLatin1(
                    reinterpret_cast<const char*>(base + payloadStart + 12), 4);
                const QString codecName = friendlyFourCcCodec(fourCc);
                if (!codecName.isEmpty() && codecName != QStringLiteral("AAC Audio") && meta->codec.isEmpty()) {
                    meta->codec = codecName;
                }
            } else if ((boxType == QByteArrayLiteral("\xA9" "nam") ||
                        boxType == QByteArrayLiteral("\xA9" "cmt") ||
                        boxType == QByteArrayLiteral("\xA9" "ART")) &&
                       payloadLen >= 16) {
                // iTunes metadata atom containing inner 'data' sub-box
                if (QByteArray(reinterpret_cast<const char*>(base + payloadStart + 4), 4) == "data" &&
                    payloadLen > 16) {
                    const QString text = QString::fromUtf8(
                        reinterpret_cast<const char*>(base + payloadStart + 16),
                        static_cast<qsizetype>(payloadLen - 16)).trimmed();
                    if (boxType == QByteArrayLiteral("\xA9" "nam") && meta->title.isEmpty()) {
                        meta->title = text;
                    } else if (boxType == QByteArrayLiteral("\xA9" "cmt") && meta->comment.isEmpty()) {
                        meta->comment = text;
                    } else if (boxType == QByteArrayLiteral("\xA9" "ART") && meta->artist.isEmpty()) {
                        meta->artist = text;
                    }
                }
            } else if (boxType == "trak" || boxType == "mdia" || boxType == "minf" ||
                       boxType == "stbl" || boxType == "udta" || boxType == "ilst") {
                walkBoxes(payloadStart, payloadEnd);
            } else if (boxType == "meta" && payloadLen > 4) {
                walkBoxes(payloadStart + 4, payloadEnd);
            }

            offset = payloadEnd;
        }
    };

    walkBoxes(0, n);
}

void inspectMp4ContainerHeaders(QFile& file, qint64 fileSize, MediaMetadata* meta) {
    qint64 pos = 0;
    int boxLimit = 64;
    while (pos + 8 <= fileSize && boxLimit-- > 0) {
        if (!file.seek(pos)) {
            break;
        }
        const QByteArray hdr = file.read(16);
        if (hdr.size() < 8) {
            break;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(hdr.constData());
        uint64_t boxSize = readBe32(p);
        const QByteArray boxType = hdr.mid(4, 4);
        qint64 headerLen = 8;
        if (boxSize == 1 && hdr.size() >= 16) {
            boxSize = readBe64(p + 8);
            headerLen = 16;
        } else if (boxSize == 0) {
            boxSize = static_cast<uint64_t>(fileSize - pos);
        }
        if (boxSize < static_cast<uint64_t>(headerLen) ||
            pos + static_cast<qint64>(boxSize) > fileSize) {
            break;
        }
        if (boxType == "moov" && boxSize <= 4ULL * 1024ULL * 1024ULL) {
            if (file.seek(pos + headerLen)) {
                const QByteArray moovData = file.read(static_cast<qint64>(boxSize) - headerLen);
                parseMp4MoovContainer(moovData, meta);
            }
            break;
        }
        pos += static_cast<qint64>(boxSize);
    }
}

void inspectAviContainerHeaders(QFile& file, MediaMetadata* meta) {
    if (!file.seek(0)) {
        return;
    }
    const QByteArray head = file.read(256);
    if (head.size() < 64 || head.left(4) != "RIFF" || head.mid(8, 4) != "AVI ") {
        return;
    }
    const qsizetype avihPos = head.indexOf("avih");
    if (avihPos >= 0 && avihPos + 44 <= head.size()) {
        const auto* d = reinterpret_cast<const uint8_t*>(head.constData() + avihPos + 8);
        const uint32_t usPerFrame = readLe32(d);
        const uint32_t totalFrames = readLe32(d + 16);
        const uint32_t w = readLe32(d + 32);
        const uint32_t h = readLe32(d + 36);
        if (w > 0 && h > 0 && meta->width <= 0) {
            meta->width = static_cast<int>(w);
            meta->height = static_cast<int>(h);
        }
        if (usPerFrame > 0) {
            if (meta->fps <= 0.0) {
                meta->fps = 1000000.0 / static_cast<double>(usPerFrame);
            }
            if (totalFrames > 0 && meta->durationSeconds <= 0) {
                meta->durationSeconds = static_cast<int>(
                    (static_cast<uint64_t>(totalFrames) * usPerFrame) / 1000000ULL);
            }
        }
    }
}

void inspectPngChunks(QFile& file, MediaMetadata* meta) {
    if (!file.seek(0)) {
        return;
    }
    const QByteArray buf = file.read(65536);
    if (buf.size() < 29 || static_cast<uint8_t>(buf[0]) != 0x89 || buf.mid(1, 3) != "PNG") {
        return;
    }
    const auto* d = reinterpret_cast<const uint8_t*>(buf.constData());
    size_t pos = 8;
    while (pos + 12 <= static_cast<size_t>(buf.size())) {
        const uint32_t len = readBe32(d + pos);
        const QByteArray type(reinterpret_cast<const char*>(d + pos + 4), 4);
        if (pos + 12ULL + len > static_cast<size_t>(buf.size())) {
            break;
        }
        const QByteArray payload(reinterpret_cast<const char*>(d + pos + 8), static_cast<qsizetype>(len));
        if (type == "IHDR" && payload.size() >= 8 && meta->width <= 0) {
            meta->width = static_cast<int>(readBe32(reinterpret_cast<const uint8_t*>(payload.constData())));
            meta->height = static_cast<int>(readBe32(reinterpret_cast<const uint8_t*>(payload.constData() + 4)));
        } else if (type == "tEXt") {
            const qsizetype sep = payload.indexOf('\0');
            if (sep > 0) {
                const QString k = QString::fromLatin1(payload.left(sep)).trimmed().toLower();
                const QString v = QString::fromLatin1(payload.mid(sep + 1)).trimmed();
                if ((k == QStringLiteral("title") || k == QStringLiteral("subject")) && meta->title.isEmpty()) {
                    meta->title = v;
                } else if ((k == QStringLiteral("description") || k == QStringLiteral("comment")) &&
                           meta->comment.isEmpty()) {
                    meta->comment = v;
                } else if (k == QStringLiteral("author") && meta->artist.isEmpty()) {
                    meta->artist = v;
                } else if (k == QStringLiteral("keywords") && meta->keywords.isEmpty()) {
                    meta->keywords = v;
                }
            }
        } else if (type == "IDAT" || type == "IEND") {
            break;
        }
        pos += 12ULL + len;
    }
}

void inspectSvgDocument(QFile& file, MediaMetadata* meta) {
    if (!file.seek(0)) {
        return;
    }
    const QByteArray xmlBytes = file.read(65536);
    QXmlStreamReader xml(xmlBytes);
    QStringList textSnippets;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString tag = xml.name().toString().toLower();
            if (tag == QStringLiteral("svg")) {
                const QString vb = xml.attributes().value(QStringLiteral("viewBox")).toString();
                const QStringList parts = vb.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
                if (parts.size() == 4 && meta->width <= 0) {
                    meta->width = static_cast<int>(std::round(parts[2].toDouble()));
                    meta->height = static_cast<int>(std::round(parts[3].toDouble()));
                }
            } else if (tag == QStringLiteral("title")) {
                const QString t = xml.readElementText().trimmed();
                if (!t.isEmpty() && meta->title.isEmpty()) {
                    meta->title = t;
                }
            } else if (tag == QStringLiteral("desc")) {
                const QString d = xml.readElementText().trimmed();
                if (!d.isEmpty() && meta->comment.isEmpty()) {
                    meta->comment = d;
                }
            } else if (tag == QStringLiteral("text") || tag == QStringLiteral("tspan")) {
                const QString tx = xml.readElementText().trimmed();
                if (!tx.isEmpty() && textSnippets.size() < 40) {
                    textSnippets.append(tx);
                }
            }
        }
    }
    if (!textSnippets.isEmpty()) {
        meta->extraText = textSnippets.join(QLatin1Char(' '));
    }
}

void inspectAudioHeaders(QFile& file, const QString& extLower, MediaMetadata* meta) {
    if (!file.seek(0)) {
        return;
    }
    const QByteArray head = file.read(32768);
    if (extLower == QStringLiteral("wav") && head.size() >= 44 &&
        head.left(4) == "RIFF" && head.mid(8, 4) == "WAVE") {
        const qsizetype fmtPos = head.indexOf("fmt ");
        if (fmtPos >= 0 && fmtPos + 24 <= head.size()) {
            const auto* f = reinterpret_cast<const uint8_t*>(head.constData() + fmtPos + 8);
            const uint16_t ch = readLe16(f + 2);
            const uint32_t sr = readLe32(f + 4);
            const uint32_t byteRate = readLe32(f + 8);
            if (ch > 0 && meta->channels <= 0) {
                meta->channels = static_cast<int>(ch);
            }
            if (sr > 0 && meta->sampleRateHz <= 0) {
                meta->sampleRateHz = static_cast<int>(sr);
            }
            if (byteRate > 0 && meta->durationSeconds <= 0) {
                const qsizetype dataPos = head.indexOf("data");
                if (dataPos >= 0 && dataPos + 8 <= head.size()) {
                    const uint32_t dataBytes = readLe32(
                        reinterpret_cast<const uint8_t*>(head.constData() + dataPos + 4));
                    meta->durationSeconds = static_cast<int>(dataBytes / byteRate);
                }
            }
        }
        return;
    }

    if (extLower == QStringLiteral("flac") && head.size() >= 42 && head.left(4) == "fLaC") {
        const auto* block = reinterpret_cast<const uint8_t*>(head.constData() + 4);
        const uint8_t type = block[0] & 0x7F;
        if (type == 0) {
            const uint32_t packed = readBe32(block + 14);
            const uint32_t sr = packed >> 12;
            const uint32_t ch = ((packed >> 9) & 0x07) + 1;
            const uint64_t totalSamples =
                (static_cast<uint64_t>(packed & 0x0F) << 32) | static_cast<uint64_t>(readBe32(block + 18));
            if (sr > 0) {
                if (meta->sampleRateHz <= 0) {
                    meta->sampleRateHz = static_cast<int>(sr);
                }
                if (meta->channels <= 0) {
                    meta->channels = static_cast<int>(ch);
                }
                if (totalSamples > 0 && meta->durationSeconds <= 0) {
                    meta->durationSeconds = static_cast<int>(totalSamples / sr);
                }
            }
        }
        return;
    }

    // Minimal ID3v2 / ID3v1 tag inspect for MP3
    if (head.size() >= 10 && head.left(3) == "ID3") {
        const auto* d = reinterpret_cast<const uint8_t*>(head.constData());
        const size_t tagSize =
            ((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F);
        const size_t scanEnd = std::min<size_t>(10 + tagSize, static_cast<size_t>(head.size()));
        size_t pos = 10;
        while (pos + 10 <= scanEnd) {
            const QByteArray fid(reinterpret_cast<const char*>(d + pos), 4);
            const uint32_t fsize = readBe32(d + pos + 4);
            if (fid[0] == '\0' || fsize == 0 || pos + 10ULL + fsize > scanEnd) {
                break;
            }
            const uint8_t enc = d[pos + 10];
            QString val;
            if (fsize > 1) {
                const char* textPtr = reinterpret_cast<const char*>(d + pos + 11);
                const qsizetype textLen = static_cast<qsizetype>(fsize - 1);
                if (enc == 0 || enc == 3) {
                    val = QString::fromUtf8(textPtr, textLen).remove(QChar('\0')).trimmed();
                }
            }
            if (fid == "TIT2" && meta->title.isEmpty()) {
                meta->title = val;
            } else if (fid == "TPE1" && meta->artist.isEmpty()) {
                meta->artist = val;
            } else if (fid == "TALB" && meta->album.isEmpty()) {
                meta->album = val;
            } else if (fid == "TCON" && meta->genre.isEmpty()) {
                meta->genre = val;
            }
            pos += 10ULL + fsize;
        }
    }
}

QString formatDurationHuman(int seconds) {
    if (seconds <= 0) {
        return QString();
    }
    const int hrs = seconds / 3600;
    const int mins = (seconds % 3600) / 60;
    const int secs = seconds % 60;
    if (hrs > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hrs, 2, 10, QLatin1Char('0'))
            .arg(mins, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(mins, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

QString resolutionTierLabel(int width, int height) {
    const int maxDim = std::max(width, height);
    const int minDim = std::min(width, height);
    if (maxDim >= 7680 || minDim >= 4320) {
        return QStringLiteral("8K UHD");
    }
    if (maxDim >= 3840 || minDim >= 2160) {
        return QStringLiteral("4K UHD");
    }
    if (maxDim >= 2560 || minDim >= 1440) {
        return QStringLiteral("1440p QHD");
    }
    if (maxDim >= 1920 || minDim >= 1080) {
        return QStringLiteral("1080p Full HD");
    }
    if (maxDim >= 1280 || minDim >= 720) {
        return QStringLiteral("720p HD");
    }
    return QStringLiteral("SD");
}

QString orientationLabel(int width, int height) {
    if (width <= 0 || height <= 0) {
        return QString();
    }
    if (width > height) {
        return QStringLiteral("Landscape");
    }
    if (height > width) {
        return QStringLiteral("Portrait");
    }
    return QStringLiteral("Square");
}

QString formatBytesHuman(qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024LL * 1024LL) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    if (bytes < 1024LL * 1024LL * 1024LL) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString tokenizeFileContext(const QString& filePath) {
    const QFileInfo fi(filePath);
    const QString stem = fi.completeBaseName();
    QStringList tokens = splitIdentifierWords(stem);
    const QString parentName = fi.dir().dirName();
    if (!parentName.isEmpty()) {
        for (const QString& w : splitIdentifierWords(parentName)) {
            if (!tokens.contains(w)) {
                tokens.append(w);
            }
        }
    }
    return tokens.join(QLatin1Char(' '));
}

} // namespace

const QStringList& DocumentExtractor::textDocumentExtensions() {
    static const QStringList kList = {
        QStringLiteral("txt"),  QStringLiteral("md"),   QStringLiteral("log"),
        QStringLiteral("rst"),  QStringLiteral("adoc"), QStringLiteral("org")
    };
    return kList;
}

const QStringList& DocumentExtractor::spreadsheetExtensions() {
    static const QStringList kList = {
        QStringLiteral("xlsx"), QStringLiteral("xlsm"),
        QStringLiteral("csv"),  QStringLiteral("tsv")
    };
    return kList;
}

const QStringList& DocumentExtractor::codeExtensions() {
    static const QStringList kList = {
        QStringLiteral("cpp"),  QStringLiteral("cc"),   QStringLiteral("cxx"),   QStringLiteral("c"),
        QStringLiteral("h"),    QStringLiteral("hpp"),  QStringLiteral("hh"),    QStringLiteral("hxx"),
        QStringLiteral("py"),   QStringLiteral("pyw"),  QStringLiteral("js"),    QStringLiteral("mjs"),
        QStringLiteral("cjs"),  QStringLiteral("jsx"),  QStringLiteral("ts"),    QStringLiteral("tsx"),
        QStringLiteral("rs"),   QStringLiteral("go"),   QStringLiteral("java"),  QStringLiteral("kt"),
        QStringLiteral("kts"),  QStringLiteral("cs"),   QStringLiteral("fs"),    QStringLiteral("vb"),
        QStringLiteral("swift"),QStringLiteral("dart"), QStringLiteral("rb"),    QStringLiteral("php"),
        QStringLiteral("lua"),  QStringLiteral("pl"),   QStringLiteral("r"),     QStringLiteral("scala"),
        QStringLiteral("sql"),  QStringLiteral("sh"),   QStringLiteral("bash"),  QStringLiteral("zsh"),
        QStringLiteral("ps1"),  QStringLiteral("psm1"), QStringLiteral("bat"),   QStringLiteral("cmd"),
        QStringLiteral("json"), QStringLiteral("jsonc"),QStringLiteral("yaml"),  QStringLiteral("yml"),
        QStringLiteral("toml"), QStringLiteral("ini"),  QStringLiteral("cfg"),   QStringLiteral("conf"),
        QStringLiteral("xml"),  QStringLiteral("html"), QStringLiteral("htm"),   QStringLiteral("css"),
        QStringLiteral("scss"), QStringLiteral("sass"), QStringLiteral("less"),  QStringLiteral("vue"),
        QStringLiteral("svelte"),QStringLiteral("cmake"),QStringLiteral("gradle"),QStringLiteral("proto"),
        QStringLiteral("graphql"),QStringLiteral("gql")
    };
    return kList;
}

const QStringList& DocumentExtractor::imageExtensions() {
    static const QStringList kList = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("webp"),
        QStringLiteral("bmp"),  QStringLiteral("gif"),  QStringLiteral("ico"),  QStringLiteral("svg"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("avif"), QStringLiteral("raw"),  QStringLiteral("cr2"),  QStringLiteral("nef"),
        QStringLiteral("arw"),  QStringLiteral("dng")
    };
    return kList;
}

const QStringList& DocumentExtractor::videoExtensions() {
    static const QStringList kList = {
        QStringLiteral("mp4"),  QStringLiteral("mkv"),  QStringLiteral("mov"),  QStringLiteral("avi"),
        QStringLiteral("webm"), QStringLiteral("wmv"),  QStringLiteral("m4v"),  QStringLiteral("flv"),
        QStringLiteral("mpg"),  QStringLiteral("mpeg"), QStringLiteral("m2ts"), QStringLiteral("3gp")
    };
    return kList;
}

const QStringList& DocumentExtractor::audioExtensions() {
    static const QStringList kList = {
        QStringLiteral("mp3"),  QStringLiteral("flac"), QStringLiteral("wav"),  QStringLiteral("m4a"),
        QStringLiteral("aac"),  QStringLiteral("ogg"),  QStringLiteral("opus"), QStringLiteral("wma"),
        QStringLiteral("aiff"), QStringLiteral("alac")
    };
    return kList;
}

QStringList DocumentExtractor::allContentSupportedExtensions() {
    QStringList all = {QStringLiteral("pdf"), QStringLiteral("docx")};
    all.append(textDocumentExtensions());
    all.append(spreadsheetExtensions());
    all.append(codeExtensions());
    all.append(imageExtensions());
    all.append(videoExtensions());
    all.append(audioExtensions());
    all.removeDuplicates();
    return all;
}

bool DocumentExtractor::isTextDocumentExtension(const QString& extensionLower) {
    static const QSet<QString> kSet = listToSet(textDocumentExtensions());
    return kSet.contains(extensionLower);
}

bool DocumentExtractor::isSpreadsheetExtension(const QString& extensionLower) {
    static const QSet<QString> kSet = listToSet(spreadsheetExtensions());
    return kSet.contains(extensionLower);
}

bool DocumentExtractor::isCodeExtension(const QString& extensionLower) {
    static const QSet<QString> kSet = listToSet(codeExtensions());
    return kSet.contains(extensionLower);
}

bool DocumentExtractor::isImageExtension(const QString& extensionLower) {
    static const QSet<QString> kSet = listToSet(imageExtensions());
    return kSet.contains(extensionLower);
}

bool DocumentExtractor::isVideoExtension(const QString& extensionLower) {
    static const QSet<QString> kSet = listToSet(videoExtensions());
    return kSet.contains(extensionLower);
}

bool DocumentExtractor::isAudioExtension(const QString& extensionLower) {
    static const QSet<QString> kSet = listToSet(audioExtensions());
    return kSet.contains(extensionLower);
}

bool DocumentExtractor::isMediaMetadataExtension(const QString& extensionLower) {
    return isImageExtension(extensionLower) ||
           isVideoExtension(extensionLower) ||
           isAudioExtension(extensionLower);
}

bool DocumentExtractor::isSupportedExtension(const QString& extensionLower) {
    static const QSet<QString> kAllSet = listToSet(allContentSupportedExtensions());
    return kAllSet.contains(extensionLower);
}

ExtractedDocument DocumentExtractor::extractXlsxBytes(const QByteArray& zipBytes) {
    ExtractedDocument doc;

    QHash<QString, QByteArray> entries;
    QString zipError;
    const bool ok = readZipArchiveEntries(
        zipBytes,
        [](const QString& name) {
            return name == QStringLiteral("xl/sharedStrings.xml") ||
                   name == QStringLiteral("xl/workbook.xml") ||
                   name == QStringLiteral("xl/_rels/workbook.xml.rels") ||
                   (name.startsWith(QStringLiteral("xl/worksheets/")) && name.endsWith(QStringLiteral(".xml")));
        },
        &entries,
        &zipError);
    if (!ok) {
        doc.errorReason = zipError;
        return doc;
    }

    // 1. Parse xl/sharedStrings.xml
    QStringList sharedStrings;
    if (entries.contains(QStringLiteral("xl/sharedStrings.xml"))) {
        QXmlStreamReader ssXml(entries.value(QStringLiteral("xl/sharedStrings.xml")));
        bool inSi = false;
        QString siAccum;
        while (!ssXml.atEnd()) {
            const auto token = ssXml.readNext();
            if (token == QXmlStreamReader::StartElement) {
                const QString elem = ssXml.name().toString();
                if (elem == QStringLiteral("si")) {
                    inSi = true;
                    siAccum.clear();
                } else if (inSi && elem == QStringLiteral("t")) {
                    siAccum.append(ssXml.readElementText(QXmlStreamReader::IncludeChildElements));
                }
            } else if (token == QXmlStreamReader::EndElement && ssXml.name() == QStringLiteral("si")) {
                sharedStrings.append(siAccum.trimmed());
                inSi = false;
            }
        }
    }

    // 2. Map relationship IDs to sheet target paths
    QHash<QString, QString> relTargetById;
    if (entries.contains(QStringLiteral("xl/_rels/workbook.xml.rels"))) {
        QXmlStreamReader relXml(entries.value(QStringLiteral("xl/_rels/workbook.xml.rels")));
        while (!relXml.atEnd()) {
            if (relXml.readNext() == QXmlStreamReader::StartElement &&
                relXml.name() == QStringLiteral("Relationship")) {
                const QString id = relXml.attributes().value(QStringLiteral("Id")).toString();
                const QString target = relXml.attributes().value(QStringLiteral("Target")).toString();
                if (!id.isEmpty() && !target.isEmpty()) {
                    relTargetById.insert(id, normalizeZipTargetPath(target));
                }
            }
        }
    }

    // 3. Determine ordered sheets and names
    struct OrderedSheet {
        QString displayName;
        QString entryPath;
    };
    QVector<OrderedSheet> sheets;
    if (entries.contains(QStringLiteral("xl/workbook.xml"))) {
        QXmlStreamReader wbXml(entries.value(QStringLiteral("xl/workbook.xml")));
        int fallbackIdx = 1;
        while (!wbXml.atEnd()) {
            if (wbXml.readNext() == QXmlStreamReader::StartElement &&
                wbXml.name() == QStringLiteral("sheet")) {
                const QString name = wbXml.attributes().value(QStringLiteral("name")).toString().trimmed();
                const QString rid = wbXml.attributes().value(
                    QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
                    QStringLiteral("id")).toString();
                QString path = relTargetById.value(rid);
                if (path.isEmpty()) {
                    path = QStringLiteral("xl/worksheets/sheet%1.xml").arg(fallbackIdx);
                }
                if (entries.contains(path)) {
                    sheets.append({name.isEmpty() ? QStringLiteral("Sheet%1").arg(fallbackIdx) : name, path});
                }
                ++fallbackIdx;
            }
        }
    }

    if (sheets.isEmpty()) {
        QStringList worksheetPaths;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("xl/worksheets/")) &&
                it.key().endsWith(QStringLiteral(".xml"))) {
                worksheetPaths.append(it.key());
            }
        }
        std::sort(worksheetPaths.begin(), worksheetPaths.end());
        for (int i = 0; i < worksheetPaths.size(); ++i) {
            sheets.append({QFileInfo(worksheetPaths[i]).completeBaseName(), worksheetPaths[i]});
        }
    }

    if (sheets.isEmpty()) {
        doc.errorReason = QStringLiteral("No worksheet XML streams found in XLSX archive.");
        return doc;
    }

    QString combined;
    int sheetPage = 0;
    for (const auto& sheet : sheets) {
        QXmlStreamReader sheetXml(entries.value(sheet.entryPath));
        QStringList rowLines;
        QStringList currentRowCells;
        QString cellType;
        bool inCell = false;

        while (!sheetXml.atEnd()) {
            const auto token = sheetXml.readNext();
            if (token == QXmlStreamReader::StartElement) {
                const QString elem = sheetXml.name().toString();
                if (elem == QStringLiteral("row")) {
                    currentRowCells.clear();
                } else if (elem == QStringLiteral("c")) {
                    inCell = true;
                    cellType = sheetXml.attributes().value(QStringLiteral("t")).toString();
                } else if (inCell && elem == QStringLiteral("v")) {
                    const QString rawVal = sheetXml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                    if (!rawVal.isEmpty()) {
                        if (cellType == QStringLiteral("s")) {
                            bool intOk = false;
                            const int idx = rawVal.toInt(&intOk);
                            if (intOk && idx >= 0 && idx < sharedStrings.size()) {
                                currentRowCells.append(sharedStrings.at(idx));
                            }
                        } else if (cellType == QStringLiteral("b")) {
                            currentRowCells.append(rawVal == QStringLiteral("1")
                                                       ? QStringLiteral("TRUE")
                                                       : QStringLiteral("FALSE"));
                        } else {
                            currentRowCells.append(rawVal);
                        }
                    }
                } else if (inCell && elem == QStringLiteral("is")) {
                    const QString inlineText = sheetXml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                    if (!inlineText.isEmpty()) {
                        currentRowCells.append(inlineText);
                    }
                }
            } else if (token == QXmlStreamReader::EndElement) {
                const QString elem = sheetXml.name().toString();
                if (elem == QStringLiteral("c")) {
                    inCell = false;
                } else if (elem == QStringLiteral("row") && !currentRowCells.isEmpty()) {
                    rowLines.append(currentRowCells.join(QStringLiteral(" | ")));
                }
            }
        }

        const QString body = normalizeDocumentText(rowLines.join(QLatin1Char('\n')));
        if (!body.isEmpty()) {
            ++sheetPage;
            if (!combined.isEmpty()) {
                combined.append(QStringLiteral("\n\n"));
            }
            doc.pageOffsets.append({sheetPage, static_cast<int>(combined.size())});
            combined.append(QStringLiteral("[Sheet: %1]\n%2").arg(sheet.displayName, body));
        }
    }

    doc.normalizedText = normalizeDocumentText(combined);
    doc.pageCount = std::max(1, sheetPage);
    if (doc.pageOffsets.isEmpty()) {
        doc.pageOffsets.append({1, 0});
    }
    doc.success = !doc.normalizedText.isEmpty();
    if (!doc.success) {
        doc.errorReason = QStringLiteral("No extractable cell content found in XLSX workbook.");
    }
    return doc;
}

ExtractedDocument DocumentExtractor::extractDelimitedTableBytes(const QByteArray& rawBytes, QChar delimiter) {
    ExtractedDocument doc;
    QString rawText;
    if (!decodeTextBytesSafely(rawBytes, &rawText, &doc.errorReason)) {
        return doc;
    }

    const QStringList rawLines = rawText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (rawLines.isEmpty()) {
        doc.errorReason = QStringLiteral("Empty spreadsheet table.");
        return doc;
    }

    QStringList headerCols = parseDelimitedLine(rawLines.first().trimmed(), delimiter);
    const bool hasStructuredHeader = headerCols.size() >= 2 && headerCols.size() <= 24;

    QStringList formattedRows;
    formattedRows.reserve(std::min<qsizetype>(rawLines.size(), 4000));
    if (hasStructuredHeader) {
        formattedRows.append(QStringLiteral("[Columns: %1]").arg(headerCols.join(QStringLiteral(" | "))));
    }

    const qsizetype maxRows = std::min<qsizetype>(rawLines.size(), 4000);
    for (qsizetype r = hasStructuredHeader ? 1 : 0; r < maxRows; ++r) {
        const QString line = rawLines.at(r).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList cells = parseDelimitedLine(line, delimiter);
        if (cells.isEmpty()) {
            continue;
        }
        if (hasStructuredHeader && cells.size() == headerCols.size()) {
            QStringList pairs;
            pairs.reserve(cells.size());
            for (qsizetype c = 0; c < cells.size(); ++c) {
                if (!cells[c].isEmpty()) {
                    pairs.append(QStringLiteral("%1: %2").arg(headerCols[c], cells[c]));
                }
            }
            if (!pairs.isEmpty()) {
                formattedRows.append(pairs.join(QStringLiteral(" | ")));
            }
        } else {
            formattedRows.append(cells.join(QStringLiteral(" | ")));
        }
    }

    doc.normalizedText = normalizeDocumentText(formattedRows.join(QLatin1Char('\n')));
    doc.pageCount = 1;
    doc.pageOffsets.append({1, 0});
    doc.success = !doc.normalizedText.isEmpty();
    if (!doc.success) {
        doc.errorReason = QStringLiteral("No extractable table rows found.");
    }
    return doc;
}

ExtractedDocument DocumentExtractor::extractCodeBytes(
    const QByteArray& rawBytes,
    const QString& fileName,
    const QString& extensionLower) {
    ExtractedDocument doc;
    QString rawText;
    if (!decodeTextBytesSafely(rawBytes, &rawText, &doc.errorReason)) {
        return doc;
    }

    const QString normalizedBody = normalizeDocumentText(rawText);
    if (normalizedBody.isEmpty()) {
        doc.errorReason = QStringLiteral("Source file contains no indexable text.");
        return doc;
    }

    const QString lang = readableCodeLanguageName(extensionLower);
    const QString topics = buildCodeTopicSummary(fileName, normalizedBody);

    QString combined;
    if (!fileName.isEmpty()) {
        combined.append(QStringLiteral("[%1 · %2]\n").arg(lang, fileName));
    } else {
        combined.append(QStringLiteral("[%1]\n").arg(lang));
    }
    if (!topics.isEmpty()) {
        combined.append(QStringLiteral("Topics: %1\n\n").arg(topics));
    }
    combined.append(normalizedBody);

    if (combined.size() > kMaxExtractedChars) {
        combined.truncate(kMaxExtractedChars);
    }

    doc.normalizedText = std::move(combined);
    doc.pageCount = 1;
    doc.pageOffsets.append({1, 0});
    doc.success = true;
    return doc;
}

ExtractedDocument DocumentExtractor::extractMediaMetadataFromFile(
    const QString& filePath,
    const QString& extensionLower,
    qint64 fileSizeBytes) {
    ExtractedDocument doc;
    MediaMetadata meta;

#ifdef _WIN32
    queryWindowsPropertyStore(filePath, &meta);
#endif

    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly)) {
        if (isImageExtension(extensionLower)) {
            if (extensionLower == QStringLiteral("svg")) {
                inspectSvgDocument(file, &meta);
            } else {
                if (meta.width <= 0 || meta.height <= 0) {
                    QImageReader reader(&file, extensionLower.toLatin1());
                    const QSize sz = reader.size();
                    if (sz.isValid()) {
                        meta.width = sz.width();
                        meta.height = sz.height();
                    }
                }
                if (extensionLower == QStringLiteral("png")) {
                    inspectPngChunks(file, &meta);
                }
            }
        } else if (isVideoExtension(extensionLower)) {
            if (extensionLower == QStringLiteral("mp4") ||
                extensionLower == QStringLiteral("m4v") ||
                extensionLower == QStringLiteral("mov") ||
                extensionLower == QStringLiteral("3gp")) {
                inspectMp4ContainerHeaders(file, fileSizeBytes, &meta);
            } else if (extensionLower == QStringLiteral("avi")) {
                inspectAviContainerHeaders(file, &meta);
            }
        } else if (isAudioExtension(extensionLower)) {
            inspectAudioHeaders(file, extensionLower, &meta);
        }
        file.close();
    }

    QString kindLabel = QStringLiteral("Media");
    if (isVideoExtension(extensionLower)) {
        kindLabel = QStringLiteral("Video");
    } else if (isImageExtension(extensionLower)) {
        kindLabel = QStringLiteral("Image");
    } else if (isAudioExtension(extensionLower)) {
        kindLabel = QStringLiteral("Audio");
    }

    QStringList headerParts = {kindLabel, extensionLower.toUpper()};
    if (meta.width > 0 && meta.height > 0) {
        headerParts.append(QStringLiteral("%1x%2").arg(meta.width).arg(meta.height));
        const QString tier = resolutionTierLabel(meta.width, meta.height);
        if (!tier.isEmpty()) {
            headerParts.append(tier);
        }
        const QString orient = orientationLabel(meta.width, meta.height);
        if (!orient.isEmpty()) {
            headerParts.append(orient);
        }
        if (isImageExtension(extensionLower)) {
            const double mp = (static_cast<double>(meta.width) * static_cast<double>(meta.height)) / 1e6;
            if (mp >= 0.1) {
                headerParts.append(QStringLiteral("%1 MP").arg(mp, 0, 'f', 1));
            }
        }
    }

    const QString durStr = formatDurationHuman(meta.durationSeconds);
    if (!durStr.isEmpty()) {
        headerParts.append(durStr);
    }
    if (meta.fps > 1.0) {
        headerParts.append(QStringLiteral("%1 fps").arg(meta.fps, 0, 'f', (meta.fps == std::floor(meta.fps)) ? 0 : 1));
    }
    if (!meta.codec.isEmpty()) {
        headerParts.append(meta.codec);
    }
    if (meta.sampleRateHz > 0) {
        headerParts.append(QStringLiteral("%1 kHz").arg(static_cast<double>(meta.sampleRateHz) / 1000.0, 0, 'f', 1));
    }
    if (meta.channels == 1) {
        headerParts.append(QStringLiteral("Mono"));
    } else if (meta.channels == 2) {
        headerParts.append(QStringLiteral("Stereo"));
    } else if (meta.channels > 2) {
        headerParts.append(QStringLiteral("%1ch").arg(meta.channels));
    }
    if (fileSizeBytes > 0) {
        headerParts.append(formatBytesHuman(fileSizeBytes));
    }

    QStringList lines;
    lines.append(QStringLiteral("[%1]").arg(headerParts.join(QStringLiteral(" · "))));

    if (!meta.title.isEmpty()) {
        lines.append(QStringLiteral("Title: %1").arg(meta.title));
    }
    if (!meta.artist.isEmpty()) {
        lines.append(QStringLiteral("Artist: %1").arg(meta.artist));
    }
    if (!meta.album.isEmpty()) {
        lines.append(QStringLiteral("Album: %1").arg(meta.album));
    }
    if (!meta.genre.isEmpty()) {
        lines.append(QStringLiteral("Genre: %1").arg(meta.genre));
    }
    if (!meta.cameraMake.isEmpty() || !meta.cameraModel.isEmpty()) {
        const QString camera = QStringLiteral("%1 %2").arg(meta.cameraMake, meta.cameraModel).trimmed();
        lines.append(QStringLiteral("Camera: %1").arg(camera));
    }
    if (meta.bitrateKbps > 0) {
        lines.append(QStringLiteral("Bitrate: %1 kbps").arg(meta.bitrateKbps));
    }
    if (!meta.keywords.isEmpty()) {
        lines.append(QStringLiteral("Keywords: %1").arg(meta.keywords));
    }
    if (!meta.comment.isEmpty()) {
        lines.append(QStringLiteral("Notes: %1").arg(meta.comment));
    }
    if (!meta.extraText.isEmpty()) {
        lines.append(QStringLiteral("Labels: %1").arg(meta.extraText));
    }

    const QString fileContext = tokenizeFileContext(filePath);
    if (!fileContext.isEmpty()) {
        lines.append(QStringLiteral("Context: %1").arg(fileContext));
    }

    doc.normalizedText = normalizeDocumentText(lines.join(QLatin1Char('\n')));
    doc.pageCount = 1;
    doc.pageOffsets.append({1, 0});
    doc.success = !doc.normalizedText.isEmpty();
    return doc;
}

QByteArray DocumentExtractor::buildValidXlsxBytes(const QVector<XlsxSheetSpec>& sheets) {
    QByteArray archive;
    QByteArray centralDir;
    uint16_t entryCount = 0;

    // Collect deduplicated shared strings for numeric/text realism
    QStringList sharedStrings;
    QHash<QString, int> sharedIndex;
    auto internString = [&](const QString& text) -> int {
        const auto it = sharedIndex.constFind(text);
        if (it != sharedIndex.constEnd()) {
            return it.value();
        }
        const int idx = static_cast<int>(sharedStrings.size());
        sharedStrings.append(text);
        sharedIndex.insert(text, idx);
        return idx;
    };

    QByteArray wbXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                       "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                       "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    QByteArray relsXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";

    for (int sIdx = 0; sIdx < sheets.size(); ++sIdx) {
        const auto& spec = sheets.at(sIdx);
        const int sheetNum = sIdx + 1;
        const QString rid = QStringLiteral("rId%1").arg(sheetNum);
        const QString target = QStringLiteral("worksheets/sheet%1.xml").arg(sheetNum);
        const QString sheetName = spec.sheetName.isEmpty() ? QStringLiteral("Sheet%1").arg(sheetNum) : spec.sheetName;

        wbXml.append(QStringLiteral("<sheet name=\"%1\" sheetId=\"%2\" r:id=\"%3\"/>")
                         .arg(escapeXmlText(sheetName))
                         .arg(sheetNum)
                         .arg(rid)
                         .toUtf8());
        relsXml.append(QStringLiteral("<Relationship Id=\"%1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"%2\"/>")
                           .arg(rid, target)
                           .toUtf8());

        QByteArray wsXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                           "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>";
        for (int r = 0; r < spec.rows.size(); ++r) {
            const int rowNum = r + 1;
            wsXml.append(QStringLiteral("<row r=\"%1\">").arg(rowNum).toUtf8());
            const QStringList& rowCells = spec.rows.at(r);
            for (int c = 0; c < rowCells.size(); ++c) {
                const QString cellRef = QStringLiteral("%1%2").arg(columnIndexToLetters(c)).arg(rowNum);
                const QString& cellVal = rowCells.at(c);
                bool isNumeric = false;
                (void)cellVal.toDouble(&isNumeric);
                if (isNumeric && !cellVal.isEmpty()) {
                    wsXml.append(QStringLiteral("<c r=\"%1\"><v>%2</v></c>")
                                     .arg(cellRef, escapeXmlText(cellVal))
                                     .toUtf8());
                } else {
                    const int sId = internString(cellVal);
                    wsXml.append(QStringLiteral("<c r=\"%1\" t=\"s\"><v>%2</v></c>")
                                     .arg(cellRef)
                                     .arg(sId)
                                     .toUtf8());
                }
            }
            wsXml.append("</row>");
        }
        wsXml.append("</sheetData></worksheet>");
        writeStoredZipEntry(&archive, &centralDir, &entryCount, QStringLiteral("xl/") + target, wsXml);
    }

    wbXml.append("</sheets></workbook>");
    relsXml.append("</Relationships>");

    QByteArray ssXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                       "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
    for (const QString& s : sharedStrings) {
        ssXml.append(QStringLiteral("<si><t>%1</t></si>").arg(escapeXmlText(s)).toUtf8());
    }
    ssXml.append("</sst>");

    writeStoredZipEntry(&archive, &centralDir, &entryCount, QStringLiteral("xl/sharedStrings.xml"), ssXml);
    writeStoredZipEntry(&archive, &centralDir, &entryCount, QStringLiteral("xl/_rels/workbook.xml.rels"), relsXml);
    writeStoredZipEntry(&archive, &centralDir, &entryCount, QStringLiteral("xl/workbook.xml"), wbXml);

    const uint32_t cdOffset = static_cast<uint32_t>(archive.size());
    archive.append(centralDir);
    const uint32_t cdSize = static_cast<uint32_t>(centralDir.size());

    appendLe32(&archive, 0x06054B50u);
    appendLe16(&archive, 0);
    appendLe16(&archive, 0);
    appendLe16(&archive, entryCount);
    appendLe16(&archive, entryCount);
    appendLe32(&archive, cdSize);
    appendLe32(&archive, cdOffset);
    appendLe16(&archive, 0);

    return archive;
}

QByteArray DocumentExtractor::buildValidMp4MetadataBytes(
    quint32 width,
    quint32 height,
    quint32 durationSeconds,
    const QString& codecFourCc,
    const QString& title) {
    auto appendBe32 = [](QByteArray* out, uint32_t v) {
        out->append(static_cast<char>((v >> 24) & 0xFF));
        out->append(static_cast<char>((v >> 16) & 0xFF));
        out->append(static_cast<char>((v >> 8) & 0xFF));
        out->append(static_cast<char>(v & 0xFF));
    };
    auto makeBox = [&](const char type[4], const QByteArray& payload) -> QByteArray {
        QByteArray box;
        appendBe32(&box, static_cast<uint32_t>(8 + payload.size()));
        box.append(type, 4);
        box.append(payload);
        return box;
    };

    // ftyp box
    QByteArray ftypPayload;
    ftypPayload.append("isom");
    appendBe32(&ftypPayload, 0x00000200u);
    ftypPayload.append("isomiso2mp41");
    const QByteArray ftypBox = makeBox("ftyp", ftypPayload);

    // mvhd (version 0: 100 bytes payload)
    QByteArray mvhdPayload(100, '\0');
    auto writeBe32At = [](QByteArray& arr, int off, uint32_t v) {
        arr[off] = static_cast<char>((v >> 24) & 0xFF);
        arr[off + 1] = static_cast<char>((v >> 16) & 0xFF);
        arr[off + 2] = static_cast<char>((v >> 8) & 0xFF);
        arr[off + 3] = static_cast<char>(v & 0xFF);
    };
    writeBe32At(mvhdPayload, 12, 1000u);
    writeBe32At(mvhdPayload, 16, durationSeconds * 1000u);
    const QByteArray mvhdBox = makeBox("mvhd", mvhdPayload);

    // tkhd (version 0: 84 bytes payload, width & height at offset 76 & 80 in 16.16 fixed point)
    QByteArray tkhdPayload(84, '\0');
    writeBe32At(tkhdPayload, 76, width << 16);
    writeBe32At(tkhdPayload, 80, height << 16);
    const QByteArray tkhdBox = makeBox("tkhd", tkhdPayload);

    // stsd (8 bytes fullbox/count + 8 bytes sample entry header including 4-char codec FourCC)
    QByteArray stsdPayload(16, '\0');
    writeBe32At(stsdPayload, 4, 1u);
    writeBe32At(stsdPayload, 8, 8u);
    const QByteArray ccBytes = codecFourCc.leftJustified(4, QLatin1Char(' '), true).toLatin1();
    for (int i = 0; i < 4; ++i) {
        stsdPayload[12 + i] = ccBytes[i];
    }
    const QByteArray stsdBox = makeBox("stsd", stsdPayload);
    const QByteArray stblBox = makeBox("stbl", stsdBox);
    const QByteArray minfBox = makeBox("minf", stblBox);
    const QByteArray mdiaBox = makeBox("mdia", minfBox);
    const QByteArray trakBox = makeBox("trak", tkhdBox + mdiaBox);

    QByteArray moovPayload = mvhdBox + trakBox;
    if (!title.isEmpty()) {
        QByteArray dataPayload(8, '\0');
        writeBe32At(dataPayload, 0, 1u); // UTF-8
        dataPayload.append(title.toUtf8());
        const QByteArray dataBox = makeBox("data", dataPayload);
        const QByteArray namBox = makeBox("\xA9nam", dataBox);
        const QByteArray ilstBox = makeBox("ilst", namBox);
        QByteArray metaPayload(4, '\0');
        metaPayload.append(ilstBox);
        const QByteArray metaBox = makeBox("meta", metaPayload);
        moovPayload.append(makeBox("udta", metaBox));
    }

    return ftypBox + makeBox("moov", moovPayload);
}

} // namespace lumen::extraction
