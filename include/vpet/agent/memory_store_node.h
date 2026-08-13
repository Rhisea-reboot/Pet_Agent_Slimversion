#ifndef VPET_AGENT_MEMORY_STORE_NODE_H
#define VPET_AGENT_MEMORY_STORE_NODE_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"
#include "vpet/memory/memory_graph.h"
#include "vpet/memory/memory_service.h"

#include <QVariantMap>
#include <QString>

namespace vpet
{

/**
 * @brief 记忆存储节点（memory.store）
 *
 * 位于 output.format 之后。阶段 1 仅接受显式记忆写入：解析当前 turn
 * 的 user.input 中的明确命令（记住 / 以后不要 / 忘了等），或消费上游
 * 已写入 memory.store.intent 的意图。绝不提交完整 conversation.history。
 */
class MemoryStoreNode
{
public:
    /**
     * @brief 执行记忆存储节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[in] service 记忆服务；为空或未启动时跳过
     * @param[in] config 记忆配置（default_scope）
     * @param[out] errorMessage 错误描述（本节点正常路径不填写）
     * @return 始终返回 true
     */
    static bool Execute(const _tagAgentDagNode &node,
                        AgentContext &context,
                        MemoryService *service,
                        const MemoryConfig &config,
                        QString &errorMessage);

    /**
     * @brief 从用户输入解析显式记忆命令
     *
     * 支持前缀模式：记住/请记住/以后记得/以后请（remember），
     * 记住流程（procedure，名称/触发/步骤等分号分隔字段），
     * 以后不要/不要再/别再（negative），更正一下/纠正一下（correction），
     * 忘了/忘记/删除记忆（forget）。问句与空内容不触发。
     *
     * @param[in] userInput 用户输入
     * @param[out] intent 解析出的意图（action/content/type/scope）
     * @return 解析出有效命令返回 true
     */
    static bool ParseCommand(const QString &userInput, QVariantMap &intent);

    /**
     * @brief 根据意图构造记忆条目
     * @param[in] intent 意图（action 必须为 remember）
     * @param[in] petId 当前宠物 ID
     * @param[in] defaultScope 默认作用域（global / pet）
     * @param[out] entry 输出的记忆条目
     * @param[out] errorMessage 错误描述
     * @return 构造成功返回 true
     */
    static bool BuildEntry(const QVariantMap &intent,
                           const QString &petId,
                           const QString &defaultScope,
                           MemoryEntry &entry,
                           QString &errorMessage);
};

} // namespace vpet

#endif // VPET_AGENT_MEMORY_STORE_NODE_H
