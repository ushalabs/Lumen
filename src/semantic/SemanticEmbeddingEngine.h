#pragma once

#include "semantic/WordPieceTokenizer.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <vector>

namespace lumen::semantic {

class SemanticEmbeddingEngine : public QObject {
    Q_OBJECT

public:
    static constexpr int kEmbeddingDimension = 384;
    static constexpr int kDefaultIdleUnloadTimeoutMs = 5 * 60 * 1000; // 5 minutes

    static SemanticEmbeddingEngine& instance();

    // Configures custom model/vocab paths (optional; defaults to bundled all-MiniLM-L6-v2).
    void setModelPaths(const QString& onnxModelPath, const QString& vocabPath);
    void setIdleUnloadTimeoutMs(int timeoutMs);

    bool isModelAvailable() const;
    bool isSessionLoaded() const;

    // Lazy-loads the ONNX session on first call and computes a 384-dim L2-normalized embedding.
    std::vector<float> embedText(const QString& text, double* outElapsedMs = nullptr);

    // Explicitly unloads the ONNX session, tokenizer vocabulary, and releases heap/working-set pages.
    void unloadModel();

    // Checks whether the inactivity timeout has elapsed and unloads if idle.
    bool checkAndUnloadIfIdle();

    // Computes cosine similarity (dot product) between two L2-normalized 384-D vectors.
    static float cosineSimilarity(const float* vecA, const float* vecB, size_t dim = kEmbeddingDimension) noexcept;

    static QString resolveDefaultModelPath();
    static QString resolveDefaultVocabPath();
    static QString resolveOnnxRuntimeDllPath();

signals:
    void modelLoaded();
    void modelUnloaded();

private:
    explicit SemanticEmbeddingEngine(QObject* parent = nullptr);
    ~SemanticEmbeddingEngine() override;

    bool ensureLoadedUnlocked();
    void unloadUnlocked();

    struct OrtState;
    std::unique_ptr<OrtState> m_ort;

    mutable QMutex m_mutex;
    WordPieceTokenizer m_tokenizer;
    QString m_modelPath;
    QString m_vocabPath;
    QElapsedTimer m_lastUsedTimer;
    int m_idleUnloadTimeoutMs = kDefaultIdleUnloadTimeoutMs;
    bool m_loaded = false;
};

} // namespace lumen::semantic
