#include "semantic/SemanticEmbeddingEngine.h"

#include "core/Logger.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cmath>
#include <malloc.h>

namespace lumen::semantic {

struct SemanticEmbeddingEngine::OrtState {
    HMODULE dllHandle = nullptr;
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    bool hasTokenTypeIds = false;
};

SemanticEmbeddingEngine& SemanticEmbeddingEngine::instance() {
    static SemanticEmbeddingEngine s_instance;
    return s_instance;
}

SemanticEmbeddingEngine::SemanticEmbeddingEngine(QObject* parent)
    : QObject(parent),
      m_ort(std::make_unique<OrtState>()) {
    m_modelPath = resolveDefaultModelPath();
    m_vocabPath = resolveDefaultVocabPath();
}

SemanticEmbeddingEngine::~SemanticEmbeddingEngine() {
    QMutexLocker locker(&m_mutex);
    unloadUnlocked();
}

QString SemanticEmbeddingEngine::resolveDefaultModelPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/models/all-MiniLM-L6-v2/model_quantized.onnx"),
        appDir + QStringLiteral("/assets/models/all-MiniLM-L6-v2/model_quantized.onnx"),
        appDir + QStringLiteral("/../assets/models/all-MiniLM-L6-v2/model_quantized.onnx"),
        appDir + QStringLiteral("/../../assets/models/all-MiniLM-L6-v2/model_quantized.onnx"),
        QDir::currentPath() + QStringLiteral("/assets/models/all-MiniLM-L6-v2/model_quantized.onnx")
    };
    for (const QString& path : candidates) {
        const QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            return fi.canonicalFilePath();
        }
    }
    return candidates.first();
}

QString SemanticEmbeddingEngine::resolveDefaultVocabPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/models/all-MiniLM-L6-v2/vocab.txt"),
        appDir + QStringLiteral("/assets/models/all-MiniLM-L6-v2/vocab.txt"),
        appDir + QStringLiteral("/../assets/models/all-MiniLM-L6-v2/vocab.txt"),
        appDir + QStringLiteral("/../../assets/models/all-MiniLM-L6-v2/vocab.txt"),
        QDir::currentPath() + QStringLiteral("/assets/models/all-MiniLM-L6-v2/vocab.txt")
    };
    for (const QString& path : candidates) {
        const QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            return fi.canonicalFilePath();
        }
    }
    return candidates.first();
}

QString SemanticEmbeddingEngine::resolveOnnxRuntimeDllPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/onnxruntime.dll"),
        appDir + QStringLiteral("/../third_party/onnxruntime/lib/onnxruntime.dll"),
        appDir + QStringLiteral("/../../third_party/onnxruntime/lib/onnxruntime.dll"),
        QDir::currentPath() + QStringLiteral("/third_party/onnxruntime/lib/onnxruntime.dll")
    };
    for (const QString& path : candidates) {
        const QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            return QDir::toNativeSeparators(fi.canonicalFilePath());
        }
    }
    return QStringLiteral("onnxruntime.dll");
}

void SemanticEmbeddingEngine::setModelPaths(const QString& onnxModelPath, const QString& vocabPath) {
    QMutexLocker locker(&m_mutex);
    if (m_modelPath != onnxModelPath || m_vocabPath != vocabPath) {
        unloadUnlocked();
        m_modelPath = onnxModelPath;
        m_vocabPath = vocabPath;
    }
}

void SemanticEmbeddingEngine::setIdleUnloadTimeoutMs(int timeoutMs) {
    QMutexLocker locker(&m_mutex);
    m_idleUnloadTimeoutMs = std::max(100, timeoutMs);
}

bool SemanticEmbeddingEngine::isModelAvailable() const {
    QMutexLocker locker(&m_mutex);
    const QString model = m_modelPath.isEmpty() ? resolveDefaultModelPath() : m_modelPath;
    const QString vocab = m_vocabPath.isEmpty() ? resolveDefaultVocabPath() : m_vocabPath;
    return QFileInfo::exists(model) && QFileInfo::exists(vocab);
}

bool SemanticEmbeddingEngine::isSessionLoaded() const {
    QMutexLocker locker(&m_mutex);
    return m_loaded;
}

bool SemanticEmbeddingEngine::ensureLoadedUnlocked() {
    if (m_loaded && m_ort->session != nullptr) {
        m_lastUsedTimer.restart();
        return true;
    }

    if (m_modelPath.isEmpty() || !QFileInfo::exists(m_modelPath)) {
        m_modelPath = resolveDefaultModelPath();
    }
    if (m_vocabPath.isEmpty() || !QFileInfo::exists(m_vocabPath)) {
        m_vocabPath = resolveDefaultVocabPath();
    }

    if (!QFileInfo::exists(m_modelPath) || !QFileInfo::exists(m_vocabPath)) {
        core::Logger::warning(QStringLiteral("Semantic"),
                              QStringLiteral("ONNX model or vocab file not found (%1).").arg(m_modelPath));
        return false;
    }

    if (!m_tokenizer.isLoaded()) {
        if (!m_tokenizer.loadVocabulary(m_vocabPath)) {
            core::Logger::error(QStringLiteral("Semantic"),
                                QStringLiteral("Failed to load WordPiece vocabulary: %1").arg(m_vocabPath));
            return false;
        }
    }

    // Dynamically load onnxruntime.dll on demand so startup/idle footprint stays untouched
    if (m_ort->dllHandle == nullptr) {
        const QString dllPath = resolveOnnxRuntimeDllPath();
        const std::wstring wDllPath = dllPath.toStdWString();
        m_ort->dllHandle = ::LoadLibraryW(wDllPath.c_str());
        if (m_ort->dllHandle == nullptr) {
            m_ort->dllHandle = ::LoadLibraryW(L"onnxruntime.dll");
        }
        if (m_ort->dllHandle == nullptr) {
            core::Logger::error(QStringLiteral("Semantic"), QStringLiteral("Failed to load onnxruntime.dll."));
            return false;
        }

        using OrtGetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)(void);
        auto getApiBase = reinterpret_cast<OrtGetApiBaseFn>(
            ::GetProcAddress(m_ort->dllHandle, "OrtGetApiBase"));
        if (getApiBase == nullptr) {
            ::FreeLibrary(m_ort->dllHandle);
            m_ort->dllHandle = nullptr;
            return false;
        }
        const OrtApi* ortApi = getApiBase()->GetApi(ORT_API_VERSION);
        if (ortApi == nullptr) {
            ::FreeLibrary(m_ort->dllHandle);
            m_ort->dllHandle = nullptr;
            return false;
        }
        Ort::InitApi(ortApi);
    }

    try {
        m_ort->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "LumenSemantic");

        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(2);
        sessionOptions.SetInterOpNumThreads(1);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Prevent ONNX Runtime from hoarding persistent memory arenas when idle
        sessionOptions.DisableCpuMemArena();
        sessionOptions.DisableMemPattern();

        const std::wstring wModelPath = QDir::toNativeSeparators(m_modelPath).toStdWString();
        m_ort->session = std::make_unique<Ort::Session>(*m_ort->env, wModelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        m_ort->inputNames.clear();
        m_ort->outputNames.clear();
        m_ort->hasTokenTypeIds = false;

        const size_t numInputs = m_ort->session->GetInputCount();
        for (size_t i = 0; i < numInputs; ++i) {
            auto nameAlloc = m_ort->session->GetInputNameAllocated(i, allocator);
            std::string nameStr(nameAlloc.get());
            if (nameStr == "token_type_ids") {
                m_ort->hasTokenTypeIds = true;
            }
            m_ort->inputNames.push_back(std::move(nameStr));
        }

        const size_t numOutputs = m_ort->session->GetOutputCount();
        for (size_t i = 0; i < numOutputs; ++i) {
            auto nameAlloc = m_ort->session->GetOutputNameAllocated(i, allocator);
            m_ort->outputNames.emplace_back(nameAlloc.get());
        }

        m_loaded = true;
        m_lastUsedTimer.start();
        core::Logger::info(QStringLiteral("Semantic"),
                           QStringLiteral("Lazy-loaded quantized ONNX model (%1).").arg(m_modelPath));
        return true;
    } catch (const Ort::Exception& ex) {
        core::Logger::error(QStringLiteral("Semantic"),
                            QStringLiteral("ONNX Runtime initialization exception: %1")
                                .arg(QString::fromUtf8(ex.what())));
        unloadUnlocked();
        return false;
    }
}

void SemanticEmbeddingEngine::unloadUnlocked() {
    const bool wasLoaded = m_loaded;
    m_ort->session.reset();
    m_ort->env.reset();
    m_ort->inputNames.clear();
    m_ort->outputNames.clear();
    m_tokenizer.unloadVocabulary();
    m_loaded = false;

    if (wasLoaded) {
        _heapmin();
        ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(),
                                     static_cast<SIZE_T>(-1),
                                     static_cast<SIZE_T>(-1),
                                     0);
        core::Logger::info(QStringLiteral("Semantic"),
                           QStringLiteral("Unloaded ONNX session and released inference memory."));
    }
}

void SemanticEmbeddingEngine::unloadModel() {
    bool notify = false;
    {
        QMutexLocker locker(&m_mutex);
        notify = m_loaded;
        unloadUnlocked();
    }
    if (notify) {
        emit modelUnloaded();
    }
}

bool SemanticEmbeddingEngine::checkAndUnloadIfIdle() {
    bool unloadedNow = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_loaded && m_lastUsedTimer.isValid() &&
            m_lastUsedTimer.elapsed() >= m_idleUnloadTimeoutMs) {
            unloadUnlocked();
            unloadedNow = true;
        }
    }
    if (unloadedNow) {
        emit modelUnloaded();
    }
    return unloadedNow;
}

std::vector<float> SemanticEmbeddingEngine::embedText(const QString& text, double* outElapsedMs) {
    QElapsedTimer timer;
    timer.start();

    std::vector<float> embedding;
    if (text.trimmed().isEmpty()) {
        if (outElapsedMs) {
            *outElapsedMs = 0.0;
        }
        return embedding;
    }

    bool newlyLoaded = false;
    {
        QMutexLocker locker(&m_mutex);
        const bool wasLoaded = m_loaded;
        if (!ensureLoadedUnlocked()) {
            if (outElapsedMs) {
                *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            }
            return embedding;
        }
        newlyLoaded = (!wasLoaded && m_loaded);

        const TokenizedInput tokens = m_tokenizer.encode(text, WordPieceTokenizer::kDefaultMaxSeqLength);
        const int64_t seqLen = static_cast<int64_t>(tokens.inputIds.size());
        if (seqLen <= 0) {
            return embedding;
        }

        try {
            Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const std::array<int64_t, 2> inputShape = {1, seqLen};

            std::vector<int64_t> inputIds = tokens.inputIds;
            std::vector<int64_t> attentionMask = tokens.attentionMask;
            std::vector<int64_t> tokenTypeIds = tokens.tokenTypeIds;

            std::vector<Ort::Value> inputTensors;
            std::vector<const char*> inputNamePtrs;

            for (const std::string& inName : m_ort->inputNames) {
                if (inName == "input_ids") {
                    inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                        memInfo, inputIds.data(), inputIds.size(), inputShape.data(), inputShape.size()));
                    inputNamePtrs.push_back(inName.c_str());
                } else if (inName == "attention_mask") {
                    inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                        memInfo, attentionMask.data(), attentionMask.size(), inputShape.data(), inputShape.size()));
                    inputNamePtrs.push_back(inName.c_str());
                } else if (inName == "token_type_ids") {
                    inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                        memInfo, tokenTypeIds.data(), tokenTypeIds.size(), inputShape.data(), inputShape.size()));
                    inputNamePtrs.push_back(inName.c_str());
                }
            }

            const char* outputNamePtrs[] = {m_ort->outputNames.front().c_str()};
            auto outputTensors = m_ort->session->Run(
                Ort::RunOptions{nullptr},
                inputNamePtrs.data(),
                inputTensors.data(),
                inputTensors.size(),
                outputNamePtrs,
                1);

            if (!outputTensors.empty() && outputTensors.front().IsTensor()) {
                const auto typeAndShape = outputTensors.front().GetTensorTypeAndShapeInfo();
                const auto shape = typeAndShape.GetShape();
                const float* rawData = outputTensors.front().GetTensorData<float>();

                embedding.assign(kEmbeddingDimension, 0.0f);

                if (shape.size() == 3 && shape[2] == kEmbeddingDimension) {
                    // Mean pooling across sequence tokens weighted by attentionMask
                    const int64_t actualSeq = std::min<int64_t>(seqLen, shape[1]);
                    float validTokenCount = 0.0f;
                    for (int64_t t = 0; t < actualSeq; ++t) {
                        if (attentionMask[static_cast<size_t>(t)] == 0) {
                            continue;
                        }
                        validTokenCount += 1.0f;
                        const float* tokenVec = rawData + (t * kEmbeddingDimension);
                        for (int d = 0; d < kEmbeddingDimension; ++d) {
                            embedding[static_cast<size_t>(d)] += tokenVec[d];
                        }
                    }
                    if (validTokenCount > 0.0f) {
                        const float invCount = 1.0f / validTokenCount;
                        for (int d = 0; d < kEmbeddingDimension; ++d) {
                            embedding[static_cast<size_t>(d)] *= invCount;
                        }
                    }
                } else if (shape.size() == 2 && shape[1] == kEmbeddingDimension) {
                    std::copy(rawData, rawData + kEmbeddingDimension, embedding.begin());
                }

                // L2 Normalization
                double normSq = 0.0;
                for (float v : embedding) {
                    normSq += static_cast<double>(v) * static_cast<double>(v);
                }
                const float norm = static_cast<float>(std::sqrt(normSq));
                if (norm > 1e-12f) {
                    const float invNorm = 1.0f / norm;
                    for (float& v : embedding) {
                        v *= invNorm;
                    }
                }
            }
            m_lastUsedTimer.restart();
        } catch (const Ort::Exception& ex) {
            core::Logger::error(QStringLiteral("Semantic"),
                                QStringLiteral("ONNX inference error: %1").arg(QString::fromUtf8(ex.what())));
            embedding.clear();
        }
    }

    if (newlyLoaded) {
        emit modelLoaded();
    }

    if (outElapsedMs != nullptr) {
        *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    }
    return embedding;
}

float SemanticEmbeddingEngine::cosineSimilarity(const float* vecA, const float* vecB, size_t dim) noexcept {
    if (vecA == nullptr || vecB == nullptr || dim == 0) {
        return 0.0f;
    }
    float sum0 = 0.0f;
    float sum1 = 0.0f;
    float sum2 = 0.0f;
    float sum3 = 0.0f;

    size_t i = 0;
    for (; i + 3 < dim; i += 4) {
        sum0 += vecA[i] * vecB[i];
        sum1 += vecA[i + 1] * vecB[i + 1];
        sum2 += vecA[i + 2] * vecB[i + 2];
        sum3 += vecA[i + 3] * vecB[i + 3];
    }
    float total = (sum0 + sum1) + (sum2 + sum3);
    for (; i < dim; ++i) {
        total += vecA[i] * vecB[i];
    }
    return total;
}

} // namespace lumen::semantic
