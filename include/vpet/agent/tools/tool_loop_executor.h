#ifndef VPET_AGENT_TOOLS_TOOL_LOOP_EXECUTOR_H
#define VPET_AGENT_TOOLS_TOOL_LOOP_EXECUTOR_H

#include "vpet/agent/tools/tool_scheduler.h"
#include "vpet/llm/llm_client.h"

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace vpet
{

/**
 * @brief tool.loop 循环收束状态。
 */
enum class ToolLoopStatus
{
    Answered,         ///< LLM 给出最终文本
    BudgetExhausted,  ///< 预算耗尽后按策略收束
    FallbackPlainChat, ///< 端点不支持 tools，降级为普通对话
    Cancelled,        ///< 被取消（invocation 替换或退出）
    Failed            ///< 失败（LLM 错误、配置错误等）
};

/**
 * @brief tool.loop 状态字符串。
 */
QString ToolLoopStatusToString(ToolLoopStatus status);

/**
 * @brief tool.loop 节点配置。
 */
struct _tagToolLoopConfig
{
    QStringList tools;             ///< 允许的工具名（引用 ToolRegistry；空数组等价 llm.chat）
    int maxRounds = 4;             ///< LLM 循环轮数硬上限
    qint64 timeBudgetMs = 20000;   ///< 墙钟预算
    int maxToolCalls = 12;         ///< 工具调用总次数上限
    int toolTimeoutMs = 10000;     ///< 单工具调用超时
    QString permission = QStringLiteral("auto_readonly"); ///< 权限策略
    QString onBudgetExhausted = QStringLiteral("answer_with_context"); ///< 预算收束策略
    QString fallback = QStringLiteral("plain_chat"); ///< tools 不支持降级策略
};

/**
 * @brief tool.loop 请求。
 *
 * 消息组装边界输入：system 提示词、截尾对话历史与本轮提示词。
 * 本轮工具转写由执行器内部维护，不进入跨轮上下文。
 */
struct _tagToolLoopRequest
{
    QString systemPrompt;            ///< 附加 system 文本；context.md 由 llm_client 自动注入
    QStringList conversationHistory; ///< 截尾前的对话历史（"user: x"/"assistant: y"）
    QString promptText;              ///< 本轮提示词（含记忆/研究注入）
    _tagToolLoopConfig config;       ///< 节点配置
};

/**
 * @brief tool.loop 执行结果。
 */
struct _tagToolLoopResult
{
    int loopId = -1;                 ///< 本次循环标识
    bool ok = false;                 ///< 是否产出最终文本
    QString textOutput;              ///< 最终回复文本（ok=true 时有效）
    ToolLoopStatus status = ToolLoopStatus::Failed; ///< 收束状态
    QString reason;                  ///< 预算耗尽/失败原因描述
    QJsonArray toolCallsAudit;       ///< semantic.tool.calls 审计数组
    QStringList trace;               ///< semantic.tool.trace 人类可读日志
};

/**
 * @brief tool.loop 节点内核。
 *
 * 持有本轮消息列表并循环执行：组装消息 -> LLM 调用（带 tools 声明）->
 * 无 tool_calls 则收束；有 tool_calls 则 Gate -> 校验 -> 执行 -> 结果回填。
 * 中间过程不冒泡；最终文本通过 Finished 信号返回。
 *
 * LLM 信号接线：执行器直接连接所注入 LlmClient 的完成/失败/工具调用/
 * tools 不支持四类信号；AgentRuntime 的同名处理器须跳过 tool.loop 节点
 * 的请求以避免双重处理。
 */
class ToolLoopExecutor : public QObject
{
    Q_OBJECT

public:
    explicit ToolLoopExecutor(QObject *parent = nullptr);

    /**
     * @brief 析构并取消活动循环。
     */
    ~ToolLoopExecutor() override;

    /**
     * @brief 注入文本 LLM 客户端（生命周期由调用方持有）。
     * @param[in] client LLM 客户端。
     */
    void SetLlmClient(LlmClient *client);

    /**
     * @brief 注入工具注册表。
     * @param[in] registry 注册表实例。
     */
    void SetToolRegistry(const std::shared_ptr<ToolRegistry> &registry);

    /**
     * @brief 预览下一次 Start 将使用的循环 ID。
     *
     * 供节点处理器在 Start 之前以该 ID 登记异步挂起状态；
     * 单线程事件循环内 Peek 与 Start 之间不会有其他 Start。
     * @return 下一个循环 ID。
     */
    int PeekNextLoopId() const;

    /**
     * @brief 启动一次工具循环。
     * @param[in] request 循环请求。
     * @return 启动成功返回 true；请求无效或已在执行返回 false。
     */
    bool Start(const _tagToolLoopRequest &request);

    /**
     * @brief 取消当前循环并级联取消活动工具。
     */
    void Cancel();

    /**
     * @brief 判断是否有活动循环。
     * @return 有活动循环返回 true。
     */
    bool IsBusy() const;

signals:
    /**
     * @brief 循环收束（成功、预算收束、降级、取消或失败均通过该信号）。
     * @param[in] result 结构化执行结果。
     */
    void Finished(const vpet::_tagToolLoopResult &result);

private slots:
    /**
     * @brief 处理 LLM 文本完成。
     */
    void OnLlmChatCompleted(int requestId, const QString &content);

    /**
     * @brief 处理 LLM 工具调用完成。
     */
    void OnLlmToolCallsCompleted(int requestId,
                                  const QVector<vpet::_tagLlmToolCall> &toolCalls);

    /**
     * @brief 处理 LLM 请求失败。
     */
    void OnLlmChatFailed(int requestId, const QString &message, int statusCode);

    /**
     * @brief 处理端点不支持 tools。
     */
    void OnLlmToolsUnsupported(int requestId, const QString &message, int statusCode);

    /**
     * @brief 处理调度器回喂的单次工具结果。
     */
    void OnToolCallFinished(const vpet::_tagToolCall &call,
                            const vpet::_tagToolExecutionResult &result);

private:
    /**
     * @brief 组装本轮消息并发起 LLM 调用（含预算检查与轮次计数）。
     * @param[in] withTools 是否携带 tools 声明。
     * @return 发送成功或已进入预算收束返回 true。
     */
    bool BeginRound(bool withTools);

    /**
     * @brief 直接发起一轮 LLM 调用（不做预算检查）。
     * @param[in] withTools 是否携带 tools 声明。
     * @return 发送成功返回 true。
     */
    bool SendRound(bool withTools);

    /**
     * @brief 组装初始消息列表（system + 截尾历史 + 提示词）。
     */
    void AssembleMessages();

    /**
     * @brief 顺序派发当前轮剩余工具调用。
     */
    void DispatchNextToolCall();

    /**
     * @brief 处理预算耗尽收束。
     */
    void HandleBudgetExhausted(const QString &reason);

    /**
     * @brief 处理 tools 不支持降级。
     */
    void HandleToolsUnsupported(const QString &message, int statusCode);

    /**
     * @brief 以指定结果收束并复位状态（Finished 延迟到事件循环发出）。
     */
    void Finish(ToolLoopStatus status, const QString &textOutput, const QString &reason);

    /**
     * @brief 追加一行 trace 日志。
     */
    void AppendTrace(const QString &line);

    /**
     * @brief 将工具结果截断为回喂文本。
     */
    static QString ClampToolOutput(const QString &text);

    /**
     * @brief 将工具结果转为 tool 角色回喂文本（成功内容或脱敏错误）。
     */
    static QString FormatToolFeedback(const _tagToolExecutionResult &result);

    QTimer m_deadline;
    bool m_terminalRound = false;
    bool m_allTerminate = false;
    QPointer<LlmClient> m_llmClient;                        ///< 文本 LLM 客户端
    std::shared_ptr<ToolRegistry> m_registry;      ///< 工具注册表
    std::unique_ptr<ToolScheduler> m_scheduler;    ///< 纪律层调度器

    int m_nextLoopId;                              ///< 下一个循环 ID
    int m_activeLoopId;                            ///< 活动循环 ID
    bool m_busy;                                   ///< 是否有活动循环
    bool m_cancelRequested;                        ///< 是否已请求取消

    _tagToolLoopRequest m_request;                 ///< 当前循环请求
    QVector<_tagLlmMessage> m_messages;            ///< 本轮消息列表
    int m_activeLlmRequestId;                      ///< 在途 LLM 请求 ID
    bool m_fallbackActive;                         ///< 是否已进入降级模式

    QVector<_tagLlmToolCall> m_pendingToolCalls;   ///< 当前轮待执行工具调用
    int m_pendingToolIndex;                        ///< 下一个待执行下标
    int m_currentRound;                            ///< 当前轮次（从 1 开始）

    _tagToolLoopResult m_result;                   ///< 收束结果累积
};

} // namespace vpet

Q_DECLARE_METATYPE(vpet::_tagToolLoopResult)
Q_DECLARE_METATYPE(vpet::ToolLoopStatus)

#endif // VPET_AGENT_TOOLS_TOOL_LOOP_EXECUTOR_H
