#ifndef VPET_MEMORY_MEMORY_SERVICE_H
#define VPET_MEMORY_MEMORY_SERVICE_H

#include "vpet/memory/embedding_client.h"
#include "vpet/memory/memory_graph.h"
#include "vpet/memory/memory_maintenance.h"
#include "vpet/memory/memory_repository.h"
#include "vpet/memory/vector_store.h"

#include <QAtomicInt>
#include <QAtomicInteger>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include <memory>

namespace vpet
{

/**
 * @brief 记忆服务配置
 */
struct MemoryConfig
{
    bool enabled = true;             ///< 记忆功能开关
    QString dataDir;                 ///< 数据目录；为空时使用 AppDataLocation
    int queueCapacity = 64;          ///< 任务队列容量
    int maxResults = 6;              ///< 检索结果上限
    int promptBudgetChars = 1200;    ///< 注入提示段字符预算
    QString defaultScope = QStringLiteral("pet"); ///< 默认记忆作用域
    bool automaticExtraction = false; ///< 自动提取开关，默认关闭
    int consolidationMaxCandidates = 4; ///< 单轮 LLM 巩固候选上限
    _tagMemoryMaintenanceConfig maintenance; ///< 检索后维护配置（阶段 4）
    EmbeddingConfig embedding;       ///< embedding 配置（阶段 2）
};

/**
 * @brief 记忆服务（后台 worker + 有界任务队列 + 分区结果 mailbox）
 *
 * 主线程只提交任务并消费已完成结果；后台 worker 独占记忆图与文件读写，
 * 主对话链路从不等待记忆模块。结果按 petId + triggerType 分区，晚到结果
 * 不会覆盖更高 requestId 的新结果。
 */
class MemoryService
    : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 后台任务类型
     */
    enum class Action
    {
        Retrieve,
        Store,
        Forget,
        Tag,
        List,
        Consolidate,
        Feedback,
        Update,
        Export,
        Import
    };

    /**
     * @brief 后台任务
     */
    struct _tagTask
    {
        quint64 requestId = 0;       ///< 请求 ID（单调递增）
        Action action = Action::Retrieve;
        QString petId;               ///< 宠物 ID
        QString triggerType;         ///< 触发类型（user / vision）
        QString query;               ///< 检索查询文本
        MemoryEntry entry;           ///< Store 目标条目
        QString memoryId;            ///< Forget / Tag 目标条目 ID
        QStringList tags;            ///< Tag 操作标签
        MemoryEntry::Scope scope = MemoryEntry::Scope::Pet; ///< List 作用域
        QVector<_tagMemoryConsolidationCandidate> candidates; ///< Consolidate 候选
        QStringList memoryIds;         ///< Feedback 目标记忆 ID
        bool helpful = false;          ///< Feedback 是否有帮助
        QString filePath;              ///< Export / Import 文件路径
    };

    /**
     * @brief 检索结果
     */
    struct _tagRetrieveResult
    {
        quint64 requestId = 0;
        QString petId;
        QString triggerType;
        QString query;
        bool ok = false;             ///< 是否成功（未匹配到也视为成功）
        QVector<MemoryEntry> entries; ///< 匹配条目（按服务端排序）
    };

    /**
     * @brief 构造函数
     * @param[in] parent 父对象
     */
    explicit MemoryService(QObject *parent = nullptr);

    /**
     * @brief 析构函数；未显式 Shutdown 时尝试安全关闭
     */
    ~MemoryService() override;

    /**
     * @brief 从配置文件解析记忆配置
     * @param[in] configPath 配置文件路径
     * @param[out] config 输出配置
     * @param[out] errorMessage 错误描述
     * @return 解析成功返回 true
     */
    static bool ParseConfig(const QString &configPath,
                            MemoryConfig &config,
                            QString &errorMessage);

    /**
     * @brief 设置 embedding 配置（必须在 Start 之前调用）
     *
     * 仅接受 local_onnx 后端；启用时模型不可用不影响服务运行，
     * 检索自动回退到阶段 1 关键词/标签路径。
     *
     * @param[in] config embedding 配置
     * @param[out] errorMessage 错误描述
     * @return 设置成功返回 true
     */
    bool SetEmbeddingConfig(const EmbeddingConfig &config, QString &errorMessage);

    /**
     * @brief 设置检索后维护配置（必须在 Start 之前调用）
     * @param[in] config 维护配置
     * @param[out] errorMessage 错误描述
     * @return 配置有效返回 true
     */
    bool SetMaintenanceConfig(const _tagMemoryMaintenanceConfig &config,
                              QString &errorMessage);

    /**
     * @brief 注入测试用向量化器（必须在 Start 之前调用）
     *
     * 测试钩子：以确定性 fake 实现验证级联检索路径，
     * 生产代码不得使用。
     *
     * @param[in] embedder 向量化器实现
     */
    void InstallEmbedderForTest(std::unique_ptr<MemoryEmbedder> embedder);

    /**
     * @brief 将条目按预算组装为只读提示段
     * @param[in] entries 记忆条目
     * @param[in] maxResults 最大条目数
     * @param[in] budgetChars 最大字符数
     * @return 提示段文本；无可用条目时返回空字符串
     */
    static QString BuildPromptSection(const QVector<MemoryEntry> &entries,
                                      int maxResults,
                                      int budgetChars);

    /**
     * @brief 启动后台 worker 线程并加载记忆图
     * @param[in] dataDir 数据目录；为空时使用 AppDataLocation
     * @param[in] queueCapacity 任务队列容量
     * @param[out] errorMessage 错误描述
     * @return 启动成功返回 true；失败时保持未启动状态
     */
    bool Start(const QString &dataDir, int queueCapacity, QString &errorMessage);

    /**
     * @brief 是否已启动并运行
     * @return 运行中返回 true
     */
    bool IsRunning() const;

    /**
     * @brief 停止接收任务、处理已提交写入并在有限时间内落盘退出
     * @param[in] timeoutMs 等待 worker 退出的最大毫秒数
     */
    void Shutdown(int timeoutMs);

    /**
     * @brief 非阻塞提交检索任务
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] query 查询文本
     * @param[out] requestId 分配的请求 ID
     * @return 入队成功返回 true；队列满、未启动或正在关闭时返回 false
     */
    bool TryEnqueueRetrieve(const QString &petId,
                            const QString &triggerType,
                            const QString &query,
                            quint64 &requestId);

    /**
     * @brief 非阻塞提交存储任务（含隐私过滤）
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] entry 待存储条目
     * @param[out] requestId 分配的请求 ID
     * @param[out] rejectCategory 隐私过滤拒绝类别（入队被拒时为空；过滤拒绝时填充）
     * @return 入队成功返回 true
     */
    bool TryEnqueueStore(const QString &petId,
                         const QString &triggerType,
                         const MemoryEntry &entry,
                         quint64 &requestId,
                          QString &rejectCategory);

    /**
     * @brief 非阻塞提交已校验的 LLM 巩固候选
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] candidates 本地 schema 校验后的候选
     * @param[out] requestId 分配的请求 ID
     * @return 入队成功返回 true；候选非法、队列满或服务不可用返回 false
     */
    bool TryEnqueueConsolidation(const QString &petId,
                                 const QString &triggerType,
                                 const QVector<_tagMemoryConsolidationCandidate> &candidates,
                                 quint64 &requestId);

    /**
     * @brief 非阻塞提交逻辑删除任务
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] memoryId 目标条目 ID
     * @param[out] requestId 分配的请求 ID
     * @return 入队成功返回 true
     */
    bool TryEnqueueForget(const QString &petId,
                          const QString &triggerType,
                          const QString &memoryId,
                          quint64 &requestId);

    /**
     * @brief 非阻塞提交按关键词逻辑删除任务
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] query 关键词查询；匹配条目将被逻辑删除
     * @param[out] requestId 分配的请求 ID
     * @return 入队成功返回 true
     */
    bool TryEnqueueForgetByKeyword(const QString &petId,
                                   const QString &triggerType,
                                   const QString &query,
                                   quint64 &requestId);

    /**
     * @brief 非阻塞提交标签维护任务
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] memoryId 目标条目 ID
     * @param[in] tags 标签列表；空列表表示清除全部标签
     * @param[out] requestId 分配的请求 ID
     * @return 入队成功返回 true
     */
    bool TryEnqueueTag(const QString &petId,
                       const QString &triggerType,
                       const QString &memoryId,
                       const QStringList &tags,
                       quint64 &requestId);

    /**
     * @brief 非阻塞提交列出记忆任务
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型（如 "list"）
     * @param[in] scope 作用域（Pet 时允许 Global 记忆跨宠物可见）
     * @param[out] requestId 分配的请求 ID
     * @return 入队成功返回 true
     */
    bool TryEnqueueList(const QString &petId,
                        const QString &triggerType,
                        MemoryEntry::Scope scope,
                        quint64 &requestId);

    /**
     * @brief 非阻塞提交记忆使用反馈
     * @param[in] petId 当前宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] memoryIds 本轮浮现的记忆 ID
     * @param[in] helpful 是否有帮助
     * @param[out] requestId 分配的请求 ID
     * @return 入队成功返回 true
     */
    bool TryEnqueueFeedback(const QString &petId,
                            const QString &triggerType,
                            const QStringList &memoryIds,
                            bool helpful,
                            quint64 &requestId);

    /**
     * @brief 非阻塞更新一条已有记忆
     * @param[in] petId 当前宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] entry 更新后的完整条目
     * @param[out] requestId 分配的后台请求 ID
     * @param[out] rejectCategory 隐私过滤拒绝类别
     * @return 入队成功返回 true
     */
    bool TryEnqueueUpdate(const QString &petId,
                          const QString &triggerType,
                          const MemoryEntry &entry,
                          quint64 &requestId,
                          QString &rejectCategory);

    /**
     * @brief 非阻塞导出记忆图
     * @param[in] petId 宠物 ID（用于结果分区）
     * @param[in] triggerType 触发类型（管理操作使用 memory.management）
     * @param[in] filePath 目标 JSON 文件路径
     * @param[out] requestId 分配的后台请求 ID
     * @return 入队成功返回 true
     */
    bool TryEnqueueExport(const QString &petId,
                          const QString &triggerType,
                          const QString &filePath,
                          quint64 &requestId);

    /**
     * @brief 非阻塞导入记忆图
     * @param[in] petId 宠物 ID（用于结果分区）
     * @param[in] triggerType 触发类型
     * @param[in] filePath 源 JSON 文件路径
     * @param[out] requestId 分配的后台请求 ID
     * @return 入队成功返回 true
     */
    bool TryEnqueueImport(const QString &petId,
                          const QString &triggerType,
                          const QString &filePath,
                          quint64 &requestId);

    /**
     * @brief 消费指定分区的最新检索结果
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[out] result 输出结果
     * @return 存在未消费结果返回 true
     */
    bool TakeLatestReadyResult(const QString &petId,
                               const QString &triggerType,
                               _tagRetrieveResult &result);

    /**
     * @brief 指定分区是否存在未消费结果（不消费）
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @return 存在未消费结果返回 true
     */
    bool HasReadyResult(const QString &petId, const QString &triggerType) const;

    /**
     * @brief 当前队列积压任务数
     * @return 积压任务数
     */
    int PendingCount() const;

    /**
     * @brief 队列容量
     * @return 队列容量
     */
    int QueueCapacity() const;

    /**
     * @brief 记录诊断日志（不含敏感原文）
     * @param[in] message 日志内容
     */
    void EmitLogMessage(const QString &message);

signals:
    /**
     * @brief 记忆模块诊断日志
     * @param[in] message 日志内容
     */
    void LogMessage(const QString &message);

private:
    /**
     * @brief 后台线程入口：循环排空任务队列，关闭时退出
     */
    void WorkerLoop();

private:
    /**
     * @brief 非阻塞入队核心
     * @param[in] task 任务
     * @param[out] requestId 分配的请求 ID（可选）
     * @return 入队成功返回 true
     */
    bool EnqueueTask(const _tagTask &task, quint64 *requestId = nullptr);

    /**
     * @brief 处理单个任务（worker 线程）
     * @param[in] task 任务
     */
    void ProcessOneTask(const _tagTask &task);

    /**
     * @brief 确保向量化器就绪（首次使用时在 worker 线程惰性加载）
     */
    void EnsureEmbedderReady();

    /**
     * @brief 为缺失或模型不匹配的活跃条目延迟回填向量
     *
     * 仅在 worker 线程执行；每次服务启动最多完整扫描一次。
     */
    void BackfillMissingVectors();

    /**
     * @brief 级联检索：向量 top-k -> 图 BFS 传播；模型不可用时回退关键词/标签
     * @param[in] query 查询文本
     * @param[in] petId 宠物 ID
     * @param[in] maxResults 结果上限
     * @return 排序后的命中条目
     */
    QVector<MemoryEntry> CascadeRetrieve(const QString &query,
                                         const QString &petId,
                                         int maxResults);

    /**
     * @brief 写入条目向量（worker 线程；embedding 不可用或失败时静默跳过）
     * @param[in] entryId 条目 ID
     * @param[in] content 记忆正文
     */
    bool TryWriteVector(const QString &entryId, const QString &content);

    /**
     * @brief 删除条目向量（worker 线程；向量库未打开时静默跳过）
     * @param[in] entryId 条目 ID
     */
    void RemoveVector(const QString &entryId);

    /**
     * @brief 在 worker 线程合并 LLM 巩固候选
     * @param[in] task Consolidate 任务
     */
    void ConsolidateCandidates(const _tagTask &task);

    /**
     * @brief 在同作用域内查找与候选最相近的活跃条目
     * @param[in] entry 候选条目
     * @param[out] similarity 相似度（0.0 到 1.0）
     * @return 命中的已有条目；未命中时 id 为空
     */
    MemoryEntry FindSimilarActiveEntry(const MemoryEntry &entry, float &similarity) const;

    /**
     * @brief 计算两段记忆文本的保守词元 Jaccard 相似度
     * @param[in] left 左侧文本
     * @param[in] right 右侧文本
     * @return 相似度（0.0 到 1.0）
     */
    static float ContentSimilarity(const QString &left, const QString &right);

    /**
     * @brief 构造 mailbox 分区键
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @return 分区键
     */
    static QString MailboxKey(const QString &petId, const QString &triggerType);

private:
    MemoryGraph m_graph;             ///< 记忆图（worker 独占）
    MemoryRepository m_repository;   ///< 持久化（worker 独占）
    MemoryMaintenance m_maintenance; ///< 检索后维护（worker 独占）
    QThread *m_workerThread;         ///< 后台线程
    mutable QMutex m_queueMutex;     ///< 队列锁
    QQueue<_tagTask> m_taskQueue;    ///< 有界任务队列
    QWaitCondition m_queueNotEmpty;  ///< 队列非空通知（worker）
    int m_queueCapacity;             ///< 队列容量
    mutable QMutex m_mailboxMutex;   ///< mailbox 锁
    QHash<QString, _tagRetrieveResult> m_mailbox; ///< petId|triggerType -> 最新结果
    QAtomicInteger<quint64> m_nextRequestId;      ///< 请求 ID 分配器
    QAtomicInt m_shuttingDown;       ///< 关闭标志
    bool m_isRunning;                ///< 是否已启动

    EmbeddingConfig m_embeddingConfig; ///< embedding 配置（Start 前设置）
    bool m_embeddingConfigured = false; ///< 是否已配置 embedding
    bool m_embeddingEnabled = false;   ///< embedding 是否启用（Start 时确定）
    std::unique_ptr<MemoryEmbedder> m_embedder; ///< 向量化器（worker 独占）
    std::unique_ptr<VectorStore> m_vectorStore; ///< 向量存储（worker 独占）
    QString m_vectorDbPath;            ///< worker 线程打开的向量数据库路径
    qint64 m_lastEmbedderAttemptMs = 0; ///< 上次加载尝试时间（重试节流）
    bool m_vectorBackfillComplete = false; ///< 当前服务启动周期是否已完成向量回填
};

} // namespace vpet

#endif // VPET_MEMORY_MEMORY_SERVICE_H
