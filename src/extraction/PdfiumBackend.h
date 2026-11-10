#pragma once

#include "extraction/DocumentExtractor.h"

#include <QByteArray>

namespace lumen::extraction {

// Thread-safe runtime wrapper around dynamically loaded PDFium (pdfium.dll).
// Handles modern PDFs (object streams, Flate/LZW/RunLength filters, Unicode CMaps, CID fonts)
// with per-page memory bounding and zero static CRT linkage constraints.
class PdfiumBackend {
public:
    static PdfiumBackend& instance();

    bool isAvailable();
    ExtractedDocument extractFromBytes(const QByteArray& pdfBytes,
                                       int maxExtractedChars = DocumentExtractor::kMaxExtractedChars);

private:
    PdfiumBackend() = default;
    ~PdfiumBackend();
    PdfiumBackend(const PdfiumBackend&) = delete;
    PdfiumBackend& operator=(const PdfiumBackend&) = delete;

    bool ensureLoadedUnlocked();
};

} // namespace lumen::extraction
