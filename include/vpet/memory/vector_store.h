#ifndef VPET_MEMORY_VECTOR_STORE_H
#define VPET_MEMORY_VECTOR_STORE_H

#include <QString>
#include <QVector>

#include <memory>

namespace vpet
{

/**
 * @brief 本地 SQLite 向量存储（vectors.sqlite3）
 *
 * 以 float32 BLOB 保存向量，按模型 ID 与维度校验；检索为精确 cosine
 * 相似度（L2 归一化后等价于内积）的暴力 top-k，适用于本地数千条目规模。
 * 仅由 MemoryService 后台 worker 线程独占访问。
 */
class VectorStore
{
public:
    /**
     * @brief 向量检索命中
     */
    struct _tagVectorHit
    {
        QString entryId;             ///< 条目 ID
        float score = 0.0f;          ///< 相似度得分（归一化向量内积）
    };

    /**
     * @brief 构造函数（定义于实现文件，避免调用方实例化内部类型）
     */
    VectorStore();

    /**
     * @brief 析构函数；关闭数据库连接
     */
    ~VectorStore();

    VectorStore(const VectorStore &) = delete;
    VectorStore &operator=(const VectorStore &) = delete;

    /**
     * @brief 打开（或创建）向量数据库并确保表结构存在
     * @param[in] dbPath SQLite 文件路径
     * @param[out] errorMessage 错误描述
     * @return 成功返回 true；已打开时直接返回 true
     */
    bool Open(const QString &dbPath, QString &errorMessage);

    /**
     * @brief 是否已打开
     * @return 已打开返回 true
     */
    bool IsOpen() const;

    /**
     * @brief 关闭数据库连接；之后可重新 Open
     */
    void Close();

    /**
     * @brief 插入或替换条目向量
     * @param[in] entryId 条目 ID
     * @param[in] modelId 模型标识
     * @param[in] dimension 向量维度
     * @param[in] embedding 向量数据（长度必须等于 dimension）
     * @param[out] errorMessage 错误描述
     * @return 成功返回 true
     */
    bool Upsert(const QString &entryId,
                const QString &modelId,
                int dimension,
                const QVector<float> &embedding,
                QString &errorMessage);

    /**
     * @brief 删除条目向量（逻辑删除记忆时同步调用）
     * @param[in] entryId 条目 ID
     * @param[out] errorMessage 错误描述
     * @return 成功返回 true；条目不存在也视为成功
     */
    bool Remove(const QString &entryId, QString &errorMessage);

    /**
     * @brief 读取条目向量（供测试与诊断）
     * @param[in] entryId 条目 ID
     * @param[out] modelId 模型标识
     * @param[out] dimension 向量维度
     * @param[out] embedding 向量数据
     * @param[out] errorMessage 错误描述
     * @return 存在返回 true
     */
    bool Get(const QString &entryId,
             QString &modelId,
             int &dimension,
             QVector<float> &embedding,
             QString &errorMessage) const;

    /**
     * @brief 按精确余弦相似度返回 top-k 命中
     *
     * 维度与查询向量不一致的行被忽略（模型变更后的旧数据安全失效）。
     *
     * @param[in] modelId 模型标识
     * @param[in] queryEmbedding 查询向量（须为 L2 归一化）
     * @param[in] maxResults 结果上限；<=0 时返回全部
     * @param[out] hits 命中列表（按得分降序，同分按条目 ID）
     * @param[out] errorMessage 错误描述
     * @return 成功返回 true
     */
    bool QueryTopK(const QString &modelId,
                   const QVector<float> &queryEmbedding,
                   int maxResults,
                   QVector<_tagVectorHit> &hits,
                   QString &errorMessage) const;

    /**
     * @brief 清空指定模型的所有向量（模型变更后清理旧数据）
     * @param[in] modelId 模型标识
     * @param[out] errorMessage 错误描述
     * @return 成功返回 true
     */
    bool ClearForModel(const QString &modelId, QString &errorMessage);

    /**
     * @brief 清空全部向量（导入替换整张记忆图时使用）
     * @param[out] errorMessage 错误描述
     * @return 成功返回 true
     */
    bool ClearAll(QString &errorMessage);

    /**
     * @brief 指定模型的向量条数
     * @param[in] modelId 模型标识
     * @return 条数；未打开时返回 0
     */
    int Count(const QString &modelId) const;

private:
    /**
     * @brief 内部实现（隔离 QSqlDatabase）
     */
    struct _tagImpl;

    std::unique_ptr<_tagImpl> m_impl; ///< 内部实现
};

} // namespace vpet

#endif // VPET_MEMORY_VECTOR_STORE_H
