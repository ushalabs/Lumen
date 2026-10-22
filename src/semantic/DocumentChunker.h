#pragma once

#include <QString>
#include <QVector>

namespace lumen::semantic {

struct DocumentChunk {
    int chunkIndex = 0;
    int pageNumber = 1;
    int charStartOffset = 0;
    QString chunkText;
    QString snippetPreview;
};

class DocumentChunker {
public:
    static constexpr int kDefaultTargetWords = 350;
    static constexpr int kDefaultOverlapWords = 50;
    static constexpr int kMaxChunksPerDocument = 32;
    static constexpr int kSnippetPreviewMaxChars = 200;

    static QVector<DocumentChunk> chunkDocumentText(
        const QString& normalizedText,
        const QString& pageOffsetsJson = QString(),
        int targetWords = kDefaultTargetWords,
        int overlapWords = kDefaultOverlapWords,
        int maxChunks = kMaxChunksPerDocument);
};

} // namespace lumen::semantic
