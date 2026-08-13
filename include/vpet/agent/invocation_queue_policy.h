#ifndef VPET_AGENT_INVOCATION_QUEUE_POLICY_H
#define VPET_AGENT_INVOCATION_QUEUE_POLICY_H

#include "vpet/agent/agent_context.h"

#include <QQueue>
#include <QSet>
#include <QString>

namespace vpet
{

/**
 * @brief 管理跨轮 Agent invocation 的排队策略。
 *
 * 用户输入保持 FIFO；视觉触发采用 latest-wins，避免处理过期画面。
 * 等待队列有固定容量，防止异常输入无限占用内存。
 */
class InvocationQueuePolicy
{
public:
    struct _tagEntry
    {
        AgentContext local;
        QSet<QString> removedKeys;
        QString trigger;
    };

    /**
     * @brief 将 invocation 加入队列
     * @param[in] context 待排队上下文
     * @param[in] sessionContext 当前会话基座
     * @return 入队成功返回 true
     */
    bool Enqueue(const AgentContext &context, const AgentContext &sessionContext);

    /**
     * @brief 取出最早待执行 invocation
     * @param[out] entry 待执行 invocation
     * @return 队列非空且取出成功返回 true
     */
    bool Dequeue(_tagEntry &entry);

    /**
     * @brief 判断队列是否为空
     * @return 为空返回 true
     */
    bool IsEmpty() const;

private:
    QQueue<_tagEntry> m_entries;
};

} // namespace vpet

#endif // VPET_AGENT_INVOCATION_QUEUE_POLICY_H
