#ifndef VPET_AGENT_EMOTION_REWRITE_NODE_H
#define VPET_AGENT_EMOTION_REWRITE_NODE_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"
#include "vpet/llm/llm_client.h"

#include <QString>
#include <QVector>

namespace vpet
{

/**
 * @brief 情感化输出节点
 *
 * 根据最近对话上下文调用 LLM 生成情绪分析结果，并在返回后产出最终改写文本。
 */
class EmotionRewriteNode
{
public:
    /**
     * @brief 执行情感化输出节点
     * @param[in] node 节点定义
     * @param[in,out] context 运行时上下文
     * @param[in] llmClient 文本 LLM 客户端
     * @param[out] pendingRequestId 异步请求 ID；未发起异步请求时为 -1
     * @param[out] errorMessage 错误描述
     * @return 请求发送成功或已直接完成时返回 true
     */
    static bool Execute(const _tagAgentDagNode &node,
                        AgentContext &context,
                        LlmClient *llmClient,
                        int &pendingRequestId,
                        QString &errorMessage);

    /**
     * @brief 处理情感节点的 LLM 返回结果
     * @param[in] requestId 请求 ID
     * @param[in] content LLM 返回内容
     * @param[in,out] context 运行时上下文
     * @param[out] errorMessage 错误描述
     * @return 处理成功返回 true
     */
    static bool Complete(int requestId,
                         const QString &content,
                         AgentContext &context,
                         QString &errorMessage);

private:
    /**
     * @brief 判断是否存在可用对话上下文
     * @param[in] context 运行时上下文
     * @return 存在上下文返回 true
     */
    static bool HasConversationContext(const AgentContext &context);

    /**
     * @brief 构建情感分析提示词
     * @param[in] context 运行时上下文
     * @param[in] sourceText 原始输出文本
     * @return 提示词文本
     */
    static QString BuildPrompt(const AgentContext &context, const QString &sourceText);

    /**
     * @brief 解析 LLM 返回中的 JSON 文本
     * @param[in] content LLM 返回内容
     * @param[out] userEmotion 用户情绪
     * @param[out] petEmotion 桌宠情绪
     * @param[out] rewriteText 改写文本
     * @param[out] errorMessage 错误描述
     * @return 解析成功返回 true
     */
    static bool ParseResponse(const QString &content,
                              QString &userEmotion,
                              QString &petEmotion,
                              QString &rewriteText,
                              QString &errorMessage);

    /**
     * @brief 收集最近对话历史文本
     * @param[in] context 运行时上下文
     * @return 对话历史文本
     */
    static QString BuildHistoryText(const AgentContext &context);
};

} // namespace vpet

#endif // VPET_AGENT_EMOTION_REWRITE_NODE_H
