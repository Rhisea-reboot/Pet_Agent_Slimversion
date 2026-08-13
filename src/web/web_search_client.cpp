#include "vpet/web/web_search_client.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <QtGlobal>

namespace vpet
{

namespace
{

constexpr int MIN_TIMEOUT_MS = 1;
constexpr int MAX_TIMEOUT_MS = 60000;
constexpr int MIN_INTERVAL_MS = 0;
constexpr int MAX_INTERVAL_MS = 60000;
constexpr int MIN_RESPONSE_BYTES = 1024;
constexpr int MAX_RESPONSE_BYTES = 16 * 1024 * 1024;
constexpr int MIN_RESULTS = 1;
constexpr int MAX_RESULTS = 50;
constexpr int MIN_DESCRIPTION_CHARS = 1;
constexpr int MAX_DESCRIPTION_CHARS = 5000;
constexpr int MAX_THROTTLE_WAIT_MS = 3000;

const char REQUEST_ID_PROPERTY[] = "webSearchRequestId";

} // anonymous namespace

WebSearchClient::WebSearchClient(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_activeReply(nullptr)
    , m_throttleTimer(new QTimer(this))
    , m_timeoutTimer(new QTimer(this))
    , m_config()
    , m_pendingQuery()
    , m_pendingEngines()
    , m_responseData()
    , m_lastSearchStartedAtMs(0)
    , m_deadlineAtMs(0)
    , m_nextRequestId(1)
    , m_activeRequestId(-1)
    , m_isConfigured(false)
{
    m_throttleTimer->setSingleShot(true);
    m_timeoutTimer->setSingleShot(true);

    connect(m_throttleTimer, &QTimer::timeout,
            this, &WebSearchClient::OnThrottleTimeout);
    connect(m_timeoutTimer, &QTimer::timeout,
            this, &WebSearchClient::OnRequestTimeout);
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &WebSearchClient::OnReplyFinished);
}

WebSearchClient::~WebSearchClient()
{
    Cancel();
}

bool WebSearchClient::SetConfig(const _tagWebSearchConfig &config)
{
    QString errorMessage;
    _tagWebSearchConfig normalizedConfig;

    if (!NormalizeConfig(config, normalizedConfig, errorMessage))
    {
        m_isConfigured = false;
        emit SearchFailed(-1, errorMessage, 0);
        return false;
    }

    m_config = normalizedConfig;
    m_isConfigured = true;
    return true;
}

bool WebSearchClient::LoadConfig(const QString &configPath, QString &errorMessage)
{
    const QString normalizedPath = configPath.trimmed();

    if (normalizedPath.isEmpty())
    {
        errorMessage = QStringLiteral("Web search config path is empty.");
        return false;
    }

    QFile configFile(normalizedPath);

    if (!configFile.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("Web search config cannot be opened.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(configFile.readAll(), &parseError);

    if ((parseError.error != QJsonParseError::NoError) || !document.isObject())
    {
        errorMessage = QStringLiteral("Web search config is not a valid JSON object.");
        return false;
    }

    const QJsonObject object = document.object();
    _tagWebSearchConfig config;
    config.baseUrl = QUrl(object.value(QStringLiteral("base_url")).toString());
    const QString tokenEnvironmentVariable =
        object.value(QStringLiteral("authorization_token_env"))
            .toString(QStringLiteral("VPET_WEB_SEARCH_TOKEN"))
            .trimmed();

    if (!tokenEnvironmentVariable.isEmpty())
    {
        config.authorizationToken = QString::fromUtf8(
            qgetenv(tokenEnvironmentVariable.toUtf8().constData()));
    }

    config.totalTimeoutMs = object.value(QStringLiteral("request_timeout_ms"))
                                .toInt(config.totalTimeoutMs);
    config.minSearchIntervalMs = object.value(QStringLiteral("min_search_interval_ms"))
                                     .toInt(config.minSearchIntervalMs);
    config.maxThrottleWaitMs = object.value(QStringLiteral("max_throttle_wait_ms"))
                                   .toInt(config.maxThrottleWaitMs);
    config.maxResponseBytes = object.value(QStringLiteral("max_response_bytes"))
                                  .toInt(config.maxResponseBytes);
    config.maxResults = object.value(QStringLiteral("max_results"))
                            .toInt(config.maxResults);
    config.maxDescriptionChars = object.value(QStringLiteral("max_description_chars"))
                                     .toInt(config.maxDescriptionChars);

    if (!SetConfig(config))
    {
        errorMessage = QStringLiteral("Web search config contains invalid values.");
        return false;
    }

    return true;
}

bool WebSearchClient::IsConfigured() const
{
    return m_isConfigured;
}

bool WebSearchClient::IsBusy() const
{
    return (m_activeRequestId > 0);
}

int WebSearchClient::Search(const QString &query, const QStringList &engines)
{
    if (!m_isConfigured)
    {
        emit SearchFailed(-1, QStringLiteral("Web search client is not configured."), 0);
        return -1;
    }

    const QString normalizedQuery = query.trimmed();

    if (normalizedQuery.isEmpty())
    {
        emit SearchFailed(-1, QStringLiteral("Web search query is empty."), 0);
        return -1;
    }

    if (IsBusy())
    {
        emit SearchFailed(-1, QStringLiteral("Web search client is busy."), 0);
        return -1;
    }

    m_pendingQuery = normalizedQuery;
    m_pendingEngines = engines;
    m_responseData.clear();
    m_activeRequestId = m_nextRequestId;
    m_nextRequestId += 1;
    m_deadlineAtMs = QDateTime::currentMSecsSinceEpoch() + m_config.totalTimeoutMs;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = nowMs - m_lastSearchStartedAtMs;
    const qint64 intervalMs = static_cast<qint64>(m_config.minSearchIntervalMs);
    const qint64 waitMs = qMax<qint64>(0, intervalMs - elapsedMs);

    if (waitMs > m_config.maxThrottleWaitMs)
    {
        const int requestId = m_activeRequestId;
        FinishCancellation(false);
        emit SearchFailed(requestId, QStringLiteral("Web search request is throttled."), 0);
        return requestId;
    }

    const qint64 boundedWaitMs = waitMs;

    if (boundedWaitMs > 0)
    {
        m_throttleTimer->start(static_cast<int>(boundedWaitMs));
        return m_activeRequestId;
    }

    StartRequest();
    return m_activeRequestId;
}

bool WebSearchClient::Cancel()
{
    if (!IsBusy())
    {
        return false;
    }

    if (m_activeReply != nullptr)
    {
        QNetworkReply *reply = m_activeReply;
        m_activeReply = nullptr;
        reply->abort();
        reply->deleteLater();
    }

    FinishCancellation(true);
    return true;
}

void WebSearchClient::OnThrottleTimeout()
{
    if (!IsBusy())
    {
        return;
    }

    StartRequest();
}

void WebSearchClient::StartRequest()
{
    if (!IsBusy() || m_activeReply != nullptr)
    {
        return;
    }

    m_lastSearchStartedAtMs = QDateTime::currentMSecsSinceEpoch();

    QUrl requestUrl(m_config.baseUrl);
    QString path = requestUrl.path();

    if (!path.endsWith(QStringLiteral("/search")))
    {
        if (!path.endsWith(QStringLiteral("/")))
        {
            path.append(QChar('/'));
        }

        path.append(QStringLiteral("search"));
        requestUrl.setPath(path);
    }

    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(m_config.totalTimeoutMs);

    if (!m_config.authorizationToken.isEmpty())
    {
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") + m_config.authorizationToken.toUtf8());
    }

    QJsonObject body;
    body[QStringLiteral("query")] = m_pendingQuery;
    QJsonArray engineArray;

    for (const QString &engine : m_pendingEngines)
    {
        const QString normalizedEngine = engine.trimmed();

        if (!normalizedEngine.isEmpty())
        {
            engineArray.append(normalizedEngine);
        }
    }

    body[QStringLiteral("engines")] = engineArray;
    m_activeReply = m_networkManager->post(request,
                                           QJsonDocument(body).toJson(QJsonDocument::Compact));

    if (m_activeReply == nullptr)
    {
        const int requestId = m_activeRequestId;
        FinishCancellation(false);
        emit SearchFailed(requestId, QStringLiteral("Failed to create web search request."), 0);
        return;
    }

    m_activeReply->setProperty(REQUEST_ID_PROPERTY, m_activeRequestId);
    connect(m_activeReply, &QNetworkReply::readyRead,
            this, &WebSearchClient::OnReadyRead);
    connect(m_activeReply, &QNetworkReply::metaDataChanged,
            this, &WebSearchClient::OnMetaDataChanged);

    const qint64 remainingMs = m_deadlineAtMs - QDateTime::currentMSecsSinceEpoch();

    if (remainingMs <= 0)
    {
        OnRequestTimeout();
        return;
    }

    m_timeoutTimer->start(static_cast<int>(remainingMs));
}

void WebSearchClient::OnMetaDataChanged()
{
    if (m_activeReply == nullptr)
    {
        return;
    }

    const QVariant contentLength =
        m_activeReply->header(QNetworkRequest::ContentLengthHeader);

    if (!contentLength.isValid())
    {
        return;
    }

    bool isValidLength = false;
    const qint64 declaredLength = contentLength.toLongLong(&isValidLength);

    if (!isValidLength || (declaredLength < 0))
    {
        return;
    }

    if (declaredLength > m_config.maxResponseBytes)
    {
        const int requestId = m_activeRequestId;
        const int statusCode =
            m_activeReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QNetworkReply *reply = m_activeReply;
        m_activeReply = nullptr;
        reply->abort();
        reply->deleteLater();
        FinishCancellation(false);
        emit SearchFailed(requestId,
                          QStringLiteral("Web search response exceeds size limit."),
                          statusCode);
    }
}

void WebSearchClient::OnReadyRead()
{
    if (m_activeReply == nullptr)
    {
        return;
    }

    m_responseData.append(m_activeReply->readAll());

    if (m_responseData.size() > m_config.maxResponseBytes)
    {
        const int requestId = m_activeRequestId;
        QNetworkReply *reply = m_activeReply;
        m_activeReply = nullptr;
        reply->abort();
        reply->deleteLater();
        FinishCancellation(false);
        emit SearchFailed(requestId, QStringLiteral("Web search response exceeds size limit."), 0);
    }
}

void WebSearchClient::OnRequestTimeout()
{
    if (!IsBusy())
    {
        return;
    }

    const int requestId = m_activeRequestId;

    if (m_activeReply != nullptr)
    {
        QNetworkReply *reply = m_activeReply;
        m_activeReply = nullptr;
        reply->abort();
        reply->deleteLater();
    }

    FinishCancellation(false);
    emit SearchFailed(requestId, QStringLiteral("Web search request timed out."), 0);
}

void WebSearchClient::OnReplyFinished(QNetworkReply *reply)
{
    if (reply == nullptr)
    {
        return;
    }

    const int requestId = reply->property(REQUEST_ID_PROPERTY).toInt();

    if ((reply != m_activeReply) || (requestId != m_activeRequestId))
    {
        reply->deleteLater();
        return;
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_responseData.append(reply->readAll());
    m_timeoutTimer->stop();

    if (m_responseData.size() > m_config.maxResponseBytes)
    {
        FinishCancellation(false);
        emit SearchFailed(requestId, QStringLiteral("Web search response exceeds size limit."),
                          statusCode);
        return;
    }

    if (reply->error() != QNetworkReply::NoError)
    {
        const QString message = QStringLiteral("Web search network error: %1")
                                    .arg(reply->errorString());
        FinishCancellation(false);
        emit SearchFailed(requestId, message, statusCode);
        return;
    }

    if ((statusCode < 200) || (statusCode >= 300))
    {
        FinishCancellation(false);
        emit SearchFailed(requestId,
                          QStringLiteral("Web search HTTP error. Response bytes: %1.")
                              .arg(m_responseData.size()),
                          statusCode);
        return;
    }

    QVector<_tagWebSearchResult> results;
    QStringList partialFailures;
    QString errorMessage;

    if (!ParseResponse(m_responseData, results, partialFailures, errorMessage))
    {
        FinishCancellation(false);
        emit SearchFailed(requestId, errorMessage, statusCode);
        return;
    }

    const QString query = m_pendingQuery;
    FinishCancellation(false);
    emit SearchCompleted(requestId, query, results, partialFailures);
}

void WebSearchClient::FinishCancellation(bool emitCancellation)
{
    const int requestId = m_activeRequestId;
    m_throttleTimer->stop();
    m_timeoutTimer->stop();

    if (m_activeReply != nullptr)
    {
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }

    m_activeRequestId = -1;
    m_deadlineAtMs = 0;
    m_pendingQuery.clear();
    m_pendingEngines.clear();
    m_responseData.clear();

    if (emitCancellation)
    {
        emit SearchFailed(requestId, QStringLiteral("Web search request cancelled."), 0);
    }
}

bool WebSearchClient::NormalizeConfig(const _tagWebSearchConfig &config,
                                      _tagWebSearchConfig &normalizedConfig,
                                      QString &errorMessage)
{
    normalizedConfig = config;
    normalizedConfig.baseUrl = normalizedConfig.baseUrl.adjusted(QUrl::StripTrailingSlash);

    if (!normalizedConfig.baseUrl.isValid()
        || ((normalizedConfig.baseUrl.scheme() != QStringLiteral("http"))
            && (normalizedConfig.baseUrl.scheme() != QStringLiteral("https")))
        || normalizedConfig.baseUrl.host().isEmpty())
    {
        errorMessage = QStringLiteral("Web search base URL is invalid.");
        return false;
    }

    const QString host = normalizedConfig.baseUrl.host().trimmed().toLower();

    if ((host != QStringLiteral("127.0.0.1"))
        && (host != QStringLiteral("::1"))
        && (host != QStringLiteral("localhost")))
    {
        errorMessage = QStringLiteral("Web search base URL must use a loopback host.");
        return false;
    }

    normalizedConfig.authorizationToken = normalizedConfig.authorizationToken.trimmed();

    if ((normalizedConfig.totalTimeoutMs < MIN_TIMEOUT_MS)
        || (normalizedConfig.totalTimeoutMs > MAX_TIMEOUT_MS))
    {
        errorMessage = QStringLiteral("Web search timeout is outside the allowed range.");
        return false;
    }

    if ((normalizedConfig.minSearchIntervalMs < MIN_INTERVAL_MS)
        || (normalizedConfig.minSearchIntervalMs > MAX_INTERVAL_MS))
    {
        errorMessage = QStringLiteral("Web search interval is outside the allowed range.");
        return false;
    }

    normalizedConfig.maxThrottleWaitMs = qBound(0,
                                                 normalizedConfig.maxThrottleWaitMs,
                                                 MAX_THROTTLE_WAIT_MS);
    normalizedConfig.maxResponseBytes = qBound(MIN_RESPONSE_BYTES,
                                               normalizedConfig.maxResponseBytes,
                                               MAX_RESPONSE_BYTES);
    normalizedConfig.maxResults = qBound(MIN_RESULTS,
                                         normalizedConfig.maxResults,
                                         MAX_RESULTS);
    normalizedConfig.maxDescriptionChars = qBound(MIN_DESCRIPTION_CHARS,
                                                  normalizedConfig.maxDescriptionChars,
                                                  MAX_DESCRIPTION_CHARS);

    return true;
}

bool WebSearchClient::ParseResponse(const QByteArray &responseData,
                                    QVector<_tagWebSearchResult> &results,
                                    QStringList &partialFailures,
                                    QString &errorMessage) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(responseData, &parseError);

    if (!document.isObject())
    {
        errorMessage = QStringLiteral("Web search response is not a JSON object.");
        return false;
    }

    const QJsonObject root = document.object();
    const QJsonValue statusValue = root.value(QStringLiteral("status"));
    const QJsonValue dataValue = root.value(QStringLiteral("data"));

    if (!statusValue.isString() || !dataValue.isObject())
    {
        errorMessage = QStringLiteral("Web search response envelope is invalid.");
        return false;
    }

    const QString status = statusValue.toString().trimmed().toLower();

    if ((status != QStringLiteral("ok")) && (status != QStringLiteral("success")))
    {
        errorMessage = QStringLiteral("Web search service returned an error status.");
        return false;
    }

    const QJsonObject dataObject = dataValue.toObject();
    const QJsonValue resultsValue = dataObject.value(QStringLiteral("results"));

    if (!resultsValue.isArray())
    {
        errorMessage = QStringLiteral("Web search response results are invalid.");
        return false;
    }

    QSet<QString> seenUrls;
    const QJsonArray resultArray = resultsValue.toArray();

    for (const QJsonValue &resultValue : resultArray)
    {
        if (results.size() >= m_config.maxResults)
        {
            break;
        }

        if (!resultValue.isObject())
        {
            continue;
        }

        const QJsonObject resultObject = resultValue.toObject();
        QString normalizedUrl;

        if (!NormalizeResultUrl(resultObject.value(QStringLiteral("url")).toString(),
                                normalizedUrl)
            || seenUrls.contains(normalizedUrl))
        {
            continue;
        }

        const QString title = resultObject.value(QStringLiteral("title")).toString().trimmed();
        const QString description = resultObject.value(QStringLiteral("description"))
                                        .toString().simplified()
                                        .left(m_config.maxDescriptionChars);

        if (title.isEmpty() && description.isEmpty())
        {
            continue;
        }

        _tagWebSearchResult result;
        result.title = title;
        result.url = normalizedUrl;
        result.description = description;
        result.source = resultObject.value(QStringLiteral("source")).toString().trimmed();
        result.engine = resultObject.value(QStringLiteral("engine")).toString().trimmed();
        results.append(result);
        seenUrls.insert(normalizedUrl);
    }

    const QJsonValue partialFailuresValue = dataObject.value(QStringLiteral("partialFailures"));

    if (!partialFailuresValue.isUndefined())
    {
        if (!partialFailuresValue.isArray())
        {
            errorMessage = QStringLiteral("Web search partial failures are invalid.");
            return false;
        }

        for (const QJsonValue &failureValue : partialFailuresValue.toArray())
        {
            if (failureValue.isString())
            {
                const QString failure = failureValue.toString().simplified();

                if (!failure.isEmpty())
                {
                    partialFailures.append(failure.left(500));
                }
            }
        }
    }

    return true;
}

bool WebSearchClient::NormalizeResultUrl(const QString &value, QString &normalizedUrl)
{
    normalizedUrl.clear();
    const QString cleanedValue = value.simplified();
    const QUrl url(cleanedValue);

    if (!url.isValid()
        || ((url.scheme() != QStringLiteral("http"))
            && (url.scheme() != QStringLiteral("https")))
        || url.host().isEmpty())
    {
        return false;
    }

    normalizedUrl = url.toString(QUrl::FullyEncoded);
    return !normalizedUrl.isEmpty();
}

} // namespace vpet
