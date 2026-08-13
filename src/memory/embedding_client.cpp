#include "vpet/memory/embedding_client.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#undef ORT_API_MANUAL_INIT

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace vpet
{

namespace
{

const QString TOKENIZER_CONFIG_FILE = QStringLiteral("tokenizer_config.json");
const QString DEFAULT_MODEL_DIR_SUFFIX = QStringLiteral("models/embedding");

/**
 * @brief 将 QJsonValue 安全转换为 int64
 */
int64_t JsonToInt64(const QJsonValue &value, int64_t defaultValue)
{
    if (!value.isDouble())
    {
        return defaultValue;
    }

    const double number = value.toDouble();
    return static_cast<int64_t>(number);
}

/**
 * @brief 判断字符是否属于 CJK 统一表意文字区
 */
bool IsCjk(const QChar &ch)
{
    const quint32 codePoint = ch.unicode();

    return ((codePoint >= 0x3400) && (codePoint <= 0x4DBF))
           || ((codePoint >= 0x4E00) && (codePoint <= 0x9FFF))
           || ((codePoint >= 0xF900) && (codePoint <= 0xFAFF));
}

/**
 * @brief 判断字符是否为分词边界（空白或标点）
 */
bool IsSplitCharacter(const QChar &ch)
{
    return ch.isSpace() || ch.isPunct();
}

/**
 * @brief 解析 special token 配置，返回特殊 token 到 id 的映射
 */
QHash<QString, int64_t> ParseSpecialTokens(const QJsonObject &rootObject)
{
    QHash<QString, int64_t> specialTokens;

    const QJsonArray addedTokens = rootObject.value(QStringLiteral("added_tokens")).toArray();

    for (const QJsonValue &value : addedTokens)
    {
        const QJsonObject object = value.toObject();

        if (!object.value(QStringLiteral("special")).toBool(false))
        {
            continue;
        }

        const QString content = object.value(QStringLiteral("content")).toString();

        if (!content.isEmpty())
        {
            specialTokens.insert(content, JsonToInt64(object.value(QStringLiteral("id")), -1));
        }
    }

    const QJsonObject postProcessor = rootObject.value(QStringLiteral("post_processor")).toObject();
    const QJsonObject specialTokensObject =
        postProcessor.value(QStringLiteral("special_tokens")).toObject();

    for (auto it = specialTokensObject.constBegin(); it != specialTokensObject.constEnd(); ++it)
    {
        if (!specialTokens.contains(it.key()))
        {
            specialTokens.insert(it.key(), JsonToInt64(it.value(), -1));
        }
    }

    return specialTokens;
}

} // anonymous namespace

/**
 * @brief EmbeddingClient 内部实现
 *
 * 隔离 onnxruntime 类型，避免在公共头文件中暴露第三方依赖。
 */
struct EmbeddingClient::_tagImpl
{
    EmbeddingConfig config;                   ///< 配置
    std::function<void(const QString &)> logCallback; ///< 诊断日志回调
    QString lastError;                        ///< 最近一次诊断信息
    bool ready = false;                       ///< 模型是否就绪
    int dimension = 0;                        ///< 输出向量维度

    std::unique_ptr<QLibrary> onnxLibrary;    ///< onnxruntime.dll 动态库句柄
    std::unique_ptr<QLibrary> providersLibrary; ///< 可选 provider 桥接库（保持加载）
    const OrtApi *api = nullptr;              ///< onnxruntime C API 表

    std::unique_ptr<Ort::Env> env;            ///< ONNX 环境
    std::unique_ptr<Ort::SessionOptions> sessionOptions; ///< 会话选项
    std::unique_ptr<Ort::Session> session;    ///< 推理会话

    QHash<QString, int64_t> vocab;            ///< WordPiece 词表（word -> id）
    int64_t unkId = 0;                        ///< [UNK] id
    int64_t clsId = 0;                        ///< [CLS] id
    int64_t sepId = 0;                        ///< [SEP] id
    bool doLowerCase = true;                  ///< 是否小写化
    std::string outputName;                   ///< 输出张量名（last_hidden_state）
    std::string inputIdsName = "input_ids";   ///< 输入张量名
    std::string attentionMaskName = "attention_mask"; ///< 输入张量名
    std::string tokenTypeIdsName = "token_type_ids";  ///< 输入张量名

    /**
     * @brief 记录诊断日志（仅类别信息，不含敏感原文）
     */
    void Log(const QString &message)
    {
        if (logCallback)
        {
            logCallback(message);
        }
    }
};

EmbeddingClient::EmbeddingClient(const EmbeddingConfig &config,
                                 std::function<void(const QString &)> logCallback)
    : m_impl(std::make_unique<_tagImpl>())
{
    m_impl->config = config;

    if (logCallback)
    {
        m_impl->logCallback = std::move(logCallback);
    }
}

EmbeddingClient::~EmbeddingClient() = default;

bool EmbeddingClient::IsReady() const
{
    return m_impl->ready;
}

QString EmbeddingClient::ModelId() const
{
    return m_impl->config.model;
}

int EmbeddingClient::Dimension() const
{
    return m_impl->dimension;
}

QString EmbeddingClient::LastError() const
{
    return m_impl->lastError;
}

bool EmbeddingClient::Reload()
{
    m_impl->ready = false;
    m_impl->dimension = 0;
    m_impl->session.reset();
    m_impl->sessionOptions.reset();
    m_impl->env.reset();
    m_impl->api = nullptr;
    m_impl->vocab.clear();
    m_impl->providersLibrary.reset();
    m_impl->onnxLibrary.reset();

    const EmbeddingConfig &config = m_impl->config;

    if (!config.enabled)
    {
        m_impl->lastError = QStringLiteral("Embedding is disabled by config.");
        return false;
    }

    if (config.backend != QStringLiteral("local_onnx"))
    {
        m_impl->lastError = QStringLiteral("Unsupported embedding backend: %1")
                                .arg(config.backend);
        return false;
    }

    QString modelDir = config.modelDir.trimmed();

    if (modelDir.isEmpty())
    {
        const QString modelName = config.model;
        const int slashIndex = qMax(modelName.lastIndexOf(QLatin1Char('/')),
                                    modelName.lastIndexOf(QLatin1Char('\\')));

        const QString fallbackName = (slashIndex >= 0)
                                         ? modelName.mid(slashIndex + 1)
                                         : modelName;
        modelDir = QCoreApplication::applicationDirPath()
                   + QLatin1Char('/')
                   + DEFAULT_MODEL_DIR_SUFFIX
                   + QLatin1Char('/')
                   + fallbackName;
    }

    const QFileInfo modelFileInfo(QDir(modelDir).filePath(config.onnxModel.trimmed()));
    const QFileInfo tokenizerFileInfo(QDir(modelDir).filePath(config.tokenizerFile.trimmed()));

    if (!modelFileInfo.isFile())
    {
        m_impl->lastError = QStringLiteral("Embedding model file not found: %1")
                                .arg(modelFileInfo.absoluteFilePath());
        m_impl->Log(m_impl->lastError);
        return false;
    }

    if (!tokenizerFileInfo.isFile())
    {
        m_impl->lastError = QStringLiteral("Embedding tokenizer file not found: %1")
                                .arg(tokenizerFileInfo.absoluteFilePath());
        m_impl->Log(m_impl->lastError);
        return false;
    }

    if (!LoadOnnxRuntime(modelFileInfo.absolutePath()))
    {
        return false;
    }

    if (!LoadTokenizer(tokenizerFileInfo.absoluteFilePath()))
    {
        return false;
    }

    try
    {
        m_impl->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                                 "vpet_memory_embedding");
        m_impl->sessionOptions = std::make_unique<Ort::SessionOptions>();
        m_impl->sessionOptions->SetIntraOpNumThreads(1);
        m_impl->sessionOptions->SetGraphOptimizationLevel(ORT_ENABLE_ALL);

        const std::wstring modelPath = modelFileInfo.absoluteFilePath().toStdWString();
        m_impl->session = std::make_unique<Ort::Session>(*m_impl->env,
                                                          modelPath.c_str(),
                                                          *m_impl->sessionOptions);
    }
    catch (const Ort::Exception &exception)
    {
        m_impl->lastError = QStringLiteral("Failed to create embedding session: %1")
                                .arg(QString::fromStdString(exception.what()));
        m_impl->Log(m_impl->lastError);
        return false;
    }
    catch (const std::exception &exception)
    {
        m_impl->lastError = QStringLiteral("Failed to create embedding session: %1")
                                .arg(QString::fromStdString(exception.what()));
        m_impl->Log(m_impl->lastError);
        return false;
    }

    if (!ResolveTensors())
    {
        return false;
    }

    m_impl->ready = true;
    m_impl->lastError.clear();

    m_impl->Log(QStringLiteral("Embedding model ready: %1 (dim %2).")
                    .arg(config.model)
                    .arg(m_impl->dimension));
    return true;
}

bool EmbeddingClient::EmbedDocument(const QString &text, QVector<float> &outVector)
{
    return EmbedText(text, false, outVector);
}

bool EmbeddingClient::EmbedQuery(const QString &text, QVector<float> &outVector)
{
    const QString prefixedText = m_impl->config.queryInstruction + text;
    return EmbedText(prefixedText, true, outVector);
}

bool EmbeddingClient::MeanPoolAndNormalize(const float *hiddenState,
                                           const int64_t *attentionMask,
                                           int sequenceLength,
                                           int hiddenDim,
                                           QVector<float> &outVector)
{
    if ((hiddenState == nullptr) || (attentionMask == nullptr))
    {
        return false;
    }

    if ((sequenceLength <= 0) || (hiddenDim <= 0))
    {
        return false;
    }

    QVector<float> pooled(hiddenDim, 0.0f);
    int64_t maskSum = 0;

    for (int position = 0; position < sequenceLength; ++position)
    {
        const int64_t maskValue = attentionMask[position];

        if (maskValue <= 0)
        {
            continue;
        }

        maskSum += maskValue;

        for (int dim = 0; dim < hiddenDim; ++dim)
        {
            pooled[dim] += hiddenState[position * hiddenDim + dim];
        }
    }

    if (maskSum <= 0)
    {
        return false;
    }

    const float maskSumFloat = static_cast<float>(maskSum);

    for (int dim = 0; dim < hiddenDim; ++dim)
    {
        pooled[dim] /= maskSumFloat;
    }

    double squaredSum = 0.0;

    for (int dim = 0; dim < hiddenDim; ++dim)
    {
        squaredSum += static_cast<double>(pooled[dim]) * pooled[dim];
    }

    if (squaredSum <= std::numeric_limits<double>::epsilon())
    {
        return false;
    }

    const double inverseNorm = 1.0 / std::sqrt(squaredSum);

    for (int dim = 0; dim < hiddenDim; ++dim)
    {
        pooled[dim] = static_cast<float>(pooled[dim] * inverseNorm);
    }

    outVector = pooled;
    return true;
}

bool EmbeddingClient::LoadOnnxRuntime(const QString &libraryDir)
{
    const QString providersLibraryName =
        QDir(libraryDir).filePath(QStringLiteral("onnxruntime_providers_shared.dll"));
    const QFileInfo providersInfo(providersLibraryName);

    if (providersInfo.isFile())
    {
        auto providersLibrary = std::make_unique<QLibrary>(providersInfo.absoluteFilePath());

        if (!providersLibrary->load())
        {
            m_impl->lastError = QStringLiteral("Failed to load %1: %2")
                                    .arg(providersInfo.fileName())
                                    .arg(providersLibrary->errorString());
            m_impl->Log(m_impl->lastError);
            return false;
        }

        m_impl->providersLibrary = std::move(providersLibrary);
    }

    auto onnxLibrary = std::make_unique<QLibrary>(libraryDir + QStringLiteral("/onnxruntime"));

    if (!onnxLibrary->load())
    {
        onnxLibrary = std::make_unique<QLibrary>(QCoreApplication::applicationDirPath()
                                                 + QStringLiteral("/onnxruntime"));

        if (!onnxLibrary->load())
        {
            onnxLibrary = std::make_unique<QLibrary>(QStringLiteral("onnxruntime"));

            if (!onnxLibrary->load())
            {
                m_impl->lastError = QStringLiteral(
                    "onnxruntime.dll not found; embedding disabled (fallback to keyword search).");
                m_impl->Log(m_impl->lastError);
                return false;
            }
        }
    }

    const auto getApiBase = reinterpret_cast<const OrtApiBase *(*)()>(
        onnxLibrary->resolve("OrtGetApiBase"));

    if (getApiBase == nullptr)
    {
        m_impl->lastError = QStringLiteral("onnxruntime.dll is invalid (OrtGetApiBase missing).");
        m_impl->Log(m_impl->lastError);
        return false;
    }

    m_impl->api = getApiBase()->GetApi(ORT_API_VERSION);

    if (m_impl->api == nullptr)
    {
        m_impl->lastError = QStringLiteral("onnxruntime API version %1 is unsupported.")
                                .arg(ORT_API_VERSION);
        m_impl->Log(m_impl->lastError);
        return false;
    }

    Ort::InitApi(m_impl->api);
    m_impl->onnxLibrary = std::move(onnxLibrary);
    return true;
}

bool EmbeddingClient::LoadTokenizer(const QString &tokenizerPath)
{
    QFile tokenizerFile(tokenizerPath);

    if (!tokenizerFile.open(QIODevice::ReadOnly))
    {
        m_impl->lastError = QStringLiteral("Failed to open tokenizer file: %1")
                                .arg(tokenizerPath);
        m_impl->Log(m_impl->lastError);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(tokenizerFile.readAll(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        m_impl->lastError = QStringLiteral("Tokenizer file is invalid JSON: %1")
                                .arg(tokenizerPath);
        m_impl->Log(m_impl->lastError);
        return false;
    }

    const QJsonObject rootObject = document.object();
    const QJsonObject modelObject = rootObject.value(QStringLiteral("model")).toObject();
    const QJsonObject vocabObject = modelObject.value(QStringLiteral("vocab")).toObject();

    if (vocabObject.isEmpty())
    {
        m_impl->lastError = QStringLiteral("Tokenizer vocab is empty or missing.");
        m_impl->Log(m_impl->lastError);
        return false;
    }

    for (auto it = vocabObject.constBegin(); it != vocabObject.constEnd(); ++it)
    {
        m_impl->vocab.insert(it.key(), JsonToInt64(it.value(), -1));
    }

    const QHash<QString, int64_t> specialTokens = ParseSpecialTokens(rootObject);

    m_impl->unkId = specialTokens.value(QStringLiteral("[UNK]"), 100);
    m_impl->clsId = specialTokens.value(QStringLiteral("[CLS]"), 101);
    m_impl->sepId = specialTokens.value(QStringLiteral("[SEP]"), 102);

    const QFileInfo tokenizerConfigInfo(
        QDir(QFileInfo(tokenizerPath).absolutePath()).filePath(TOKENIZER_CONFIG_FILE));

    if (tokenizerConfigInfo.isFile())
    {
        QFile configFile(tokenizerConfigInfo.absoluteFilePath());

        if (configFile.open(QIODevice::ReadOnly))
        {
            QJsonParseError configParseError;
            const QJsonDocument configDocument =
                QJsonDocument::fromJson(configFile.readAll(), &configParseError);

            if ((configParseError.error == QJsonParseError::NoError)
                && configDocument.isObject())
            {
                const QJsonValue lowerCaseValue =
                    configDocument.object().value(QStringLiteral("do_lower_case"));

                if (lowerCaseValue.isBool())
                {
                    m_impl->doLowerCase = lowerCaseValue.toBool();
                }
            }
        }
    }

    return true;
}

bool EmbeddingClient::ResolveTensors()
{
    try
    {
        const size_t inputCount = m_impl->session->GetInputCount();
        const size_t outputCount = m_impl->session->GetOutputCount();

        if ((inputCount < 1) || (outputCount < 1))
        {
            m_impl->lastError = QStringLiteral("Embedding model has no inputs or outputs.");
            m_impl->Log(m_impl->lastError);
            return false;
        }

        for (size_t index = 0; index < inputCount; ++index)
        {
            const Ort::AllocatedStringPtr namePtr = m_impl->session->GetInputNameAllocated(
                index, Ort::AllocatorWithDefaultOptions());
            const QString name = QString::fromUtf8(namePtr.get());

            if (name.contains(QStringLiteral("token_type"), Qt::CaseInsensitive))
            {
                m_impl->tokenTypeIdsName = name.toStdString();
            }
            else if (name.contains(QStringLiteral("attention"), Qt::CaseInsensitive))
            {
                m_impl->attentionMaskName = name.toStdString();
            }
            else
            {
                m_impl->inputIdsName = name.toStdString();
            }
        }

        QString fallbackOutputName;

        for (size_t index = 0; index < outputCount; ++index)
        {
            const Ort::AllocatedStringPtr namePtr = m_impl->session->GetOutputNameAllocated(
                index, Ort::AllocatorWithDefaultOptions());
            const QString name = QString::fromUtf8(namePtr.get());

            if (fallbackOutputName.isEmpty())
            {
                fallbackOutputName = name;
            }

            if (name.contains(QStringLiteral("last_hidden_state"), Qt::CaseInsensitive))
            {
                m_impl->outputName = name.toStdString();
                break;
            }
        }

        if (m_impl->outputName.empty())
        {
            m_impl->outputName = fallbackOutputName.toStdString();
        }

        const Ort::TypeInfo outputTypeInfo = m_impl->session->GetOutputTypeInfo(0);
        const auto tensorInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> outputShape = tensorInfo.GetShape();

        if ((outputShape.size() != 3) || (outputShape[2] <= 0))
        {
            m_impl->lastError = QStringLiteral(
                "Embedding model output shape is not [1, sequence, hidden]: %1.")
                                    .arg(QString::fromStdString(
                                        [&outputShape]()
                                        {
                                            std::string buffer;

                                            for (const int64_t value : outputShape)
                                            {
                                                buffer += std::to_string(value) + ",";
                                            }

                                            return buffer;
                                        }()));
            m_impl->Log(m_impl->lastError);
            return false;
        }

        m_impl->dimension = static_cast<int>(outputShape[2]);
        return true;
    }
    catch (const Ort::Exception &exception)
    {
        m_impl->lastError = QStringLiteral("Failed to inspect embedding model tensors: %1")
                                .arg(QString::fromStdString(exception.what()));
        m_impl->Log(m_impl->lastError);
        return false;
    }
}

bool EmbeddingClient::EmbedText(const QString &text, bool isQuery, QVector<float> &outVector)
{
    if (!m_impl->ready || (m_impl->session == nullptr))
    {
        return false;
    }

    if (text.trimmed().isEmpty())
    {
        m_impl->lastError = QStringLiteral("Embedding input text is empty.");
        return false;
    }

    QVector<int64_t> inputIds;
    QVector<int64_t> attentionMask;
    QVector<int64_t> tokenTypeIds;

    if (!Tokenize(text, inputIds, attentionMask, tokenTypeIds))
    {
        return false;
    }

    const int64_t sequenceLength = inputIds.size();

    try
    {
        const std::array<int64_t, 2> shape = { 1, sequenceLength };
        Ort::MemoryInfo memoryInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value inputIdsValue = Ort::Value::CreateTensor<int64_t>(
            memoryInfo, inputIds.data(), inputIds.size(), shape.data(), shape.size());
        Ort::Value attentionMaskValue = Ort::Value::CreateTensor<int64_t>(
            memoryInfo, attentionMask.data(), attentionMask.size(), shape.data(), shape.size());
        Ort::Value tokenTypeIdsValue = Ort::Value::CreateTensor<int64_t>(
            memoryInfo, tokenTypeIds.data(), tokenTypeIds.size(), shape.data(), shape.size());

        const std::array<const char *, 3> inputNames = {
            m_impl->inputIdsName.c_str(),
            m_impl->attentionMaskName.c_str(),
            m_impl->tokenTypeIdsName.c_str()
        };
        const std::array<Ort::Value, 3> inputValues = {
            std::move(inputIdsValue),
            std::move(attentionMaskValue),
            std::move(tokenTypeIdsValue)
        };
        const std::array<const char *, 1> outputNames = { m_impl->outputName.c_str() };

        Ort::RunOptions runOptions;
        std::vector<Ort::Value> outputs = m_impl->session->Run(
            runOptions,
            inputNames.data(),
            inputValues.data(),
            inputValues.size(),
            outputNames.data(),
            outputNames.size());

        if (outputs.empty())
        {
            m_impl->lastError = QStringLiteral("Embedding model produced no outputs.");
            return false;
        }

        const Ort::TensorTypeAndShapeInfo tensorInfo = outputs[0].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> outputShape = tensorInfo.GetShape();

        if ((outputShape.size() != 3)
            || (outputShape[0] != 1)
            || (outputShape[1] != sequenceLength)
            || (outputShape[2] != m_impl->dimension))
        {
            m_impl->lastError = QStringLiteral(
                "Embedding model output shape mismatch during inference.");
            return false;
        }

        const float *hiddenState = outputs[0].GetTensorData<float>();

        if (hiddenState == nullptr)
        {
            m_impl->lastError = QStringLiteral("Embedding model output data is null.");
            return false;
        }

        if (!MeanPoolAndNormalize(hiddenState,
                                  attentionMask.constData(),
                                  static_cast<int>(sequenceLength),
                                  m_impl->dimension,
                                  outVector))
        {
            m_impl->lastError = QStringLiteral("Embedding pooling failed.");
            return false;
        }

        m_impl->lastError.clear();

        if (isQuery)
        {
            m_impl->Log(QStringLiteral("Embedded query (%1 tokens).").arg(sequenceLength));
        }

        return true;
    }
    catch (const Ort::Exception &exception)
    {
        m_impl->lastError = QStringLiteral("Embedding inference failed: %1")
                                .arg(QString::fromStdString(exception.what()));
        m_impl->Log(m_impl->lastError);
        return false;
    }
    catch (const std::exception &exception)
    {
        m_impl->lastError = QStringLiteral("Embedding inference failed: %1")
                                .arg(QString::fromStdString(exception.what()));
        m_impl->Log(m_impl->lastError);
        return false;
    }
}

bool EmbeddingClient::Tokenize(const QString &text,
                               QVector<int64_t> &inputIds,
                               QVector<int64_t> &attentionMask,
                               QVector<int64_t> &tokenTypeIds) const
{
    QString normalizedText = text.trimmed();

    if (m_impl->doLowerCase)
    {
        normalizedText = normalizedText.toLower();
    }

    QString spacedText;

    for (const QChar &ch : normalizedText)
    {
        if (IsCjk(ch))
        {
            spacedText += QLatin1Char(' ');
            spacedText += ch;
            spacedText += QLatin1Char(' ');
        }
        else
        {
            spacedText += ch;
        }
    }

    QStringList pieces;

    for (const QString &piece : spacedText.split(QLatin1Char(' '), Qt::SkipEmptyParts))
    {
        if (piece.isEmpty())
        {
            continue;
        }

        pieces.append(SplitOnPunctuation(piece));
    }

    QVector<int64_t> tokenIds;
    tokenIds.reserve(pieces.size());

    for (const QString &piece : pieces)
    {
        const QVector<int64_t> wordPiece = WordPiece(piece);

        if (wordPiece.isEmpty())
        {
            tokenIds.append(m_impl->unkId);
        }
        else
        {
            tokenIds.append(wordPiece);
        }
    }

    const int maxTokens = qMax(0, m_impl->config.maxSequenceLength - 2);

    if (tokenIds.size() > maxTokens)
    {
        tokenIds.resize(maxTokens);
    }

    inputIds.clear();
    inputIds.reserve(tokenIds.size() + 2);
    inputIds.append(m_impl->clsId);
    inputIds.append(tokenIds);
    inputIds.append(m_impl->sepId);

    attentionMask.clear();
    attentionMask.fill(1, inputIds.size());

    tokenTypeIds.clear();
    tokenTypeIds.fill(0, inputIds.size());

    return true;
}

QStringList EmbeddingClient::SplitOnPunctuation(const QString &piece) const
{
    QStringList result;
    QString current;

    const auto flushCurrent = [&result, &current]()
    {
        if (!current.isEmpty())
        {
            result.append(current);
            current.clear();
        }
    };

    for (const QChar &ch : piece)
    {
        if (IsSplitCharacter(ch))
        {
            flushCurrent();
        }
        else
        {
            current += ch;
        }
    }

    flushCurrent();
    return result;
}

QVector<int64_t> EmbeddingClient::WordPiece(const QString &token) const
{
    if (m_impl->vocab.contains(token))
    {
        return { m_impl->vocab.value(token, m_impl->unkId) };
    }

    QVector<int64_t> pieces;
    int startIndex = 0;
    const int tokenSize = token.size();

    while (startIndex < tokenSize)
    {
        int endIndex = tokenSize;
        int64_t matchedId = -1;

        while (startIndex < endIndex)
        {
            QString candidate = token.mid(startIndex, endIndex - startIndex);

            if (startIndex > 0)
            {
                candidate = QStringLiteral("##") + candidate;
            }

            if (m_impl->vocab.contains(candidate))
            {
                matchedId = m_impl->vocab.value(candidate, -1);
                break;
            }

            endIndex -= 1;
        }

        if (matchedId < 0)
        {
            return {};
        }

        pieces.append(matchedId);
        startIndex = endIndex;
    }

    return pieces;
}

} // namespace vpet
