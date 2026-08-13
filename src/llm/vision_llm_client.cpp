#include "vpet/llm/vision_llm_client.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QUrl>
#include <QtGlobal>

namespace vpet
{

namespace
{

constexpr int DEFAULT_TIMEOUT_MS = 30000;
constexpr int MIN_TIMEOUT_MS = 1000;
constexpr int MAX_TIMEOUT_MS = 120000;
constexpr int MIN_MAX_TOKENS = 1;
constexpr int MAX_MAX_TOKENS = 128000;
constexpr double MIN_TEMPERATURE = 0.0;
constexpr double MAX_TEMPERATURE = 2.0;
constexpr qsizetype MAX_BASE64_IMAGE_BYTES = 30000000;

} // anonymous namespace

VisionLlmClient::VisionLlmClient(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_gptConfig()
    , m_mimoConfig()
    , m_config()
    , m_defaultOptions()
    , m_isConfigured(false)
    , m_hasGptConfig(false)
    , m_hasMimoConfig(false)
    , m_activeProfile(VISION_LLM_MODEL_PROFILE::GPT)
    , m_nextRequestId(1)
{
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &VisionLlmClient::OnReplyFinished);
}

VisionLlmClient::~VisionLlmClient()
{
    // m_networkManager 由 QObject 父子关系自动销毁。
}

bool VisionLlmClient::LoadConfig(const QString &configPath)
{
    if (configPath.trimmed().isEmpty())
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM config path is empty."), 0);
        return false;
    }

    QFile file(configPath);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        emit AnalysisFailed(-1, QStringLiteral("Failed to open Vision LLM config file."), 0);
        return false;
    }

    const QByteArray configData = file.readAll();
    file.close();

    const QJsonDocument document = QJsonDocument::fromJson(configData);

    if (!document.isObject())
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM config is not a JSON object."), 0);
        return false;
    }

    const QJsonObject object = document.object();
    _tagVisionLlmConfig config;
    config.baseUrl = object.value(QStringLiteral("base_url")).toString();
    config.apiKey = object.value(QStringLiteral("api_key")).toString();
    config.model = object.value(QStringLiteral("model")).toString();
    config.profile = InferProfileFromModelName(config.model);
    config.defaultPrompt = object.value(QStringLiteral("default_prompt")).toString();
    config.mediaType = object.value(QStringLiteral("media_type")).toString();
    config.timeoutMs = object.value(QStringLiteral("timeout_ms")).toInt(DEFAULT_TIMEOUT_MS);

    _tagVisionLlmRequestOptions options;
    options.temperature = object.value(QStringLiteral("temperature")).toDouble(options.temperature);
    options.maxTokens = object.value(QStringLiteral("max_tokens")).toInt(options.maxTokens);
    options.detailLevel = StringToDetailLevel(object.value(QStringLiteral("detail")).toString());

    const bool configured = SetConfig(config);

    if (configured)
    {
        m_defaultOptions = NormalizeOptions(options);

        const _tagVisionLlmConfig gptCandidate = [config]() -> _tagVisionLlmConfig
        {
            _tagVisionLlmConfig candidate;
            candidate.baseUrl = qEnvironmentVariable("OPENAI_BASE_URL",
                                                     QStringLiteral("https://api.openai.com/v1"));
            candidate.apiKey = qEnvironmentVariable("OPENAI_API_KEY");
            candidate.model = qEnvironmentVariable("OPENAI_MODEL", QStringLiteral("gpt-4o"));
            candidate.profile = VISION_LLM_MODEL_PROFILE::GPT;
            candidate.defaultPrompt = config.defaultPrompt;
            candidate.mediaType = config.mediaType;
            candidate.timeoutMs = config.timeoutMs;
            return candidate;
        }();

        const _tagVisionLlmConfig mimoCandidate = [config]() -> _tagVisionLlmConfig
        {
            _tagVisionLlmConfig candidate;
            candidate.baseUrl = QStringLiteral("https://api.xiaomimimo.com/v1");
            candidate.apiKey = qEnvironmentVariable("MIMO_API_KEY");
            candidate.model = QStringLiteral("mimo-v2.5");
            candidate.profile = VISION_LLM_MODEL_PROFILE::MIMO_V2_5;
            candidate.defaultPrompt = config.defaultPrompt;
            candidate.mediaType = config.mediaType;
            candidate.timeoutMs = config.timeoutMs;
            return candidate;
        }();

        QString errorMessage;
        _tagVisionLlmConfig normalizedCandidate;

        if (NormalizeConfig(gptCandidate, normalizedCandidate, errorMessage))
        {
            m_gptConfig = normalizedCandidate;
            m_hasGptConfig = true;
        }

        if (NormalizeConfig(mimoCandidate, normalizedCandidate, errorMessage))
        {
            m_mimoConfig = normalizedCandidate;
            m_hasMimoConfig = true;
        }
    }

    return configured;
}

bool VisionLlmClient::SetConfig(const _tagVisionLlmConfig &config)
{
    QString errorMessage;
    _tagVisionLlmConfig normalizedConfig;
    const bool isValid = NormalizeConfig(config, normalizedConfig, errorMessage);

    if (!isValid)
    {
        m_isConfigured = false;
        emit AnalysisFailed(-1, errorMessage, 0);
        return false;
    }

    if (normalizedConfig.profile == VISION_LLM_MODEL_PROFILE::MIMO_V2_5)
    {
        m_mimoConfig = normalizedConfig;
        m_hasMimoConfig = true;
    }
    else
    {
        m_gptConfig = normalizedConfig;
        m_hasGptConfig = true;
    }

    m_activeProfile = normalizedConfig.profile;
    m_config = normalizedConfig;
    m_defaultOptions = NormalizeOptions(m_defaultOptions);
    m_isConfigured = true;

    return true;
}

bool VisionLlmClient::IsConfigured() const
{
    return m_isConfigured;
}

bool VisionLlmClient::SetActiveProfile(VISION_LLM_MODEL_PROFILE profile)
{
    if (!ActivateProfileConfig(profile))
    {
        return false;
    }

    m_activeProfile = profile;
    qDebug() << "[VisionLLM] Active model profile changed:" << m_config.model;
    return true;
}

VISION_LLM_MODEL_PROFILE VisionLlmClient::GetActiveProfile() const
{
    return m_activeProfile;
}

int VisionLlmClient::AnalyzeScreenshot(const QString &prompt,
                                       const QByteArray &base64Image,
                                       const QString &mediaType,
                                       const _tagVisionLlmRequestOptions &options)
{
    if (!m_isConfigured)
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM client is not configured."), 0);
        return -1;
    }

    if (prompt.trimmed().isEmpty())
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM prompt is empty."), 0);
        return -1;
    }

    QString imageDataUrl;
    QString errorMessage;

    if (!BuildImageDataUrl(base64Image, mediaType, imageDataUrl, errorMessage))
    {
        emit AnalysisFailed(-1, errorMessage, 0);
        return -1;
    }

    const _tagVisionLlmRequestOptions normalizedOptions = NormalizeOptions(options);
    QJsonArray contentArray;
    QJsonObject imageUrlObject;
    imageUrlObject[QStringLiteral("url")] = imageDataUrl;
    imageUrlObject[QStringLiteral("detail")] = DetailLevelToString(normalizedOptions.detailLevel);

    QJsonObject imageObject;
    imageObject[QStringLiteral("type")] = QStringLiteral("image_url");
    imageObject[QStringLiteral("image_url")] = imageUrlObject;
    contentArray.append(imageObject);

    QJsonObject textObject;
    textObject[QStringLiteral("type")] = QStringLiteral("text");
    textObject[QStringLiteral("text")] = prompt;
    contentArray.append(textObject);

    QJsonObject messageObject;
    messageObject[QStringLiteral("role")] = QStringLiteral("user");
    messageObject[QStringLiteral("content")] = contentArray;

    QJsonArray messageArray;

    if (m_config.profile == VISION_LLM_MODEL_PROFILE::MIMO_V2_5)
    {
        QJsonObject systemMessageObject;
        systemMessageObject[QStringLiteral("role")] = QStringLiteral("system");
        systemMessageObject[QStringLiteral("content")] = QStringLiteral(
            "You are MiMo, an AI assistant developed by Xiaomi.");
        messageArray.append(systemMessageObject);
    }

    messageArray.append(messageObject);

    QJsonObject body;
    body[QStringLiteral("model")] = m_config.model;
    body[QStringLiteral("messages")] = messageArray;
    body[QStringLiteral("temperature")] = normalizedOptions.temperature;

    if (m_config.profile == VISION_LLM_MODEL_PROFILE::MIMO_V2_5)
    {
        body[QStringLiteral("max_completion_tokens")] = normalizedOptions.maxTokens;
    }
    else
    {
        body[QStringLiteral("max_tokens")] = normalizedOptions.maxTokens;
    }

    body[QStringLiteral("stream")] = false;

    const QByteArray bodyData = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QUrl requestUrl(m_config.baseUrl + QStringLiteral("/chat/completions"));

    if (!requestUrl.isValid())
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM request URL is invalid."), 0);
        return -1;
    }

    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_config.apiKey.toUtf8());
    request.setTransferTimeout(m_config.timeoutMs);

    const int requestId = m_nextRequestId;
    m_nextRequestId += 1;

    QNetworkReply *reply = m_networkManager->post(request, bodyData);

    if (reply == nullptr)
    {
        emit AnalysisFailed(requestId,
                            QStringLiteral("Failed to create Vision LLM HTTP request."),
                            0);
        return -1;
    }

    reply->setProperty("requestId", requestId);

    return requestId;
}

int VisionLlmClient::AnalyzeScreenshot(const QString &prompt,
                                       const QByteArray &base64Image,
                                       const QString &mediaType)
{
    const _tagVisionLlmRequestOptions options;

    return AnalyzeScreenshot(prompt, base64Image, mediaType, options);
}

bool VisionLlmClient::CancelRequest(int requestId)
{
    if ((requestId <= 0) || (m_networkManager == nullptr))
    {
        return false;
    }

    const QList<QNetworkReply *> replies = m_networkManager->findChildren<QNetworkReply *>();

    for (QNetworkReply *reply : replies)
    {
        if ((reply != nullptr) && (reply->property("requestId").toInt() == requestId))
        {
            reply->abort();
            return true;
        }
    }

    return false;
}

void VisionLlmClient::AnalyzeCapturedFrame(const QByteArray &base64Data,
                                           int frameCount,
                                           const QSize &frameSize)
{
    if (frameCount <= 0)
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM frame count is invalid."), 0);
        return;
    }

    if (!frameSize.isValid())
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM frame size is invalid."), 0);
        return;
    }

    AnalyzeScreenshot(m_config.defaultPrompt,
                      base64Data,
                      m_config.mediaType,
                      m_defaultOptions);
}

void VisionLlmClient::OnReplyFinished(QNetworkReply *reply)
{
    if (reply == nullptr)
    {
        emit AnalysisFailed(-1, QStringLiteral("Vision LLM reply is null."), 0);
        return;
    }

    const auto replyGuard = qScopeGuard([reply]()
    {
        reply->deleteLater();
    });

    const int requestId = reply->property("requestId").toInt();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseData = reply->readAll();

    qDebug().noquote() << QStringLiteral("[VisionLLM] Response received, request: %1 status: %2 bytes: %3")
                          .arg(requestId)
                          .arg(statusCode)
                          .arg(responseData.size());

    if (requestId <= 0)
    {
        emit AnalysisFailed(-1,
                            QStringLiteral("Vision LLM reply does not contain request ID."),
                            statusCode);
        return;
    }

    if (reply->error() != QNetworkReply::NoError)
    {
        const QString message = QStringLiteral("Vision LLM network error: %1").arg(
                                    reply->errorString());
        emit AnalysisFailed(requestId, message, statusCode);
        return;
    }

    if ((statusCode < 200) || (statusCode >= 300))
    {
        const QString message = QStringLiteral("Vision LLM HTTP error. Response bytes: %1.")
                                .arg(responseData.size());
        emit AnalysisFailed(requestId, message, statusCode);
        return;
    }

    QString content;
    QString errorMessage;

    if (!ExtractAssistantContent(responseData, m_config.profile, content, errorMessage))
    {
        emit AnalysisFailed(requestId, errorMessage, statusCode);
        return;
    }

    emit AnalysisCompleted(requestId, content);
}

bool VisionLlmClient::NormalizeConfig(const _tagVisionLlmConfig &config,
                                      _tagVisionLlmConfig &normalizedConfig,
                                      QString &errorMessage)
{
    normalizedConfig = config;
    normalizedConfig.baseUrl = normalizedConfig.baseUrl.trimmed();
    normalizedConfig.apiKey = normalizedConfig.apiKey.trimmed();
    normalizedConfig.model = normalizedConfig.model.trimmed();
    normalizedConfig.profile = InferProfileFromModelName(normalizedConfig.model);
    normalizedConfig.defaultPrompt = normalizedConfig.defaultPrompt.trimmed();
    normalizedConfig.mediaType = normalizedConfig.mediaType.trimmed().toLower();

    if (normalizedConfig.baseUrl.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM base URL is empty.");
        return false;
    }

    if (normalizedConfig.baseUrl.endsWith(QStringLiteral("/")))
    {
        normalizedConfig.baseUrl.chop(1);
    }

    const QUrl baseUrl(normalizedConfig.baseUrl);

    if (!baseUrl.isValid() || baseUrl.scheme().isEmpty() || baseUrl.host().isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM base URL is invalid.");
        return false;
    }

    if (normalizedConfig.apiKey.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM API key is empty.");
        return false;
    }

    if (normalizedConfig.model.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM model is empty.");
        return false;
    }

    if (normalizedConfig.defaultPrompt.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM default prompt is empty.");
        return false;
    }

    if (normalizedConfig.mediaType.isEmpty())
    {
        normalizedConfig.mediaType = QStringLiteral("image/png");
    }

    if (normalizedConfig.mediaType == QStringLiteral("image/jpg"))
    {
        normalizedConfig.mediaType = QStringLiteral("image/jpeg");
    }

    if ((normalizedConfig.mediaType != QStringLiteral("image/png"))
        && (normalizedConfig.mediaType != QStringLiteral("image/jpeg")))
    {
        errorMessage = QStringLiteral("Vision LLM media type is unsupported.");
        return false;
    }

    if ((normalizedConfig.timeoutMs < MIN_TIMEOUT_MS)
        || (normalizedConfig.timeoutMs > MAX_TIMEOUT_MS))
    {
        normalizedConfig.timeoutMs = DEFAULT_TIMEOUT_MS;
    }

    return true;
}

_tagVisionLlmRequestOptions VisionLlmClient::NormalizeOptions(
    const _tagVisionLlmRequestOptions &options)
{
    _tagVisionLlmRequestOptions normalizedOptions = options;
    normalizedOptions.temperature = qBound(MIN_TEMPERATURE,
                                           normalizedOptions.temperature,
                                           MAX_TEMPERATURE);
    normalizedOptions.maxTokens = qBound(MIN_MAX_TOKENS,
                                         normalizedOptions.maxTokens,
                                         MAX_MAX_TOKENS);

    return normalizedOptions;
}

QString VisionLlmClient::DetailLevelToString(VISION_LLM_DETAIL_LEVEL detailLevel)
{
    switch (detailLevel)
    {
    case VISION_LLM_DETAIL_LEVEL::LOW:
        return QStringLiteral("low");

    case VISION_LLM_DETAIL_LEVEL::HIGH:
        return QStringLiteral("high");

    case VISION_LLM_DETAIL_LEVEL::AUTO:
        return QStringLiteral("auto");
    }

    return QStringLiteral("auto");
}

VISION_LLM_DETAIL_LEVEL VisionLlmClient::StringToDetailLevel(const QString &detailLevelName)
{
    const QString normalizedName = detailLevelName.trimmed().toLower();

    if (normalizedName == QStringLiteral("low"))
    {
        return VISION_LLM_DETAIL_LEVEL::LOW;
    }

    if (normalizedName == QStringLiteral("high"))
    {
        return VISION_LLM_DETAIL_LEVEL::HIGH;
    }

    return VISION_LLM_DETAIL_LEVEL::AUTO;
}

bool VisionLlmClient::ExtractAssistantContent(const QByteArray &responseData,
                                              VISION_LLM_MODEL_PROFILE profile,
                                              QString &content,
                                              QString &errorMessage)
{
    if (responseData.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM response body is empty.");
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(responseData);

    if (!document.isObject())
    {
        errorMessage = QStringLiteral("Vision LLM response is not a JSON object.");
        return false;
    }

    const QJsonArray choices = document.object().value(QStringLiteral("choices")).toArray();

    if (choices.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM response choices are empty.");
        return false;
    }

    const QJsonObject firstChoice = choices.at(0).toObject();
    const QJsonObject message = firstChoice.value(QStringLiteral("message")).toObject();
    content = ExtractMessageText(message, profile, errorMessage);

    if (content.isEmpty())
    {
        if (errorMessage.isEmpty())
        {
            errorMessage = QStringLiteral("Vision LLM response content is empty.");
        }

        return false;
    }

    return true;
}

QString VisionLlmClient::ExtractMessageText(const QJsonObject &message,
                                            VISION_LLM_MODEL_PROFILE profile,
                                            QString &errorMessage)
{
    if (message.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM response message is empty.");
        return QString();
    }

    if (profile == VISION_LLM_MODEL_PROFILE::MIMO_V2_5)
    {
        const QString content = message.value(QStringLiteral("content")).toString().trimmed();

        if (!content.isEmpty())
        {
            return content;
        }

        return message.value(QStringLiteral("reasoning_content")).toString().trimmed();
    }

    return message.value(QStringLiteral("content")).toString().trimmed();
}

bool VisionLlmClient::BuildImageDataUrl(const QByteArray &base64Image,
                                        const QString &mediaType,
                                        QString &dataUrl,
                                        QString &errorMessage)
{
    if (base64Image.isEmpty())
    {
        errorMessage = QStringLiteral("Vision LLM image Base64 data is empty.");
        return false;
    }

    if (base64Image.size() > MAX_BASE64_IMAGE_BYTES)
    {
        errorMessage = QStringLiteral("Vision LLM image Base64 data is too large.");
        return false;
    }

    const QString normalizedMediaType = mediaType.trimmed().toLower();

    if ((normalizedMediaType != QStringLiteral("image/png"))
        && (normalizedMediaType != QStringLiteral("image/jpeg"))
        && (normalizedMediaType != QStringLiteral("image/jpg")))
    {
        errorMessage = QStringLiteral("Vision LLM media type is unsupported.");
        return false;
    }

    const QString apiMediaType = normalizedMediaType == QStringLiteral("image/jpg")
                                 ? QStringLiteral("image/jpeg")
                                 : normalizedMediaType;
    dataUrl = QStringLiteral("data:%1;base64,%2").arg(apiMediaType,
                                                       QString::fromLatin1(base64Image));

    return true;
}

bool VisionLlmClient::ActivateProfileConfig(VISION_LLM_MODEL_PROFILE profile)
{
    if (profile == VISION_LLM_MODEL_PROFILE::MIMO_V2_5)
    {
        if (!m_hasMimoConfig)
        {
            emit AnalysisFailed(-1, QStringLiteral("MiMo vision model config is not available."), 0);
            return false;
        }

        m_config = m_mimoConfig;
        m_isConfigured = true;
        return true;
    }

    if (!m_hasGptConfig)
    {
        emit AnalysisFailed(-1, QStringLiteral("GPT vision model config is not available."), 0);
        return false;
    }

    m_config = m_gptConfig;
    m_isConfigured = true;
    return true;
}

VISION_LLM_MODEL_PROFILE VisionLlmClient::InferProfileFromModelName(const QString &modelName)
{
    const QString normalizedModelName = modelName.trimmed().toLower();

    if (normalizedModelName == QStringLiteral("mimo-v2.5"))
    {
        return VISION_LLM_MODEL_PROFILE::MIMO_V2_5;
    }

    return VISION_LLM_MODEL_PROFILE::GPT;
}

} // namespace vpet
