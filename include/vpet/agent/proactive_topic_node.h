#ifndef VPET_AGENT_PROACTIVE_TOPIC_NODE_H
#define VPET_AGENT_PROACTIVE_TOPIC_NODE_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"

#include <QString>

namespace vpet
{

/**
 * @brief 主动话题编排节点
 *
 * 根据视觉摘要生成主动发话主题和下游文本 LLM 提示词，不覆盖已有用户提示词。
 */
class ProactiveTopicNode
{
public:
    /**
     * @brief 执行主动话题编排节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 执行成功返回 true
     */
    static bool Execute(const _tagAgentDagNode &node,
                        AgentContext &context,
                        QString &errorMessage);

private:
    /**
     * @brief 根据视觉摘要构建文本 LLM 提示词
     * @param[in] instruction 节点配置中的发话要求
     * @param[in] visionSummary 当前视觉摘要
     * @return 文本 LLM 提示词
     */
    static QString BuildPrompt(const QString &instruction,
                               const QString &visionSummary);

    /**
     * @brief 判断当前摘要是否允许主动发话
     * @param[in] node 节点定义
     * @param[in] context 运行时上下文
     * @param[in] summaryHash 当前摘要指纹
     * @param[out] reason 抑制原因
     * @return 允许发话返回 true
     */
    static bool IsSpeechAllowed(const _tagAgentDagNode &node,
                                const AgentContext &context,
                                const QString &summaryHash,
                                QString &reason);
};

} // namespace vpet

#endif // VPET_AGENT_PROACTIVE_TOPIC_NODE_H
