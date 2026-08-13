#ifndef VPET_AGENT_WEB_RESEARCH_NODE_H
#define VPET_AGENT_WEB_RESEARCH_NODE_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"
#include "vpet/web/web_research_engine.h"

#include <QString>

namespace vpet
{

/**
 * @brief web.research 节点的协议适配器。
 *
 * 负责从节点配置构造研究请求，并把 invocation-local 研究结果序列化到
 * semantic.web.research.*，不负责异步请求关联或图调度。
 */
class WebResearchNode
{
public:
    /**
     * @brief 从节点和上下文构造研究请求。
     * @param[in] node 节点定义。
     * @param[in] context 当前 invocation 上下文。
     * @param[out] request 归一化前的研究请求。
     * @param[out] errorMessage 错误描述。
     * @return 构造成功返回 true。
     */
    static bool BuildRequest(const _tagAgentDagNode &node,
                             const AgentContext &context,
                             _tagWebResearchRequest &request,
                             QString &errorMessage);

    /**
     * @brief 将研究完成结果写入协议键并组装下游 LLM 提示词。
     * @param[in] response 结构化研究结果。
     * @param[in,out] context 当前 invocation 上下文。
     * @param[out] errorMessage 错误描述。
     * @return 写入成功返回 true。
     */
    static bool Complete(const _tagWebResearchResponse &response,
                         AgentContext &context,
                         QString &errorMessage);

    /**
     * @brief 将研究失败转换为 continue 策略的降级上下文。
     * @param[in] message 脱敏失败描述。
     * @param[in,out] context 当前 invocation 上下文。
     * @param[out] errorMessage 错误描述。
     * @return 写入成功返回 true。
     */
    static bool CompleteFailure(const QString &message,
                                AgentContext &context,
                                QString &errorMessage);

    /**
     * @brief 读取节点失败策略。
     * @param[in] node 节点定义。
     * @return continue 或 fail。
     */
    static QString ReadFailurePolicy(const _tagAgentDagNode &node);
};

} // namespace vpet

#endif // VPET_AGENT_WEB_RESEARCH_NODE_H
