#ifndef VPET_MEMORY_MEMORY_GRAPH_H
#define VPET_MEMORY_MEMORY_GRAPH_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vpet
{

/**
 * @brief 长期记忆条目
 *
 * 阶段 1 数据模型保留后续图模型的兼容形态，但只使用记忆、标签及 has_tag 关系。
 * 阶段 2 的边（has_tag / relates_to / supersedes 等）使用明确的边对象实现，
 * 不能复用本结构或邻接表伪装。
 */
struct MemoryEntry
{
    /**
     * @brief 程序性记忆的结构化步骤
     */
    struct _tagProcedure
    {
        QString name;                      ///< 流程名称
        QString trigger;                   ///< 流程触发文本
        QStringList steps;                 ///< 有序执行步骤
        QStringList prerequisites;         ///< 执行前置条件
        QStringList warnings;              ///< 风险和注意事项
    };

    QString id;                          ///< 全局唯一标识
    QString content;                     ///< 记忆正文
    QString category;                    ///< 可选类别标签

    enum class Type
    {
        Fact,
        Preference,
        Procedure,
        Correction,
        Negative
    };
    enum class Scope
    {
        Global,
        Pet
    };
    enum class Provenance
    {
        UserStated,
        UserCorrected,
        Observed,
        Extracted,
        Inferred
    };

    Type type = Type::Fact;             ///< 记忆类型
    Scope scope = Scope::Pet;           ///< 记忆作用域
    Provenance provenance = Provenance::UserStated; ///< 来源

    QStringList tags;                    ///< 显式标签
    QStringList triggerPatterns;         ///< 负面/流程记忆触发词或受限正则
    _tagProcedure procedure;             ///< Procedure 类型的结构化内容
    QString petId;                       ///< 所属宠物标识；Global 记忆可置空

    qint64 createdAt = 0;                ///< 创建时间（epoch ms）
    qint64 updatedAt = 0;                ///< 最近修改时间（epoch ms）
    qint64 lastAccessed = 0;             ///< 最近访问时间（epoch ms）
    qint64 confidenceUpdatedAt = 0;      ///< 上次置信度衰减/反馈时间（epoch ms）
    quint32 accessCount = 0;             ///< 访问计数
    quint32 strength = 0;                ///< 巩固计数
    float confidence = 1.0f;             ///< 置信度（0.0-1.0）
    float trustScore = 1.0f;             ///< 来源信任分
    bool active = true;                  ///< 逻辑删除标记
    bool hasConflict = false;            ///< 管理列表派生状态，不持久化
};

/**
 * @brief 记忆图边（阶段 2 起使用明确的边对象）
 *
 * TagShared、Explicit 与 Conflict 边用于双向检索传播；Supersedes 边保持
 * sourceId（新条目）到 targetId（被覆盖条目）的语义方向，权重用于 BFS 传播衰减。
 */
struct MemoryEdge
{
    QString id;                          ///< 复合键：sourceId|targetId|type|tag（规范序）
    QString sourceId;                    ///< 端点 A
    QString targetId;                    ///< 端点 B

    enum class Type
    {
        Explicit,                        ///< 显式关系边
        Related,                         ///< 检索共现发现的相关边
        TagShared,                       ///< 共享标签派生边
        Supersedes,                      ///< 新条目确认覆盖旧条目
        Conflict                         ///< 相近但无法确认覆盖的条目
    };

    Type type = Type::TagShared;         ///< 边类型
    QString tag;                         ///< TagShared 边对应的共享标签；Explicit 边为空
    float weight = 1.0f;                 ///< 传播权重（>0）
    qint64 createdAt = 0;                ///< 创建时间（epoch ms）
    bool active = true;                  ///< 逻辑删除标记
};

/**
 * @brief LLM 巩固候选与已有记忆之间的关系
 */
enum class MEMORY_CONSOLIDATION_RELATION
{
    NONE,
    SUPERSEDES,
    CONFLICTS
};

/**
 * @brief 经本地 schema 校验后的 LLM 巩固候选
 *
 * 该 DTO 仅在线程边界上传递 Qt 值类型；MemoryService 仍会再次执行内容隐私校验。
 */
struct _tagMemoryConsolidationCandidate
{
    MemoryEntry entry;                         ///< 候选记忆条目
    MEMORY_CONSOLIDATION_RELATION relation = MEMORY_CONSOLIDATION_RELATION::NONE;
    QString relatedMemoryId;                   ///< 可选的已有条目 ID
};

/**
 * @brief 图检索命中（条目 + 传播得分）
 */
struct _tagGraphHit
{
    MemoryEntry entry;                   ///< 命中条目
    float score = 0.0f;                  ///< 检索/传播得分
};

/**
 * @brief 记忆图（阶段 1：条目 + 标签/关键词索引；阶段 2：显式边 + BFS）
 *
 * 仅由 MemoryService 后台 worker 线程独占访问，不提供内部锁。
 * 检索为标签命中与中文关键词子串匹配的混合召回，排序结果稳定；
 * 阶段 2 支持以向量种子为起点的深度 ≤ 2 BFS 传播。
 */
class MemoryGraph
{
public:
    MemoryGraph() = default;

    /**
     * @brief 新增条目；重复 ID 或空内容被拒绝
     * @param[in] entry 待新增条目
     * @param[out] errorMessage 错误描述
     * @return 新增成功返回 true
     */
    bool AddEntry(const MemoryEntry &entry, QString &errorMessage);

    /**
     * @brief 按 ID 获取条目
     * @param[in] id 条目 ID
     * @param[out] entry 输出条目
     * @return 获取成功返回 true
     */
    bool GetEntry(const QString &id, MemoryEntry &entry) const;

    /**
     * @brief 逻辑删除条目（active = false，保留数据供未来恢复）
     * @param[in] id 条目 ID
     * @return 删除成功返回 true
     */
    bool SoftDeleteEntry(const QString &id);

    /**
     * @brief 按 ID 更新条目；更新时维护索引一致性
     * @param[in] entry 更新后的条目（ID 必须已存在）
     * @param[out] errorMessage 错误描述
     * @return 更新成功返回 true
     */
    bool UpdateEntry(const MemoryEntry &entry, QString &errorMessage);

    /**
     * @brief 为条目新增标签
     * @param[in] id 条目 ID
     * @param[in] tag 标签名
     * @param[out] errorMessage 错误描述
     * @return 添加成功返回 true
     */
    bool AddTag(const QString &id, const QString &tag, QString &errorMessage);

    /**
     * @brief 移除条目标签
     * @param[in] id 条目 ID
     * @param[in] tag 标签名
     * @param[out] errorMessage 错误描述
     * @return 移除成功返回 true
     */
    bool RemoveTag(const QString &id, const QString &tag, QString &errorMessage);

    /**
     * @brief 列出活跃条目
     * @param[in] scope 作用域过滤
     * @param[in] petId 宠物 ID；Global 记忆不受 petId 过滤
     * @return 活跃条目列表（按插入顺序）
     */
    QVector<MemoryEntry> ListEntries(MemoryEntry::Scope scope, const QString &petId) const;

    /**
     * @brief 按标签精确匹配列出活跃条目
     * @param[in] tags 标签列表
     * @param[in] scope 作用域过滤
     * @param[in] petId 宠物 ID
     * @param[in] maxResults 结果上限
     * @return 匹配条目，按访问热度与最近访问排序
     */
    QVector<MemoryEntry> SearchByTags(const QStringList &tags,
                                      MemoryEntry::Scope scope,
                                      const QString &petId,
                                      int maxResults) const;

    /**
     * @brief 中文关键词/标签混合召回
     * @param[in] query 查询文本（支持中文子串与英文关键词）
     * @param[in] scope 作用域过滤
     * @param[in] petId 宠物 ID
     * @param[in] maxResults 结果上限
     * @return 匹配条目，按匹配强度、访问热度与最近访问排序
     */
    QVector<MemoryEntry> SearchByKeywords(const QString &query,
                                          MemoryEntry::Scope scope,
                                          const QString &petId,
                                          int maxResults) const;

    /**
     * @brief 匹配当前上下文中的负面和程序性记忆触发模式
     * @param[in] contextText 当前上下文文本
     * @param[in] scope 作用域过滤
     * @param[in] petId 宠物 ID
     * @param[in] maxResults 结果上限
     * @return 按负面记忆优先、触发强度和更新时间排序的命中条目
     */
    QVector<MemoryEntry> MatchTriggeredEntries(const QString &contextText,
                                                MemoryEntry::Scope scope,
                                                const QString &petId,
                                                int maxResults) const;

    /**
     * @brief 统计活跃条目数量
     * @return 活跃条目数量
     */
    int ActiveEntryCount() const;

    /**
     * @brief 统计全部条目数量（含逻辑删除）
     * @return 全部条目数量
     */
    int EntryCount() const;

    /**
     * @brief 返回全部标签（按使用次数降序）
     * @return 标签列表
     */
    QStringList AllTags() const;

    /**
     * @brief 导出全部条目（含逻辑删除，按插入顺序），供序列化
     * @return 条目列表
     */
    QVector<MemoryEntry> AllEntries() const;

    /**
     * @brief 以条目列表重建图与索引；忽略重复 ID 条目
     * @param[in] entries 条目列表
     * @param[out] errorMessage 错误描述
     * @return 重建成功返回 true
     */
    bool LoadEntries(const QVector<MemoryEntry> &entries, QString &errorMessage);

    /**
     * @brief 新增一条无向边；重复边（相同键）幂等返回成功
     * @param[in] sourceId 端点 A
     * @param[in] targetId 端点 B
     * @param[in] type 边类型
     * @param[in] tag 共享标签（仅 TagShared 边使用）
     * @param[in] weight 传播权重（必须 > 0）
     * @param[out] errorMessage 错误描述
     * @return 添加成功返回 true
     */
    bool AddEdge(const QString &sourceId,
                 const QString &targetId,
                 MemoryEdge::Type type,
                 const QString &tag,
                 float weight,
                 QString &errorMessage);

    /**
     * @brief 按复合键删除边
     * @param[in] edgeId 边 ID
     * @param[out] errorMessage 错误描述
     * @return 删除成功返回 true；边不存在返回 false
     */
    bool RemoveEdge(const QString &edgeId, QString &errorMessage);

    /**
     * @brief 更新已有边权重
     * @param[in] edgeId 边 ID
     * @param[in] weight 新权重（必须 > 0）
     * @param[out] errorMessage 错误描述
     * @return 更新成功返回 true
     */
    bool SetEdgeWeight(const QString &edgeId, float weight, QString &errorMessage);

    /**
     * @brief 导出全部边（含逻辑删除），供序列化
     * @return 边列表
     */
    QVector<MemoryEdge> AllEdges() const;

    /**
     * @brief 以边列表重建边表与邻接索引；忽略端点缺失或重复 ID 的边
     * @param[in] edges 边列表
     * @param[out] errorMessage 错误描述
     * @return 重建成功返回 true
     */
    bool LoadEdges(const QVector<MemoryEdge> &edges, QString &errorMessage);

    /**
     * @brief 按 ID 集合返回活跃条目（保持插入顺序，应用作用域过滤）
     * @param[in] ids 条目 ID 集合
     * @param[in] scope 作用域过滤
     * @param[in] petId 宠物 ID
     * @return 活跃条目列表
     */
    QVector<MemoryEntry> ActiveEntriesByIds(const QSet<QString> &ids,
                                            MemoryEntry::Scope scope,
                                            const QString &petId) const;

    /**
     * @brief 从种子得分出发执行 BFS 传播（不含种子本身）
     *
     * 沿活跃边展开，每跳得分乘以边权重与衰减系数；深度不超过 maxDepth，
     * 邻居须通过作用域过滤。结果按得分降序，同分按访问热度排序。
     *
     * @param[in] seedScores 种子条目 ID -> 基础得分
     * @param[in] scope 作用域过滤
     * @param[in] petId 宠物 ID
     * @param[in] maxDepth 最大传播深度（<=0 时不展开）
     * @param[in] decayPerHop 每跳得分衰减（0.0-1.0）
     * @return 传播命中（不含种子）
     */
    QVector<_tagGraphHit> ExpandByEdges(const QHash<QString, float> &seedScores,
                                        MemoryEntry::Scope scope,
                                        const QString &petId,
                                        int maxDepth,
                                        float decayPerHop) const;

    /**
     * @brief 清空图与索引
     */
    void Clear();

private:
    /**
     * @brief 从正文提取检索关键词（英文词 + 中文连续片段）
     * @param[in] text 原始文本
     * @return 关键词列表
     */
    static QStringList ExtractKeywords(const QString &text);

    /**
     * @brief 重建指定条目的索引（标签与关键词）
     * @param[in] entry 条目
     */
    void RebuildIndexes(const MemoryEntry &entry);

    /**
     * @brief 移除指定条目的索引（标签与关键词）
     * @param[in] entry 条目
     */
    void RemoveIndexes(const MemoryEntry &entry);

    /**
     * @brief 构造边的规范复合键（端点按字典序）
     * @param[in] sourceId 端点 A
     * @param[in] targetId 端点 B
     * @param[in] type 边类型
     * @param[in] tag 共享标签
     * @return 边键
     */
    static QString MakeEdgeKey(const QString &sourceId,
                               const QString &targetId,
                               MemoryEdge::Type type,
                               const QString &tag);

    /**
     * @brief 将条目与其标签关联的其它活跃条目建立 TagShared 边
     * @param[in] entry 条目
     */
    void SyncTagEdges(const MemoryEntry &entry);

    /**
     * @brief 移除条目在指定标签上的全部 TagShared 边
     * @param[in] entryId 条目 ID
     * @param[in] tag 标签；为空时移除该条目全部 TagShared 边
     */
    void RemoveTagEdgesOfEntry(const QString &entryId, const QString &tag = QString());

    /**
     * @brief 将边插入双向邻接索引
     * @param[in] edge 边
     */
    void IndexEdge(const MemoryEdge &edge);

    /**
     * @brief 从双向邻接索引移除边
     * @param[in] edge 边
     */
    void UnindexEdge(const MemoryEdge &edge);

    /**
     * @brief 计算条目对查询的匹配得分
     * @param[in] entry 条目
     * @param[in] queryKeywords 查询关键词
     * @param[in] query 原始查询（用于子串回退）
     * @return 得分；无匹配返回 0
     */
    int ScoreEntry(const MemoryEntry &entry,
                   const QStringList &queryKeywords,
                   const QString &query) const;

    QHash<QString, MemoryEntry> m_entries;       ///< 条目表
    QVector<QString> m_insertionOrder;           ///< 插入顺序，保证序列化稳定
    QHash<QString, QSet<QString>> m_tagIndex;    ///< 标签 -> 条目 ID 集
    QHash<QString, QSet<QString>> m_keywordIndex; ///< 关键词 -> 条目 ID 集
    QHash<QString, MemoryEdge> m_edges;          ///< 边表（键为复合键）
    QHash<QString, QStringList> m_adjacency;     ///< 条目 ID -> 关联边 ID 列表
};

} // namespace vpet

#endif // VPET_MEMORY_MEMORY_GRAPH_H
