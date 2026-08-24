#ifndef VPET_DAGEDITOR_DAG_DOCUMENT_STORE_H
#define VPET_DAGEDITOR_DAG_DOCUMENT_STORE_H

#include "vpet/dageditor/agent_dag_validator.h"
#include "vpet/dageditor/dag_document.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

namespace vpet
{

class AgentNodeCatalog;

/**
 * @brief 编辑器文档仓库：唯一状态持有者
 *
 * 职责：内存文档基线 + 进程内单调 revision + 乐观并发检查 + 原子落盘（QSaveFile）
 * + 旧文件 .bak 备份 + 外部修改采纳。仅允许在主线程使用。
 */
class DagDocumentStore : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 保存结果
     */
    enum class SaveStatus
    {
        Saved,   ///< 已落盘并更新基线
        Conflict, ///< baseRevision 过期；调用方应重读当前文档
        Invalid  ///< 校验未通过；issuesOut 携带问题列表
    };

    /**
     * @brief 构造空仓库
     * @param[in] parent 父对象
     */
    explicit DagDocumentStore(QObject *parent = nullptr);

    /**
     * @brief 绑定配置文件并建立内存基线
     *
     * 文件不存在时以空文档为基线（首次保存时创建）；存在时解析失败返回 false。
     *
     * @param[in] configPath 配置文件路径
     * @param[in] catalog 节点目录（校验用）；空则使用进程默认实例
     * @param[out] errorMessage 失败描述
     * @return 初始化成功返回 true
     */
    bool Initialize(const QString &configPath,
                    const AgentNodeCatalog *catalog,
                    QString &errorMessage);

    /**
     * @brief 获取当前文档（只读）
     * @return 文档引用
     */
    const DagDocument &GetDocument() const;

    /**
     * @brief 获取当前修订号
     * @return 修订号
     */
    quint64 GetRevision() const;

    /**
     * @brief 获取绑定的配置文件路径
     * @return 配置文件路径
     */
    QString GetConfigPath() const;

    /**
     * @brief 乐观并发保存：校验 → 备份 → 原子落盘 → 更新基线
     * @param[in] document 待保存文档
     * @param[in] baseRevision 调用方基于的修订号
     * @param[out] issuesOut 校验问题（Invalid 时非空）
     * @param[out] errorMessage 磁盘错误描述
     * @return 保存结果状态
     */
    SaveStatus Save(const DagDocument &document,
                    quint64 baseRevision,
                    QVector<DagValidationIssue> &issuesOut,
                    QString &errorMessage);

    /**
     * @brief 采纳磁盘上的外部修改为新基线
     * @param[out] errorMessage 失败描述
     * @return 内容确实变化并采纳返回 true；内容未变化返回 false 且不视为错误
     */
    bool AdoptExternalFile(QString &errorMessage);

    /**
     * @brief 干跑校验（不入盘）
     * @param[in] document 待校验文档
     * @return 问题列表
     */
    QVector<DagValidationIssue> Validate(const DagDocument &document) const;

signals:
    /**
     * @brief 文档已保存
     * @param[in] revision 新修订号
     */
    void DocumentSaved(quint64 revision);

    /**
     * @brief 检测到外部修改并已采纳
     * @param[in] revision 新修订号
     */
    void ExternalChanged(quint64 revision);

private:
    /**
     * @brief 计算文件内容指纹
     * @param[in] rawBytes 文件字节
     * @return SHA-256 指纹
     */
    static QByteArray Fingerprint(const QByteArray &rawBytes);

    QString m_configPath;             ///< 配置文件路径
    DagDocument m_document;           ///< 当前文档基线
    quint64 m_revision;               ///< 进程内单调修订号
    QByteArray m_fingerprint;         ///< 最近一次读取/写入的内容指纹
    AgentDagValidator m_validator;    ///< 校验器
};

} // namespace vpet

#endif // VPET_DAGEDITOR_DAG_DOCUMENT_STORE_H
