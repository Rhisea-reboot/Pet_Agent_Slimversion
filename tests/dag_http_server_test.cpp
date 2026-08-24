#include "vpet/dageditor/dag_editor_server.h"
#include "vpet/dageditor/dag_document.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace vpet;

namespace
{

const char *kValidDagJson = R"({
    "nodes": [
        {"id": "a", "type": "user.input", "config": {"trigger": "user"}},
        {"id": "b", "type": "llm.chat", "config": {}}
    ],
    "edges": [{"from": "a", "to": "b"}]
})";

const QByteArray kValidDagBytes(kValidDagJson);

bool WriteConfig(const QString &path, const QByteArray &content)
{
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }

    return file.write(content) == content.size();
}

/**
 * @brief 等待一个 QNetworkReply 完成（带超时保护）
 */
QNetworkReply *WaitForReply(QNetworkAccessManager &manager, QNetworkReply *reply)
{
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(5000);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    loop.exec();

    return reply;
}

} // namespace

class DagHttpServerTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void MetaRequiresToken();
    void MetaAcceptsTokenHeaderAndQuery();
    void HostHeaderWhitelistRejectsForeignHosts();
    void GetGraphReturnsBaseline();
    void PutGraphSavesReloadsConflictsAndValidates();
    void ValidateEndpointReturnsIssues();
    void RuntimeStatusIsReadOnly();
    void SseDeliversSavedEvent();
    void OversizedBodyIsRejected();
    void FrontendAssetsAreAllServed();

private:
    QString BaseUrl() const;
    QNetworkReply *Get(const QString &path,
                       const QByteArray &token = QByteArray(),
                       const QByteArray &hostOverride = QByteArray());
    QNetworkReply *Send(const QString &method,
                        const QString &path,
                        const QByteArray &body,
                        const QByteArray &token = QByteArray(),
                        const QByteArray &querySuffix = QString().toUtf8());

    QTemporaryDir m_temporaryDirectory;
    DagEditorServer m_server;
    QNetworkAccessManager m_networkManager;
    QString m_configPath;
};

void DagHttpServerTest::init()
{
    m_configPath = m_temporaryDirectory.filePath(QStringLiteral("http_test_dag.json"));
    QVERIFY(WriteConfig(m_configPath, kValidDagBytes));

    QString errorMessage;
    QVERIFY2(m_server.Start(m_configPath, nullptr, nullptr, errorMessage),
             qPrintable(errorMessage));
}

void DagHttpServerTest::cleanup()
{
    m_server.Stop();
}

QString DagHttpServerTest::BaseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(
        m_server.EditorUrl().port());
}

QNetworkReply *DagHttpServerTest::Get(const QString &path,
                                      const QByteArray &token,
                                      const QByteArray &hostOverride)
{
    QNetworkRequest request{QUrl(BaseUrl() + path)};

    if (!token.isEmpty())
    {
        request.setRawHeader(QByteArrayLiteral("X-DAG-Token"), token);
    }

    if (!hostOverride.isEmpty())
    {
        request.setRawHeader(QByteArrayLiteral("Host"), hostOverride);
    }

    return WaitForReply(m_networkManager, m_networkManager.get(request));
}

QNetworkReply *DagHttpServerTest::Send(const QString &method,
                                       const QString &path,
                                       const QByteArray &body,
                                       const QByteArray &token,
                                       const QByteArray &querySuffix)
{
    QUrl url(BaseUrl() + path);

    if (!querySuffix.isEmpty())
    {
        QString queryString = QString::fromUtf8(querySuffix);

        if (queryString.startsWith(QLatin1Char('?')))
        {
            queryString.remove(0, 1); // QUrl::setQuery 不接受前导问号
        }

        url.setQuery(queryString);
    }

    QNetworkRequest requestObject{url};
    requestObject.setRawHeader(QByteArrayLiteral("Content-Type"),
                               QByteArrayLiteral("application/json"));

    if (!token.isEmpty())
    {
        requestObject.setRawHeader(QByteArrayLiteral("X-DAG-Token"), token);
    }

    const QByteArray verb = method.toUtf8();
    return WaitForReply(m_networkManager,
                        m_networkManager.sendCustomRequest(requestObject, verb, body));
}

/**
 * 无 token 的 API 请求一律 401。
 */
void DagHttpServerTest::MetaRequiresToken()
{
    QNetworkReply *reply = Get(QStringLiteral("/api/meta"));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
    reply->deleteLater();
}

/**
 * token 头或 ?token= 查询参数均可通过鉴权。
 */
void DagHttpServerTest::MetaAcceptsTokenHeaderAndQuery()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();
    QVERIFY(!token.isEmpty());

    QNetworkReply *headerReply = Get(QStringLiteral("/api/meta"), token);
    QCOMPARE(headerReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);

    const QJsonObject meta = QJsonDocument::fromJson(headerReply->readAll()).object();
    headerReply->deleteLater();
    QCOMPARE(meta.value(QStringLiteral("config_path")).toString(),
             QFileInfo(m_configPath).absoluteFilePath());

    QNetworkReply *queryReply = Get(
        QStringLiteral("/api/meta?token=") + token);
    QCOMPARE(queryReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    queryReply->deleteLater();
}

/**
 * Host 头不在白名单（127.0.0.1:port / localhost:port）时拒绝。
 */
void DagHttpServerTest::HostHeaderWhitelistRejectsForeignHosts()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();

    // 注意：回环请求的 Host 无法真正伪造为外部域名（QNAM 会覆盖），此处用
    // 显式错误端口验证白名单逻辑。
    QNetworkReply *reply =
        Get(QStringLiteral("/api/meta"), token, QByteArrayLiteral("127.0.0.1:1"));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
    reply->deleteLater();
}

void DagHttpServerTest::GetGraphReturnsBaseline()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();

    QNetworkReply *reply = Get(QStringLiteral("/api/graph"), token);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);

    const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();

    QCOMPARE(response.value(QStringLiteral("revision")).toInteger(), 1);
    const QJsonObject graph = response.value(QStringLiteral("graph")).toObject();
    QCOMPARE(graph.value(QStringLiteral("nodes")).toArray().size(), 2);
    QVERIFY(graph.contains(QStringLiteral("editor")));
}

/**
 * PUT 全链路：保存成功 → revision 递增；旧 baseRevision → 409；坏图 → 400。
 */
void DagHttpServerTest::PutGraphSavesReloadsConflictsAndValidates()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();

    // 取基线
    QNetworkReply *getReply = Get(QStringLiteral("/api/graph"), token);
    const QJsonObject baseline =
        QJsonDocument::fromJson(getReply->readAll()).object();
    getReply->deleteLater();

    // 1) 合法保存：新增节点。
    QJsonObject document = baseline.value(QStringLiteral("graph")).toObject();
    QJsonArray nodes = document.value(QStringLiteral("nodes")).toArray();
    nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("c")},
                             {QStringLiteral("type"), QStringLiteral("output.format")},
                             {QStringLiteral("config"), QJsonObject()}});
    document[QStringLiteral("nodes")] = nodes;

    QJsonArray edges = document.value(QStringLiteral("edges")).toArray();
    edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("b")},
                             {QStringLiteral("to"), QStringLiteral("c")}});
    document[QStringLiteral("edges")] = edges;

    const quint64 baseRevision =
        baseline.value(QStringLiteral("revision")).toInteger();
    const QByteArray saveQuery =
        QStringLiteral("?baseRevision=%1").arg(baseRevision).toUtf8();

    QNetworkReply *saveReply = Send(QStringLiteral("PUT"),
                                    QStringLiteral("/api/graph"),
                                    QJsonDocument(document).toJson(QJsonDocument::Compact),
                                    token,
                                    saveQuery);
    const int saveStatus =
        saveReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (saveStatus != 200)
    {
        qWarning() << "PUT failed body:"
                   << saveReply->readAll();
    }

    QCOMPARE(saveStatus, 200);

    const QJsonObject savedResponse =
        QJsonDocument::fromJson(saveReply->readAll()).object();
    saveReply->deleteLater();
    QCOMPARE(savedResponse.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(savedResponse.value(QStringLiteral("revision")).toInteger(),
             static_cast<qint64>(baseRevision + 1));
    QCOMPARE(savedResponse.value(QStringLiteral("reload"))
                  .toObject()
                  .value(QStringLiteral("reason"))
                  .toString(),
             QStringLiteral("runtime_unavailable")); // 测试未注入运行时

    // 2) 用旧 baseRevision 再保存 → 409 + 最新文档。
    QNetworkReply *conflictReply = Send(QStringLiteral("PUT"),
                                        QStringLiteral("/api/graph"),
                                        QJsonDocument(document).toJson(QJsonDocument::Compact),
                                        token,
                                        saveQuery);
    QCOMPARE(conflictReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 409);

    const QJsonObject conflictResponse =
        QJsonDocument::fromJson(conflictReply->readAll()).object();
    conflictReply->deleteLater();
    QCOMPARE(conflictResponse.value(QStringLiteral("revision")).toInteger(),
             static_cast<qint64>(baseRevision + 1));
    QVERIFY(conflictResponse.contains(QStringLiteral("graph")));

    // 3) 带环文档 → 400 + issues。
    QJsonObject cyclic = conflictResponse.value(QStringLiteral("graph")).toObject();
    QJsonArray cyclicEdges = cyclic.value(QStringLiteral("edges")).toArray();
    cyclicEdges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("b")},
                                   {QStringLiteral("to"), QStringLiteral("a")}});
    cyclic[QStringLiteral("edges")] = cyclicEdges;

    QNetworkReply *invalidReply = Send(QStringLiteral("PUT"),
                                       QStringLiteral("/api/graph"),
                                       QJsonDocument(cyclic).toJson(QJsonDocument::Compact),
                                       token,
                                       QStringLiteral("?baseRevision=%1")
                                           .arg(baseRevision + 1).toUtf8());
    QCOMPARE(invalidReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 400);

    const QJsonObject invalidResponse =
        QJsonDocument::fromJson(invalidReply->readAll()).object();
    invalidReply->deleteLater();
    QCOMPARE(invalidResponse.value(QStringLiteral("has_errors")).toBool(), true);
}

void DagHttpServerTest::ValidateEndpointReturnsIssues()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();

    const QByteArray badBody = QByteArrayLiteral(
        "{\"nodes\":[{\"id\":\"a\",\"type\":\"not.a.type\",\"config\":{}}],\"edges\":[]}");

    QNetworkReply *reply = Send(QStringLiteral("POST"),
                                QStringLiteral("/api/graph/validate"),
                                badBody,
                                token);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);

    const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    QCOMPARE(response.value(QStringLiteral("has_errors")).toBool(), true);

    const QJsonArray issues = response.value(QStringLiteral("issues")).toArray();
    QVERIFY(!issues.isEmpty());
    QCOMPARE(issues.at(0).toObject().value(QStringLiteral("code")).toString(),
             QStringLiteral("unknown_type"));
}

void DagHttpServerTest::RuntimeStatusIsReadOnly()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();

    QNetworkReply *reply = Get(QStringLiteral("/api/runtime/status"), token);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);

    const QJsonObject status = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    QCOMPARE(status.value(QStringLiteral("busy")).toBool(), false);
    QCOMPARE(status.value(QStringLiteral("runtime_available")).toBool(), false); // 未注入运行时
}

void DagHttpServerTest::SseDeliversSavedEvent()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();

    // 注意：同线程内服务端事件依赖 Qt 事件循环；阻塞式 waitForXxx 会饿死
    // 服务端套接字通知，因此这里统一用 QEventLoop 等待。
    QTcpSocket stream;
    QByteArray receivedBytes;

    QObject::connect(&stream, &QTcpSocket::readyRead, &stream,
                     [&receivedBytes, &stream]()
                     { receivedBytes += stream.readAll(); });

    QEventLoop connectLoop;
    QTimer connectTimeout;
    connectTimeout.setSingleShot(true);
    QObject::connect(&stream, &QTcpSocket::connected,
                     &connectLoop, &QEventLoop::quit);
    QObject::connect(&connectTimeout, &QTimer::timeout,
                     &connectLoop, &QEventLoop::quit);
    connectTimeout.start(3000);
    stream.connectToHost(QHostAddress::LocalHost,
                         static_cast<quint16>(m_server.EditorUrl().port()));
    connectLoop.exec();
    QVERIFY2(stream.state() == QAbstractSocket::ConnectedState,
             "SSE 连接超时");

    stream.write(QStringLiteral("GET /api/events?token=%1 HTTP/1.1\r\n"
                                "Host: 127.0.0.1:%2\r\n"
                                "Accept: text/event-stream\r\n\r\n")
                     .arg(QString::fromUtf8(token))
                     .arg(m_server.EditorUrl().port())
                     .toUtf8());

    // 等待初始响应头（": connected" 注释帧随后到达）。
    const QByteArray initialNeed = QByteArrayLiteral(": connected");

    QElapsedTimer waitTimer;
    waitTimer.start();

    while (!receivedBytes.contains(initialNeed) && waitTimer.elapsed() < 3000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }

    QVERIFY(receivedBytes.startsWith(QByteArrayLiteral("HTTP/1.1 200 OK")));
    QVERIFY(receivedBytes.contains(QByteArrayLiteral("Content-Type: text/event-stream")));

    // 触发一次保存以产生 graph.saved 广播。
    QNetworkReply *getReply = Get(QStringLiteral("/api/graph"), token);
    const QJsonObject baseline = QJsonDocument::fromJson(getReply->readAll()).object();
    getReply->deleteLater();

    const QByteArray saveQuery =
        QStringLiteral("?baseRevision=%1")
            .arg(baseline.value(QStringLiteral("revision")).toInteger())
            .toUtf8();

    QNetworkReply *saveReply = Send(QStringLiteral("PUT"),
                                    QStringLiteral("/api/graph"),
                                    QJsonDocument(baseline.value(QStringLiteral("graph"))
                                                      .toObject())
                                        .toJson(QJsonDocument::Compact),
                                    token,
                                    saveQuery);
    QCOMPARE(saveReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    saveReply->deleteLater();

    // 轮询事件帧（最多约 3 秒）。
    waitTimer.restart();

    while (!receivedBytes.contains("event:") && waitTimer.elapsed() < 3000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }

    QVERIFY(receivedBytes.contains(QByteArrayLiteral("event: graph.saved")));
    QVERIFY(receivedBytes.contains(QByteArrayLiteral("data: {\"revision\":")));
    stream.disconnectFromHost();
}

/**
 * 超过 1 MB 的请求体必须被拒（413 或连接关闭）。
 */
void DagHttpServerTest::OversizedBodyIsRejected()
{
    const QByteArray token = m_server.EditorUrl().query(QUrl::FullyDecoded)
                                 .mid(QStringLiteral("token=").size()).toUtf8();

    QByteArray hugeBody = QByteArrayLiteral("{\"nodes\":[");
    hugeBody.append(QByteArray(1024 * 1024 + 128, 'x'));

    QNetworkReply *reply = Send(QStringLiteral("POST"),
                                QStringLiteral("/api/graph/validate"),
                                hugeBody,
                                token);

    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QVERIFY(statusCode == 413 || statusCode == 0); // 0 = 连接被服务器关闭
    reply->deleteLater();
}

/**
 * index.html 引用的每个前端资源都必须能以 200 + 非空内容伺服。
 * （回归防护：曾因静态路由表漏掉 /api.js 导致整页功能静默失效。）
 */
void DagHttpServerTest::FrontendAssetsAreAllServed()
{
    // 静态资源豁免 token——这里故意不带 token，与浏览器首屏加载路径一致。
    const QStringList assets = {
        QStringLiteral("/"),
        QStringLiteral("/index.html"),
        QStringLiteral("/api.js"),
        QStringLiteral("/app.js"),
        QStringLiteral("/style.css"),
        QStringLiteral("/graph_adapter.js"),
        QStringLiteral("/widgets.js"),
        QStringLiteral("/panels/library.js"),
        QStringLiteral("/panels/property.js"),
        QStringLiteral("/vendor/litegraph.min.js"),
        QStringLiteral("/vendor/litegraph.css"),
    };

    for (const QString &asset : assets)
    {
        QNetworkReply *reply = Get(asset);
        const int statusCode =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        QVERIFY2(statusCode == 200,
                 qPrintable(QStringLiteral("资源 %1 返回 %2").arg(asset).arg(statusCode)));
        QVERIFY2(!body.isEmpty(),
                 qPrintable(QStringLiteral("资源 %1 响应体为空").arg(asset)));
    }

    // index.html 中引用的脚本必须都能在路由表中命中（防再次漏配）。
    QNetworkReply *indexReply = Get(QStringLiteral("/"));
    const QByteArray indexHtml = indexReply->readAll();
    indexReply->deleteLater();

    const QRegularExpression srcPattern(
        QStringLiteral("(?:src|href)=\"(/[^\"]+)\""));
    QRegularExpressionMatchIterator iterator = srcPattern.globalMatch(
        QString::fromUtf8(indexHtml));

    while (iterator.hasNext())
    {
        const QString referenced = iterator.next().captured(1);

        if (referenced.startsWith(QStringLiteral("http")))
        {
            continue; // 外部链接不检查
        }

        QNetworkReply *reply = Get(referenced);
        const int statusCode =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        QVERIFY2(statusCode == 200,
                 qPrintable(QStringLiteral("index.html 引用的 %1 返回 %2")
                                .arg(referenced).arg(statusCode)));
    }
}

QTEST_MAIN(DagHttpServerTest)

#include "dag_http_server_test.moc"
