#ifndef VPET_AGENT_TOOL_LOOP_NODE_H
#define VPET_AGENT_TOOL_LOOP_NODE_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"
#include "vpet/agent/tools/tool_loop_executor.h"

#include <QString>

namespace vpet
{

/**
 * @brief tool.loop 节点的协议适配器。
 *
 * 负责节点配置解析与校验（未知工具名、非法枚举值报错）、从 invocation
 * 上下文组装循环请求，以及把循环结果写入 semantic.text.* 与
 * semantic.tool.* 协议键；不负责异步关联或图调度。
 */
class ToolLoopNode
{
public:
    /**
     * @brief 解析并校验节点配置。
     * @param[in] node 节点定义。
     * @param[in] registry 工具注册表（用于工具名校验）。
     * @param[out] config 归一化配置。
     * @param[out] errorMessage 错误描述。
     * @return 配置有效返回 true。
     */
    static bool ParseConfig(const _tagAgentDagNode &node,
                            const ToolRegistry &registry,
                            _tagToolLoopConfig &config,
                            QString &errorMessage);

    /**
     * @brief 从节点与上下文构造循环请求。
     * @param[in] node 节点定义。
     * @param[in] context 当前 invocation 上下文。
     * @param[in] registry 工具注册表。
     * @param[out] request 循环请求。
     * @param[out] errorMessage 错误描述。
     * @return 构造成功返回 true。
     */
    static bool BuildRequest(const _tagAgentDagNode &node,
                             const AgentContext &context,
                             const ToolRegistry &registry,
                             _tagToolLoopRequest &request,
                             QString &errorMessage);

    /**
     * @brief 将循环结果写入协议键。
     * @param[in] result 循环结果。
     * @param[in,out] context 当前 invocation 上下文。
     * @param[out] errorMessage 错误描述。
     * @return 写入成功返回 true。
     */
    static bool Complete(const _tagToolLoopResult &result,
                         AgentContext &context,
                         QString &errorMessage);
};

} // namespace vpet

#endif // VPET_AGENT_TOOL_LOOP_NODE_H
