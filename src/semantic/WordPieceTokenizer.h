#pragma once

#include <QHash>
#include <QString>
#include <cstdint>
#include <vector>

namespace lumen::semantic {

struct TokenizedInput {
    std::vector<int64_t> inputIds;
    std::vector<int64_t> attentionMask;
    std::vector<int64_t> tokenTypeIds;
};

class WordPieceTokenizer {
public:
    static constexpr int64_t kPadTokenId = 0;
    static constexpr int64_t kUnkTokenId = 100;
    static constexpr int64_t kClsTokenId = 101;
    static constexpr int64_t kSepTokenId = 102;
    static constexpr int kDefaultMaxSeqLength = 128;

    WordPieceTokenizer() = default;

    bool loadVocabulary(const QString& vocabFilePath);
    void unloadVocabulary();
    bool isLoaded() const noexcept { return !m_vocab.isEmpty(); }
    int vocabularySize() const noexcept { return static_cast<int>(m_vocab.size()); }

    TokenizedInput encode(const QString& text, int maxSeqLength = kDefaultMaxSeqLength) const;

private:
    static QVector<QString> basicTokenize(const QString& text);
    QHash<QString, int64_t> m_vocab;
};

} // namespace lumen::semantic
