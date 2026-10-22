#include "semantic/DocumentChunker.h"

#include "extraction/DocumentExtractor.h"

#include <algorithm>

namespace lumen::semantic {

namespace {
struct WordSpan {
    int start = 0;
    int end = 0;
};
} // namespace

QVector<DocumentChunk> DocumentChunker::chunkDocumentText(
    const QString& normalizedText,
    const QString& pageOffsetsJson,
    int targetWords,
    int overlapWords,
    int maxChunks) {
    QVector<DocumentChunk> chunks;
    if (normalizedText.isEmpty() || targetWords <= 0 || maxChunks <= 0) {
        return chunks;
    }

    overlapWords = std::clamp(overlapWords, 0, targetWords - 1);
    const int stepWords = std::max(1, targetWords - overlapWords);

    // Locate word spans without allocating a QString per word
    QVector<WordSpan> words;
    words.reserve(std::min<qsizetype>(normalizedText.size() / 5 + 1, 20000));

    const int n = static_cast<int>(normalizedText.size());
    int pos = 0;
    while (pos < n) {
        while (pos < n && normalizedText.at(pos).isSpace()) {
            ++pos;
        }
        if (pos >= n) {
            break;
        }
        const int wordStart = pos;
        while (pos < n && !normalizedText.at(pos).isSpace()) {
            ++pos;
        }
        words.push_back({wordStart, pos});
    }

    if (words.isEmpty()) {
        return chunks;
    }

    const int totalWords = static_cast<int>(words.size());
    int chunkIdx = 0;

    for (int startWord = 0; startWord < totalWords && chunkIdx < maxChunks; startWord += stepWords) {
        const int endWord = std::min(totalWords - 1, startWord + targetWords - 1);
        const int charStart = words.at(startWord).start;
        const int charEnd = words.at(endWord).end;

        DocumentChunk chunk;
        chunk.chunkIndex = chunkIdx++;
        chunk.charStartOffset = charStart;
        chunk.pageNumber = pageOffsetsJson.isEmpty()
                               ? 1
                               : extraction::DocumentExtractor::resolvePageForCharOffset(pageOffsetsJson, charStart);
        chunk.chunkText = normalizedText.mid(charStart, charEnd - charStart).trimmed();

        QString preview = chunk.chunkText.left(kSnippetPreviewMaxChars * 2).simplified();
        if (preview.size() > kSnippetPreviewMaxChars) {
            preview = preview.left(kSnippetPreviewMaxChars).trimmed() + QStringLiteral(" …");
        }
        chunk.snippetPreview = std::move(preview);

        chunks.push_back(std::move(chunk));

        if (endWord >= totalWords - 1) {
            break;
        }
    }

    return chunks;
}

} // namespace lumen::semantic
