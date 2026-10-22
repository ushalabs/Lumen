#include "semantic/WordPieceTokenizer.h"

#include <QFile>
#include <QTextStream>

namespace lumen::semantic {

namespace {
bool isBertPunctuation(QChar c) {
    const ushort u = c.unicode();
    if ((u >= 33 && u <= 47) || (u >= 58 && u <= 64) ||
        (u >= 91 && u <= 96) || (u >= 123 && u <= 126)) {
        return true;
    }
    return c.isPunct() || c.isSymbol();
}
} // namespace

bool WordPieceTokenizer::loadVocabulary(const QString& vocabFilePath) {
    QFile file(vocabFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QHash<QString, int64_t> loaded;
    loaded.reserve(31000);

    int64_t index = 0;
    while (!file.atEnd()) {
        QByteArray line = file.readLine();
        while (!line.isEmpty() && (line.endsWith('\n') || line.endsWith('\r'))) {
            line.chop(1);
        }
        const QString token = QString::fromUtf8(line);
        loaded.insert(token, index++);
    }
    file.close();

    if (loaded.size() < 1000) {
        return false;
    }

    m_vocab = std::move(loaded);
    return true;
}

void WordPieceTokenizer::unloadVocabulary() {
    m_vocab.clear();
    m_vocab.squeeze();
}

QVector<QString> WordPieceTokenizer::basicTokenize(const QString& text) {
    // 1. NFD decomposition to strip accents + lowercase (BERT uncased preprocessing)
    const QString decomposed = text.normalized(QString::NormalizationForm_D).toLower();
    QString cleaned;
    cleaned.reserve(decomposed.size());

    for (QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing) {
            continue; // Strip combining accent marks
        }
        if (c.isSpace() || c == QLatin1Char('\n') || c == QLatin1Char('\r') || c == QLatin1Char('\t')) {
            cleaned.append(QLatin1Char(' '));
        } else if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format) {
            continue;
        } else {
            cleaned.append(c);
        }
    }

    // 2. Split on whitespace and isolate punctuation characters
    QVector<QString> tokens;
    QString current;
    for (QChar c : cleaned) {
        if (c == QLatin1Char(' ')) {
            if (!current.isEmpty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else if (isBertPunctuation(c)) {
            if (!current.isEmpty()) {
                tokens.push_back(current);
                current.clear();
            }
            tokens.push_back(QString(c));
        } else {
            current.append(c);
        }
    }
    if (!current.isEmpty()) {
        tokens.push_back(current);
    }
    return tokens;
}

TokenizedInput WordPieceTokenizer::encode(const QString& text, int maxSeqLength) const {
    TokenizedInput out;
    if (maxSeqLength < 4) {
        maxSeqLength = 4;
    }

    out.inputIds.reserve(static_cast<size_t>(maxSeqLength));
    out.inputIds.push_back(kClsTokenId);

    const int maxWordPieces = maxSeqLength - 2; // Reserve [CLS] and [SEP]
    const QVector<QString> basicTokens = basicTokenize(text);

    for (const QString& token : basicTokens) {
        if (static_cast<int>(out.inputIds.size()) - 1 >= maxWordPieces) {
            break;
        }

        if (m_vocab.isEmpty()) {
            // Deterministic hash fallback only when vocabulary file is not loaded
            const size_t h = qHash(token, 0x9E3779B9u);
            out.inputIds.push_back(1000 + static_cast<int64_t>(h % 28000));
            continue;
        }

        if (token.size() > 100) {
            out.inputIds.push_back(kUnkTokenId);
            continue;
        }

        bool isBad = false;
        int start = 0;
        std::vector<int64_t> subTokens;

        while (start < token.size()) {
            int end = static_cast<int>(token.size());
            int64_t curSubId = -1;

            while (start < end) {
                QString substr = token.mid(start, end - start);
                if (start > 0) {
                    substr.prepend(QStringLiteral("##"));
                }
                const auto it = m_vocab.constFind(substr);
                if (it != m_vocab.constEnd()) {
                    curSubId = it.value();
                    break;
                }
                --end;
            }

            if (curSubId < 0) {
                isBad = true;
                break;
            }
            subTokens.push_back(curSubId);
            start = end;
        }

        if (isBad) {
            out.inputIds.push_back(kUnkTokenId);
        } else {
            for (int64_t id : subTokens) {
                if (static_cast<int>(out.inputIds.size()) - 1 >= maxWordPieces) {
                    break;
                }
                out.inputIds.push_back(id);
            }
        }
    }

    out.inputIds.push_back(kSepTokenId);

    const size_t seqLen = out.inputIds.size();
    out.attentionMask.assign(seqLen, 1);
    out.tokenTypeIds.assign(seqLen, 0);
    return out;
}

} // namespace lumen::semantic
