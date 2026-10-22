#include "extraction/DocumentExtractor.h"

#include "extraction/DeflateCodec.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstring>

namespace lumen::extraction {

namespace {

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void appendLe16(QByteArray* out, uint16_t v) {
    out->append(char(v & 0xFF));
    out->append(char((v >> 8) & 0xFF));
}

void appendLe32(QByteArray* out, uint32_t v) {
    out->append(char(v & 0xFF));
    out->append(char((v >> 8) & 0xFF));
    out->append(char((v >> 16) & 0xFF));
    out->append(char((v >> 24) & 0xFF));
}

QString decodePdfLiteralString(const QByteArray& content, qsizetype startIdx, qsizetype* outEndIdx) {
    QByteArray raw;
    int depth = 1;
    qsizetype i = startIdx;
    while (i < content.size() && depth > 0) {
        const char c = content.at(i);
        if (c == '\\') {
            if (i + 1 >= content.size()) {
                break;
            }
            const char next = content.at(++i);
            switch (next) {
                case 'n': raw.append('\n'); break;
                case 'r': raw.append('\r'); break;
                case 't': raw.append('\t'); break;
                case 'b': raw.append('\b'); break;
                case 'f': raw.append('\f'); break;
                case '(': raw.append('('); break;
                case ')': raw.append(')'); break;
                case '\\': raw.append('\\'); break;
                case '\r':
                    if (i + 1 < content.size() && content.at(i + 1) == '\n') {
                        ++i;
                    }
                    break;
                case '\n':
                    break;
                default:
                    if (next >= '0' && next <= '7') {
                        int oct = next - '0';
                        for (int k = 0; k < 2 && i + 1 < content.size(); ++k) {
                            const char oc = content.at(i + 1);
                            if (oc >= '0' && oc <= '7') {
                                oct = (oct * 8) + (oc - '0');
                                ++i;
                            } else {
                                break;
                            }
                        }
                        raw.append(static_cast<char>(oct & 0xFF));
                    } else {
                        raw.append(next);
                    }
                    break;
            }
        } else if (c == '(') {
            ++depth;
            raw.append(c);
        } else if (c == ')') {
            --depth;
            if (depth > 0) {
                raw.append(c);
            }
        } else {
            raw.append(c);
        }
        ++i;
    }
    if (outEndIdx != nullptr) {
        *outEndIdx = i;
    }

    // Check UTF-16BE BOM
    if (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFE && static_cast<uint8_t>(raw[1]) == 0xFF) {
        QString utf16;
        for (qsizetype j = 2; j + 1 < raw.size(); j += 2) {
            const uint16_t ch = (static_cast<uint16_t>(static_cast<uint8_t>(raw[j])) << 8) |
                                static_cast<uint16_t>(static_cast<uint8_t>(raw[j + 1]));
            utf16.append(QChar(ch));
        }
        return utf16;
    }
    return QString::fromUtf8(raw);
}

QString decodePdfHexString(const QByteArray& content, qsizetype startIdx, qsizetype* outEndIdx) {
    QByteArray hexDigits;
    qsizetype i = startIdx;
    while (i < content.size() && content.at(i) != '>') {
        const char c = content.at(i++);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            hexDigits.append(c);
        }
    }
    if (i < content.size() && content.at(i) == '>') {
        ++i;
    }
    if (outEndIdx != nullptr) {
        *outEndIdx = i;
    }
    if ((hexDigits.size() & 1) != 0) {
        hexDigits.append('0');
    }
    const QByteArray bytes = QByteArray::fromHex(hexDigits);
    if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFE && static_cast<uint8_t>(bytes[1]) == 0xFF) {
        QString utf16;
        for (qsizetype j = 2; j + 1 < bytes.size(); j += 2) {
            const uint16_t ch = (static_cast<uint16_t>(static_cast<uint8_t>(bytes[j])) << 8) |
                                static_cast<uint16_t>(static_cast<uint8_t>(bytes[j + 1]));
            utf16.append(QChar(ch));
        }
        return utf16;
    }
    return QString::fromLatin1(bytes);
}

QString parsePdfContentStreamText(const QByteArray& streamData) {
    QString result;
    qsizetype pos = 0;
    const qsizetype n = streamData.size();

    while (pos < n) {
        // Find next BT (Begin Text) token or string literal
        const char c = streamData.at(pos);
        if (c == '(') {
            qsizetype nextPos = pos + 1;
            const QString s = decodePdfLiteralString(streamData, pos + 1, &nextPos);
            if (!s.isEmpty()) {
                result.append(s);
            }
            pos = nextPos;
            // Check if followed by T*, TD, Td, ', or " which imply space/line break
            while (pos < n && (streamData.at(pos) == ' ' || streamData.at(pos) == '\r' || streamData.at(pos) == '\n')) {
                ++pos;
            }
            if (pos + 1 < n && streamData.at(pos) == 'T' &&
                (streamData.at(pos + 1) == 'j' || streamData.at(pos + 1) == 'J' ||
                 streamData.at(pos + 1) == 'd' || streamData.at(pos + 1) == 'D' ||
                 streamData.at(pos + 1) == '*')) {
                if (streamData.at(pos + 1) == 'd' || streamData.at(pos + 1) == 'D' || streamData.at(pos + 1) == '*') {
                    result.append(QLatin1Char('\n'));
                } else {
                    result.append(QLatin1Char(' '));
                }
                pos += 2;
            }
        } else if (c == '[') {
            // TJ array: [(Hello) -120 (World)] TJ
            ++pos;
            while (pos < n && streamData.at(pos) != ']') {
                const char ac = streamData.at(pos);
                if (ac == '(') {
                    qsizetype nextPos = pos + 1;
                    result.append(decodePdfLiteralString(streamData, pos + 1, &nextPos));
                    pos = nextPos;
                } else if (ac == '<' && (pos + 1 >= n || streamData.at(pos + 1) != '<')) {
                    qsizetype nextPos = pos + 1;
                    result.append(decodePdfHexString(streamData, pos + 1, &nextPos));
                    pos = nextPos;
                } else if (ac == '-') {
                    // Large negative kerning (< -100) in TJ array represents an inter-word space
                    ++pos;
                    int val = 0;
                    while (pos < n && streamData.at(pos) >= '0' && streamData.at(pos) <= '9') {
                        val = val * 10 + (streamData.at(pos) - '0');
                        ++pos;
                    }
                    if (val >= 100 && !result.endsWith(QLatin1Char(' '))) {
                        result.append(QLatin1Char(' '));
                    }
                } else {
                    ++pos;
                }
            }
            if (pos < n && streamData.at(pos) == ']') {
                ++pos;
            }
            result.append(QLatin1Char(' '));
        } else if (c == 'T' && pos + 1 < n &&
                   (streamData.at(pos + 1) == 'D' || streamData.at(pos + 1) == 'd' || streamData.at(pos + 1) == '*')) {
            if (!result.isEmpty() && !result.endsWith(QLatin1Char('\n'))) {
                result.append(QLatin1Char('\n'));
            }
            pos += 2;
        } else if (c == 'E' && pos + 1 < n && streamData.at(pos + 1) == 'T') {
            if (!result.isEmpty() && !result.endsWith(QLatin1Char('\n'))) {
                result.append(QLatin1Char('\n'));
            }
            pos += 2;
        } else {
            ++pos;
        }
    }
    return result;
}

} // namespace

bool DocumentExtractor::isSupportedExtension(const QString& extensionLower) {
    return extensionLower == QStringLiteral("txt") ||
           extensionLower == QStringLiteral("md") ||
           extensionLower == QStringLiteral("pdf") ||
           extensionLower == QStringLiteral("docx");
}

QString DocumentExtractor::normalizeDocumentText(const QString& input, int maxChars) {
    if (input.isEmpty()) {
        return {};
    }

    const QString nfkc = input.normalized(QString::NormalizationForm_KC);
    QString out;
    out.reserve(std::min<int>(static_cast<int>(nfkc.size()), maxChars));

    bool lastWasSpace = false;
    int consecutiveNewlines = 0;

    for (const QChar ch : nfkc) {
        if (out.size() >= maxChars) {
            break;
        }
        if (ch == QLatin1Char('\r')) {
            continue;
        }
        if (ch == QLatin1Char('\n')) {
            if (consecutiveNewlines < 2 && !out.isEmpty()) {
                out.append(QLatin1Char('\n'));
                ++consecutiveNewlines;
            }
            lastWasSpace = true;
            continue;
        }
        if (ch.isSpace() || ch == QChar(0x00A0)) {
            if (!lastWasSpace && !out.isEmpty()) {
                out.append(QLatin1Char(' '));
                lastWasSpace = true;
            }
            continue;
        }
        if (ch.category() == QChar::Other_Control && ch != QLatin1Char('\t')) {
            continue;
        }
        out.append(ch);
        lastWasSpace = false;
        consecutiveNewlines = 0;
    }

    return out.trimmed();
}

ExtractedDocument DocumentExtractor::extractFromFile(const QString& filePath, qint64 maxFileSizeBytes) {
    QElapsedTimer timer;
    timer.start();

    ExtractedDocument doc;
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        doc.errorReason = QStringLiteral("File does not exist.");
        return doc;
    }

    const qint64 fileSize = fi.size();
    if (fileSize <= 0) {
        doc.errorReason = QStringLiteral("File is empty.");
        return doc;
    }
    if (fileSize > maxFileSizeBytes) {
        doc.errorReason = QStringLiteral("File size (%1 bytes) exceeds limit (%2 bytes).")
                              .arg(fileSize)
                              .arg(maxFileSizeBytes);
        return doc;
    }

    const QString ext = fi.suffix().toLower();
    if (!isSupportedExtension(ext)) {
        doc.errorReason = QStringLiteral("Unsupported document extension: %1").arg(ext);
        return doc;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        doc.errorReason = QStringLiteral("Failed to open file for reading.");
        return doc;
    }

    const QByteArray rawBytes = file.readAll();
    file.close();

    if (ext == QStringLiteral("txt") || ext == QStringLiteral("md")) {
        doc = extractTxtOrMarkdown(rawBytes);
    } else if (ext == QStringLiteral("docx")) {
        doc = extractDocxBytes(rawBytes);
    } else if (ext == QStringLiteral("pdf")) {
        doc = extractPdfBytes(rawBytes);
    }

    doc.extractionTimeMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    return doc;
}

ExtractedDocument DocumentExtractor::extractTxtOrMarkdown(const QByteArray& rawBytes) {
    ExtractedDocument doc;
    if (rawBytes.isEmpty()) {
        doc.errorReason = QStringLiteral("Empty text buffer.");
        return doc;
    }

    QString decoded;
    const auto* u8 = reinterpret_cast<const uint8_t*>(rawBytes.constData());

    if (rawBytes.size() >= 3 && u8[0] == 0xEF && u8[1] == 0xBB && u8[2] == 0xBF) {
        decoded = QString::fromUtf8(rawBytes.constData() + 3, rawBytes.size() - 3);
    } else if (rawBytes.size() >= 2 && u8[0] == 0xFF && u8[1] == 0xFE) {
        decoded = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(rawBytes.constData() + 2),
            (rawBytes.size() - 2) / 2);
    } else if (rawBytes.size() >= 2 && u8[0] == 0xFE && u8[1] == 0xFF) {
        for (qsizetype i = 2; i + 1 < rawBytes.size(); i += 2) {
            const uint16_t ch = (static_cast<uint16_t>(u8[i]) << 8) | static_cast<uint16_t>(u8[i + 1]);
            decoded.append(QChar(ch));
        }
    } else {
        // Reject binary data masquerading as .txt if null bytes appear in first 4 KB
        const qsizetype checkLen = std::min<qsizetype>(rawBytes.size(), 4096);
        for (qsizetype i = 0; i < checkLen; ++i) {
            if (rawBytes.at(i) == '\0') {
                doc.errorReason = QStringLiteral("Binary file detected.");
                return doc;
            }
        }
        decoded = QString::fromUtf8(rawBytes);
    }

    doc.normalizedText = normalizeDocumentText(decoded);
    doc.pageCount = 1;
    doc.pageOffsets.append({1, 0});
    doc.success = !doc.normalizedText.isEmpty();
    if (!doc.success) {
        doc.errorReason = QStringLiteral("No extractable text found.");
    }
    return doc;
}

ExtractedDocument DocumentExtractor::extractDocxBytes(const QByteArray& zipBytes) {
    ExtractedDocument doc;
    if (zipBytes.size() < 22) {
        doc.errorReason = QStringLiteral("Invalid ZIP/DOCX container.");
        return doc;
    }

    const auto* data = reinterpret_cast<const uint8_t*>(zipBytes.constData());
    const size_t totalSize = static_cast<size_t>(zipBytes.size());

    // Locate End of Central Directory (EOCD) signature 0x06054b50 near end of file
    const size_t maxCommentSearch = std::min<size_t>(totalSize, 65557);
    qsizetype eocdOffset = -1;
    for (size_t back = 22; back <= maxCommentSearch; ++back) {
        const size_t pos = totalSize - back;
        if (readLe32(data + pos) == 0x06054b50u) {
            eocdOffset = static_cast<qsizetype>(pos);
            break;
        }
    }

    if (eocdOffset < 0) {
        doc.errorReason = QStringLiteral("DOCX Central Directory not found.");
        return doc;
    }

    const uint16_t numEntries = readLe16(data + eocdOffset + 10);
    const uint32_t cdSize = readLe32(data + eocdOffset + 12);
    const uint32_t cdOffset = readLe32(data + eocdOffset + 16);

    if (numEntries > 2048 || static_cast<size_t>(cdOffset) + cdSize > totalSize) {
        doc.errorReason = QStringLiteral("DOCX archive structure exceeds safety bounds.");
        return doc;
    }

    size_t cursor = cdOffset;
    bool foundDocXml = false;
    uint16_t compMethod = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint32_t localHeaderOffset = 0;

    for (uint16_t i = 0; i < numEntries && cursor + 46 <= totalSize; ++i) {
        if (readLe32(data + cursor) != 0x02014b50u) {
            break;
        }
        const uint16_t method = readLe16(data + cursor + 10);
        const uint32_t cSize = readLe32(data + cursor + 20);
        const uint32_t uSize = readLe32(data + cursor + 24);
        const uint16_t nameLen = readLe16(data + cursor + 28);
        const uint16_t extraLen = readLe16(data + cursor + 30);
        const uint16_t commentLen = readLe16(data + cursor + 32);
        const uint32_t lOffset = readLe32(data + cursor + 42);

        if (cursor + 46 + nameLen + extraLen + commentLen > totalSize) {
            break;
        }

        const QString entryName = QString::fromUtf8(
            reinterpret_cast<const char*>(data + cursor + 46), nameLen);

        if (entryName.compare(QStringLiteral("word/document.xml"), Qt::CaseInsensitive) == 0) {
            foundDocXml = true;
            compMethod = method;
            compSize = cSize;
            uncompSize = uSize;
            localHeaderOffset = lOffset;
            break;
        }

        cursor += 46 + nameLen + extraLen + commentLen;
    }

    if (!foundDocXml) {
        doc.errorReason = QStringLiteral("word/document.xml not found in DOCX.");
        return doc;
    }

    // Enforce strict ZIP-bomb protection
    if (uncompSize > kMaxDecompressedStreamBytes || compSize > totalSize) {
        doc.errorReason = QStringLiteral("DOCX XML stream exceeds decompression safety cap.");
        return doc;
    }
    if (compSize > 0 && (uncompSize / compSize) > 120) {
        doc.errorReason = QStringLiteral("Suspicious DOCX compression ratio rejected.");
        return doc;
    }

    if (static_cast<size_t>(localHeaderOffset) + 30 > totalSize ||
        readLe32(data + localHeaderOffset) != 0x04034b50u) {
        doc.errorReason = QStringLiteral("Invalid DOCX Local File Header.");
        return doc;
    }

    const uint16_t localNameLen = readLe16(data + localHeaderOffset + 26);
    const uint16_t localExtraLen = readLe16(data + localHeaderOffset + 28);
    const size_t payloadOffset = static_cast<size_t>(localHeaderOffset) + 30 + localNameLen + localExtraLen;

    if (payloadOffset + compSize > totalSize) {
        doc.errorReason = QStringLiteral("Truncated DOCX stream payload.");
        return doc;
    }

    QByteArray xmlBytes;
    if (compMethod == 0) {
        xmlBytes = QByteArray(reinterpret_cast<const char*>(data + payloadOffset), static_cast<qsizetype>(compSize));
    } else if (compMethod == 8) {
        if (!DeflateCodec::inflateRaw(data + payloadOffset, compSize, &xmlBytes, kMaxDecompressedStreamBytes)) {
            doc.errorReason = QStringLiteral("Failed to inflate word/document.xml.");
            return doc;
        }
    } else {
        doc.errorReason = QStringLiteral("Unsupported DOCX compression method: %1").arg(compMethod);
        return doc;
    }

    // Parse WordprocessingML XML via Qt6 QXmlStreamReader
    QXmlStreamReader xml(xmlBytes);
    QString rawText;
    int currentPage = 1;
    doc.pageOffsets.append({1, 0});

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringView localName = xml.name();
            if (localName == QStringLiteral("t")) {
                rawText.append(xml.readElementText());
            } else if (localName == QStringLiteral("tab")) {
                rawText.append(QLatin1Char(' '));
            } else if (localName == QStringLiteral("br") || localName == QStringLiteral("cr")) {
                const auto typeAttr = xml.attributes().value(QStringLiteral("w:type"));
                if (typeAttr == QStringLiteral("page")) {
                    ++currentPage;
                    doc.pageOffsets.append({currentPage, static_cast<int>(rawText.size())});
                }
                rawText.append(QLatin1Char('\n'));
            } else if (localName == QStringLiteral("lastRenderedPageBreak")) {
                ++currentPage;
                doc.pageOffsets.append({currentPage, static_cast<int>(rawText.size())});
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QStringView localName = xml.name();
            if (localName == QStringLiteral("p") || localName == QStringLiteral("tr")) {
                if (!rawText.endsWith(QLatin1Char('\n'))) {
                    rawText.append(QLatin1Char('\n'));
                }
            } else if (localName == QStringLiteral("tc")) {
                rawText.append(QLatin1Char(' '));
            }
        }
    }

    doc.normalizedText = normalizeDocumentText(rawText);
    doc.pageCount = currentPage;
    doc.success = !doc.normalizedText.isEmpty();
    if (!doc.success) {
        doc.errorReason = QStringLiteral("DOCX contains no extractable text.");
    }
    return doc;
}

ExtractedDocument DocumentExtractor::extractPdfBytes(const QByteArray& pdfBytes) {
    ExtractedDocument doc;
    if (pdfBytes.size() < 16 || !pdfBytes.startsWith("%PDF-")) {
        doc.errorReason = QStringLiteral("Invalid PDF header.");
        return doc;
    }

    // 1. Parse indirect objects: "N G obj ... endobj"
    struct PdfObjectData {
        QByteArray dictAndBody;
        QByteArray streamBytes;
        bool hasStream = false;
        bool isFlate = false;
        bool isPageObject = false;
        QVector<int> contentObjIds;
    };

    QHash<int, PdfObjectData> objects;
    QVector<int> orderedObjectIds;

    static const QRegularExpression objHeaderRe(QStringLiteral("(?:^|[\\r\\n\\s])(\\d+)\\s+(\\d+)\\s+obj\\b"));
    static const QRegularExpression typePageRe(QStringLiteral("/Type\\s*/Page\\b(?![sS])"));
    static const QRegularExpression contentsSingleRe(QStringLiteral("/Contents\\s+(\\d+)\\s+\\d+\\s+R"));
    static const QRegularExpression contentsArrayRe(QStringLiteral("/Contents\\s*\\[([^\\]]+)\\]"));
    static const QRegularExpression refNumberRe(QStringLiteral("(\\d+)\\s+\\d+\\s+R"));

    const QString latinView = QString::fromLatin1(pdfBytes);
    auto it = objHeaderRe.globalMatch(latinView);

    while (it.hasNext()) {
        const auto match = it.next();
        const int objId = match.captured(1).toInt();
        const qsizetype bodyStart = match.capturedEnd(0);
        const qsizetype endObjPos = pdfBytes.indexOf("endobj", bodyStart);
        if (endObjPos < 0) {
            break;
        }

        PdfObjectData obj;
        obj.dictAndBody = pdfBytes.mid(bodyStart, endObjPos - bodyStart);

        const qsizetype streamKeyword = obj.dictAndBody.indexOf("stream");
        const qsizetype endStreamKeyword = obj.dictAndBody.lastIndexOf("endstream");
        if (streamKeyword >= 0 && endStreamKeyword > streamKeyword) {
            qsizetype dataStart = streamKeyword + 6;
            if (dataStart < obj.dictAndBody.size() && obj.dictAndBody.at(dataStart) == '\r') {
                ++dataStart;
            }
            if (dataStart < obj.dictAndBody.size() && obj.dictAndBody.at(dataStart) == '\n') {
                ++dataStart;
            }
            qsizetype dataEnd = endStreamKeyword;
            if (dataEnd > dataStart && obj.dictAndBody.at(dataEnd - 1) == '\n') {
                --dataEnd;
            }
            if (dataEnd > dataStart && obj.dictAndBody.at(dataEnd - 1) == '\r') {
                --dataEnd;
            }

            obj.hasStream = true;
            obj.streamBytes = obj.dictAndBody.mid(dataStart, dataEnd - dataStart);
            const QByteArray headerPart = obj.dictAndBody.left(streamKeyword);
            obj.isFlate = headerPart.contains("/FlateDecode");
        } else {
            const QString dictStr = QString::fromLatin1(obj.dictAndBody);
            if (typePageRe.match(dictStr).hasMatch()) {
                obj.isPageObject = true;
                const auto singleMatch = contentsSingleRe.match(dictStr);
                if (singleMatch.hasMatch()) {
                    obj.contentObjIds.append(singleMatch.captured(1).toInt());
                } else {
                    const auto arrMatch = contentsArrayRe.match(dictStr);
                    if (arrMatch.hasMatch()) {
                        auto refIt = refNumberRe.globalMatch(arrMatch.captured(1));
                        while (refIt.hasNext()) {
                            obj.contentObjIds.append(refIt.next().captured(1).toInt());
                        }
                    }
                }
            }
        }

        objects.insert(objId, std::move(obj));
        orderedObjectIds.append(objId);
    }

    auto decodeObjectStream = [&](const PdfObjectData& obj, QByteArray* outDecoded) -> bool {
        if (!obj.hasStream) {
            return false;
        }
        if (obj.isFlate) {
            return DeflateCodec::inflateZlibOrRaw(
                reinterpret_cast<const uint8_t*>(obj.streamBytes.constData()),
                static_cast<size_t>(obj.streamBytes.size()),
                outDecoded,
                kMaxDecompressedStreamBytes);
        }
        if (static_cast<size_t>(obj.streamBytes.size()) > kMaxDecompressedStreamBytes) {
            return false;
        }
        *outDecoded = obj.streamBytes;
        return true;
    };

    QString combinedText;
    int pageNum = 0;

    // 2. Traverse /Page objects in document order for accurate page-location metadata
    for (const int objId : orderedObjectIds) {
        const auto& obj = objects[objId];
        if (!obj.isPageObject) {
            continue;
        }

        ++pageNum;
        QString pageRawText;
        for (const int contentId : obj.contentObjIds) {
            if (!objects.contains(contentId)) {
                continue;
            }
            QByteArray decodedStream;
            if (decodeObjectStream(objects[contentId], &decodedStream)) {
                pageRawText.append(parsePdfContentStreamText(decodedStream));
                pageRawText.append(QLatin1Char('\n'));
            }
        }

        const QString cleanPageText = normalizeDocumentText(pageRawText);
        if (!cleanPageText.isEmpty()) {
            if (!combinedText.isEmpty()) {
                combinedText.append(QStringLiteral("\n\n"));
            }
            doc.pageOffsets.append({pageNum, static_cast<int>(combinedText.size())});
            combinedText.append(cleanPageText);
        }
    }

    // Fallback: if PDF had streams without explicit /Type /Page links, scan all content streams
    if (combinedText.isEmpty()) {
        for (const int objId : orderedObjectIds) {
            const auto& obj = objects[objId];
            if (!obj.hasStream) {
                continue;
            }
            QByteArray decodedStream;
            if (decodeObjectStream(obj, &decodedStream) && decodedStream.contains("BT")) {
                const QString pageText = normalizeDocumentText(parsePdfContentStreamText(decodedStream));
                if (!pageText.isEmpty()) {
                    ++pageNum;
                    if (!combinedText.isEmpty()) {
                        combinedText.append(QStringLiteral("\n\n"));
                    }
                    doc.pageOffsets.append({pageNum, static_cast<int>(combinedText.size())});
                    combinedText.append(pageText);
                }
            }
        }
    }

    doc.normalizedText = normalizeDocumentText(combinedText);
    doc.pageCount = std::max(1, pageNum);
    if (doc.pageOffsets.isEmpty()) {
        doc.pageOffsets.append({1, 0});
    }
    doc.success = !doc.normalizedText.isEmpty();
    if (!doc.success) {
        doc.errorReason = QStringLiteral("No extractable text found in PDF.");
    }
    return doc;
}

QString DocumentExtractor::serializePageOffsets(const QVector<PageOffsetEntry>& offsets) {
    QJsonArray arr;
    for (const auto& entry : offsets) {
        QJsonObject obj;
        obj.insert(QStringLiteral("p"), entry.pageNumber);
        obj.insert(QStringLiteral("o"), entry.charOffset);
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

int DocumentExtractor::resolvePageForCharOffset(const QString& pageOffsetsJson, int charOffset) {
    if (pageOffsetsJson.isEmpty() || charOffset <= 0) {
        return 1;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(pageOffsetsJson.toUtf8());
    if (!doc.isArray()) {
        return 1;
    }
    const QJsonArray arr = doc.array();
    int resolvedPage = 1;
    for (const QJsonValue& val : arr) {
        const QJsonObject obj = val.toObject();
        const int page = obj.value(QStringLiteral("p")).toInt(1);
        const int offset = obj.value(QStringLiteral("o")).toInt(0);
        if (offset <= charOffset) {
            resolvedPage = page;
        } else {
            break;
        }
    }
    return resolvedPage;
}

QByteArray DocumentExtractor::buildValidDocxBytes(const QStringList& paragraphs) {
    QByteArray xml;
    xml.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
    xml.append("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>");
    for (const QString& para : paragraphs) {
        xml.append("<w:p><w:r><w:t>");
        QString escaped = para;
        escaped.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
        escaped.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
        escaped.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
        xml.append(escaped.toUtf8());
        xml.append("</w:t></w:r></w:p>");
    }
    xml.append("</w:body></w:document>");

    const QByteArray entryName = QByteArrayLiteral("word/document.xml");
    const uint32_t uncompSize = static_cast<uint32_t>(xml.size());

    // Build minimal valid ZIP archive with stored (method 0) word/document.xml
    QByteArray zip;
    // Local File Header (0x04034b50)
    appendLe32(&zip, 0x04034b50u);
    appendLe16(&zip, 20); // Version needed
    appendLe16(&zip, 0);  // Flags
    appendLe16(&zip, 0);  // Method 0 (Stored)
    appendLe16(&zip, 0);  // Mod time
    appendLe16(&zip, 0);  // Mod date
    appendLe32(&zip, 0);  // CRC-32
    appendLe32(&zip, uncompSize);
    appendLe32(&zip, uncompSize);
    appendLe16(&zip, static_cast<uint16_t>(entryName.size()));
    appendLe16(&zip, 0);  // Extra len
    zip.append(entryName);
    zip.append(xml);

    const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
    // Central Directory Header (0x02014b50)
    appendLe32(&zip, 0x02014b50u);
    appendLe16(&zip, 20); // Version made by
    appendLe16(&zip, 20); // Version needed
    appendLe16(&zip, 0);  // Flags
    appendLe16(&zip, 0);  // Method 0
    appendLe16(&zip, 0);  // Mod time
    appendLe16(&zip, 0);  // Mod date
    appendLe32(&zip, 0);  // CRC-32
    appendLe32(&zip, uncompSize);
    appendLe32(&zip, uncompSize);
    appendLe16(&zip, static_cast<uint16_t>(entryName.size()));
    appendLe16(&zip, 0);  // Extra len
    appendLe16(&zip, 0);  // Comment len
    appendLe16(&zip, 0);  // Disk number
    appendLe16(&zip, 0);  // Internal attrs
    appendLe32(&zip, 0);  // External attrs
    appendLe32(&zip, 0);  // Local header offset = 0
    zip.append(entryName);

    const uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;
    // End of Central Directory (0x06054b50)
    appendLe32(&zip, 0x06054b50u);
    appendLe16(&zip, 0);  // Disk number
    appendLe16(&zip, 0);  // CD start disk
    appendLe16(&zip, 1);  // Entries on disk
    appendLe16(&zip, 1);  // Total entries
    appendLe32(&zip, cdSize);
    appendLe32(&zip, cdOffset);
    appendLe16(&zip, 0);  // Comment len

    return zip;
}

QByteArray DocumentExtractor::buildValidPdfBytes(const QStringList& pagesText, bool flateCompress) {
    QByteArray pdf;
    pdf.append("%PDF-1.4\n");

    // Object 1: Catalog, Object 2: Pages
    pdf.append("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    QByteArray kidsArray;
    int nextObjId = 3;
    for (int i = 0; i < pagesText.size(); ++i) {
        kidsArray.append(QStringLiteral("%1 0 R ").arg(nextObjId).toLatin1());
        nextObjId += 2;
    }

    pdf.append(QStringLiteral("2 0 obj\n<< /Type /Pages /Kids [ %1] /Count %2 >>\nendobj\n")
                   .arg(QString::fromLatin1(kidsArray))
                   .arg(pagesText.size())
                   .toLatin1());

    nextObjId = 3;
    for (const QString& pageStr : pagesText) {
        const int pageObjId = nextObjId++;
        const int contentObjId = nextObjId++;

        pdf.append(QStringLiteral("%1 0 obj\n<< /Type /Page /Parent 2 0 R /Contents %2 0 R >>\nendobj\n")
                       .arg(pageObjId)
                       .arg(contentObjId)
                       .toLatin1());

        QString escaped = pageStr;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('('), QStringLiteral("\\("));
        escaped.replace(QLatin1Char(')'), QStringLiteral("\\)"));

        const QByteArray rawStream = QStringLiteral("BT /F1 12 Tf 72 720 Td (%1) Tj ET")
                                         .arg(escaped)
                                         .toUtf8();
        const QByteArray payload = flateCompress ? DeflateCodec::compressZlibStored(rawStream) : rawStream;

        pdf.append(QStringLiteral("%1 0 obj\n<< /Length %2 %3>>\nstream\n")
                       .arg(contentObjId)
                       .arg(payload.size())
                       .arg(flateCompress ? QStringLiteral("/Filter /FlateDecode ") : QString())
                       .toLatin1());
        pdf.append(payload);
        pdf.append("\nendstream\nendobj\n");
    }

    pdf.append("%%EOF\n");
    return pdf;
}

} // namespace lumen::extraction
