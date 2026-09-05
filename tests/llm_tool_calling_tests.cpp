#include "vpet/llm/llm_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace
{

/**
 * @brief 本地回环 LLM stub 服务器，按队列回放预设响应。
 */
class MockLlmServer : public QObject
{
    Q_OBJECT

public:
    explicit MockLlmServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
        , m_responses()
        , m_statusCode(200)
        , m_requestCount(0)
        , m_lastRequestBody()
    {
        connect(m_server, &QTcpServer::newConnection,
                this, &MockLlmServer::OnNewConnection);
    }

    bool Start()
    {
        return m_server->listen(QHostAddress::LocalHost, 0);
    }

    quint16 Port() const
    {
        return m_server->serverPort();
    }

    /**
     * @brief 入队下一个请求的响应 JSON 正文。
     */
    void EnqueueResponse(const QByteArray &body)
    {
        m_responses.append(body);
    }

    void SetStatusCode(int statusCode)
    {
        m_statusCode = statusCode;
    }

    int RequestCount() const
    {
        return m_requestCount;
    }

    QByteArray LastRequestBody() const
    {
        return m_lastRequestBody;
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
                if (socket->property("answered").toBool()) return;
                const QByteArray requestData = socket->property("requestBuffer").toByteArray() + socket->readAll();
                socket->setProperty("requestBuffer", requestData);

                if (!requestData.contains(QByteArrayLiteral("\r\n\r\n")))
                {
                    return;
                }

                const int headerEnd = requestData.indexOf(QByteArrayLiteral("\r\n\r\n")) + 4;
                int contentLength = 0;
                for (const auto &line : requestData.left(headerEnd).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        contentLength = line.mid(15).trimmed().toInt();
                    }
                }
                if (requestData.size() < headerEnd + contentLength) {
                    return;
                }
                socket->setProperty("answered", true);

                m_requestCount += 1;
                m_lastRequestBody = requestData.mid(headerEnd);

                QByteArray body;
                const QString statusText = (m_statusCode == 200)
                                           ? QStringLiteral("200 OK")
                                           : QStringLiteral("400 Bad Request");

                if (!m_responses.isEmpty())
                {
                    body = m_responses.takeFirst();
                }

                const QByteArray response =
                    QByteArrayLiteral("HTTP/1.1 ") + statusText.toUtf8()
                    + QByteArrayLiteral("\r\nContent-Type: application/json\r\n")
                    + QByteArrayLiteral("Content-Length: ")
                    + QByteArray::number(body.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                    + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    }

private:
    QTcpServer *m_server;
    QList<QByteArray> m_responses;
    int m_statusCode;
    int m_requestCount;
    QByteArray m_lastRequestBody;
};

/**
 * @brief 用 stub 服务器端口配置 LLM 客户端。
 */
void ConfigureClient(vpet::LlmClient &client, MockLlmServer &server)
{
    vpet::_tagLlmConfig config;
    config.baseUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.Port());
    config.apiKey = QStringLiteral("test-key");
    config.model = QStringLiteral("test-model");
    QVERIFY(client.SetConfig(config));
}

/**
 * @brief 构造标准 tools 声明数组。
 */
QJsonArray MakeToolsArray()
{
    QJsonObject searchTool;
    QJsonObject searchFunction;
    searchFunction[QStringLiteral("name")] = QStringLiteral("web.search");
    searchFunction[QStringLiteral("description")] = QStringLiteral("搜索网络");
    searchTool[QStringLiteral("type")] = QStringLiteral("function");
    searchTool[QStringLiteral("function")] = searchFunction;

    QJsonArray tools;
    tools.append(searchTool);
    return tools;
}

/**
 * @brief 构造单个用户消息。
 */
vpet::_tagLlmMessage MakeUserMessage(const QString &content)
{
    vpet::_tagLlmMessage message;
    message.role = vpet::LLM_MESSAGE_ROLE::USER;
    message.content = content;
    return message;
}

class LlmToolCallingTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<vpet::_tagLlmMessage>();
        qRegisterMetaType<vpet::_tagLlmToolCall>();
    }

    // 请求序列化
    void RequestIncludesToolsAndToolChoice();
    void AssistantToolCallsSerializedAsJsonString();
    void ToolResultMessageRoundTrip();
    void ToolMessageWithoutToolCallIdRejected();

    // 响应解析
    void TextResponseStillEmitsChatCompleted();
    void ToolCallsResponseEmitsToolCallsCompleted();
    void ToolCallMissingIdGetsPlaceholder();
    void MalformedToolArgumentsMarkedInvalid();
    void EmptyContentAndNoToolCallsFails();
    void StreamingWithToolsIsExplicitlyRejected();
    void RequestWithoutToolsDoesNotEmitToolsUnsupportedOn4xx();

    // 端点降级
    void FourXxWithToolsErrorEmitsUnsupported();
    void FourXxWithoutToolsKeywordEmitsFailed();
    void TwoXxErrorBodyIsNotTreatedAsUnsupported();
};

void LlmToolCallingTests::RequestIncludesToolsAndToolChoice()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    vpet::_tagLlmRequestOptions options;
    options.tools = MakeToolsArray();
    options.toolChoice = QStringLiteral("auto");

    QSignalSpy completedSpy(&client, &vpet::LlmClient::ChatCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    QVERIFY(client.SendChat(messages, options) > 0);
    QVERIFY(completedSpy.wait(3000));
    QCOMPARE(server.RequestCount(), 1);

    const QJsonDocument requestDocument =
        QJsonDocument::fromJson(server.LastRequestBody());
    QVERIFY(requestDocument.isObject());
    const QJsonObject body = requestDocument.object();

    QVERIFY(body.contains(QStringLiteral("tools")));
    const QJsonArray toolsArray = body.value(QStringLiteral("tools")).toArray();
    QCOMPARE(toolsArray.size(), 1);
    QCOMPARE(toolsArray.at(0).toObject().value(QStringLiteral("function"))
                 .toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("web.search"));
    QCOMPARE(body.value(QStringLiteral("tool_choice")).toString(), QStringLiteral("auto"));
}

void LlmToolCallingTests::AssistantToolCallsSerializedAsJsonString()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    vpet::_tagLlmMessage assistantMessage;
    assistantMessage.role = vpet::LLM_MESSAGE_ROLE::ASSISTANT;

    vpet::_tagLlmToolCall toolCall;
    toolCall.id = QStringLiteral("call_abc");
    toolCall.name = QStringLiteral("web.search");
    toolCall.arguments = QJsonDocument::fromJson(
                             QByteArrayLiteral("{\"query\":\"天气\"}")).object();
    assistantMessage.toolCalls.append(toolCall);

    vpet::_tagLlmMessage userMessage = MakeUserMessage(QStringLiteral("接着做"));

    QSignalSpy completedSpy(&client, &vpet::LlmClient::ChatCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(userMessage);
    messages.append(assistantMessage);

    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(completedSpy.wait(3000));

    const QJsonDocument requestDocument =
        QJsonDocument::fromJson(server.LastRequestBody());
    const QJsonArray messagesArray = requestDocument.object()
                                         .value(QStringLiteral("messages")).toArray();
    QCOMPARE(messagesArray.size(), 2);

    const QJsonObject assistantObject = messagesArray.at(1).toObject();
    QCOMPARE(assistantObject.value(QStringLiteral("role")).toString(),
             QStringLiteral("assistant"));
    QVERIFY(assistantObject.value(QStringLiteral("tool_calls")).isArray());

    const QJsonObject serializedCall = assistantObject
                                           .value(QStringLiteral("tool_calls"))
                                           .toArray().at(0).toObject();
    QCOMPARE(serializedCall.value(QStringLiteral("id")).toString(), QStringLiteral("call_abc"));
    QCOMPARE(serializedCall.value(QStringLiteral("type")).toString(), QStringLiteral("function"));

    // arguments 必须是 JSON 字符串，且内容为合法对象。
    const QString argumentsText = serializedCall.value(QStringLiteral("function"))
                                      .toObject().value(QStringLiteral("arguments")).toString();
    const QJsonDocument argumentsDocument = QJsonDocument::fromJson(argumentsText.toUtf8());
    QVERIFY(argumentsDocument.isObject());
    QCOMPARE(argumentsDocument.object().value(QStringLiteral("query")).toString(),
             QStringLiteral("天气"));
}

void LlmToolCallingTests::ToolResultMessageRoundTrip()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    vpet::_tagLlmMessage toolMessage;
    toolMessage.role = vpet::LLM_MESSAGE_ROLE::TOOL;
    toolMessage.toolCallId = QStringLiteral("call_abc");
    toolMessage.content = QStringLiteral("搜索完成");

    QSignalSpy completedSpy(&client, &vpet::LlmClient::ChatCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));
    messages.append(toolMessage);

    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(completedSpy.wait(3000));

    const QJsonDocument requestDocument =
        QJsonDocument::fromJson(server.LastRequestBody());
    const QJsonArray messagesArray = requestDocument.object()
                                         .value(QStringLiteral("messages")).toArray();
    const QJsonObject toolObject = messagesArray.at(1).toObject();
    QCOMPARE(toolObject.value(QStringLiteral("role")).toString(), QStringLiteral("tool"));
    QCOMPARE(toolObject.value(QStringLiteral("tool_call_id")).toString(),
             QStringLiteral("call_abc"));
    QCOMPARE(toolObject.value(QStringLiteral("content")).toString(),
             QStringLiteral("搜索完成"));
}

void LlmToolCallingTests::ToolMessageWithoutToolCallIdRejected()
{
    MockLlmServer server;
    QVERIFY(server.Start());

    vpet::LlmClient client;
    ConfigureClient(client, server);

    vpet::_tagLlmMessage toolMessage;
    toolMessage.role = vpet::LLM_MESSAGE_ROLE::TOOL;
    toolMessage.content = QStringLiteral("结果");

    QSignalSpy failedSpy(&client, &vpet::LlmClient::ChatFailed);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));
    messages.append(toolMessage);

    QCOMPARE(client.SendChat(messages), -1);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(failedSpy.at(0).at(1).toString().contains(QStringLiteral("tool call ID")));
    QCOMPARE(server.RequestCount(), 0);
}

void LlmToolCallingTests::TextResponseStillEmitsChatCompleted()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"你好\"}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy completedSpy(&client, &vpet::LlmClient::ChatCompleted);
    QSignalSpy toolSpy(&client, &vpet::LlmClient::ChatToolCallsCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(completedSpy.wait(3000));
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(1).toString(), QStringLiteral("你好"));
    QCOMPARE(toolSpy.count(), 0);
}

void LlmToolCallingTests::ToolCallsResponseEmitsToolCallsCompleted()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{"
        "\"content\":\"\","
        "\"tool_calls\":["
        "{\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"web.search\",\"arguments\":\"{\\\"query\\\":\\\"天气\\\",\\\"limit\\\":3}\"}},"
        "{\"id\":\"call_2\",\"type\":\"function\",\"function\":{"
        "\"name\":\"memory.search\",\"arguments\":\"{\\\"query\\\":\\\"昨天\\\"}\"}}"
        "]}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy completedSpy(&client, &vpet::LlmClient::ChatCompleted);
    QSignalSpy toolSpy(&client, &vpet::LlmClient::ChatToolCallsCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("查两个")));

    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(toolSpy.wait(3000));
    QCOMPARE(toolSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 0);

    const QVector<vpet::_tagLlmToolCall> toolCalls =
        toolSpy.at(0).at(1).value<QVector<vpet::_tagLlmToolCall>>();
    QCOMPARE(toolCalls.size(), 2);

    QCOMPARE(toolCalls.at(0).id, QStringLiteral("call_1"));
    QCOMPARE(toolCalls.at(0).name, QStringLiteral("web.search"));
    QVERIFY(toolCalls.at(0).argumentsValid);
    QCOMPARE(toolCalls.at(0).arguments.value(QStringLiteral("query")).toString(),
             QStringLiteral("天气"));
    QCOMPARE(toolCalls.at(0).arguments.value(QStringLiteral("limit")).toInt(), 3);

    QCOMPARE(toolCalls.at(1).id, QStringLiteral("call_2"));
    QCOMPARE(toolCalls.at(1).name, QStringLiteral("memory.search"));
}

void LlmToolCallingTests::ToolCallMissingIdGetsPlaceholder()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{"
        "\"tool_calls\":[{"
        "\"type\":\"function\",\"function\":{\"name\":\"web.search\","
        "\"arguments\":\"{}\"}}]}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy toolSpy(&client, &vpet::LlmClient::ChatToolCallsCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(toolSpy.wait(3000));

    const QVector<vpet::_tagLlmToolCall> toolCalls =
        toolSpy.at(0).at(1).value<QVector<vpet::_tagLlmToolCall>>();
    QCOMPARE(toolCalls.size(), 1);
    QCOMPARE(toolCalls.first().id, QStringLiteral("call_1"));
    QVERIFY(toolCalls.first().argumentsValid);
}

void LlmToolCallingTests::MalformedToolArgumentsMarkedInvalid()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{"
        "\"tool_calls\":[{"
        "\"id\":\"call_x\",\"type\":\"function\",\"function\":{"
        "\"name\":\"web.search\",\"arguments\":\"{not valid json\"}}]}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy toolSpy(&client, &vpet::LlmClient::ChatToolCallsCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(toolSpy.wait(3000));

    const QVector<vpet::_tagLlmToolCall> toolCalls =
        toolSpy.at(0).at(1).value<QVector<vpet::_tagLlmToolCall>>();
    QCOMPARE(toolCalls.size(), 1);
    QVERIFY(!toolCalls.first().argumentsValid);
    QVERIFY(toolCalls.first().arguments.isEmpty());
}

void LlmToolCallingTests::EmptyContentAndNoToolCallsFails()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"\"}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy failedSpy(&client, &vpet::LlmClient::ChatFailed);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(failedSpy.wait(3000));
    QCOMPARE(failedSpy.count(), 1);
}

void LlmToolCallingTests::FourXxWithToolsErrorEmitsUnsupported()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.SetStatusCode(400);
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"error\":{\"message\":\"The model does not support tools / function calling.\"}}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy unsupportedSpy(&client, &vpet::LlmClient::ChatToolsUnsupported);
    QSignalSpy failedSpy(&client, &vpet::LlmClient::ChatFailed);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    vpet::_tagLlmRequestOptions options;
    options.tools = MakeToolsArray();

    QVERIFY(client.SendChat(messages, options) > 0);
    QVERIFY(unsupportedSpy.wait(3000));
    QCOMPARE(unsupportedSpy.count(), 1);
    QCOMPARE(unsupportedSpy.at(0).at(2).toInt(), 400);
    QCOMPARE(failedSpy.count(), 0);
}

void LlmToolCallingTests::FourXxWithoutToolsKeywordEmitsFailed()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.SetStatusCode(400);
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"error\":{\"message\":\"Rate limit exceeded.\"}}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy unsupportedSpy(&client, &vpet::LlmClient::ChatToolsUnsupported);
    QSignalSpy failedSpy(&client, &vpet::LlmClient::ChatFailed);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    vpet::_tagLlmRequestOptions options;
    options.tools = MakeToolsArray();

    QVERIFY(client.SendChat(messages, options) > 0);
    QVERIFY(failedSpy.wait(3000));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(unsupportedSpy.count(), 0);
}

void LlmToolCallingTests::TwoXxErrorBodyIsNotTreatedAsUnsupported()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{"
        "\"content\":\"tools are mentioned in normal text\"}}]}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy unsupportedSpy(&client, &vpet::LlmClient::ChatToolsUnsupported);
    QSignalSpy completedSpy(&client, &vpet::LlmClient::ChatCompleted);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    vpet::_tagLlmRequestOptions options;
    options.tools = MakeToolsArray();

    QVERIFY(client.SendChat(messages, options) > 0);
    QVERIFY(completedSpy.wait(3000));
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(unsupportedSpy.count(), 0);
}

void LlmToolCallingTests::StreamingWithToolsIsExplicitlyRejected()
{
    vpet::LlmClient client;
    vpet::_tagLlmConfig config;
    config.baseUrl = QStringLiteral("http://127.0.0.1:9999/v1");
    config.apiKey = QStringLiteral("key");
    config.model = QStringLiteral("model");
    QVERIFY(client.SetConfig(config));

    QSignalSpy failedSpy(&client, &vpet::LlmClient::ChatFailed);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    vpet::_tagLlmRequestOptions options;
    options.stream = true;
    options.tools = MakeToolsArray();

    const int id = client.SendChat(messages, options);
    QCOMPARE(id, -1);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(failedSpy.first().at(1).toString().contains(QStringLiteral("Streaming tool calls are not supported")));
}

void LlmToolCallingTests::RequestWithoutToolsDoesNotEmitToolsUnsupportedOn4xx()
{
    MockLlmServer server;
    QVERIFY(server.Start());
    server.SetStatusCode(400);
    server.EnqueueResponse(QByteArrayLiteral(
        "{\"error\":{\"message\":\"Tools are unsupported here.\"}}"));

    vpet::LlmClient client;
    ConfigureClient(client, server);

    QSignalSpy unsupportedSpy(&client, &vpet::LlmClient::ChatToolsUnsupported);
    QSignalSpy failedSpy(&client, &vpet::LlmClient::ChatFailed);
    QVector<vpet::_tagLlmMessage> messages;
    messages.append(MakeUserMessage(QStringLiteral("hello")));

    // 没有传 tools
    QVERIFY(client.SendChat(messages) > 0);
    QVERIFY(failedSpy.wait(3000));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(unsupportedSpy.count(), 0);
}

} // anonymous namespace

QTEST_MAIN(LlmToolCallingTests)

#include "llm_tool_calling_tests.moc"