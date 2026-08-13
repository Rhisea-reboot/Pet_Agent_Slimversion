#include "vpet/web/web_search_tool.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QUrl>
#include <QtTest>

namespace
{

constexpr int DAEMON_PROBE_TIMEOUT_MS = 5000;
constexpr int REAL_SEARCH_TIMEOUT_MS = 20000;

QUrl DaemonBaseUrl()
{
    const QByteArray overrideUrl = qgetenv("VPET_WEB_SEARCH_DAEMON_URL");

    if (!overrideUrl.isEmpty())
    {
        return QUrl(QString::fromUtf8(overrideUrl));
    }

    return QUrl(QStringLiteral("http://127.0.0.1:3210"));
}

struct ProbeResponse
{
    bool ok = false;
    int statusCode = 0;
    QByteArray body;
    QList<QByteArray> headerNames;
};

ProbeResponse ProbeGet(const QUrl &url, int timeoutMs = DAEMON_PROBE_TIMEOUT_MS)
{
    ProbeResponse result;
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("Origin"),
                         QByteArrayLiteral("https://attacker.example.com"));
    QNetworkReply *reply = manager.get(request);
    QSignalSpy finishedSpy(reply, &QNetworkReply::finished);

    if (!finishedSpy.wait(timeoutMs))
    {
        reply->abort();
        reply->deleteLater();
        return result;
    }

    result.ok = true;
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();

    for (const QByteArray &name : reply->rawHeaderList())
    {
        result.headerNames.append(name.toLower());
    }

    reply->deleteLater();
    return result;
}

ProbeResponse ProbeEmptyQueryPost(const QUrl &baseUrl)
{
    ProbeResponse result;
    QNetworkAccessManager manager;
    QNetworkRequest request(baseUrl.toString() + QStringLiteral("/search"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray body = QByteArrayLiteral("{\"query\":\"\",\"engines\":[\"bing\"]}");
    QNetworkReply *reply = manager.post(request, body);
    QSignalSpy finishedSpy(reply, &QNetworkReply::finished);

    if (!finishedSpy.wait(DAEMON_PROBE_TIMEOUT_MS))
    {
        reply->abort();
        reply->deleteLater();
        return result;
    }

    result.ok = true;
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

bool HealthIsUp()
{
    const ProbeResponse health = ProbeGet(DaemonBaseUrl().toString() + QStringLiteral("/health"));
    return health.ok && (health.statusCode == 200);
}

} // namespace

class WebSearchDaemonIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void HealthStatusAndProtocolMatrix();
    void RealSearchThroughClient();

private:
    QUrl m_daemonBaseUrl;
};

void WebSearchDaemonIntegrationTest::initTestCase()
{
    m_daemonBaseUrl = DaemonBaseUrl();
    qRegisterMetaType<QVector<vpet::_tagWebSearchResult>>();
    qRegisterMetaType<vpet::_tagWebSearchToolResponse>();
}

void WebSearchDaemonIntegrationTest::HealthStatusAndProtocolMatrix()
{
    if (!HealthIsUp())
    {
        QSKIP("open-webSearch daemon is not reachable; integration test skipped.");
    }

    const ProbeResponse health =
        ProbeGet(m_daemonBaseUrl.toString() + QStringLiteral("/health"));
    QCOMPARE(health.statusCode, 200);
    const QJsonObject healthObject = QJsonDocument::fromJson(health.body).object();
    QCOMPARE(healthObject.value(QStringLiteral("status")).toString(),
             QStringLiteral("ok"));
    QCOMPARE(healthObject.value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("daemon")).toString(),
             QStringLiteral("running"));
    QVERIFY(!health.headerNames.contains(QByteArrayLiteral("access-control-allow-origin")));

    const ProbeResponse status =
        ProbeGet(m_daemonBaseUrl.toString() + QStringLiteral("/status"));
    QCOMPARE(status.statusCode, 200);
    const QJsonObject statusObject = QJsonDocument::fromJson(status.body).object();
    QCOMPARE(statusObject.value(QStringLiteral("status")).toString(),
             QStringLiteral("ok"));
    const QJsonObject configSummary =
        statusObject.value(QStringLiteral("data")).toObject()
                    .value(QStringLiteral("configSummary")).toObject();
    QCOMPARE(configSummary.value(QStringLiteral("defaultSearchEngine")).toString(),
             QStringLiteral("bing"));
    QVERIFY(configSummary.value(QStringLiteral("allowedSearchEngines")).toArray().contains(
        QJsonValue(QStringLiteral("bing"))));
    QCOMPARE(configSummary.value(QStringLiteral("searchMode")).toString(),
             QStringLiteral("request"));
    QCOMPARE(configSummary.value(QStringLiteral("useProxy")).toBool(), false);
    QCOMPARE(configSummary.value(QStringLiteral("fetchWebAllowInsecureTls")).toBool(), false);

    const ProbeResponse mcp =
        ProbeGet(m_daemonBaseUrl.toString() + QStringLiteral("/mcp"));
    QCOMPARE(mcp.statusCode, 404);

    const ProbeResponse sse =
        ProbeGet(m_daemonBaseUrl.toString() + QStringLiteral("/sse"));
    QCOMPARE(sse.statusCode, 404);

    const ProbeResponse emptyQuery = ProbeEmptyQueryPost(m_daemonBaseUrl);
    QCOMPARE(emptyQuery.statusCode, 400);
}

void WebSearchDaemonIntegrationTest::RealSearchThroughClient()
{
    if (!HealthIsUp())
    {
        QSKIP("open-webSearch daemon is not reachable; integration test skipped.");
    }

    vpet::_tagWebSearchConfig config;
    config.baseUrl = m_daemonBaseUrl;
    config.totalTimeoutMs = 15000;
    config.minSearchIntervalMs = 0;
    config.maxThrottleWaitMs = 0;
    config.maxResponseBytes = 1024 * 1024;
    config.maxResults = 5;
    config.maxDescriptionChars = 500;

    vpet::WebSearchTool tool;
    QVERIFY(tool.SetClientConfig(config));

    QSignalSpy completedSpy(&tool, &vpet::WebSearchTool::Completed);
    QSignalSpy failedSpy(&tool, &vpet::WebSearchTool::Failed);

    vpet::_tagWebSearchToolRequest request;
    request.query = QStringLiteral("Qt 6 network");
    request.engines = QStringList{QStringLiteral("bing")};

    const int requestId = tool.Execute(request);
    QVERIFY(requestId > 0);

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() + failedSpy.count() > 0,
                             REAL_SEARCH_TIMEOUT_MS);

    if (failedSpy.count() > 0)
    {
        QFAIL(qPrintable(QStringLiteral("Real Bing search failed: %1")
                             .arg(failedSpy.at(0).at(2).toString())));
    }

    QCOMPARE(completedSpy.count(), 1);

    const vpet::_tagWebSearchToolResponse response =
        qvariant_cast<vpet::_tagWebSearchToolResponse>(completedSpy.at(0).at(0));
    QCOMPARE(response.requestId, requestId);
    QCOMPARE(response.query, QStringLiteral("Qt 6 network"));

    if (response.results.isEmpty())
    {
        qInfo("Real search returned a legitimate empty result set.");
        return;
    }

    QVERIFY(response.results.size() <= config.maxResults);

    for (const vpet::_tagWebSearchResult &result : response.results)
    {
        QVERIFY(!result.title.trimmed().isEmpty());
        QVERIFY(!result.url.isEmpty());
        QVERIFY(result.url.startsWith(QStringLiteral("http://"))
                || result.url.startsWith(QStringLiteral("https://")));
    }
}

QTEST_MAIN(WebSearchDaemonIntegrationTest)

#include "web_search_daemon_integration_test.moc"
