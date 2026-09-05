#ifndef VPET_AGENT_TOOLS_TOOL_SCHEDULER_H
#define VPET_AGENT_TOOLS_TOOL_SCHEDULER_H

#include "vpet/agent/tools/itool.h"
#include "vpet/agent/tools/tool_registry.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QTimer;

namespace vpet
{

/**
 * @brief 权限门控决策结果。
 */
enum class ToolGateDecision
{
    Allow, ///< 放行
    Deny   ///< 拒绝（拒绝原因回喂 LLM）
};

/**
 * @brief 权限策略。
 *
 * v1 提供只读放行（auto_readonly）与开发档全放行（allow_all）；
 * ask 为 v2 气泡确认占位，v1 行为等同 auto_readonly。
 */
enum class ToolPermissionPolicy
{
    AutoReadonly, ///< 只读放行，副作用拒绝
    AllowAll,     ///< 全部放行（开发档）
    Ask           ///< v2 气泡确认占位，v1 等同 AutoReadonly
};

/**
 * @brief 工具循环预算配置。
 */
struct _tagToolBudgetConfig
{
    int maxRounds = 4;           ///< LLM 循环轮数硬上限
    qint64 timeBudgetMs = 20000; ///< 墙钟预算（毫秒）
    int maxToolCalls = 12;       ///< 单次 invocation 工具调用总次数上限
    int toolTimeoutMs = 10000;   ///< 单工具调用超时（毫秒）
};

/**
 * @brief 单条工具调用审计记录（对应 pi 的 afterToolCall 语义）。
 */
struct _tagSchedulerAuditEntry
{
    int round = 0;      ///< 所属 LLM 循环轮次（从 1 开始）
    QString tool;       ///< 工具名
    QString status;     ///< ok/error/denied/budget_exhausted/timeout/cancelled/invalid_arguments/unknown_tool
    qint64 durationMs = 0; ///< 执行耗时
    QString argsDigest; ///< 参数摘要
    QString detail;     ///< 拒绝原因或脱敏错误描述
};

/**
 * @brief 工具调度纪律层。
 *
 * 纯逻辑、可单测。职责：权限门控（Gate）、预算检查（IsBudgetExhausted）、
 * 单工具超时 watchdog、取消传播（CancelActive）、审计（AuditEntries）与
 * Sequential 单飞执行（ExecuteCall）。错误一律以 ok=false 结果返回回喂，
 * 不中断整个循环。
 */
class ToolScheduler : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造调度器。
     * @param[in] parent QObject 父对象。
     */
    explicit ToolScheduler(QObject *parent = nullptr);

    /**
     * @brief 析构调度器并取消活动调用。
     */
    ~ToolScheduler() override;

    /**
     * @brief 设置工具注册表。
     * @param[in] registry 注册表实例。
     */
    void SetRegistry(const std::shared_ptr<ToolRegistry> &registry);

    /**
     * @brief 设置权限策略。
     * @param[in] policy 权限策略。
     */
    void SetPermissionPolicy(ToolPermissionPolicy policy);

    /**
     * @brief 设置预算配置。
     * @param[in] budget 预算配置。
     */
    void SetBudget(const _tagToolBudgetConfig &budget);

    /**
     * @brief 解析权限策略字符串。
     * @param[in] policy 策略字符串：auto_readonly/allow_all/ask。
     * @param[out] isValid 字符串是否为合法枚举值。
     * @return 解析出的策略；非法时返回 AutoReadonly。
     */
    static ToolPermissionPolicy ParsePermissionPolicy(const QString &policy,
                                                      bool &isValid);

    /**
     * @brief 权限门控（对应 pi 的 beforeToolCall）。
     * @param[in] spec 工具规格。
     * @param[out] denyReason 拒绝原因；放行时为空。
     * @return 门控决策。
     */
    ToolGateDecision Gate(const _tagToolSpec &spec, QString &denyReason) const;

    /**
     * @brief 判断工具在当前策略下是否会被门控阻塞。
     *
     * v1 只按 trustTier 判断；拖拽中/播报中等忙闲状态待组件落地后接入。
     * @param[in] toolName 工具名。
     * @return 会被阻塞返回 true；工具未注册返回 false。
     */
    bool IsGateBlocked(const QString &toolName) const;

    /**
     * @brief 开始一次 invocation 的预算窗口。
     *
     * 记录墙钟起点并清零轮数、调用计数与审计记录。
     */
    void BeginInvocation();

    /**
     * @brief 记录一轮 LLM 循环开始。
     */
    void NotifyRoundStarted();

    /**
     * @brief 判断预算是否耗尽（对应 pi 的 shouldStopAfterTurn）。
     * @param[out] reason 耗尽原因；未耗尽时为空。
     * @return 预算耗尽返回 true。
     */
    bool IsBudgetExhausted(QString &reason) const;

    /**
     * @brief 返回已开始的轮数。
     * @return 轮数。
     */
    int RoundCount() const;

    /**
     * @brief 顺序执行一次工具调用（异步）。
     *
     * 未知工具、权限拒绝、参数校验失败与预算耗尽均合成 ok=false 结果
     * 通过 ToolCallFinished 返回；真实执行带超时 watchdog。
     * @param[in] call 工具调用请求。
     * @param[in] round 所属轮次（审计用）。
     * @return 调用已受理返回 true；调度器忙（Sequential 单飞）返回 false。
     */
    bool ExecuteCall(const _tagToolCall &call, int round);

    /**
     * @brief 取消活动工具调用并合成 cancelled 结果（取消传播）。
     */
    void CancelActive();

    /**
     * @brief 判断是否有活动工具调用。
     * @return 有活动调用返回 true。
     */
    bool IsBusy() const;

    /**
     * @brief 返回本 invocation 的审计记录。
     * @return 审计记录列表。
     */
    const QVector<_tagSchedulerAuditEntry> &AuditEntries() const;

    /**
     * @brief 生成 semantic.tool.calls 审计 JSON 数组。
     * @return 审计 JSON 数组：{round, tool, status, duration_ms, args_digest[, error]}。
     */
    QJsonArray BuildAuditJson() const;

signals:
    /**
     * @brief 一次工具调用收束（成功、失败、拒绝、超时或取消均通过该信号）。
     * @param[in] call 原工具调用请求。
     * @param[in] result 结构化执行结果。
     */
    void ToolCallFinished(const vpet::_tagToolCall &call,
                          const vpet::_tagToolExecutionResult &result);

private slots:
    /**
     * @brief 处理工具完成信号。
     * @param[in] result 结构化执行结果。
     */
    void OnToolCompleted(const vpet::_tagToolExecutionResult &result);

    /**
     * @brief 处理单工具调用超时。
     */
    void OnToolTimeout();

private:
    /**
     * @brief 清空活动调用状态并返回原调用请求。
     * @param[in] stopTimer 是否同时停止超时定时器。
     * @return 原活动调用请求。
     */
    _tagToolCall TakeActiveCall(bool stopTimer);

    /**
     * @brief 记录审计并发出收束信号。
     * @param[in] call 工具调用请求。
     * @param[in] round 所属轮次。
     * @param[in] result 执行结果。
     * @param[in] status 审计状态。
     * @param[in] durationMs 执行耗时；合成结果传 0。
     */
    void FinalizeCall(const _tagToolCall &call,
                      int round,
                      const _tagToolExecutionResult &result,
                      const QString &status,
                      qint64 durationMs);

    /**
     * @brief 构造合成的失败结果。
     * @param[in] textOutput 脱敏错误描述。
     * @return 失败结果。
     */
    static _tagToolExecutionResult MakeFailureResult(const QString &textOutput);

    std::shared_ptr<ToolRegistry> m_registry; ///< 工具注册表
    ToolPermissionPolicy m_permissionPolicy;  ///< 权限策略
    _tagToolBudgetConfig m_budget;            ///< 预算配置

    QElapsedTimer m_invocationClock;   ///< invocation 墙钟
    bool m_invocationActive = false;   ///< 预算窗口是否已开启
    int m_roundCount = 0;              ///< 已开始的轮数
    int m_toolCallCount = 0;           ///< 已受理的工具调用数

    quint64 m_nextExecutionId = 0;
    std::shared_ptr<ITool> m_activeOwner;
    ITool *m_activeTool = nullptr;     ///< 活动工具（生命周期由注册表持有）
    _tagToolCall m_activeCall;         ///< 活动调用请求
    int m_activeRound = 0;             ///< 活动调用所属轮次
    QElapsedTimer m_callClock;         ///< 活动调用计时
    QTimer *m_timeoutTimer = nullptr;  ///< 单工具超时 watchdog

    QVector<_tagSchedulerAuditEntry> m_auditEntries; ///< 审计记录
};

} // namespace vpet

#endif // VPET_AGENT_TOOLS_TOOL_SCHEDULER_H
