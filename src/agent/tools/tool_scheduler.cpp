#include "vpet/agent/tools/tool_scheduler.h"

#include <QTimer>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QVariant>

namespace vpet
{

namespace
{

constexpr int MIN_TOOL_TIMEOUT_MS = 100;
constexpr int MAX_TOOL_TIMEOUT_MS = 120000;
constexpr int DEFAULT_ARGS_DIGEST_CHARS = 200;

} // anonymous namespace

ToolScheduler::ToolScheduler(QObject *parent)
    : QObject(parent)
    , m_registry()
    , m_permissionPolicy(ToolPermissionPolicy::AutoReadonly)
    , m_budget()
    , m_timeoutTimer(new QTimer(this))
{
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout,
            this, &ToolScheduler::OnToolTimeout);
}

ToolScheduler::~ToolScheduler()
{
    CancelActive();
}

void ToolScheduler::SetRegistry(const std::shared_ptr<ToolRegistry> &registry)
{
    m_registry = registry;
}

void ToolScheduler::SetPermissionPolicy(ToolPermissionPolicy policy)
{
    m_permissionPolicy = policy;
}

void ToolScheduler::SetBudget(const _tagToolBudgetConfig &budget)
{
    m_budget = budget;

    if (m_budget.toolTimeoutMs < MIN_TOOL_TIMEOUT_MS)
    {
        m_budget.toolTimeoutMs = MIN_TOOL_TIMEOUT_MS;
    }
    else if (m_budget.toolTimeoutMs > MAX_TOOL_TIMEOUT_MS)
    {
        m_budget.toolTimeoutMs = MAX_TOOL_TIMEOUT_MS;
    }
}

ToolPermissionPolicy ToolScheduler::ParsePermissionPolicy(const QString &policy,
                                                          bool &isValid)
{
    const QString normalizedPolicy = policy.trimmed().toLower();
    isValid = true;

    if (normalizedPolicy == QStringLiteral("auto_readonly"))
    {
        return ToolPermissionPolicy::AutoReadonly;
    }

    if (normalizedPolicy == QStringLiteral("allow_all"))
    {
        return ToolPermissionPolicy::AllowAll;
    }

    if (normalizedPolicy == QStringLiteral("ask"))
    {
        // v2 气泡确认占位：v1 行为等同 auto_readonly。
        return ToolPermissionPolicy::Ask;
    }

    isValid = false;
    return ToolPermissionPolicy::AutoReadonly;
}

ToolGateDecision ToolScheduler::Gate(const _tagToolSpec &spec, QString &denyReason) const
{
    denyReason.clear();

    if (m_permissionPolicy == ToolPermissionPolicy::AllowAll)
    {
        return ToolGateDecision::Allow;
    }

    if (spec.trustTier == ToolTrustTier::ReadOnly)
    {
        return ToolGateDecision::Allow;
    }

    denyReason = QStringLiteral("Tool '%1' has side effects and is denied by the current permission policy.")
                     .arg(spec.name);
    return ToolGateDecision::Deny;
}

bool ToolScheduler::IsGateBlocked(const QString &toolName) const
{
    if (m_registry == nullptr)
    {
        return false;
    }

    const std::shared_ptr<ITool> tool = m_registry->Find(toolName);

    if (tool == nullptr)
    {
        return false;
    }

    QString denyReason;
    return Gate(tool->Spec(), denyReason) == ToolGateDecision::Deny;
}

void ToolScheduler::BeginInvocation()
{
    m_invocationClock.start();
    m_invocationActive = true;
    m_roundCount = 0;
    m_toolCallCount = 0;
    m_auditEntries.clear();
}

void ToolScheduler::NotifyRoundStarted()
{
    m_roundCount += 1;
}

bool ToolScheduler::IsBudgetExhausted(QString &reason) const
{
    reason.clear();

    if (m_roundCount >= m_budget.maxRounds)
    {
        reason = QStringLiteral("Round budget exhausted (%1/%2 rounds).")
                     .arg(m_roundCount)
                     .arg(m_budget.maxRounds);
        return true;
    }

    if (m_toolCallCount >= m_budget.maxToolCalls)
    {
        reason = QStringLiteral("Tool call budget exhausted (%1/%2 calls).")
                     .arg(m_toolCallCount)
                     .arg(m_budget.maxToolCalls);
        return true;
    }

    if (m_invocationActive
        && (m_budget.timeBudgetMs > 0)
        && (m_invocationClock.elapsed() >= m_budget.timeBudgetMs))
    {
        reason = QStringLiteral("Time budget exhausted (%1 ms).")
                     .arg(m_budget.timeBudgetMs);
        return true;
    }

    return false;
}

int ToolScheduler::RoundCount() const
{
    return m_roundCount;
}

bool ToolScheduler::ExecuteCall(const _tagToolCall &call, int round)
{
    if (IsBusy())
    {
        // Sequential 单飞：上一调用未收束前不接受新调用。
        return false;
    }

    if (m_registry == nullptr)
    {
        FinalizeCall(call, round,
                     MakeFailureResult(QStringLiteral("Tool registry is unavailable.")),
                     QStringLiteral("unknown_tool"), 0);
        return true;
    }

    if (m_toolCallCount >= m_budget.maxToolCalls || round > m_budget.maxRounds
        || (m_invocationActive && m_budget.timeBudgetMs > 0
            && m_invocationClock.elapsed() >= m_budget.timeBudgetMs))
    {
        FinalizeCall(call, round,
                     MakeFailureResult(QStringLiteral("Tool call budget is exhausted; no further tool calls are allowed.")),
                     QStringLiteral("budget_exhausted"), 0);
        return true;
    }

    m_toolCallCount += 1;

    const std::shared_ptr<ITool> tool = m_registry->Find(call.toolName);

    if (tool == nullptr)
    {
        FinalizeCall(call, round,
                     MakeFailureResult(QStringLiteral("Unknown tool '%1'; no tool call was executed.")
                                           .arg(call.toolName)),
                     QStringLiteral("unknown_tool"), 0);
        return true;
    }

    const _tagToolSpec spec = tool->Spec();
    QString denyReason;

    if (Gate(spec, denyReason) == ToolGateDecision::Deny)
    {
        FinalizeCall(call, round, MakeFailureResult(denyReason),
                     QStringLiteral("denied"), 0);
        return true;
    }

    QString validationError;

    if (!tool->ValidateArguments(call.arguments, validationError))
    {
        FinalizeCall(call, round, MakeFailureResult(validationError),
                     QStringLiteral("invalid_arguments"), 0);
        return true;
    }

    if (tool->IsBusy()) return false;
    m_activeOwner = tool;
    m_activeTool = tool.get();
    m_activeCall = call;
    m_activeCall.executionId = ++m_nextExecutionId;
    m_activeRound = round;
    m_callClock.start();
    m_timeoutTimer->start(m_budget.toolTimeoutMs);

    connect(tool.get(), &ITool::Completed,
            this, &ToolScheduler::OnToolCompleted, Qt::UniqueConnection);

    tool->Execute(m_activeCall);
    return true;
}

void ToolScheduler::CancelActive()
{
    if (m_activeTool == nullptr)
    {
        return;
    }

    ITool *tool = m_activeTool;
    const int activeRound = m_activeRound;
    const _tagToolCall activeCall = TakeActiveCall(true);
    tool->Cancel();

    FinalizeCall(activeCall, activeRound,
                 MakeFailureResult(QStringLiteral("Tool call was cancelled.")),
                 QStringLiteral("cancelled"), m_callClock.elapsed());
}

bool ToolScheduler::IsBusy() const
{
    return m_activeTool != nullptr;
}

const QVector<_tagSchedulerAuditEntry> &ToolScheduler::AuditEntries() const
{
    return m_auditEntries;
}

QJsonArray ToolScheduler::BuildAuditJson() const
{
    QJsonArray auditArray;

    for (const _tagSchedulerAuditEntry &entry : m_auditEntries)
    {
        QJsonObject entryObject;
        entryObject[QStringLiteral("round")] = entry.round;
        entryObject[QStringLiteral("tool")] = entry.tool;
        entryObject[QStringLiteral("status")] = entry.status;
        entryObject[QStringLiteral("duration_ms")] = static_cast<double>(entry.durationMs);
        entryObject[QStringLiteral("args_digest")] = entry.argsDigest;

        if (!entry.detail.isEmpty())
        {
            entryObject[QStringLiteral("error")] = entry.detail;
        }

        auditArray.append(entryObject);
    }

    return auditArray;
}

void ToolScheduler::OnToolCompleted(const _tagToolExecutionResult &result)
{
    ITool *tool = qobject_cast<ITool *>(sender());

    // 迟到信号（超时或取消后）与活动调用不匹配时丢弃。
    if ((tool == nullptr) || (tool != m_activeTool)
        || result.executionId != m_activeCall.executionId)
    {
        return;
    }

    const qint64 durationMs = m_callClock.elapsed();
    const int activeRound = m_activeRound;
    const _tagToolCall activeCall = TakeActiveCall(true);
    const QString status = result.ok ? QStringLiteral("ok") : QStringLiteral("error");

    FinalizeCall(activeCall, activeRound, result, status, durationMs);
}

void ToolScheduler::OnToolTimeout()
{
    if (m_activeTool == nullptr)
    {
        return;
    }

    ITool *tool = m_activeTool;
    const qint64 durationMs = m_callClock.elapsed();
    const int activeRound = m_activeRound;
    const _tagToolCall activeCall = TakeActiveCall(true);

    // 超时不中断循环：取消工具后合成失败结果回喂 LLM。
    tool->Cancel();

    FinalizeCall(activeCall, activeRound,
                 MakeFailureResult(QStringLiteral("Tool call timed out after %1 ms.")
                                       .arg(m_budget.toolTimeoutMs)),
                 QStringLiteral("timeout"), durationMs);
}

_tagToolCall ToolScheduler::TakeActiveCall(bool stopTimer)
{
    if (stopTimer)
    {
        m_timeoutTimer->stop();
    }

    m_activeTool = nullptr;
    m_activeOwner.reset();
    const _tagToolCall activeCall = m_activeCall;
    m_activeCall = _tagToolCall();
    m_activeRound = 0;

    return activeCall;
}

void ToolScheduler::FinalizeCall(const _tagToolCall &call,
                                 int round,
                                 const _tagToolExecutionResult &result,
                                 const QString &status,
                                 qint64 durationMs)
{
    _tagSchedulerAuditEntry entry;
    entry.round = round;
    entry.tool = call.toolName;
    entry.status = status;
    entry.durationMs = durationMs;
    entry.argsDigest = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(call.arguments).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    entry.detail = result.ok ? QString() : result.textOutput;
    m_auditEntries.append(entry);

    emit ToolCallFinished(call, result);
}

_tagToolExecutionResult ToolScheduler::MakeFailureResult(const QString &textOutput)
{
    _tagToolExecutionResult result;
    result.ok = false;
    result.textOutput = textOutput;
    return result;
}

} // namespace vpet
