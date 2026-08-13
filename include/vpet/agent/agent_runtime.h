#ifndef VPET_AGENT_AGENT_RUNTIME_H
#define VPET_AGENT_AGENT_RUNTIME_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_async_bridge.h"
#include "vpet/agent/agent_graph_executor.h"
#include "vpet/agent/agent_node_registry.h"
#include "vpet/agent/invocation_queue_policy.h"
#include "vpet/agent/agent_output_policy.h"
#include "vpet/llm/vision_llm_client.h"
#include "vpet/memory/memory_consolidator.h"
#include "vpet/memory/memory_service.h"
#include "vpet/web/web_research_engine.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>

namespace vpet
{

class LlmClient;
struct _tagLlmRequestOptions;

/**
 * @brief Agent DAG 运行时启动器
 *
 * 负责加载 Agent DAG 配置、输出拓扑序，并按节点类型执行当前可用节点。
 */
class AgentRuntime
    : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Agent 节点处理器类型
     */
    using NodeHandler = std::function<bool(const _tagAgentDagNode &, AgentContext &, QString &)>;

    /**
     * @brief 构造函数
     * @param[in] parent 父对象
     */
    explicit AgentRuntime(QObject *parent = nullptr);

    /**
     * @brief 使用可注入研究引擎构造运行时
     * @param[in] webResearchEngine 外部研究引擎；为空时由运行时创建
     * @param[in] parent 父对象
     */
    AgentRuntime(WebResearchEngine *webResearchEngine, QObject *parent);

    /**
     * @brief 使用可注入研究引擎与记忆服务构造运行时
     * @param[in] webResearchEngine 外部研究引擎；为空时由运行时创建
     * @param[in] memoryService 外部记忆服务；为空时由运行时创建
     * @param[in] parent 父对象
     */
    AgentRuntime(WebResearchEngine *webResearchEngine,
                 MemoryService *memoryService,
                 QObject *parent);

    /**
     * @brief 析构函数
     */
    ~AgentRuntime() override;

    /**
     * @brief 加载 Agent DAG 配置并准备运行时
     * @param[in] configPath Agent DAG 配置文件路径
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    bool Load(const QString &configPath, QString &errorMessage);

    /**
     * @brief 按在线就绪队列执行 Agent 节点
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool Execute(QString &errorMessage);

    /**
     * @brief 写入用户输入并按在线就绪队列执行 Agent 节点
     * @param[in] userInput 用户输入文本
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteWithUserInput(const QString &userInput, QString &errorMessage);

    /**
     * @brief 写入最新视觉感知帧到运行时上下文
     * @param[in] encodedData 编码后的图像数据
     * @param[in] frameId 帧序号
     * @param[in] frameSize 帧尺寸
     * @param[in] modality 模态名称
     * @param[out] errorMessage 错误描述
     * @return 写入成功返回 true
     */
    bool UpdatePerceptionFrame(const QByteArray &encodedData,
                               int frameId,
                               const QSize &frameSize,
                               const QString &modality,
                               QString &errorMessage);

    /**
     * @brief 从指定文件加载文本 LLM 配置
     * @param[in] configPath LLM 配置文件路径
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    bool LoadLlmConfig(const QString &configPath, QString &errorMessage);

    /**
     * @brief 自动查找并加载文本 LLM 配置
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    bool LoadDefaultLlmConfig(QString &errorMessage);

    /**
     * @brief 从指定文件加载视觉 LLM 配置
     * @param[in] configPath 视觉 LLM 配置文件路径
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    bool LoadVisionLlmConfig(const QString &configPath, QString &errorMessage);

    /**
     * @brief 自动查找并加载视觉 LLM 配置
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    bool LoadDefaultVisionLlmConfig(QString &errorMessage);

    /** @brief 加载联网搜索配置。 @param[in] configPath 配置路径。 @param[out] errorMessage 错误描述。 @return 成功返回 true。 */
    bool LoadWebSearchConfig(const QString &configPath, QString &errorMessage);
    /** @brief 自动加载联网搜索配置。 @param[out] errorMessage 错误描述。 @return 成功返回 true。 */
    bool LoadDefaultWebSearchConfig(QString &errorMessage);

    /**
     * @brief 加载记忆服务配置并启动记忆服务
     * @param[in] configPath 记忆配置文件路径
     * @param[out] errorMessage 错误描述
     * @return 配置解析与启动成功返回 true；启动失败不影响 Agent 运行
     */
    bool LoadMemoryConfig(const QString &configPath, QString &errorMessage);

    /**
     * @brief 自动查找并加载记忆服务配置
     * @param[out] errorMessage 错误描述
     * @return 加载成功返回 true
     */
    bool LoadDefaultMemoryConfig(QString &errorMessage);

    /**
     * @brief 停止记忆服务并在有限时间内落盘退出
     */
    void ShutdownMemory();

    /**
     * @brief 判断记忆服务是否已配置并运行
     * @return 运行中返回 true
     */
    bool IsMemoryEnabled() const;

    /**
     * @brief 非阻塞提交本轮记忆使用反馈
     * @param[in] memoryIds 本轮浮现的记忆 ID
     * @param[in] helpful 是否有帮助
     * @param[out] requestId 分配的后台请求 ID
     * @return 入队成功返回 true
     */
    bool SubmitMemoryFeedback(const QStringList &memoryIds,
                              bool helpful,
                              quint64 &requestId);

    /**
     * @brief 返回最近一次实际注入回答的记忆 ID
     * @return 去重后的记忆 ID 列表
     */
    QStringList GetLatestSurfacedMemoryIds() const;

    /**
     * @brief 请求列出当前宠物可见记忆
     * @param[out] requestId 分配的后台请求 ID
     * @return 入队成功返回 true
     */
    bool RequestMemoryList(quint64 &requestId);

    /**
     * @brief 消费最新的记忆管理列表结果
     * @param[out] entries 输出条目
     * @return 存在结果返回 true
     */
    bool TakeMemoryListResult(QVector<MemoryEntry> &entries);

    /**
     * @brief 请求更新已有记忆
     * @param[in] entry 更新后的条目
     * @param[out] requestId 分配的后台请求 ID
     * @param[out] errorMessage 拒绝原因
     * @return 入队成功返回 true
     */
    bool UpdateMemory(const MemoryEntry &entry,
                      quint64 &requestId,
                      QString &errorMessage);

    /**
     * @brief 请求逻辑删除已有记忆
     * @param[in] memoryId 目标记忆 ID
     * @param[out] requestId 分配的后台请求 ID
     * @return 入队成功返回 true
     */
    bool ForgetMemory(const QString &memoryId, quint64 &requestId);

    /**
     * @brief 请求导出记忆 JSON
     * @param[in] filePath 目标路径
     * @param[out] requestId 分配的后台请求 ID
     * @return 入队成功返回 true
     */
    bool ExportMemory(const QString &filePath, quint64 &requestId);

    /**
     * @brief 请求从 JSON 导入记忆
     * @param[in] filePath 源路径
     * @param[out] requestId 分配的后台请求 ID
     * @return 入队成功返回 true
     */
    bool ImportMemory(const QString &filePath, quint64 &requestId);

    /**
     * @brief 判断文本 LLM 是否可用
     * @return 可用返回 true
     */
    bool IsLlmConfigured() const;

    /**
     * @brief 判断视觉 LLM 是否可用
     * @return 可用返回 true
     */
    bool IsVisionLlmConfigured() const;

    /**
     * @brief 设置当前视觉 LLM 模型档位
     * @param[in] profile 目标模型档位
     * @return 切换成功返回 true
     */
    bool SetActiveVisionLlmProfile(VISION_LLM_MODEL_PROFILE profile);

    /**
     * @brief 获取当前视觉 LLM 模型档位
     * @return 当前模型档位
     */
    VISION_LLM_MODEL_PROFILE GetActiveVisionLlmProfile() const;

    /**
     * @brief 判断运行时是否有异步请求等待回调
     * @return 有等待中的异步请求返回 true
     */
    bool HasPendingAsyncRequest() const;

    /**
     * @brief 加载配置并立即执行 Agent DAG
     * @param[in] configPath Agent DAG 配置文件路径
     * @param[out] errorMessage 错误描述
     * @return 启动并执行成功返回 true
     */
    bool Start(const QString &configPath, QString &errorMessage);

    /**
     * @brief 获取当前拓扑执行顺序
     * @return 节点名称列表
     */
    QVector<QString> GetExecutionOrder() const;

    /**
     * @brief 获取运行时上下文
     * @return 运行时上下文引用
     */
    AgentContext &GetContext();

    /**
     * @brief 获取只读运行时上下文
     * @return 运行时上下文只读引用
     */
    const AgentContext &GetContext() const;

    /**
     * @brief 设置运行时上下文
     * @param[in] context 外部上下文对象
     */
    void SetContext(const AgentContext &context);

    /**
     * @brief 注册节点处理器
     * @param[in] nodeType 节点类型
     * @param[in] handler 节点处理器
     * @return 注册成功返回 true
     */
    bool RegisterNodeHandler(const QString &nodeType, const NodeHandler &handler);

signals:
    /**
     * @brief Agent 日志信号
     * @param[in] message 日志内容
     */
    void LogMessage(const QString &message);

    /**
     * @brief LLM 回复完成信号
     * @param[in] requestId 请求 ID
     * @param[in] content 回复文本
     */
    void LlmResponseReceived(int requestId, const QString &content);

    /**
     * @brief Agent 最终输出就绪信号
     * @param[in] requestId 触发最终输出的请求 ID
     * @param[in] content 经过拓扑链路处理后的最终输出文本
     * @param[in] source 输出来源，允许值为 user_response 或 vision_proactive
     */
    void AgentOutputReady(int requestId, const QString &content, const QString &source);

    /**
     * @brief LLM 请求失败信号
     * @param[in] requestId 请求 ID
     * @param[in] message 错误描述
     * @param[in] statusCode HTTP 状态码
     */
    void LlmRequestFailed(int requestId, const QString &message, int statusCode);

    /**
     * @brief Agent 请求失败，附带触发来源供 UI 区分用户请求与后台感知。
     * @param[in] requestId 请求 ID。
     * @param[in] message 错误描述。
     * @param[in] statusCode HTTP 状态码。
     * @param[in] source user_response 或 vision_proactive。
     */
    void AgentRequestFailed(int requestId,
                            const QString &message,
                            int statusCode,
                            const QString &source);

private slots:
    /**
     * @brief 处理文本 LLM 回复完成
     * @param[in] requestId 请求 ID
     * @param[in] content 回复文本
     */
    void OnLlmChatCompleted(int requestId, const QString &content);

    /**
     * @brief 处理文本 LLM 请求失败
     * @param[in] requestId 请求 ID
     * @param[in] message 错误描述
     * @param[in] statusCode HTTP 状态码
     */
    void OnLlmChatFailed(int requestId, const QString &message, int statusCode);

    /**
     * @brief 处理视觉 LLM 识别完成
     * @param[in] requestId 请求 ID
     * @param[in] content 视觉识别文本
     */
    void OnVisionAnalysisCompleted(int requestId, const QString &content);

    /**
     * @brief 处理视觉 LLM 识别失败
     * @param[in] requestId 请求 ID
     * @param[in] message 错误描述
     * @param[in] statusCode HTTP 状态码
     */
    void OnVisionAnalysisFailed(int requestId, const QString &message, int statusCode);

    /** @brief 处理联网研究完成。 @param[in] response 结构化研究结果。 */
    void OnWebResearchCompleted(const _tagWebResearchResponse &response);
    /** @brief 处理联网研究失败。 @param[in] researchId 研究 ID。 @param[in] message 错误描述。 @param[in] statusCode HTTP 状态码。 */
    void OnWebResearchFailed(int researchId, const QString &message, int statusCode);

private:
    /**
     * @brief 执行单个 Agent 节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteNode(const _tagAgentDagNode &node,
                     AgentContext &context,
                     QString &errorMessage);

    /**
     * @brief 将新的触发上下文加入等待队列
     * @param[in] context 新 invocation 的输入快照
     * @return 入队成功返回 true
     */
    bool EnqueueInvocation(const AgentContext &context);

    /**
     * @brief 启动等待队列中的下一轮 invocation
     * @param[out] errorMessage 错误描述
     * @return 启动成功或队列为空返回 true
     */
    bool StartNextQueuedInvocation(QString &errorMessage);

    /**
     * @brief 登记一个等待异步回调的节点
     * @param[in] node 等待回调的节点定义
     * @param[in] context 节点挂起时的独立上下文
     * @param[in] invocationId 节点所属执行轮次标识
     * @param[in] branchId 节点所属图分支标识
     * @param[out] errorMessage 错误描述
     * @return 登记成功返回 true
     */
    bool RegisterPendingNode(const _tagAgentDagNode &node,
                             const AgentContext &context,
                             quint64 invocationId,
                             const QString &branchId,
                             QString &errorMessage);

    /**
     * @brief 构建图执行器回调集合
     * @return 不持有运行时反向引用的单次调用回调集合
     */
    AgentGraphExecutor::_tagCallbacks BuildGraphCallbacks();

    /**
     * @brief 根据异步客户端来源和请求 ID 构造 pending 表键
     * @param[in] clientType 异步客户端类型
     * @param[in] requestId 客户端请求 ID
     * @return pending 表键
     */
    QString BuildPendingRequestKey(const QString &clientType, int requestId) const;

    /**
     * @brief 恢复指定异步节点并继续在线调度
     * @param[in] pendingKey 异步请求关联键
     * @param[in] requestId 异步请求标识
     * @param[in] context 回调结果写入后的节点上下文
     * @param[out] errorMessage 错误描述
     * @return 恢复成功返回 true
     */
    bool ResumePendingNode(const QString &pendingKey,
                           int requestId,
                           const AgentContext &context,
                           QString &errorMessage);

    /**
     * @brief 处理异步请求超时并终止所属执行轮次
     * @param[in] pendingKey 异步请求关联键
     * @param[in] requestId 异步请求标识
     * @param[in] invocationId 请求登记时的执行轮次标识
     */
    void HandlePendingRequestTimeout(const QString &pendingKey,
                                     int requestId,
                                     quint64 invocationId);

    /**
     * @brief 准备基础文本输入上下文
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 准备成功返回 true
     */
    bool PrepareTextInputContext(AgentContext &context, QString &errorMessage);

    /**
     * @brief 清理当前一轮执行的输入和触发来源
     * @param[in,out] context 运行时上下文
     */
    void ClearInvocationInputState(AgentContext &context);

    /**
     * @brief 执行视觉输入节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteVisionInputNode(const _tagAgentDagNode &node,
                                AgentContext &context,
                                QString &errorMessage);

    /**
     * @brief 执行视觉 LLM 节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteVisionLlmNode(const _tagAgentDagNode &node,
                              AgentContext &context,
                              QString &errorMessage);

    /**
     * @brief 执行文本 LLM 节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteLlmChatNode(const _tagAgentDagNode &node,
                             AgentContext &context,
                             QString &errorMessage);

    /**
     * @brief 从节点配置解析文本 LLM 请求参数
     * @param[in] node 节点定义
     * @param[out] options 解析后的请求参数
     * @param[out] errorMessage 错误描述
     * @return 解析成功返回 true
     */
    static bool ParseLlmRequestOptions(const _tagAgentDagNode &node,
                                       _tagLlmRequestOptions &options,
                                       QString &errorMessage);

    /** @brief 执行联网研究节点。 @param[in] node 节点定义。 @param[in,out] context 上下文。 @param[out] errorMessage 错误描述。 @return 成功返回 true。 */
    bool ExecuteWebResearchNode(const _tagAgentDagNode &node,
                                AgentContext &context,
                                QString &errorMessage);

    /**
     * @brief 执行记忆检索节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteMemoryRetrieveNode(const _tagAgentDagNode &node,
                                   AgentContext &context,
                                   QString &errorMessage);

    /**
     * @brief 执行记忆存储节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteMemoryStoreNode(const _tagAgentDagNode &node,
                                AgentContext &context,
                                QString &errorMessage);

    /**
     * @brief 执行输出格式化节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecuteOutputFormatNode(const _tagAgentDagNode &node,
                                  AgentContext &context,
                                  QString &errorMessage);

    /**
     * @brief 根据触发类型解析并写入最终输出来源
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 写入成功返回 true
     */
    bool WriteOutputSource(AgentContext &context, QString &errorMessage);

    /**
     * @brief 读取最终输出来源
     * @param[in] context 运行时上下文
     * @return 输出来源字符串；缺失时返回 user_response
     */
    QString ReadOutputSource(const AgentContext &context) const;

    /**
     * @brief 发射最终输出就绪信号
     * @param[in] requestId 触发最终输出的请求 ID
     * @param[in] content 最终输出文本
     * @param[in] context 完成当前输出的运行时上下文
     */
    void EmitAgentOutputReady(int requestId,
                               const QString &content,
                               const AgentContext &context);

    /** @brief 发射带来源的失败信号，并保留兼容的 LLM 失败信号。 */
    void EmitAgentRequestFailed(int requestId,
                                const QString &message,
                                int statusCode,
                                const QString &source);

    /**
     * @brief 记录用户输入和最终输出到最近对话历史
     * @param[in,out] context 运行时上下文
     * @param[in] outputText 最终输出文本
     * @param[out] errorMessage 错误描述
     * @return 记录成功返回 true
     */
    bool AppendConversationHistory(AgentContext &context,
                                   const QString &outputText,
                                   QString &errorMessage);

    /**
     * @brief 清理运行时异步等待状态
     * @param[in,out] context 运行时上下文
     */
    void ClearAsyncPendingState(AgentContext &context);

    /**
     * @brief 统一解除异步执行阻塞并清理本轮输入
     * @param[in,out] context 运行时上下文
     */
    void ResetAsyncExecutionState(AgentContext &context);

    /**
     * @brief 设置运行时异步等待状态
     * @param[in] node 等待回调的节点定义
     * @param[in,out] context 运行时上下文
     * @param[in] requestId 等待回调的请求 ID
     * @param[out] errorMessage 错误描述
     * @return 设置成功返回 true
     */
    bool SetAsyncPendingState(const _tagAgentDagNode &node,
                              AgentContext &context,
                              int requestId,
                              QString &errorMessage);

    /**
     * @brief 执行尚未实现的透传节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    bool ExecutePassThroughNode(const _tagAgentDagNode &node,
                                AgentContext &context,
                                QString &errorMessage);

    /**
     * @brief 注册默认节点处理器
     */
    void RegisterDefaultNodeHandlers();

    /**
     * @brief 查找默认文本 LLM 配置文件
     * @return 配置文件绝对路径；未找到返回空字符串
     */
    QString FindDefaultLlmConfigPath() const;

    /**
     * @brief 查找默认视觉 LLM 配置文件
     * @return 配置文件绝对路径；未找到返回空字符串
     */
    QString FindDefaultVisionLlmConfigPath() const;
    /** @brief 查找默认联网搜索配置。 @return 配置绝对路径，未找到返回空字符串。 */
    QString FindDefaultWebSearchConfigPath() const;
    /** @brief 查找默认记忆服务配置。 @return 配置绝对路径，未找到返回空字符串。 */
    QString FindDefaultMemoryConfigPath() const;

    /**
     * @brief 将用户输入提交到文本 LLM
     * @param[in] userInput 用户输入文本
     * @param[out] errorMessage 错误描述
     * @return 发送成功返回 true
     */
    bool SendUserInputToLlm(const QString &userInput, QString &errorMessage);

private:
    /**
     * @brief 单轮在线调度状态
     */
    AgentContext m_context;               ///< 当前执行视图或最近一次结果上下文
    AgentContext m_sessionContext;        ///< 跨调用持久化的会话基座
    LlmClient *m_llmClient;                ///< 文本 LLM 客户端
    VisionLlmClient *m_visionLlmClient;    ///< 视觉 LLM 客户端
    WebResearchEngine *m_webResearchEngine; ///< 联网研究引擎
    MemoryService *m_memoryService;        ///< 长期记忆服务
    bool m_ownsMemoryService;              ///< 记忆服务是否由运行时创建
    MemoryConfig m_memoryConfig;           ///< 记忆服务配置
    bool m_memoryConfigLoaded;             ///< 是否已加载记忆配置
    MemoryConsolidator m_memoryConsolidator; ///< LLM 巩固请求关联器
    QStringList m_latestSurfacedMemoryIds; ///< 最近一次回答实际注入的记忆 ID
    AgentNodeRegistry m_nodeRegistry;     ///< 节点注册与别名执行组件
    AgentGraphExecutor m_graphExecutor;   ///< DAG 与单轮调度组件
    AgentAsyncBridge m_asyncBridge;       ///< 异步请求关联组件
    InvocationQueuePolicy m_invocationQueue; ///< 跨轮触发排队策略
    QString m_lastPerceptionFrameHash;     ///< 最近已接受视觉帧内容指纹
    bool m_isLoaded;                      ///< 是否已加载配置
    bool m_contextWasQueued;              ///< 最近一次上下文是否已由入口加入 FIFO
    bool m_webResearchStartInProgress;    ///< web.research Start 调用期间暂存同步回调
    bool m_hasBufferedWebResearchCompletion; ///< 是否收到 Start 期间的同步完成回调
    bool m_hasBufferedWebResearchFailure; ///< 是否收到 Start 期间的同步失败回调
    _tagWebResearchResponse m_bufferedWebResearchCompletion; ///< 暂存的同步完成结果
    int m_bufferedWebResearchFailureId;   ///< 暂存的同步失败研究 ID
    QString m_bufferedWebResearchFailureMessage; ///< 暂存的同步失败原因
    int m_bufferedWebResearchFailureStatusCode; ///< 暂存的同步失败状态码
};

} // namespace vpet

#endif // VPET_AGENT_AGENT_RUNTIME_H
