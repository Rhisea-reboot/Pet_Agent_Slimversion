#ifndef VPET_MEMORY_MEMORY_REPOSITORY_H
#define VPET_MEMORY_MEMORY_REPOSITORY_H

#include "vpet/memory/memory_graph.h"

#include <QString>
#include <QVector>

namespace vpet
{

/**
 * @brief 记忆持久化与隐私过滤
 *
 * 负责 graph.json 的原子读写、schema 校验、损坏文件备份与隐私过滤。
 * 仅由 MemoryService 后台 worker 线程独占访问。
 */
class MemoryRepository
{
public:
    static constexpr int CURRENT_SCHEMA_VERSION = 5;
    static constexpr int FIRST_SUPPORTED_SCHEMA_VERSION = 1;
    static constexpr int MAX_MEMORY_CONTENT_CHARS = 2000;

    /**
     * @brief 加载结果描述
     *
     * 阶段 1（schemaVersion=1）文件按无边数据兼容加载；
     * 阶段 2（schemaVersion=2）额外读取边表；阶段 4（schemaVersion=4）
     * 额外保存置信度维护时间戳和 related 边类型；阶段 5（schemaVersion=5）
     * 保存触发模式与结构化程序性记忆。
     */
    struct _tagLoadResult
    {
        bool ok = false;            ///< 是否成功以数据初始化图
        bool recovered = false;     ///< 是否因损坏/未知版本而恢复空图
        QString detail;             ///< 诊断信息（不包含敏感原文）
        QString backupPath;         ///< 损坏文件备份路径（发生恢复时非空）
    };

    /**
     * @brief 设置数据根目录并在其下创建 memory/ 子目录
     * @param[in] dataDir 数据根目录；为空时使用 AppDataLocation
     * @param[out] errorMessage 错误描述
     * @return 设置成功返回 true
     */
    bool SetDataDir(const QString &dataDir, QString &errorMessage);

    /**
     * @brief 获取记忆数据目录（<dataDir>/memory）
     * @return 记忆数据目录
     */
    QString MemoryDir() const;

    /**
     * @brief 获取 graph.json 完整路径
     * @return graph.json 路径
     */
    QString GraphFilePath() const;

    /**
     * @brief 从 graph.json 加载记忆图
     *
     * 文件不存在时返回空图；JSON 损坏、字段缺失或 schema 版本未知时
     * 保留损坏文件副本、记录诊断信息并返回空图，绝不阻止应用启动。
     *
     * @param[out] graph 输出的记忆图
     * @return 加载结果
     */
    _tagLoadResult Load(MemoryGraph &graph);

    /**
     * @brief 原子写入 graph.json（QSaveFile）
     * @param[in] graph 记忆图
     * @param[out] errorMessage 错误描述
     * @return 写入成功返回 true；失败时原文件保持完整
     */
    bool Save(const MemoryGraph &graph, QString &errorMessage) const;

    /**
     * @brief 将当前记忆图原子导出到指定 JSON 文件
     * @param[in] graph 待导出的记忆图
     * @param[in] filePath 目标文件路径
     * @param[out] errorMessage 错误描述
     * @return 导出成功返回 true
     */
    bool Export(const MemoryGraph &graph,
                const QString &filePath,
                QString &errorMessage) const;

    /**
     * @brief 从指定 JSON 文件导入记忆图
     * @param[in] filePath 源文件路径
     * @param[out] graph 输出记忆图；失败时保持不变
     * @param[out] errorMessage 错误描述
     * @return 导入成功返回 true
     */
    bool Import(const QString &filePath,
                MemoryGraph &graph,
                QString &errorMessage) const;

    /**
     * @brief 隐私过滤统一入口
     *
     * 拒绝明确凭证、PEM 私钥、JWT、疑似 .env 内容、高风险个人标识符
     * 与超长文本。规则保守：不确定时不存储。
     *
     * @param[in] content 待校验内容
     * @param[out] errorCategory 拒绝原因类别（credential / private_key / jwt /
     *                            env_file / id_card / bank_card / too_long）
     * @return 允许存储返回 true
     */
    static bool ValidateContent(const QString &content, QString &errorCategory);

    /**
     * @brief 校验记忆条目的全部可持久化文本字段
     * @param[in] entry 待校验条目
     * @param[out] errorCategory 拒绝原因类别
     * @return 全部字段允许存储返回 true
     */
    static bool ValidateEntry(const MemoryEntry &entry, QString &errorCategory);

private:
    QString m_dataDir; ///< 数据根目录（空表示未设置）
};

} // namespace vpet

#endif // VPET_MEMORY_MEMORY_REPOSITORY_H
