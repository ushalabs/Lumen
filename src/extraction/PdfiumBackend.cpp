#include "extraction/PdfiumBackend.h"

#include "core/Logger.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "fpdf_text.h"
#include "fpdfview.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace lumen::extraction {

namespace {

struct PdfiumApi {
    std::mutex mutex;
    HMODULE dll = nullptr;
    bool loadAttempted = false;
    bool initialized = false;

    decltype(&FPDF_InitLibraryWithConfig) initLibraryWithConfig = nullptr;
    decltype(&FPDF_DestroyLibrary) destroyLibrary = nullptr;
    decltype(&FPDF_LoadMemDocument) loadMemDocument = nullptr;
    decltype(&FPDF_GetLastError) getLastError = nullptr;
    decltype(&FPDF_CloseDocument) closeDocument = nullptr;
    decltype(&FPDF_GetPageCount) getPageCount = nullptr;
    decltype(&FPDF_LoadPage) loadPage = nullptr;
    decltype(&FPDF_ClosePage) closePage = nullptr;
    decltype(&FPDFText_LoadPage) textLoadPage = nullptr;
    decltype(&FPDFText_ClosePage) textClosePage = nullptr;
    decltype(&FPDFText_CountChars) textCountChars = nullptr;
    decltype(&FPDFText_GetText) textGetText = nullptr;
};

PdfiumApi& apiState() {
    static PdfiumApi s_api;
    return s_api;
}

QString executableDirectory() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return QFileInfo(QString::fromStdWString(buf)).absolutePath();
}

HMODULE loadPdfiumLibrary() {
    const QString exeDir = executableDirectory();
    QStringList candidates;
    if (!exeDir.isEmpty()) {
        candidates.append(QDir(exeDir).filePath(QStringLiteral("pdfium.dll")));

        // Support uninstalled build/test binaries locating repo third_party/pdfium/bin/pdfium.dll
        QDir probeDir(exeDir);
        for (int depth = 0; depth < 5; ++depth) {
            const QString thirdPartyDll =
                probeDir.filePath(QStringLiteral("third_party/pdfium/bin/pdfium.dll"));
            if (QFileInfo::exists(thirdPartyDll)) {
                candidates.append(thirdPartyDll);
            }
            if (!probeDir.cdUp()) {
                break;
            }
        }
    }

    for (const QString& candidate : candidates) {
        if (!QFileInfo::exists(candidate)) {
            continue;
        }
        const std::wstring wide = QDir::toNativeSeparators(candidate).toStdWString();
        if (HMODULE mod = LoadLibraryW(wide.c_str())) {
            return mod;
        }
    }

    return LoadLibraryW(L"pdfium.dll");
}

template <typename FnPtr>
bool bindSymbol(HMODULE mod, const char* name, FnPtr* outFn) {
    *outFn = reinterpret_cast<FnPtr>(GetProcAddress(mod, name));
    return *outFn != nullptr;
}

class ScopedPdfDoc {
public:
    ScopedPdfDoc(FPDF_DOCUMENT doc, decltype(&FPDF_CloseDocument) closer) noexcept
        : m_doc(doc), m_closer(closer) {}
    ~ScopedPdfDoc() {
        if (m_doc != nullptr && m_closer != nullptr) {
            m_closer(m_doc);
        }
    }
    ScopedPdfDoc(const ScopedPdfDoc&) = delete;
    ScopedPdfDoc& operator=(const ScopedPdfDoc&) = delete;

    FPDF_DOCUMENT get() const noexcept { return m_doc; }

private:
    FPDF_DOCUMENT m_doc = nullptr;
    decltype(&FPDF_CloseDocument) m_closer = nullptr;
};

class ScopedPdfPage {
public:
    ScopedPdfPage(FPDF_PAGE page, decltype(&FPDF_ClosePage) closer) noexcept
        : m_page(page), m_closer(closer) {}
    ~ScopedPdfPage() {
        if (m_page != nullptr && m_closer != nullptr) {
            m_closer(m_page);
        }
    }
    ScopedPdfPage(const ScopedPdfPage&) = delete;
    ScopedPdfPage& operator=(const ScopedPdfPage&) = delete;

    FPDF_PAGE get() const noexcept { return m_page; }

private:
    FPDF_PAGE m_page = nullptr;
    decltype(&FPDF_ClosePage) m_closer = nullptr;
};

class ScopedPdfTextPage {
public:
    ScopedPdfTextPage(FPDF_TEXTPAGE textPage, decltype(&FPDFText_ClosePage) closer) noexcept
        : m_textPage(textPage), m_closer(closer) {}
    ~ScopedPdfTextPage() {
        if (m_textPage != nullptr && m_closer != nullptr) {
            m_closer(m_textPage);
        }
    }
    ScopedPdfTextPage(const ScopedPdfTextPage&) = delete;
    ScopedPdfTextPage& operator=(const ScopedPdfTextPage&) = delete;

    FPDF_TEXTPAGE get() const noexcept { return m_textPage; }

private:
    FPDF_TEXTPAGE m_textPage = nullptr;
    decltype(&FPDFText_ClosePage) m_closer = nullptr;
};

QString decodePdfiumError(unsigned long err) {
    switch (err) {
        case FPDF_ERR_PASSWORD:
            return QStringLiteral("PDF is password-protected.");
        case FPDF_ERR_SECURITY:
            return QStringLiteral("Unsupported PDF encryption scheme.");
        case FPDF_ERR_FORMAT:
        case FPDF_ERR_FILE:
            return QStringLiteral("Invalid or corrupted PDF document.");
        default:
            return QStringLiteral("Unable to parse PDF document.");
    }
}

} // namespace

PdfiumBackend& PdfiumBackend::instance() {
    static PdfiumBackend s_instance;
    return s_instance;
}

PdfiumBackend::~PdfiumBackend() {
    auto& state = apiState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized && state.destroyLibrary != nullptr) {
        state.destroyLibrary();
        state.initialized = false;
    }
    if (state.dll != nullptr) {
        FreeLibrary(state.dll);
        state.dll = nullptr;
    }
}

bool PdfiumBackend::isAvailable() {
    auto& state = apiState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return ensureLoadedUnlocked();
}

bool PdfiumBackend::ensureLoadedUnlocked() {
    auto& state = apiState();
    if (state.initialized) {
        return true;
    }
    if (state.loadAttempted) {
        return false;
    }
    state.loadAttempted = true;

    state.dll = loadPdfiumLibrary();
    if (state.dll == nullptr) {
        core::Logger::warning(QStringLiteral("Extraction"),
                              QStringLiteral("pdfium.dll could not be loaded; falling back to limited PDF parser."));
        return false;
    }

    const bool allBound =
        bindSymbol(state.dll, "FPDF_InitLibraryWithConfig", &state.initLibraryWithConfig) &&
        bindSymbol(state.dll, "FPDF_DestroyLibrary", &state.destroyLibrary) &&
        bindSymbol(state.dll, "FPDF_LoadMemDocument", &state.loadMemDocument) &&
        bindSymbol(state.dll, "FPDF_GetLastError", &state.getLastError) &&
        bindSymbol(state.dll, "FPDF_CloseDocument", &state.closeDocument) &&
        bindSymbol(state.dll, "FPDF_GetPageCount", &state.getPageCount) &&
        bindSymbol(state.dll, "FPDF_LoadPage", &state.loadPage) &&
        bindSymbol(state.dll, "FPDF_ClosePage", &state.closePage) &&
        bindSymbol(state.dll, "FPDFText_LoadPage", &state.textLoadPage) &&
        bindSymbol(state.dll, "FPDFText_ClosePage", &state.textClosePage) &&
        bindSymbol(state.dll, "FPDFText_CountChars", &state.textCountChars) &&
        bindSymbol(state.dll, "FPDFText_GetText", &state.textGetText);

    if (!allBound) {
        core::Logger::error(QStringLiteral("Extraction"),
                            QStringLiteral("pdfium.dll is missing required FPDF C exports."));
        FreeLibrary(state.dll);
        state.dll = nullptr;
        return false;
    }

    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    config.m_pUserFontPaths = nullptr;
    config.m_pIsolate = nullptr;
    config.m_v8EmbedderSlot = 0;
    state.initLibraryWithConfig(&config);
    state.initialized = true;
    return true;
}

ExtractedDocument PdfiumBackend::extractFromBytes(const QByteArray& pdfBytes, int maxExtractedChars) {
    ExtractedDocument doc;
    if (pdfBytes.size() < 16 || pdfBytes.size() > std::numeric_limits<int>::max() ||
        pdfBytes.left(1024).indexOf("%PDF-") < 0) {
        doc.errorReason = QStringLiteral("Invalid PDF header.");
        return doc;
    }

    auto& state = apiState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!ensureLoadedUnlocked()) {
        doc.errorReason = QStringLiteral("PDFium runtime unavailable.");
        return doc;
    }

    ScopedPdfDoc pdfDoc(
        state.loadMemDocument(pdfBytes.constData(), static_cast<int>(pdfBytes.size()), nullptr),
        state.closeDocument);
    if (pdfDoc.get() == nullptr) {
        const unsigned long err = state.getLastError != nullptr ? state.getLastError() : 0;
        doc.errorReason = decodePdfiumError(err);
        return doc;
    }

    const int totalPages = state.getPageCount(pdfDoc.get());
    if (totalPages <= 0) {
        doc.errorReason = QStringLiteral("PDF contains no pages.");
        return doc;
    }

    const int cappedChars = std::max(1, maxExtractedChars);
    QString combinedText;
    std::vector<unsigned short> charBuf;

    for (int pageIdx = 0; pageIdx < totalPages; ++pageIdx) {
        if (combinedText.size() >= cappedChars) {
            break;
        }

        ScopedPdfPage page(state.loadPage(pdfDoc.get(), pageIdx), state.closePage);
        if (page.get() == nullptr) {
            continue;
        }

        ScopedPdfTextPage textPage(state.textLoadPage(page.get()), state.textClosePage);
        if (textPage.get() == nullptr) {
            continue;
        }

        const int charCount = state.textCountChars(textPage.get());
        if (charCount <= 0) {
            continue;
        }

        charBuf.assign(static_cast<size_t>(charCount) + 2, 0);
        const int written = state.textGetText(textPage.get(), 0, charCount, charBuf.data());
        if (written <= 1) {
            continue;
        }

        int codeUnitLen = std::min(written - 1, charCount);
        while (codeUnitLen > 0 && charBuf[static_cast<size_t>(codeUnitLen - 1)] == 0) {
            --codeUnitLen;
        }
        if (codeUnitLen <= 0) {
            continue;
        }

        QString pageRaw = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(charBuf.data()),
            codeUnitLen);
        pageRaw.replace(QLatin1Char('\0'), QLatin1Char(' '));
        pageRaw.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        pageRaw.replace(QLatin1Char('\r'), QLatin1Char('\n'));

        const QString cleanPageText = DocumentExtractor::normalizeDocumentText(pageRaw, cappedChars);
        if (!cleanPageText.isEmpty()) {
            const int sepLen = combinedText.isEmpty() ? 0 : 2;
            const int remaining = cappedChars - static_cast<int>(combinedText.size()) - sepLen;
            if (remaining <= 0) {
                break;
            }
            if (!combinedText.isEmpty()) {
                combinedText.append(QStringLiteral("\n\n"));
            }
            doc.pageOffsets.append({pageIdx + 1, static_cast<int>(combinedText.size())});
            combinedText.append(cleanPageText.left(remaining));
        }
    }

    doc.normalizedText = std::move(combinedText);
    doc.pageCount = std::max(1, totalPages);
    if (doc.pageOffsets.isEmpty()) {
        doc.pageOffsets.append({1, 0});
    }
    doc.success = !doc.normalizedText.isEmpty();
    if (!doc.success) {
        doc.errorReason = QStringLiteral("No extractable text found in PDF.");
    }
    return doc;
}

} // namespace lumen::extraction
