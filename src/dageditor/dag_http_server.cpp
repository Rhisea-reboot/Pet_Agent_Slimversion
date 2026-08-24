#include "vpet/dageditor/dag_http_request.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <utility>

namespace vpet
{

namespace
{

constexpr int MAX_REQUEST_BODY_BYTES = 1024 * 1024; // 请求体上限 1 MB
constexpr int MAX_REQUEST_LINE_BYTES = 16 * 1024;   // 请求行/头区上限
constexpr int HEARTBEAT_INTERVAL_MS = 15000;        // SSE 心跳间隔

/**
 * @brief 标准状态描述文本
 */
const char *StatusCodeText(int statusCode)
{
    switch (statusCode)
    {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

} // anonymous namespace

QString _tagDagHttpRequest::HeaderValue(const QString &name) const
{
    return headers.value(name.toLower());
}

QString _tagDagHttpRequest::QueryValue(const QString &key) const
{
    return query.value(key);
}

DagHttpServer::DagHttpServer(QObject *parent)
    : QObject(parent)
    , m_tcpServer(new QTcpServer(this))
    , m_requestHandler()
    , m_heartbeatTimer(new QTimer(this))
{
    connect(m_tcpServer, &QTcpServer::newConnection,
            this, &DagHttpServer::OnNewConnection);

    m_heartbeatTimer->setInterval(HEARTBEAT_INTERVAL_MS);
    connect(m_heartbeatTimer, &QTimer::timeout,
            this, &DagHttpServer::SendHeartbeats);
}

DagHttpServer::~DagHttpServer()
{
    Stop();
}

bool DagHttpServer::Start(quint16 port, QString &errorMessage)
{
    if (m_tcpServer->isListening())
    {
        errorMessage.clear();
        return true;
    }

    // 仅绑定回环地址：本机任意网页无法从外部网络访问该端口，
    // 回环绑定也不会触发 Windows 防火墙弹窗。
    if (!m_tcpServer->listen(QHostAddress::LocalHost, port))
    {
        errorMessage = QStringLiteral("HTTP server listen failed: %1")
                           .arg(m_tcpServer->errorString());
        return false;
    }

    errorMessage.clear();
    m_heartbeatTimer->start();
    return true;
}

void DagHttpServer::Stop()
{
    if (m_tcpServer->isListening())
    {
        m_tcpServer->close();
    }

    m_heartbeatTimer->stop();

    // 映射表只持有存活 socket（断开时同步移除），此处统一收尾。
    const QList<QTcpSocket *> sockets = m_receiveBuffers.keys();

    for (QTcpSocket *socket : sockets)
    {
        socket->abort();
        socket->deleteLater();
    }

    m_receiveBuffers.clear();

    const QList<QTcpSocket *> streams = m_eventStreams;

    for (QTcpSocket *socket : streams)
    {
        socket->abort();
        socket->deleteLater();
    }

    m_eventStreams.clear();
}

quint16 DagHttpServer::Port() const
{
    return m_tcpServer->isListening() ? m_tcpServer->serverPort() : quint16(0);
}

void DagHttpServer::SetRequestHandler(RequestHandler handler)
{
    m_requestHandler = std::move(handler);
}

void DagHttpServer::BroadcastEvent(const QString &channel,
                                   const QString &eventName,
                                   const QString &jsonPayload)
{
    CleanupClosedSockets();

    const QByteArray frame = QStringLiteral("event: %1\ndata: %2\n\n")
                                 .arg(eventName, jsonPayload)
                                 .toUtf8();

    for (QTcpSocket *socket : std::as_const(m_eventStreams))
    {
        if (socket->property("dsh_channel").toString() != channel)
        {
            continue;
        }

        socket->write(frame);
    }
}

void DagHttpServer::OnNewConnection()
{
    while (m_tcpServer->hasPendingConnections())
    {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();

        if (socket == nullptr)
        {
            continue;
        }

        connect(socket, &QTcpSocket::readyRead,
                this, &DagHttpServer::OnReadyRead);
        // 断开时同步移出映射表，再延迟销毁，保证表内指针始终有效。
        connect(socket, &QTcpSocket::disconnected,
                this, [this, socket]() { HandleSocketClosed(socket); });

        socket->setProperty("dsh_peer",
                            socket->peerAddress().toString());
        m_receiveBuffers.insert(socket, QByteArray());
    }
}

void DagHttpServer::HandleSocketClosed(QTcpSocket *socket)
{
    m_receiveBuffers.remove(socket);
    m_eventStreams.removeOne(socket);
    socket->deleteLater();
}

void DagHttpServer::OnReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());

    if (socket == nullptr || !m_receiveBuffers.contains(socket))
    {
        return;
    }

    QByteArray &buffer = m_receiveBuffers[socket];
    buffer.append(socket->readAll());

    // 头部区域超限直接断开，防止恶意超长头部耗尽内存。
    if (buffer.size() > MAX_REQUEST_BODY_BYTES + MAX_REQUEST_LINE_BYTES)
    {
        WriteErrorAndClose(socket, 413, QStringLiteral("Request too large."));
        return;
    }

    _tagDagHttpRequest request;

    if (!TryParseRequest(socket, request))
    {
        return; // 数据未到齐
    }

    // 只记录 token 是否存在，不输出 token 值本身，避免同机日志读取者复用。
    const bool hasQueryToken =
        !request.QueryValue(QStringLiteral("token")).isEmpty();
    qDebug() << "[DagHttp] dispatch" << request.method << request.path
             << "token-query-present=" << hasQueryToken;
    DispatchRequest(socket, request);
}

bool DagHttpServer::TryParseRequest(QTcpSocket *socket, _tagDagHttpRequest &request)
{
    QByteArray &buffer = m_receiveBuffers[socket];

    const int headerEnd = buffer.indexOf("\r\n\r\n");

    if (headerEnd < 0)
    {
        if (buffer.size() > MAX_REQUEST_LINE_BYTES)
        {
            WriteErrorAndClose(socket, 400, QStringLiteral("Malformed request head."));
        }

        return false;
    }

    const QByteArray headArea = buffer.left(headerEnd);
    const QList<QByteArray> lines = headArea.split('\n');

    if (lines.isEmpty())
    {
        WriteErrorAndClose(socket, 400, QStringLiteral("Empty request line."));
        return true;
    }

    QByteArray requestLine = lines.first().trimmed();
    const QList<QByteArray> requestParts = requestLine.split(' ');

    if (requestParts.size() < 2)
    {
        WriteErrorAndClose(socket, 400, QStringLiteral("Malformed request line."));
        return true;
    }

    request.method = QString::fromLatin1(requestParts.at(0)).toUpper();

    const QString rawTarget = QString::fromUtf8(requestParts.at(1));
    const int queryIndex = rawTarget.indexOf(QLatin1Char('?'));
    const QString rawPath = queryIndex >= 0 ? rawTarget.left(queryIndex) : rawTarget;
    request.path = QUrl::fromPercentEncoding(rawPath.toUtf8());

    if (queryIndex >= 0)
    {
        const QUrlQuery queryString(rawTarget.mid(queryIndex + 1));

        for (const auto &pair : queryString.queryItems(QUrl::FullyDecoded))
        {
            request.query.insert(pair.first, pair.second);
        }
    }

    bool ok = false;
    int contentLength = 0;

    for (int lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
    {
        const QByteArray headerLine = lines.at(lineIndex).trimmed();
        const int colonIndex = headerLine.indexOf(':');

        if (colonIndex <= 0)
        {
            continue;
        }

        const QString name =
            QString::fromLatin1(headerLine.left(colonIndex).trimmed()).toLower();
        const QString value =
            QString::fromUtf8(headerLine.mid(colonIndex + 1).trimmed());
        request.headers.insert(name, value);

        if (name == QStringLiteral("content-length"))
        {
            contentLength = value.toInt(&ok);

            if (!ok || contentLength < 0 || contentLength > MAX_REQUEST_BODY_BYTES)
            {
                WriteErrorAndClose(socket, 413,
                                   QStringLiteral("Body exceeds the 1 MB limit."));
                return true;
            }
        }
    }

    const int totalBytes = headerEnd + 4 + contentLength;

    if (buffer.size() < totalBytes)
    {
        return false; // 体数据未到齐，等待下一次 readyRead
    }

    request.body = buffer.mid(headerEnd + 4, contentLength);
    buffer.remove(0, totalBytes);
    return true;
}

void DagHttpServer::DispatchRequest(QTcpSocket *socket, const _tagDagHttpRequest &request)
{
    if (m_requestHandler == nullptr)
    {
        WriteErrorAndClose(socket, 500, QStringLiteral("No handler configured."));
        return;
    }

    const _tagDagHttpResponse response = m_requestHandler(request);

    if (response.isEventStream)
    {
        UpgradeToEventStream(socket, response.eventChannel);
        return;
    }

    QByteArray body = response.body;
    const bool isHeadRequest = (request.method == QStringLiteral("HEAD"));

    if (isHeadRequest)
    {
        body.clear();
    }

    QByteArray headerBlock;
    headerBlock += QStringLiteral("HTTP/1.1 %1 %2\r\n")
                       .arg(response.statusCode)
                       .arg(QLatin1String(StatusCodeText(response.statusCode)))
                       .toLatin1();
    headerBlock += "Content-Type: "
                   + (response.contentType.isEmpty()
                          ? QByteArrayLiteral("application/json; charset=utf-8")
                          : response.contentType)
                   + "\r\n";
    headerBlock += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    headerBlock += "Connection: close\r\n";

    for (const auto &extra : response.extraHeaders)
    {
        headerBlock += extra.first + ": " + extra.second + "\r\n";
    }

    headerBlock += "\r\n";

    socket->write(headerBlock);

    if (!body.isEmpty())
    {
        socket->write(body);
    }

    // 关闭式处理：写完后断开，规避 keep-alive 状态机复杂度。
    socket->disconnectFromHost();

    // 缓冲区里可能还有同一连接上的下一条请求；关闭式策略下直接丢弃。
    if (m_receiveBuffers.contains(socket))
    {
        m_receiveBuffers[socket].clear();
    }
}

void DagHttpServer::WriteErrorAndClose(QTcpSocket *socket,
                                        int statusCode,
                                        const QString &message)
{
    QJsonObject errorObject;
    errorObject.insert(QStringLiteral("error"), message);
    const QByteArray body = QJsonDocument(errorObject).toJson(QJsonDocument::Compact);

    QByteArray headerBlock;
    headerBlock += QStringLiteral("HTTP/1.1 %1 %2\r\n")
                       .arg(statusCode)
                       .arg(QLatin1String(StatusCodeText(statusCode)))
                       .toLatin1();
    headerBlock += "Content-Type: application/json; charset=utf-8\r\n";
    headerBlock += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    headerBlock += "Connection: close\r\n\r\n";

    socket->write(headerBlock + body);
    socket->disconnectFromHost();

    if (m_receiveBuffers.contains(socket))
    {
        m_receiveBuffers[socket].clear();
    }
}

void DagHttpServer::UpgradeToEventStream(QTcpSocket *socket, const QString &channel)
{
    QByteArray headerBlock;
    headerBlock += "HTTP/1.1 200 OK\r\n";
    headerBlock += "Content-Type: text/event-stream; charset=utf-8\r\n";
    headerBlock += "Cache-Control: no-cache\r\n";
    headerBlock += "Connection: keep-alive\r\n\r\n";
    headerBlock += ": connected\n\n"; // 初始注释帧，便于客户端确认通道建立

    socket->write(headerBlock);
    socket->setProperty("dsh_channel", channel);
    socket->setProperty("dsh_is_stream", true);
    m_eventStreams.append(socket);

    // SSE 连接不再走请求解析路径。
    disconnect(socket, &QTcpSocket::readyRead, this, &DagHttpServer::OnReadyRead);
    m_receiveBuffers.remove(socket);
}

void DagHttpServer::SendHeartbeats()
{
    CleanupClosedSockets();
    const QByteArray ping = ": ping\n\n";

    for (QTcpSocket *socket : std::as_const(m_eventStreams))
    {
        socket->write(ping);
    }
}

void DagHttpServer::CleanupClosedSockets()
{
    for (int index = m_eventStreams.size() - 1; index >= 0; --index)
    {
        QTcpSocket *socket = m_eventStreams.at(index);

        if (socket->state() != QAbstractSocket::ConnectedState)
        {
            socket->deleteLater();
            m_eventStreams.removeAt(index);
        }
    }
}

} // namespace vpet
