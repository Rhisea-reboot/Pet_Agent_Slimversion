#ifndef VPET_MEMORY_MEMORY_MAINTENANCE_H
#define VPET_MEMORY_MEMORY_MAINTENANCE_H

#include "vpet/memory/memory_graph.h"

#include <QString>
#include <QVector>

namespace vpet
{

/**
 * @brief 检索后维护配置
 */
struct _tagMemoryMaintenanceConfig
{
    bool enabled = true;                 ///< 检索后维护总开关
    int decayIntervalHours = 24;         ///< 置信度衰减最小执行间隔
    int clusterUpdateRetrievals = 20;    ///< 每 N 次检索重建簇元数据
    int maxGapRecords = 128;             ///< 最多保留的缺口摘要数量
    int inferredTagMinSupport = 2;       ///< 标签推断所需的最小命中支持数
    int deepMaintenanceRetrievals = 50;  ///< 每 N 次检索运行深度维护
    float duplicateSimilarityThreshold = 0.95f; ///< 全图重复合并阈值
    float weakConfidenceThreshold = 0.05f; ///< 弱记忆剪枝置信度阈值
    quint32 weakStrengthLimit = 1;       ///< 弱记忆剪枝强度上限
    float relatedInitialWeight = 0.4f;   ///< 新共现链接初始权重
    float relatedWeightIncrement = 0.1f; ///< 重复共现时的权重增量
    float maxRelatedWeight = 1.0f;       ///< 共现链接权重上限
};

/**
 * @brief 记忆检索后维护器
 *
 * 仅由 MemoryService worker 线程调用，不提供内部锁。缺口文件只保存查询哈希，
 * 不保存普通对话原文；簇元数据和缺口记录均通过 QSaveFile 原子写入。
 */
class MemoryMaintenance
{
public:
    MemoryMaintenance() = default;

    /**
     * @brief 设置维护配置
     * @param[in] config 维护配置
     * @param[out] errorMessage 错误描述
     * @return 配置有效返回 true
     */
    bool SetConfig(const _tagMemoryMaintenanceConfig &config, QString &errorMessage);

    /**
     * @brief 设置记忆数据目录
     * @param[in] memoryDir memory/ 目录
     * @param[out] errorMessage 错误描述
     * @return 目录有效且可创建返回 true
     */
    bool SetMemoryDir(const QString &memoryDir, QString &errorMessage);

    /**
     * @brief 检索前按类型半衰期增量衰减活跃记忆置信度
     * @param[in,out] graph 记忆图
     * @param[in] nowMs 当前 epoch 毫秒时间
     * @return 图发生变化返回 true
     */
    bool ApplyConfidenceDecay(MemoryGraph &graph, qint64 nowMs) const;

    /**
     * @brief 对检索结果执行访问更新、链接发现、标签推断、缺口和簇维护
     * @param[in,out] graph 记忆图
     * @param[in] entries 本次检索命中
     * @param[in] query 查询文本；仅计算哈希，不写入磁盘
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] nowMs 当前 epoch 毫秒时间
     * @param[out] removedIds 深度维护停用的条目 ID
     * @param[out] errorMessage 文件维护诊断
     * @return 图发生变化返回 true
     */
    bool OnRetrieved(MemoryGraph &graph,
                     const QVector<MemoryEntry> &entries,
                     const QString &query,
                     const QString &petId,
                     const QString &triggerType,
                     qint64 nowMs,
                     QStringList &removedIds,
                     QString &errorMessage);

    /**
     * @brief 应用用户对已浮现记忆的有帮助/无帮助反馈
     * @param[in,out] graph 记忆图
     * @param[in] memoryIds 反馈涉及的记忆 ID
     * @param[in] petId 当前宠物 ID
     * @param[in] helpful 是否有帮助
     * @param[in] nowMs 当前 epoch 毫秒时间
     * @return 至少更新一条记忆返回 true
     */
    bool ApplyFeedback(MemoryGraph &graph,
                       const QStringList &memoryIds,
                       const QString &petId,
                       bool helpful,
                       qint64 nowMs) const;

    /**
     * @brief 计算检索排序乘数（置信度、来源信任、近期和访问强化）
     * @param[in] entry 记忆条目
     * @param[in] nowMs 当前 epoch 毫秒时间
     * @return 非负排序乘数
     */
    static float RetrievalWeight(const MemoryEntry &entry, qint64 nowMs);

    /**
     * @brief 执行保守的全图重复合并和弱记忆逻辑删除
     * @param[in,out] graph 记忆图
     * @param[in] nowMs 当前 epoch 毫秒时间
     * @param[out] removedIds 被合并或剪枝的条目 ID
     * @return 图发生变化返回 true
     */
    bool RunDeepConsolidation(MemoryGraph &graph,
                              qint64 nowMs,
                              QStringList &removedIds) const;

private:
    /**
     * @brief 发现或加强同次检索条目之间的 related 边
     * @param[in,out] graph 记忆图
     * @param[in] entries 本次检索命中
     * @return 图发生变化返回 true
     */
    bool DiscoverLinks(MemoryGraph &graph, const QVector<MemoryEntry> &entries) const;

    /**
     * @brief 根据命中集合中已有标签的共同支持进行保守标签推断
     * @param[in,out] graph 记忆图
     * @param[in] entries 本次检索命中
     * @return 图发生变化返回 true
     */
    bool InferTags(MemoryGraph &graph, const QVector<MemoryEntry> &entries) const;

    /**
     * @brief 以查询哈希记录无命中的检索缺口
     * @param[in] query 查询文本
     * @param[in] petId 宠物 ID
     * @param[in] triggerType 触发类型
     * @param[in] nowMs 当前 epoch 毫秒时间
     * @param[out] errorMessage 错误描述
     * @return 写入成功返回 true
     */
    bool RecordGap(const QString &query,
                   const QString &petId,
                   const QString &triggerType,
                   qint64 nowMs,
                   QString &errorMessage) const;

    /**
     * @brief 从 related 边重建连通簇元数据
     * @param[in] graph 记忆图
     * @param[in] nowMs 当前 epoch 毫秒时间
     * @param[out] errorMessage 错误描述
     * @return 写入成功返回 true
     */
    bool RebuildClusters(const MemoryGraph &graph,
                         qint64 nowMs,
                         QString &errorMessage) const;

    _tagMemoryMaintenanceConfig m_config; ///< 当前维护配置
    QString m_memoryDir;                  ///< memory/ 数据目录
    quint64 m_retrievalCount = 0;         ///< 当前进程累计检索次数
};

} // namespace vpet

#endif // VPET_MEMORY_MEMORY_MAINTENANCE_H
