#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lumen::extraction {

struct PageOffsetEntry {
    int pageNumber = 1;
    int charOffset = 0;
};

struct ExtractedDocument {
    bool success = false;
    QString errorReason;
    QString normalizedText;
    int pageCount = 1;
    QVector<PageOffsetEntry> pageOffsets;
    double extractionTimeMs = 0.0;
};

class DocumentExtractor {
public:
    static constexpr qint64 kDefaultMaxFileSizeBytes = 10LL * 1024LL * 1024LL; // 10 MB
    static constexpr int kMaxExtractedChars = 250000;
    static constexpr size_t kMaxDecompressedStreamBytes = 16 * 1024 * 1024;    // 16 MB ZIP/stream bomb cap

    static bool isSupportedExtension(const QString& extensionLower);

    static ExtractedDocument extractFromFile(
        const QString& filePath,
        qint64 maxFileSizeBytes = kDefaultMaxFileSizeBytes);

    static ExtractedDocument extractTxtOrMarkdown(const QByteArray& rawBytes);
    static ExtractedDocument extractDocxBytes(const QByteArray& zipBytes);
    static ExtractedDocument extractPdfBytes(const QByteArray& pdfBytes);

    // Normalizes Unicode (NFKC) and collapses redundant horizontal whitespace while preserving paragraph breaks.
    static QString normalizeDocumentText(const QString& input, int maxChars = kMaxExtractedChars);

    // Serializes/deserializes page offset mappings for SQLite storage and snippet page resolution.
    static QString serializePageOffsets(const QVector<PageOffsetEntry>& offsets);
    static int resolvePageForCharOffset(const QString& pageOffsetsJson, int charOffset);

    // Test & benchmark builders for valid multi-page PDF and DOCX files
    static QByteArray buildValidDocxBytes(const QStringList& paragraphs);
    static QByteArray buildValidPdfBytes(const QStringList& pagesText, bool flateCompress = true);
};

} // namespace lumen::extraction
