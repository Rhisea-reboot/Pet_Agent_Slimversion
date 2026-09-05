#include "vpet/agent/tools/tool_loop_executor.h"

#include <QTimer>
#include <QVariant>

namespace vpet
{

namespace
{

constexpr int MAX_HISTORY_LINES = 20;        ///< 注入消息的历史条数上限
constexpr int MAX_TOOL_OUTPUT_CHARS = 4000;  ///< 单条工具回喂文本上限

} // anonymous namespace

QString ToolLoopStatusToString(ToolLoopStatus status)
{
    switch (status)
    {
    case ToolLoopStatus::Answered:
        return QStringLiteral("answered");

    case ToolLoopStatus::BudgetExhausted:
        return QStringLiteral("budget_exhausted");

    case ToolLoopStatus::FallbackPlainChat:
        return QStringLiteral("fallback_plain_chat");

    case ToolLoopStatus::Cancelled:
        return QStringLiteral("cancelled");

    case ToolLoopStatus::Failed:
        return QStringLiteral("failed");
    }

    return QStringLiteral("failed");
}

ToolLoopExecutor::ToolLoopExecutor(QObject *parent)
    : QObject(parent)
    , m_llmClient(nullptr)
    , m_registry()
    , m_scheduler(new ToolScheduler(this))
    , m_nextLoopId(0)
    , m_activeLoopId(-1)
    , m_busy(false)
    , m_cancelRequested(false)
    , m_activeLlmRequestId(-1)
    , m_fallbackActive(false)
    , m_pendingToolIndex(0)
    , m_currentRound(0)
    , m_result()
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this]() {
        const int requestId = m_activeLlmRequestId;
        Finish(ToolLoopStatus::BudgetExhausted, QString(), QStringLiteral("Wall-clock budget exhausted."));
        if (m_llmClient && requestId > 0) m_llmClient->CancelRequest(requestId);
        m_scheduler->CancelActive();
    });
    connect(m_scheduler.get(), &ToolScheduler::ToolCallFinished,
            this, &ToolLoopExecutor::OnToolCallFinished);
}

ToolLoopExecutor::~ToolLoopExecutor()
{
    Cancel();
}

void ToolLoopExecutor::SetLlmClient(LlmClient *client)
{
    if (m_llmClient == client)
    {
        return;
    }

    if (m_llmClient != nullptr)
    {
        disconnect(m_llmClient, nullptr, this, nullptr);
    }

    Cancel();
    m_llmClient = client;

    if (m_llmClient != nullptr)
    {
        connect(m_llmClient, &LlmClient::ChatCompleted,
                this, &ToolLoopExecutor::OnLlmChatCompleted);
        connect(m_llmClient, &LlmClient::ChatToolCallsCompleted,
                this, &ToolLoopExecutor::OnLlmToolCallsCompleted);
        connect(m_llmClient, &LlmClient::ChatFailed,
                this, &ToolLoopExecutor::OnLlmChatFailed);
        connect(m_llmClient, &LlmClient::ChatToolsUnsupported,
                this, &ToolLoopExecutor::OnLlmToolsUnsupported);
    }
}

void ToolLoopExecutor::SetToolRegistry(const std::shared_ptr<ToolRegistry> &registry)
{
    m_registry = registry;
}

int ToolLoopExecutor::PeekNextLoopId() const
{
    return m_nextLoopId + 1;
}

bool ToolLoopExecutor::Start(const _tagToolLoopRequest &request)
{
    if (m_busy || (m_llmClient == nullptr) || !m_llmClient->IsConfigured())
    {
        return false;
    }

    if (request.promptText.trimmed().isEmpty())
    {
        return false;
    }

    m_terminalRound = false;
    m_allTerminate = false;
    if (request.config.timeBudgetMs > 0)
        m_deadline.start(static_cast<int>(qMin<qint64>(request.config.timeBudgetMs, 2147483647)));
    m_request = request;
    m_busy = true;
    m_cancelRequested = false;
    m_fallbackActive = false;
    m_currentRound = 0;
    m_activeLlmRequestId = -1;
    m_pendingToolCalls.clear();
    m_pendingToolIndex = 0;
    m_result = _tagToolLoopResult();
    m_result.loopId = ++m_nextLoopId;
    m_activeLoopId = m_result.loopId;

    m_scheduler->SetRegistry(m_registry);

    bool permissionValid = false;
    m_scheduler->SetPermissionPolicy(
        ToolScheduler::ParsePermissionPolicy(m_request.config.permission, permissionValid));

    _tagToolBudgetConfig budget;
    budget.maxRounds = qMax(1, m_request.config.maxRounds);
    budget.timeBudgetMs = qMax<qint64>(0, m_request.config.timeBudgetMs);
    budget.maxToolCalls = qMax(1, m_request.config.maxToolCalls);
    budget.toolTimeoutMs = qMax(100, m_request.config.toolTimeoutMs);
    m_scheduler->SetBudget(budget);
    m_scheduler->BeginInvocation();

    AppendTrace(QStringLiteral("loop %1 开始（tools=%2, max_rounds=%3）")
                    .arg(m_result.loopId)
                    .arg(m_request.config.tools.join(QStringLiteral(",")))
                    .arg(budget.maxRounds));

    AssembleMessages();

    if (!BeginRound(true))
    {
        return false;
    }

    return true;
}

void ToolLoopExecutor::Cancel()
{
    if (!m_busy || m_cancelRequested)
    {
        return;
    }

    m_cancelRequested = true;
    AppendTrace(QStringLiteral("loop %1 收到取消请求").arg(m_activeLoopId));

    if (m_activeLlmRequestId > 0)
    {
        const int requestId = m_activeLlmRequestId;
        m_activeLlmRequestId = -1;
        if (m_llmClient) m_llmClient->CancelRequest(requestId);
    }

    if (m_scheduler->IsBusy())
    {
        m_scheduler->CancelActive();
        // OnToolCallFinished 收到取消结果后执行取消收束。
        return;
    }

    Finish(ToolLoopStatus::Cancelled, QString(),
           QStringLiteral("Tool loop was cancelled."));
}

bool ToolLoopExecutor::IsBusy() const
{
    return m_busy;
}

void ToolLoopExecutor::OnLlmChatCompleted(int requestId, const QString &content)
{
    if (!m_busy || (requestId != m_activeLlmRequestId))
    {
        return;
    }

    m_activeLlmRequestId = -1;
    const QString finalText = content.trimmed();

    if (finalText.isEmpty())
    {
        Finish(ToolLoopStatus::Failed, QString(),
               QStringLiteral("LLM returned an empty final response."));
        return;
    }

    if (m_fallbackActive)
    {
        Finish(ToolLoopStatus::FallbackPlainChat, finalText, QString());
        return;
    }

    Finish(ToolLoopStatus::Answered, finalText, QString());
}

void ToolLoopExecutor::OnLlmToolCallsCompleted(int requestId,
                                               const QVector<_tagLlmToolCall> &toolCalls)
{
    if (!m_busy || (requestId != m_activeLlmRequestId))
    {
        return;
    }

    m_activeLlmRequestId = -1;

    if (m_fallbackActive || m_terminalRound)
    {
        // 降级模式下端点不应再返回工具调用；按失败收束避免循环失控。
        Finish(ToolLoopStatus::Failed, QString(),
               QStringLiteral("Endpoint returned tool calls in plain-chat fallback mode."));
        return;
    }

    _tagLlmMessage assistantMessage;
    assistantMessage.role = LLM_MESSAGE_ROLE::ASSISTANT;
    assistantMessage.toolCalls = toolCalls;
    m_messages.append(assistantMessage);

    m_allTerminate = !toolCalls.isEmpty();
    m_pendingToolCalls = toolCalls;
    m_pendingToolIndex = 0;

    AppendTrace(QStringLiteral("round %1: LLM 请求 %2 个工具调用")
                    .arg(m_currentRound)
                    .arg(toolCalls.size()));

    DispatchNextToolCall();
}

void ToolLoopExecutor::OnLlmChatFailed(int requestId, const QString &message, int statusCode)
{
    Q_UNUSED(statusCode);

    if (!m_busy || (requestId != m_activeLlmRequestId))
    {
        return;
    }

    m_activeLlmRequestId = -1;

    if (m_cancelRequested)
    {
        Finish(ToolLoopStatus::Cancelled, QString(),
               QStringLiteral("Tool loop was cancelled."));
        return;
    }

    Finish(ToolLoopStatus::Failed, QString(),
           QStringLiteral("LLM request failed: %1").arg(message));
}

void ToolLoopExecutor::OnLlmToolsUnsupported(int requestId,
                                             const QString &message,
                                             int statusCode)
{
    Q_UNUSED(statusCode);

    if (!m_busy || (requestId != m_activeLlmRequestId))
    {
        return;
    }

    m_activeLlmRequestId = -1;
    HandleToolsUnsupported(message, statusCode);
}

void ToolLoopExecutor::OnToolCallFinished(const _tagToolCall &call,
                                          const _tagToolExecutionResult &result)
{
    if (!m_busy)
    {
        return;
    }

    if (m_cancelRequested)
    {
        Finish(ToolLoopStatus::Cancelled, QString(),
               QStringLiteral("Tool loop was cancelled."));
        return;
    }

    _tagLlmMessage toolMessage;
    toolMessage.role = LLM_MESSAGE_ROLE::TOOL;
    toolMessage.toolCallId = call.callId;
    toolMessage.content = FormatToolFeedback(result);
    m_messages.append(toolMessage);

    AppendTrace(QStringLiteral("round %1: 工具 %2 %3")
                    .arg(m_currentRound)
                    .arg(call.toolName)
                    .arg(result.ok ? QStringLiteral("成功") : QStringLiteral("失败")));

    m_allTerminate = m_allTerminate && result.terminateHint;
    // Defer synchronous tool results to avoid recursive dispatch stacks.
    const int loopId = m_activeLoopId;
    QTimer::singleShot(0, this, [this, loopId]() {
        if (m_busy && m_activeLoopId == loopId) DispatchNextToolCall();
    });
}

bool ToolLoopExecutor::BeginRound(bool withTools)
{
    // 预算检查先于轮次计数：max_rounds 限制常规轮数，收束作答不计入。
    QString budgetReason;

    if (m_scheduler->IsBudgetExhausted(budgetReason))
    {
        HandleBudgetExhausted(budgetReason);
        return true;
    }

    m_currentRound += 1;
    m_scheduler->NotifyRoundStarted();
    return SendRound(withTools);
}

bool ToolLoopExecutor::SendRound(bool withTools)
{
    if (!m_llmClient) {
        Finish(ToolLoopStatus::Failed, QString(), QStringLiteral("LLM client unavailable."));
        return false;
    }
    m_terminalRound = !withTools;
    _tagLlmRequestOptions options;
    options.stream = false;

    // 空 tools 配置等价 llm.chat：显式配置为空列表时不发送任何 tools 声明。
    const bool toolsAvailable = !m_request.config.tools.isEmpty()
                                && (m_registry != nullptr)
                                && !m_registry->BuildLlmToolsArray(
                                       m_request.config.tools).isEmpty();

    if (withTools && !m_fallbackActive && toolsAvailable)
    {
        options.tools = m_registry->BuildLlmToolsArray(m_request.config.tools);

        if (!options.tools.isEmpty())
        {
            options.toolChoice = QStringLiteral("auto");
        }
    }

    const int requestId = m_llmClient->SendChat(m_messages, options);

    if (requestId <= 0)
    {
        Finish(ToolLoopStatus::Failed, QString(),
               QStringLiteral("Failed to send LLM request for round %1.").arg(m_currentRound));
        return false;
    }

    m_activeLlmRequestId = requestId;
    AppendTrace(QStringLiteral("round %1: 发送 LLM 请求 %2（%3 个 tools 声明）")
                    .arg(m_currentRound)
                    .arg(requestId)
                    .arg(options.tools.size()));
    return true;
}

void ToolLoopExecutor::AssembleMessages()
{
    m_messages.clear();

    if (!m_request.systemPrompt.trimmed().isEmpty())
    {
        _tagLlmMessage systemMessage;
        systemMessage.role = LLM_MESSAGE_ROLE::SYSTEM;
        systemMessage.content = m_request.systemPrompt.trimmed();
        m_messages.append(systemMessage);
    }

    QStringList historyLines;

    for (const QString &entry : m_request.conversationHistory)
    {
        const QString normalizedEntry = entry.trimmed();

        if (!normalizedEntry.isEmpty())
        {
            historyLines.append(normalizedEntry);
        }
    }

    if (historyLines.size() > MAX_HISTORY_LINES)
    {
        historyLines = historyLines.mid(historyLines.size() - MAX_HISTORY_LINES);
    }

    if (!historyLines.isEmpty())
    {
        _tagLlmMessage historyMessage;
        historyMessage.role = LLM_MESSAGE_ROLE::USER;
        historyMessage.content = QStringLiteral("【对话历史（旧→新，仅供参考）】\n%1")
                                     .arg(historyLines.join(QStringLiteral("\n")));
        m_messages.append(historyMessage);
    }

    _tagLlmMessage promptMessage;
    promptMessage.role = LLM_MESSAGE_ROLE::USER;
    promptMessage.content = m_request.promptText.trimmed();
    m_messages.append(promptMessage);
}

void ToolLoopExecutor::DispatchNextToolCall()
{
    while (m_pendingToolIndex < m_pendingToolCalls.size())
    {
        const _tagLlmToolCall toolCall = m_pendingToolCalls.at(m_pendingToolIndex);
        m_pendingToolIndex += 1;

        _tagToolCall call;
        call.callId = toolCall.id;
        call.toolName = toolCall.name;
        call.arguments = toolCall.arguments;

        if (!toolCall.argumentsValid)
        {
            _tagLlmMessage toolMessage;
            toolMessage.role = LLM_MESSAGE_ROLE::TOOL;
            toolMessage.toolCallId = call.callId;
            toolMessage.content = QStringLiteral(
                "Tool call failed: arguments are not valid JSON.");
            m_messages.append(toolMessage);
            AppendTrace(QStringLiteral("round %1: 工具 %2 参数 JSON 非法，已回喂错误")
                            .arg(m_currentRound)
                            .arg(call.toolName));
            continue;
        }

        if (!m_request.config.tools.contains(call.toolName))
        {
            _tagLlmMessage toolMessage;
            toolMessage.role = LLM_MESSAGE_ROLE::TOOL;
            toolMessage.toolCallId = call.callId;
            toolMessage.content = QStringLiteral("Tool '%1' is not available in this loop.")
                                      .arg(call.toolName);
            m_messages.append(toolMessage);
            AppendTrace(QStringLiteral("round %1: 工具 %2 不在允许列表，已回喂错误")
                            .arg(m_currentRound)
                            .arg(call.toolName));
            continue;
        }

        if (!m_scheduler->ExecuteCall(call, m_currentRound))
        {
            _tagLlmMessage toolMessage;
            toolMessage.role = LLM_MESSAGE_ROLE::TOOL;
            toolMessage.toolCallId = call.callId;
            toolMessage.content = QStringLiteral(
                "Tool call failed: the scheduler is busy.");
            m_messages.append(toolMessage);
            AppendTrace(QStringLiteral("round %1: 调度器忙，工具 %2 已回喂错误")
                            .arg(m_currentRound)
                            .arg(call.toolName));
            continue;
        }

        // 等待 OnToolCallFinished。
        return;
    }

    m_pendingToolCalls.clear();
    m_pendingToolIndex = 0;
    if (m_allTerminate) SendRound(false);
    else BeginRound(true);
}

void ToolLoopExecutor::HandleBudgetExhausted(const QString &reason)
{
    AppendTrace(QStringLiteral("预算耗尽：%1").arg(reason));

    if (m_request.config.onBudgetExhausted.trimmed().toLower()
        == QStringLiteral("end"))
    {
        Finish(ToolLoopStatus::BudgetExhausted, QString(), reason);
        return;
    }

    // answer_with_context（默认）：基于当前转写做一次无工具的收束作答；
    // 直发不重复预算检查，避免递归。
    m_result.reason = reason;
    SendRound(false);
}

void ToolLoopExecutor::HandleToolsUnsupported(const QString &message, int statusCode)
{
    Q_UNUSED(statusCode);
    AppendTrace(QStringLiteral("端点不支持 tools（%1）").arg(message));

    const QString fallback = m_request.config.fallback.trimmed().toLower();

    if (fallback != QStringLiteral("plain_chat"))
    {
        Finish(ToolLoopStatus::Failed, QString(),
               QStringLiteral("Endpoint does not support tools: %1").arg(message));
        return;
    }

    m_fallbackActive = true;
    AppendTrace(QStringLiteral("降级为普通对话（plain_chat）"));
    BeginRound(false);
}

void ToolLoopExecutor::Finish(ToolLoopStatus status,
                              const QString &textOutput,
                              const QString &reason)
{
    if (!m_busy)
    {
        return;
    }

    m_deadline.stop();
    m_busy = false;
    m_activeLlmRequestId = -1;
    m_cancelRequested = false;
    m_pendingToolCalls.clear();
    m_pendingToolIndex = 0;

    m_result.ok = (status == ToolLoopStatus::Answered)
                  || (status == ToolLoopStatus::FallbackPlainChat)
                  || ((status == ToolLoopStatus::BudgetExhausted) && !textOutput.trimmed().isEmpty());
    m_result.textOutput = textOutput.trimmed();
    m_result.status = status;
    m_result.reason = reason.isEmpty() ? m_result.reason : reason;
    m_result.toolCallsAudit = m_scheduler->BuildAuditJson();

    AppendTrace(QStringLiteral("loop %1 收束：%2").arg(m_result.loopId)
                    .arg(ToolLoopStatusToString(status)));

    const _tagToolLoopResult result = m_result;
    m_activeLoopId = -1;

    // 延迟到事件循环发出，避免在节点处理器调用栈内同步恢复 DAG。
    QTimer::singleShot(0, this, [this, result]()
    {
        emit Finished(result);
    });
}

void ToolLoopExecutor::AppendTrace(const QString &line)
{
    m_result.trace.append(line);
}

QString ToolLoopExecutor::ClampToolOutput(const QString &text)
{
    const QString normalizedText = text.trimmed();

    if (normalizedText.size() <= MAX_TOOL_OUTPUT_CHARS)
    {
        return normalizedText;
    }

    return normalizedText.left(MAX_TOOL_OUTPUT_CHARS)
           + QStringLiteral("...(truncated)");
}

QString ToolLoopExecutor::FormatToolFeedback(const _tagToolExecutionResult &result)
{
    const QString clampedText = ClampToolOutput(result.textOutput);

    if (result.ok)
    {
        return clampedText;
    }

    return QStringLiteral("Tool call failed: %1").arg(clampedText);
}

} // namespace vpet
