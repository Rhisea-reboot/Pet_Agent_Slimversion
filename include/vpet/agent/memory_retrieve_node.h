#ifndef VPET_AGENT_MEMORY_RETRIEVE_NODE_H
#define VPET_AGENT_MEMORY_RETRIEVE_NODE_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"
#include "vpet/memory/memory_service.h"

#include <QString>

namespace vpet
{

/**
 * @brief 记忆检索节点（memory.retrieve）
 *
 * 读取 mailbox 中已完成的最新结果并组装只读提示段，随后立刻提交
 * 当前 prompt 的检索任务供下一轮使用。任何记忆服务故障都不影响
 * 正常回复：本节点始终成功返回，绝不设置 runtime.async.pending。
 */
class MemoryRetrieveNode
{
public:
    /**
     * @brief 执行记忆检索节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[in] service 记忆服务；为空或未启动时跳过
     * @param[in] config 记忆配置（max_results / prompt_budget_chars）
     * @param[out] errorMessage 错误描述（本节点正常路径不填写）
     * @return 始终返回 true
     */
    static bool Execute(const _tagAgentDagNode &node,
                        AgentContext &context,
                        MemoryService *service,
                        const MemoryConfig &config,
                        QString &errorMessage);

private:
    /**
     * @brief 读取当前 prompt（依次尝试 node.input.prompt /
     *        semantic.text.prompt / prompt.text，最后回退用户输入）
     * @param[in] context 运行时上下文
     * @return 当前 prompt 文本
     */
    static QString ReadPrompt(const AgentContext &context);

    /**
     * @brief 解析当前宠物 ID
     * @param[in] context 运行时上下文
     * @return 宠物 ID；缺失时返回 default
     */
    static QString ResolvePetId(const AgentContext &context);
};

} // namespace vpet

#endif // VPET_AGENT_MEMORY_RETRIEVE_NODE_H
