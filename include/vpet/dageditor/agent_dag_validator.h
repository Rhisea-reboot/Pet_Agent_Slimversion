#ifndef VPET_DAGEDITOR_AGENT_DAG_VALIDATOR_H
#define VPET_DAGEDITOR_AGENT_DAG_VALIDATOR_H

#include "vpet/dageditor/agent_node_catalog.h"
#include "vpet/dageditor/dag_document.h"

#include <QString>
#include <QVector>

namespace vpet
{

/**
 * @brief 校验问题级别
 */
enum class DagValidationSeverity
{
    Error,   ///< 阻断保存与热重载
    Warning  ///< 仅提示，不阻断
};

/**
 * @brief 单条校验结论
 */
struct DagValidationIssue
{
    DagValidationSeverity severity = DagValidationSeverity::Error; ///< 级别
    QString nodeId;   ///< 关联节点标识；图级问题为空
    QString code;     ///< 稳定英文标识（如 unknown_type），供前端映射文案
    QString message;  ///< 中文描述
};

/**
 * @brief DAG 文档校验器（结构规则复用 AgentDagGraph 规则 + 目录语义规则）
 *
 * 同一实例可反复调用 Validate；仅在主线程使用。
 */
class AgentDagValidator
{
public:
    /**
     * @brief 构造校验器
     * @param[in] catalog 节点目录；传空时使用进程默认实例
     */
    explicit AgentDagValidator(const AgentNodeCatalog *catalog = nullptr);

    /**
     * @brief 全量校验一份文档
     *
     * 结构规则：空节点表、空/重复节点 id、未知类型、边引用缺失节点、自环、
     * 重复边、环检测。
     * 语义规则：必填 config 缺失、数值越界、枚举非法、join 无 merge 策略提示、
     * 孤立节点、非 source 节点声明 trigger。
     *
     * @param[in] document 待校验文档
     * @return 问题列表；error 级存在时保存应被拒绝
     */
    QVector<DagValidationIssue> Validate(const DagDocument &document) const;

    /**
     * @brief 判断问题列表中是否存在 error 级问题
     * @param[in] issues 问题列表
     * @return 存在 error 返回 true
     */
    static bool HasErrors(const QVector<DagValidationIssue> &issues);

private:
    /**
     * @brief 校验单个节点的 config 与目录 spec 是否一致
     * @param[in] node 待检查节点
     * @param[in] spec 目录 spec
     * @param[in,out] issues 追加发现的问题
     */
    void ValidateNodeConfig(const _tagDagDocumentNode &node,
                            const DagNodeSpec &spec,
                            QVector<DagValidationIssue> &issues) const;

    const AgentNodeCatalog *m_catalog; ///< 节点目录引用（不持有所有权）
};

} // namespace vpet

#endif // VPET_DAGEDITOR_AGENT_DAG_VALIDATOR_H
