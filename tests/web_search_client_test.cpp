#include "vpet/web/web_search_client.h"
#include "vpet/web/web_search_tool.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

class MockSearchServer : public QObject
{
    Q_OBJECT

public:
    explicit MockSearchServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
        , m_response()
        , m_responseDelayMs(0)
        , m_requestData()
    {
        connect(m_server, &QTcpServer::newConnection,
                this, &MockSearchServer::OnNewConnection);
    }

    bool Start()
    {
        return m_server->listen(QHostAddress::LocalHost, 0);
    }

    void Stop()
    {
        m_server->close();
    }

    quint16 Port() const
    {
        return m_server->serverPort();
    }

    void SetResponse(const QByteArray &response, int delayMs = 0)
    {
        m_response = response;
        m_responseDelayMs = delayMs;
    }

    QByteArray RequestData() const
    {
        return m_requestData;
    }

private slots:
    void OnNewConnection()
    {
        while (m_server->hasPendingConnections())
        {
            QTcpSocket *socket = m_server->nextPendingConnection();

            if (socket == nullptr)
            {
                continue;
            }

            connect(socket, &QTcpSocket::readyRead, this, [this, socket]()
            {
                m_requestData.append(socket->readAll());

                if (!m_requestData.contains(QByteArrayLiteral("\r\n\r\n")))
                {
                    return;
                }

                const QByteArray response =
                    QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n")
                    + QByteArrayLiteral("Content-Length: ")
                    + QByteArray::number(m_response.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                    + m_response;

                QTimer::singleShot(m_responseDelayMs, socket, [socket, response]()
                {
                    if (socket->state() == QAbstractSocket::ConnectedState)
                    {
                        socket->write(response);
                        socket->disconnectFromHost();
                    }
                });
            });
        }
    }

private:
    QTcpServer *m_server;
    QByteArray m_response;
    int m_responseDelayMs;
    QByteArray m_requestData;
};

QByteArray SuccessResponse()
{
    return QByteArray("{\"status\":\"ok\",\"data\":{\"results\":[{\"title\":\"Qt\",\"url\":\"https://qt.io/docs\",\"description\":\"  Qt documentation  \",\"source\":\"qt.io\",\"engine\":\"duckduckgo\"},{\"title\":\"Bad scheme\",\"url\":\"file:///secret\",\"description\":\"ignore\"},{\"title\":\"Duplicate\",\"url\":\"https://qt.io/docs\",\"description\":\"ignore\"}],\"partialFailures\":[\"startpage timeout\"]}}");
}

QByteArray RealBingEnvelopeResponse()
{
    return QByteArray(R"({"status":"ok","data":{"query":"Qt 6 network","engines":["bing"],"totalResults":3,"results":[{"title":"Qt | 软件开发全周期的各阶段工具","url":"https://www.qt.io/zh-cn/","description":" 1 天前 · Qt Group支持英伟达CUDA安全与编码指南 ","source":"qt.iohttps://www.qt.io","engine":"bing"},{"title":"Qt | Tools for Each Stage of Software Development Lifecycle","url":"https://www.qt.io/","description":" 11 小时之前 · All the essential Qt tools for all stages of Software Development Lifecycle: planning, design, development, testing, and deployment. ","source":"qt.iohttps://www.qt.io","engine":"bing"},{"title":"Qt 文档 | 首页 - Qt 框架","url":"https://doc.qt.ac.cn/","description":"2026年3月7日 · 获取并安装 Qt 从源码构建 Qt Qt Debian 软件包（技术预览）","source":"qt.ac.cnhttps://doc.qt.ac.cn","engine":"bing"}],"partialFailures":[]},"error":null,"hint":null})");
}

vpet::_tagWebSearchConfig MakeConfig(const MockSearchServer &server)
{
    vpet::_tagWebSearchConfig config;
    config.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.Port()));
    config.authorizationToken = QStringLiteral("test-token");
    config.totalTimeoutMs = 300;
    config.minSearchIntervalMs = 0;
    config.maxThrottleWaitMs = 0;
    config.maxResponseBytes = 1024 * 1024;
    config.maxResults = 5;
    config.maxDescriptionChars = 500;
    return config;
}

} // namespace

class WebSearchClientTest : public QObject
{
    Q_OBJECT

private slots:
    void ParsesValidResponseAndBuildsRequest();
    void ParsesRealBingEnvelope();
    void RejectsMalformedEnvelope();
    void RejectsBusyRequest();
    void CancelsDelayedRequest();
    void TimesOutDelayedRequest();
    void RejectsOversizedResponse();
    void ToolReturnsStructuredResponse();
    void ToolRejectsEmptyQuery();
    void LoadsConfigAndReadsTokenFromEnvironment();

private:
    void initTestCase();
};

void WebSearchClientTest::initTestCase()
{
    qRegisterMetaType<QVector<vpet::_tagWebSearchResult>>();
    qRegisterMetaType<vpet::_tagWebSearchToolResponse>();
}

void WebSearchClientTest::LoadsConfigAndReadsTokenFromEnvironment()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(SuccessResponse());
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString configPath = temporaryDirectory.filePath(QStringLiteral("web_search_config.json"));
    QFile configFile(configPath);
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Text));
    const QJsonObject configObject =
    {
        {QStringLiteral("base_url"),
         QStringLiteral("http://127.0.0.1:%1").arg(server.Port())},
        {QStringLiteral("authorization_token_env"), QStringLiteral("VPET_TEST_WEB_TOKEN")},
        {QStringLiteral("request_timeout_ms"), 1000},
        {QStringLiteral("min_search_interval_ms"), 0},
        {QStringLiteral("max_throttle_wait_ms"), 0},
        {QStringLiteral("max_response_bytes"), 1048576},
        {QStringLiteral("max_results"), 5},
        {QStringLiteral("max_description_chars"), 500}
    };
    const QByteArray configData = QJsonDocument(configObject).toJson(QJsonDocument::Compact);
    QCOMPARE(configFile.write(configData), configData.size());
    configFile.close();
    QVERIFY(qputenv("VPET_TEST_WEB_TOKEN", QByteArrayLiteral("environment-token")));

    vpet::WebSearchClient client;
    QString errorMessage;
    QVERIFY(client.LoadConfig(configPath, errorMessage));
    QSignalSpy completedSpy(&client, &vpet::WebSearchClient::SearchCompleted);
    QVERIFY(client.Search(QStringLiteral("config test"),
                          QStringList({QStringLiteral("bing")})) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 1000);
    QVERIFY(server.RequestData().toLower().contains(
        QByteArrayLiteral("authorization: bearer environment-token")));
    qunsetenv("VPET_TEST_WEB_TOKEN");
}

void WebSearchClientTest::ParsesValidResponseAndBuildsRequest()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(SuccessResponse());

    vpet::WebSearchClient client;
    QVERIFY(client.SetConfig(MakeConfig(server)));

    QSignalSpy completedSpy(&client, &vpet::WebSearchClient::SearchCompleted);
    QSignalSpy failedSpy(&client, &vpet::WebSearchClient::SearchFailed);
    const int requestId = client.Search(QStringLiteral("qt network"),
                                        QStringList{QStringLiteral("duckduckgo")});

    QVERIFY(requestId > 0);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 1000);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(completedSpy.at(0).at(0).toInt(), requestId);

    const QVector<vpet::_tagWebSearchResult> results =
        qvariant_cast<QVector<vpet::_tagWebSearchResult>>(completedSpy.at(0).at(2));
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.at(0).title, QStringLiteral("Qt"));
    QCOMPARE(results.at(0).description, QStringLiteral("Qt documentation"));
    QCOMPARE(completedSpy.at(0).at(3).toStringList(), QStringList{QStringLiteral("startpage timeout")});

    const QByteArray requestData = server.RequestData();
    QVERIFY(requestData.startsWith(QByteArrayLiteral("POST /search HTTP/1.1")));
    QVERIFY(requestData.toLower().contains(QByteArrayLiteral("authorization: bearer test-token")));
    QVERIFY(requestData.contains(QByteArrayLiteral("\"query\":\"qt network\"")));
    QVERIFY(requestData.contains(QByteArrayLiteral("\"engines\":[\"duckduckgo\"]")));
}

void WebSearchClientTest::ParsesRealBingEnvelope()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(RealBingEnvelopeResponse());

    vpet::WebSearchClient client;
    QVERIFY(client.SetConfig(MakeConfig(server)));

    QSignalSpy completedSpy(&client, &vpet::WebSearchClient::SearchCompleted);
    QSignalSpy failedSpy(&client, &vpet::WebSearchClient::SearchFailed);
    QVERIFY(client.Search(QStringLiteral("Qt 6 network"),
                          QStringList{QStringLiteral("bing")}) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 1000);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(completedSpy.at(0).at(1).toString(), QStringLiteral("Qt 6 network"));

    const QVector<vpet::_tagWebSearchResult> results =
        qvariant_cast<QVector<vpet::_tagWebSearchResult>>(completedSpy.at(0).at(2));
    QCOMPARE(results.size(), 3);
    QCOMPARE(results.at(0).title, QStringLiteral("Qt | 软件开发全周期的各阶段工具"));
    QCOMPARE(results.at(0).url, QStringLiteral("https://www.qt.io/zh-cn/"));
    QVERIFY(results.at(0).description.startsWith(QStringLiteral("1 天前")));
    QCOMPARE(results.at(0).source, QStringLiteral("qt.iohttps://www.qt.io"));
    QCOMPARE(results.at(0).engine, QStringLiteral("bing"));
    QCOMPARE(results.at(2).url, QStringLiteral("https://doc.qt.ac.cn/"));
    QCOMPARE(completedSpy.at(0).at(3).toStringList().size(), 0);
}

void WebSearchClientTest::RejectsMalformedEnvelope()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(QByteArrayLiteral("{\"status\":\"ok\",\"data\":{}}"));

    vpet::WebSearchClient client;
    QVERIFY(client.SetConfig(MakeConfig(server)));
    QSignalSpy failedSpy(&client, &vpet::WebSearchClient::SearchFailed);

    QVERIFY(client.Search(QStringLiteral("query"), QStringList()) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 1000);
    QCOMPARE(failedSpy.at(0).at(2).toInt(), 200);
    QVERIFY(failedSpy.at(0).at(1).toString().contains(QStringLiteral("results")));
}

void WebSearchClientTest::RejectsBusyRequest()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(SuccessResponse(), 100);

    vpet::WebSearchClient client;
    QVERIFY(client.SetConfig(MakeConfig(server)));
    QSignalSpy failedSpy(&client, &vpet::WebSearchClient::SearchFailed);

    QVERIFY(client.Search(QStringLiteral("first"), QStringList()) > 0);
    QCOMPARE(client.Search(QStringLiteral("second"), QStringList()), -1);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() >= 1, 1000);
    QVERIFY(failedSpy.at(0).at(1).toString().contains(QStringLiteral("busy")));
}

void WebSearchClientTest::CancelsDelayedRequest()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(SuccessResponse(), 200);

    vpet::WebSearchClient client;
    QVERIFY(client.SetConfig(MakeConfig(server)));
    QSignalSpy failedSpy(&client, &vpet::WebSearchClient::SearchFailed);

    QVERIFY(client.Search(QStringLiteral("cancel"), QStringList()) > 0);
    QVERIFY(client.Cancel());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 1000);
    QVERIFY(failedSpy.at(0).at(1).toString().contains(QStringLiteral("cancelled")));
}

void WebSearchClientTest::TimesOutDelayedRequest()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(SuccessResponse(), 1000);

    vpet::WebSearchClient client;
    QVERIFY(client.SetConfig(MakeConfig(server)));
    QSignalSpy failedSpy(&client, &vpet::WebSearchClient::SearchFailed);

    QVERIFY(client.Search(QStringLiteral("timeout"), QStringList()) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 1000);
    QVERIFY(failedSpy.at(0).at(1).toString().contains(QStringLiteral("timed out")));
}

void WebSearchClientTest::RejectsOversizedResponse()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(QByteArray(2048, 'x'));

    vpet::_tagWebSearchConfig config = MakeConfig(server);
    config.maxResponseBytes = 1024;

    vpet::WebSearchClient client;
    QVERIFY(client.SetConfig(config));
    QSignalSpy failedSpy(&client, &vpet::WebSearchClient::SearchFailed);

    QVERIFY(client.Search(QStringLiteral("large"), QStringList()) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 1000);
    QVERIFY(failedSpy.at(0).at(1).toString().contains(QStringLiteral("size limit")));
}

void WebSearchClientTest::ToolReturnsStructuredResponse()
{
    MockSearchServer server;
    QVERIFY(server.Start());
    server.SetResponse(SuccessResponse());

    vpet::WebSearchTool tool;
    QVERIFY(tool.SetClientConfig(MakeConfig(server)));

    QSignalSpy completedSpy(&tool, &vpet::WebSearchTool::Completed);
    QSignalSpy failedSpy(&tool, &vpet::WebSearchTool::Failed);
    vpet::_tagWebSearchToolRequest request;
    request.query = QStringLiteral("  qt network  ");
    request.engines = QStringList{QStringLiteral("DuckDuckGo"),
                                  QStringLiteral("duckduckgo"),
                                  QStringLiteral(" ")};

    QVERIFY(tool.Execute(request) > 0);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 1000);
    QCOMPARE(failedSpy.count(), 0);

    const vpet::_tagWebSearchToolResponse response =
        qvariant_cast<vpet::_tagWebSearchToolResponse>(completedSpy.at(0).at(0));
    QCOMPARE(response.query, QStringLiteral("qt network"));
    QCOMPARE(response.results.size(), 1);
    QCOMPARE(response.results.at(0).url, QStringLiteral("https://qt.io/docs"));
    QCOMPARE(server.RequestData().count(QByteArrayLiteral("duckduckgo")), 1);
}

void WebSearchClientTest::ToolRejectsEmptyQuery()
{
    vpet::WebSearchTool tool;
    QSignalSpy failedSpy(&tool, &vpet::WebSearchTool::Failed);
    vpet::_tagWebSearchToolRequest request;
    request.query = QStringLiteral(" \t ");

    QCOMPARE(tool.Execute(request), -1);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.at(0).at(0).toInt(), -1);
    QVERIFY(failedSpy.at(0).at(2).toString().contains(QStringLiteral("query is empty")));
}

QTEST_MAIN(WebSearchClientTest)

#include "web_search_client_test.moc"
