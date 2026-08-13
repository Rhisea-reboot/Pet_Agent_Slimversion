#include "vpet/agent/invocation_queue_policy.h"
#include "vpet/agent/agent_context_keys.h"

namespace vpet
{

namespace
{

constexpr int MAX_PENDING_INVOCATIONS = 32;

} // anonymous namespace

bool InvocationQueuePolicy::Enqueue(const AgentContext &context,
                                    const AgentContext &sessionContext)
{
    _tagEntry entry;
    QVariant triggerValue;

    if (context.GetValue(AgentContextKeys::RUNTIME_TRIGGER_TYPE, triggerValue))
    {
        entry.trigger = triggerValue.toString().trimmed();
    }

    if (!context.BuildDelta(sessionContext, entry.local, entry.removedKeys))
    {
        return false;
    }

    if (entry.trigger == QStringLiteral("vision"))
    {
        for (int index = m_entries.size() - 1; index >= 0; --index)
        {
            if (m_entries.at(index).trigger == QStringLiteral("vision"))
            {
                // Move the freshest frame to the tail so it stays behind already queued input.
                m_entries.removeAt(index);
                m_entries.enqueue(entry);
                return true;
            }
        }
    }

    if (m_entries.size() >= MAX_PENDING_INVOCATIONS)
    {
        return false;
    }

    m_entries.enqueue(entry);
    return true;
}

bool InvocationQueuePolicy::Dequeue(_tagEntry &entry)
{
    if (m_entries.isEmpty())
    {
        return false;
    }

    entry = m_entries.dequeue();
    return true;
}

bool InvocationQueuePolicy::IsEmpty() const
{
    return m_entries.isEmpty();
}

} // namespace vpet
