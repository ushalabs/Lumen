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
    struct XlsxSheetSpec {
        QString sheetName;
        QVector<QStringList> rows;
    };

    static constexpr qint64 kDefaultMaxFileSizeBytes = 10LL * 1024LL * 1024LL; // 10 MB
    static constexpr int kMaxExtractedChars = 250000;
    static constexpr size_t kMaxDecompressedStreamBytes = 16 * 1024 * 1024;    // 16 MB ZIP/stream bomb cap

    static const QStringList& textDocumentExtensions();
    static const QStringList& spreadsheetExtensions();
    static const QStringList& codeExtensions();
    static const QStringList& imageExtensions();
    static const QStringList& videoExtensions();
    static const QStringList& audioExtensions();
    static QStringList allContentSupportedExtensions();

    static bool isTextDocumentExtension(const QString& extensionLower);
    static bool isSpreadsheetExtension(const QString& extensionLower);
    static bool isCodeExtension(const QString& extensionLower);
    static bool isImageExtension(const QString& extensionLower);
    static bool isVideoExtension(const QString& extensionLower);
    static bool isAudioExtension(const QString& extensionLower);
    static bool isMediaMetadataExtension(const QString& extensionLower);
    static bool isSupportedExtension(const QString& extensionLower);

    static ExtractedDocument extractFromFile(
        const QString& filePath,
        qint64 maxFileSizeBytes = kDefaultMaxFileSizeBytes);

    static ExtractedDocument extractTxtOrMarkdown(const QByteArray& rawBytes);
    static ExtractedDocument extractDocxBytes(const QByteArray& zipBytes);
    static ExtractedDocument extractPdfBytes(const QByteArray& pdfBytes);
    static ExtractedDocument extractXlsxBytes(const QByteArray& zipBytes);
    static ExtractedDocument extractDelimitedTableBytes(
        const QByteArray& rawBytes,
        QChar delimiter = QLatin1Char(','));
    static ExtractedDocument extractCodeBytes(
        const QByteArray& rawBytes,
        const QString& fileName = QString(),
        const QString& extensionLower = QString());
    static ExtractedDocument extractMediaMetadataFromFile(
        const QString& filePath,
        const QString& extensionLower,
        qint64 fileSizeBytes);

    static QByteArray buildValidXlsxBytes(const QVector<XlsxSheetSpec>& sheets);
    static QByteArray buildValidMp4MetadataBytes(
        quint32 width,
        quint32 height,
        quint32 durationSeconds,
        const QString& codecFourCc = QStringLiteral("avc1"),
        const QString& title = QString());

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
