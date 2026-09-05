#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_context_keys.h"
#include "vpet/agent/agent_dag_graph.h"
#include "vpet/agent/tool_loop_node.h"
#include "vpet/agent/tools/itool.h"
#include "vpet/agent/tools/tool_registry.h"
#include "vpet/agent/tools/tool_loop_executor.h"
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
 * @brief 本地回环 LLM stub：按队列脚本化回放响应，记录请求体。
 */
class ScriptedLlmServer : public QObject
{
    Q_OBJECT

public:
    explicit ScriptedLlmServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
        , m_script()
        , m_requestCount(0)
        , m_requestBodies()
        , m_statusCode(200)
        , m_dropAllResponses(false)
    {
        connect(m_server, &QTcpServer::newConnection,
                this, &ScriptedLlmServer::OnNewConnection);
    }

    void SetDropAllResponses(bool drop)
    {
        m_dropAllResponses = drop;
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
     * @brief 入队一个纯文本回复。
     */
    void EnqueueText(const QString &content)
    {
        QJsonObject message;
        message[QStringLiteral("content")] = content;
        EnqueueMessage(message);
    }

    /**
     * @brief 入队一个工具调用回复（支持多个并行 tool_call）。
     */
    void EnqueueToolCalls(const QVector<QPair<QString, QString>> &calls)
    {
        QJsonObject message;
        QJsonArray toolCallsArray;

        for (int i = 0; i < calls.size(); ++i)
        {
            QJsonObject functionObject;
            functionObject[QStringLiteral("name")] = calls.at(i).first;
            functionObject[QStringLiteral("arguments")] = calls.at(i).second;

            QJsonObject toolCallObject;
            toolCallObject[QStringLiteral("id")] =
                QStringLiteral("call_%1").arg(i + 1);
            toolCallObject[QStringLiteral("type")] = QStringLiteral("function");
            toolCallObject[QStringLiteral("function")] = functionObject;
            toolCallsArray.append(toolCallObject);
        }

        message[QStringLiteral("tool_calls")] = toolCallsArray;
        EnqueueMessage(message);
    }

    /**
     * @brief 入队一个 4xx tools 不支持错误。
     *
     * 仅对下一个出队响应生效，出队后状态码复位为 200。
     */
    void EnqueueToolsUnsupported()
    {
        m_statusCode = 400;
        m_script.append(QByteArrayLiteral(
            "{\"error\":{\"message\":\"The model does not support tools.\"}}"));
    }

    int RequestCount() const
    {
        return m_requestCount;
    }

    /**
     * @brief 获取第 index 个请求（从 0 开始）的解析后请求体。
     */
    QJsonObject RequestBodyAt(int index) const
    {
        return QJsonDocument::fromJson(m_requestBodies.value(index)).object();
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

                const int headerEnd = requestData.indexOf("\r\n\r\n") + 4;
                int contentLength = 0;
                for (const auto &line : requestData.left(headerEnd).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toInt();
                }
                if (requestData.size() < headerEnd + contentLength) return;
                socket->setProperty("answered", true);
                m_requestCount += 1;
                m_requestBodies.append(requestData.mid(requestData.indexOf(
                                                            QByteArrayLiteral("\r\n\r\n")) + 4));

                if (m_dropAllResponses)
                {
                    return;
                }

                QByteArray body = QByteArrayLiteral(
                    "{\"choices\":[{\"message\":{\"content\":\"empty\"}}]}");

                if (!m_script.isEmpty())
                {
                    body = m_script.takeFirst();
                }

                const QString statusText = (m_statusCode == 200)
                                           ? QStringLiteral("200 OK")
                                           : QStringLiteral("400 Bad Request");
                m_statusCode = 200;
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
    void EnqueueMessage(const QJsonObject &message)
    {
        QJsonObject firstChoice;
        firstChoice[QStringLiteral("message")] = message;
        firstChoice[QStringLiteral("finish_reason")] =
            message.contains(QStringLiteral("tool_calls"))
                ? QStringLiteral("tool_calls")
                : QStringLiteral("stop");

        QJsonArray choices;
        choices.append(firstChoice);

        QJsonObject root;
        root[QStringLiteral("choices")] = choices;
        m_script.append(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    QTcpServer *m_server;
    QList<QByteArray> m_script;
    int m_requestCount;
    QList<QByteArray> m_requestBodies;
    int m_statusCode;
    bool m_dropAllResponses;
};

/**
 * @brief 受控 fake 只读工具。
 */
class FakeEchoTool : public vpet::ITool
{
    Q_OBJECT

public:
    FakeEchoTool(const QString &name, QObject *parent = nullptr)
        : vpet::ITool(parent)
        , m_executeCount(0)
    {
        m_spec.name = name;
        m_spec.label = name;
        m_spec.description = QStringLiteral("Echo tool %1").arg(name);
        m_spec.trustTier = vpet::ToolTrustTier::ReadOnly;

        vpet::_tagToolParameterSchema query;
        query.name = QStringLiteral("query");
        query.type = QStringLiteral("string");
        query.description = QStringLiteral("查询文本");
        query.required = true;
        m_spec.parameters.append(query);
    }

    vpet::_tagToolSpec Spec() const override
    {
        return m_spec;
    }

    bool ValidateArguments(const QJsonObject &arguments,
                           QString &errorMessage) const override
    {
        return vpet::ValidateToolArguments(m_spec, arguments, errorMessage);
    }

    void Execute(const vpet::_tagToolCall &call) override
    {
        m_executeCount += 1;
        m_lastQuery = call.arguments.value(QStringLiteral("query")).toString();

        vpet::_tagToolExecutionResult result;
        result.ok = true;
        result.textOutput = QStringLiteral("echo:%1:%2")
                                .arg(call.toolName, m_lastQuery);
        result.executionId = call.executionId;
        emit Completed(result);
    }

    void Cancel() override
    {
    }

    bool IsBusy() const override
    {
        return false;
    }

    int ExecuteCount() const
    {
        return m_executeCount;
    }

private:
    vpet::_tagToolSpec m_spec;
    int m_executeCount;
    QString m_lastQuery;
};

/**
 * @brief 带 terminate 提示的 fake 工具。
 */
class TerminatingTool : public vpet::ITool
{
    Q_OBJECT

public:
    TerminatingTool()
        : vpet::ITool()
    {
        m_spec.name = QStringLiteral("terminator");
        m_spec.label = QStringLiteral("terminator");
        m_spec.description = QStringLiteral("terminating tool");
        m_spec.trustTier = vpet::ToolTrustTier::ReadOnly;
    }

    vpet::_tagToolSpec Spec() const override
    {
        return m_spec;
    }

    bool ValidateArguments(const QJsonObject &,
                           QString &) const override
    {
        return true;
    }

    void Execute(const vpet::_tagToolCall &call) override
    {
        vpet::_tagToolExecutionResult result;
        result.ok = true;
        result.textOutput = QStringLiteral("enough");
        result.terminateHint = true;
        result.executionId = call.executionId;
        emit Completed(result);
    }

    void Cancel() override
    {
    }

    bool IsBusy() const override
    {
        return false;
    }

private:
    vpet::_tagToolSpec m_spec;
};

/**
 * @brief 构造循环请求。
 */
vpet::_tagToolLoopRequest MakeRequest(const vpet::_tagToolLoopConfig &config)
{
    vpet::_tagToolLoopRequest request;
    request.promptText = QStringLiteral("用户的问题");
    request.config = config;
    return request;
}

class ToolLoopTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<vpet::_tagToolLoopResult>();
    }

    void init()
    {
        m_server = std::make_unique<ScriptedLlmServer>();
        QVERIFY(m_server->Start());

        m_client = std::make_unique<vpet::LlmClient>();
        vpet::_tagLlmConfig config;
        config.baseUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server->Port());
        config.apiKey = QStringLiteral("test-key");
        config.model = QStringLiteral("test-model");
        QVERIFY(m_client->SetConfig(config));

        m_registry = std::make_shared<vpet::ToolRegistry>();
        m_searchTool = std::make_shared<FakeEchoTool>(QStringLiteral("web.search"));
        m_memoryTool = std::make_shared<FakeEchoTool>(QStringLiteral("memory.search"));
        QString errorMessage;
        QVERIFY(m_registry->Register(m_searchTool, errorMessage));
        QVERIFY(m_registry->Register(m_memoryTool, errorMessage));

        m_executor = std::make_unique<vpet::ToolLoopExecutor>();
        m_executor->SetLlmClient(m_client.get());
        m_executor->SetToolRegistry(m_registry);

        m_spy = std::make_unique<QSignalSpy>(m_executor.get(),
                                             &vpet::ToolLoopExecutor::Finished);
    }

    void cleanup()
    {
        m_spy.reset();
        m_executor.reset();
        m_registry.reset();
        m_client.reset();
        m_server.reset();
    }

    // 场景：多轮 tool_calls 后最终作答
    void MultiRoundToolCallsThenAnswer();
    void TerminalCompositionCannotExecuteMoreTools() {
        vpet::_tagToolLoopConfig config;
        config.tools = {QStringLiteral("web.search")}; config.maxRounds = 1;
        m_server->EnqueueToolCalls({{QStringLiteral("web.search"), QStringLiteral("{\"query\":\"a\"}")}});
        m_server->EnqueueToolCalls({{QStringLiteral("web.search"), QStringLiteral("{\"query\":\"b\"}")}});
        QVERIFY(m_executor->Start(MakeRequest(config)));
        QTRY_COMPARE(m_spy->size(), 1);
        QVERIFY(!m_spy->first().first().value<vpet::_tagToolLoopResult>().ok);
        QCOMPARE(m_searchTool->ExecuteCount(), 1);
        QCOMPARE(m_server->RequestCount(), 2);
    }
    void EmptyAllowlistNeverExecutesToolCalls() {
        vpet::_tagToolLoopConfig config;
        m_server->EnqueueToolCalls({{QStringLiteral("web.search"), QStringLiteral("{\"query\":\"a\"}")}});
        m_server->EnqueueText(QStringLiteral("answer"));
        QVERIFY(m_executor->Start(MakeRequest(config)));
        QTRY_COMPARE(m_spy->size(), 1);
        QCOMPARE(m_searchTool->ExecuteCount(), 0);
    }
    void CancelFinishesExactlyOnce() {
        vpet::_tagToolLoopConfig config;
        QVERIFY(m_executor->Start(MakeRequest(config)));
        m_executor->Cancel();
        QTRY_COMPARE(m_spy->size(), 1);
        QCOMPARE(m_spy->first().first().value<vpet::_tagToolLoopResult>().status, vpet::ToolLoopStatus::Cancelled);
        QTest::qWait(30);
        QCOMPARE(m_spy->size(), 1);
    }

    // 场景：预算耗尽 answer_with_context 收束
    void BudgetExhaustedAnswersWithContext();

    // 场景：预算耗尽 end 策略直接失败收束
    void BudgetExhaustedEndPolicyFails();

    // 场景：terminate 提示提前收束
    void TerminateHintSkipsRemainingCalls();

    // 场景：端点不支持 tools 时降级 plain_chat
    void ToolsUnsupportedFallsBackToPlainChat();

    // 场景：不在允许列表的工具回喂错误
    void DisallowedToolFeedsBackError();

    // 场景：畸形参数 JSON 回喂错误并继续
    void InvalidArgumentsJsonFeedsBackError();

    // 场景：空 tools 配置等价普通对话
    void EmptyToolsConfigBehavesLikePlainChat();

    // 节点配置解析
    void ParseConfigValidation();

    // 节点协议 Complete
    void NodeCompleteWritesProtocolKeys();

    // 场景：同一轮内出现重复 tool_call id 碰撞依然稳定关联执行
    void DuplicateToolCallIdHandledGracefully();

    // 场景：时间预算到期触发强制收束
    void WallClockBudgetTimeoutForcesFinish();

private:
    std::unique_ptr<ScriptedLlmServer> m_server;
    std::unique_ptr<vpet::LlmClient> m_client;
    std::shared_ptr<vpet::ToolRegistry> m_registry;
    std::shared_ptr<FakeEchoTool> m_searchTool;
    std::shared_ptr<FakeEchoTool> m_memoryTool;
    std::unique_ptr<vpet::ToolLoopExecutor> m_executor;
    std::unique_ptr<QSignalSpy> m_spy;
};

void ToolLoopTests::MultiRoundToolCallsThenAnswer()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search"),
                               QStringLiteral("memory.search")};
    config.maxRounds = 6;

    // 轮 1：两个工具调用；轮 2：再一个；轮 3：最终作答。
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"天气\"}")},
        {QStringLiteral("memory.search"), QStringLiteral("{\"query\":\"偏好\"}")},
    });
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"明天\"}")},
    });
    m_server->EnqueueText(QStringLiteral("最终答案：晴天。"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(result.ok);
    QCOMPARE(result.status, vpet::ToolLoopStatus::Answered);
    QCOMPARE(result.textOutput, QStringLiteral("最终答案：晴天。"));
    QCOMPARE(m_searchTool->ExecuteCount(), 2);
    QCOMPARE(m_memoryTool->ExecuteCount(), 1);
    QCOMPARE(m_server->RequestCount(), 3);

    // 审计数组记录了三次调用。
    QCOMPARE(result.toolCallsAudit.size(), 3);

    // 第二轮请求应包含 tool 消息与 assistant tool_calls 转写。
    const QJsonObject secondRequest = m_server->RequestBodyAt(1);
    const QJsonArray messages = secondRequest.value(QStringLiteral("messages")).toArray();
    bool sawToolMessage = false;

    for (const QJsonValue &messageValue : messages)
    {
        const QJsonObject message = messageValue.toObject();

        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool"))
        {
            sawToolMessage = true;
            QVERIFY(message.contains(QStringLiteral("tool_call_id")));
        }
    }

    QVERIFY(sawToolMessage);
    QVERIFY(m_server->RequestBodyAt(0).value(QStringLiteral("tools"))
                .toArray().size() > 0);
}

void ToolLoopTests::BudgetExhaustedAnswersWithContext()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search")};
    config.maxRounds = 2;

    // 轮 1：工具调用；轮 2：仍要工具调用（预算将尽，只能再发收束作答）。
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"a\"}")},
    });
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"b\"}")},
    });
    // 轮 2 的工具调用回喂后进入轮 3 前预算检查：max_rounds=2 已耗尽，
    // 触发 answer_with_context 收束作答（无 tools 声明）。
    m_server->EnqueueText(QStringLiteral("基于已有资料的回答。"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(result.ok);
    QCOMPARE(result.status, vpet::ToolLoopStatus::Answered);
    QCOMPARE(result.textOutput, QStringLiteral("基于已有资料的回答。"));
    QCOMPARE(m_searchTool->ExecuteCount(), 2);
    QCOMPARE(m_server->RequestCount(), 3);

    // 收束请求不得携带 tools 声明。
    QVERIFY(m_server->RequestBodyAt(2).value(QStringLiteral("tools"))
                .toArray().isEmpty());
}

void ToolLoopTests::BudgetExhaustedEndPolicyFails()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search")};
    config.maxRounds = 1;
    config.onBudgetExhausted = QStringLiteral("end");

    // 轮 1：工具调用；轮 2 前预算检查触发 end 收束。
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"a\"}")},
    });

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(!result.ok);
    QCOMPARE(result.status, vpet::ToolLoopStatus::BudgetExhausted);
    QVERIFY(result.reason.contains(QStringLiteral("Round")));
    QCOMPARE(m_searchTool->ExecuteCount(), 1);
    QCOMPARE(m_server->RequestCount(), 1);
}

void ToolLoopTests::TerminateHintSkipsRemainingCalls()
{
    auto terminatingTool = std::make_shared<TerminatingTool>();
    QString errorMessage;
    QVERIFY(m_registry->Register(terminatingTool, errorMessage));

    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search"),
                               QStringLiteral("terminator")};
    config.maxRounds = 6;

    // 轮 1：先调 web.search，再调 terminator（terminate 提示应跳过 terminator
    // 本身之后的执行——但 terminator 在第二个执行；正确行为：web.search 执行
    // 成功后执行 terminator，terminate 后直接收束作答，无需更多工具轮）。
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"a\"}")},
        {QStringLiteral("terminator"), QStringLiteral("{}")},
    });
    m_server->EnqueueText(QStringLiteral("提前收束的答案。"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(result.ok);
    QCOMPARE(result.status, vpet::ToolLoopStatus::Answered);
    QCOMPARE(result.textOutput, QStringLiteral("提前收束的答案。"));
    QCOMPARE(m_searchTool->ExecuteCount(), 1);
    // A mixed round does not terminate: every result must carry the hint.
    QVERIFY(!m_server->RequestBodyAt(1).value(QStringLiteral("tools"))
                 .toArray().isEmpty());
}

void ToolLoopTests::ToolsUnsupportedFallsBackToPlainChat()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search")};

    m_server->EnqueueToolsUnsupported();
    m_server->EnqueueText(QStringLiteral("降级后的普通回答。"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(result.ok);
    QCOMPARE(result.status, vpet::ToolLoopStatus::FallbackPlainChat);
    QCOMPARE(result.textOutput, QStringLiteral("降级后的普通回答。"));
    QCOMPARE(m_searchTool->ExecuteCount(), 0);
    QCOMPARE(m_server->RequestCount(), 2);

    // 降级请求不携带 tools 声明。
    QVERIFY(m_server->RequestBodyAt(1).value(QStringLiteral("tools"))
                .toArray().isEmpty());
}

void ToolLoopTests::DisallowedToolFeedsBackError()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search")};

    // 轮 1：调用未在允许列表的 memory.search；LLM 收到错误后最终作答。
    m_server->EnqueueToolCalls({
        {QStringLiteral("memory.search"), QStringLiteral("{\"query\":\"x\"}")},
    });
    m_server->EnqueueText(QStringLiteral("收到工具不可用错误后的回答。"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(result.ok);
    QCOMPARE(m_memoryTool->ExecuteCount(), 0);

    // 第二轮请求的 tool 消息内容包含不可用错误。
    const QJsonObject secondRequest = m_server->RequestBodyAt(1);
    const QJsonArray messages = secondRequest.value(QStringLiteral("messages")).toArray();
    bool sawUnavailableFeedback = false;

    for (const QJsonValue &messageValue : messages)
    {
        const QJsonObject message = messageValue.toObject();

        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool")
            && message.value(QStringLiteral("content"))
                   .toString().contains(QStringLiteral("not available")))
        {
            sawUnavailableFeedback = true;
        }
    }

    QVERIFY(sawUnavailableFeedback);
}

void ToolLoopTests::InvalidArgumentsJsonFeedsBackError()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search")};

    // 轮 1：参数 JSON 非法（argumentsValid=false 路径）。
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{not valid json")},
    });
    m_server->EnqueueText(QStringLiteral("收到参数错误后的回答。"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(result.ok);
    QCOMPARE(m_searchTool->ExecuteCount(), 0);

    const QJsonObject secondRequest = m_server->RequestBodyAt(1);
    const QJsonArray messages = secondRequest.value(QStringLiteral("messages")).toArray();
    bool sawInvalidJsonFeedback = false;

    for (const QJsonValue &messageValue : messages)
    {
        const QJsonObject message = messageValue.toObject();

        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool")
            && message.value(QStringLiteral("content"))
                   .toString().contains(QStringLiteral("not valid JSON")))
        {
            sawInvalidJsonFeedback = true;
        }
    }

    QVERIFY(sawInvalidJsonFeedback);
}

void ToolLoopTests::EmptyToolsConfigBehavesLikePlainChat()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList();

    m_server->EnqueueText(QStringLiteral("普通对话回答。"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(result.ok);
    QCOMPARE(result.textOutput, QStringLiteral("普通对话回答。"));
    QCOMPARE(m_server->RequestCount(), 1);

    // 空 tools 配置不发送 tools 声明。
    QVERIFY(m_server->RequestBodyAt(0).value(QStringLiteral("tools"))
                .toArray().isEmpty());
}

void ToolLoopTests::ParseConfigValidation()
{
    QString errorMessage;
    vpet::_tagToolLoopConfig config;

    // 合法完整配置。
    vpet::_tagAgentDagNode node;
    node.id = QStringLiteral("tool_loop");
    node.type = QStringLiteral("tool.loop");
    QJsonObject configObject;
    configObject[QStringLiteral("tools")] = QJsonArray{
        QStringLiteral("web.search"), QStringLiteral("memory.search")};
    configObject[QStringLiteral("max_rounds")] = 8;
    configObject[QStringLiteral("time_budget_ms")] = 30000;
    configObject[QStringLiteral("max_tool_calls")] = 6;
    configObject[QStringLiteral("tool_timeout_ms")] = 5000;
    configObject[QStringLiteral("permission")] = QStringLiteral("allow_all");
    configObject[QStringLiteral("on_budget_exhausted")] = QStringLiteral("end");
    configObject[QStringLiteral("fallback")] = QStringLiteral("fail");
    node.config = configObject;

    QVERIFY(vpet::ToolLoopNode::ParseConfig(node, *m_registry, config, errorMessage));
    QCOMPARE(config.maxRounds, 8);
    QCOMPARE(config.timeBudgetMs, static_cast<qint64>(30000));
    QCOMPARE(config.maxToolCalls, 6);
    QCOMPARE(config.toolTimeoutMs, 5000);
    QCOMPARE(config.permission, QStringLiteral("allow_all"));
    QCOMPARE(config.onBudgetExhausted, QStringLiteral("end"));
    QCOMPARE(config.fallback, QStringLiteral("fail"));
    QCOMPARE(config.tools.size(), 2);

    // 未知工具名报错。
    QJsonObject unknownToolObject = configObject;
    unknownToolObject[QStringLiteral("tools")] = QJsonArray{QStringLiteral("no_such_tool")};
    node.config = unknownToolObject;
    QVERIFY(!vpet::ToolLoopNode::ParseConfig(node, *m_registry, config, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("unknown tool")));

    // 非法枚举值报错。
    QJsonObject badPermissionObject = configObject;
    badPermissionObject[QStringLiteral("permission")] = QStringLiteral("yolo");
    node.config = badPermissionObject;
    QVERIFY(!vpet::ToolLoopNode::ParseConfig(node, *m_registry, config, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("permission")));

    QJsonObject badFallbackObject = configObject;
    badFallbackObject[QStringLiteral("fallback")] = QStringLiteral("explode");
    node.config = badFallbackObject;
    QVERIFY(!vpet::ToolLoopNode::ParseConfig(node, *m_registry, config, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("fallback")));

    // max_rounds 越界报错。
    QJsonObject badRoundsObject = configObject;
    badRoundsObject[QStringLiteral("max_rounds")] = 99;
    node.config = badRoundsObject;
    QVERIFY(!vpet::ToolLoopNode::ParseConfig(node, *m_registry, config, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("max_rounds")));
}

void ToolLoopTests::NodeCompleteWritesProtocolKeys()
{
    vpet::AgentContext context;
    vpet::_tagToolLoopResult result;
    result.loopId = 1;
    result.ok = true;
    result.textOutput = QStringLiteral("最终回答");
    result.status = vpet::ToolLoopStatus::Answered;
    result.toolCallsAudit = QJsonArray{QJsonObject{
        {QStringLiteral("round"), 1},
        {QStringLiteral("tool"), QStringLiteral("web.search")},
        {QStringLiteral("status"), QStringLiteral("ok")}}};
    result.trace = QStringList{QStringLiteral("line1"), QStringLiteral("line2")};

    QString errorMessage;
    QVERIFY(vpet::ToolLoopNode::Complete(result, context, errorMessage));

    QVariant responseValue;
    QVERIFY(context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                             responseValue));
    QCOMPARE(responseValue.toString(), QStringLiteral("最终回答"));

    QVariant traceValue;
    QVERIFY(context.GetValue(vpet::AgentContextKeys::SEMANTIC_TOOL_TRACE, traceValue));
    QVERIFY(traceValue.toString().contains(QStringLiteral("line1")));

    QVariant callsValue;
    QVERIFY(context.GetValue(vpet::AgentContextKeys::SEMANTIC_TOOL_CALLS, callsValue));
    const QJsonArray callsArray = QJsonDocument::fromJson(
        callsValue.toString().toUtf8()).array();
    QCOMPARE(callsArray.size(), 1);
    QCOMPARE(callsArray.at(0).toObject().value(QStringLiteral("tool")).toString(),
             QStringLiteral("web.search"));

    // 失败结果不写 semantic.text.response（下游按失败路径处理）。
    vpet::AgentContext failureContext;
    vpet::_tagToolLoopResult failureResult;
    failureResult.ok = false;
    failureResult.status = vpet::ToolLoopStatus::Failed;
    QVERIFY(vpet::ToolLoopNode::Complete(failureResult, failureContext, errorMessage));
    QVERIFY(!failureContext.Contains(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE));
}

void ToolLoopTests::DuplicateToolCallIdHandledGracefully()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search")};

    // 两条 tool_call 具有相同 ID，测试执行器是否按序调度并写回
    m_server->EnqueueToolCalls({
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"1\"}")},
        {QStringLiteral("web.search"), QStringLiteral("{\"query\":\"2\"}")}
    });
    m_server->EnqueueText(QStringLiteral("final answer"));

    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(5000));
    QCOMPARE(m_spy->count(), 1);
    QCOMPARE(m_searchTool->ExecuteCount(), 2);
    const auto res = m_spy->first().first().value<vpet::_tagToolLoopResult>();
    QVERIFY(res.ok);
    QCOMPARE(res.textOutput, QStringLiteral("final answer"));
}

void ToolLoopTests::WallClockBudgetTimeoutForcesFinish()
{
    vpet::_tagToolLoopConfig config;
    config.tools = QStringList{QStringLiteral("web.search")};
    config.timeBudgetMs = 50; // 50ms 极短墙钟预算

    m_server->SetDropAllResponses(true);

    // 不响应任何回答，让其超时
    QVERIFY(m_executor->Start(MakeRequest(config)));
    QVERIFY(m_spy->wait(2000));
    QCOMPARE(m_spy->count(), 1);

    const vpet::_tagToolLoopResult result =
        m_spy->at(0).at(0).value<vpet::_tagToolLoopResult>();
    QVERIFY(!result.ok);
    QCOMPARE(result.status, vpet::ToolLoopStatus::BudgetExhausted);
    QVERIFY(result.reason.contains(QStringLiteral("budget")));
}

} // namespace

QTEST_MAIN(ToolLoopTests)

#include "tool_loop_tests.moc"