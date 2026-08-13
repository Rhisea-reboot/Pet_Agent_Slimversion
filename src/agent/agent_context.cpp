#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_context_keys.h"

namespace vpet
{

namespace
{

const QString &USER_INPUT_KEY = AgentContextKeys::USER_INPUT;
const QString &EXECUTED_NODES_KEY = AgentContextKeys::EXECUTED_NODES;

} // anonymous namespace

AgentContext::AgentContext()
    : m_values()
{
}

void AgentContext::Clear()
{
    m_values.clear();
}

bool AgentContext::SetValue(const QString &key, const QVariant &value)
{
    const QString normalizedKey = key.trimmed();

    if (normalizedKey.isEmpty() || !value.isValid())
    {
        return false;
    }

    m_values.insert(normalizedKey, value);

    return true;
}

bool AgentContext::GetValue(const QString &key, QVariant &value) const
{
    const QString normalizedKey = key.trimmed();

    if (normalizedKey.isEmpty() || !m_values.contains(normalizedKey))
    {
        value.clear();
        return false;
    }

    value = m_values.value(normalizedKey);

    return true;
}

bool AgentContext::Contains(const QString &key) const
{
    const QString normalizedKey = key.trimmed();

    if (normalizedKey.isEmpty())
    {
        return false;
    }

    return m_values.contains(normalizedKey);
}

bool AgentContext::RemoveValue(const QString &key)
{
    const QString normalizedKey = key.trimmed();

    if (normalizedKey.isEmpty())
    {
        return false;
    }

    return (m_values.remove(normalizedKey) > 0);
}

QStringList AgentContext::GetKeys() const
{
    QStringList keys = m_values.keys();
    keys.sort();

    return keys;
}

AgentContext AgentContext::Snapshot() const
{
    return *this;
}

bool AgentContext::Overlay(const AgentContext &overlay)
{
    const QStringList keys = overlay.GetKeys();

    for (const QString &key : keys)
    {
        QVariant value;

        if (!overlay.GetValue(key, value) || !SetValue(key, value))
        {
            return false;
        }
    }

    return true;
}

bool AgentContext::BuildDelta(const AgentContext &base,
                              AgentContext &delta,
                              QSet<QString> &removedKeys) const
{
    delta.Clear();
    removedKeys.clear();

    const QStringList currentKeys = GetKeys();

    for (const QString &key : currentKeys)
    {
        QVariant currentValue;
        QVariant baseValue;

        if (!GetValue(key, currentValue))
        {
            return false;
        }

        if (!base.GetValue(key, baseValue) || (currentValue != baseValue))
        {
            if (!delta.SetValue(key, currentValue))
            {
                return false;
            }
        }
    }

    const QStringList baseKeys = base.GetKeys();

    for (const QString &key : baseKeys)
    {
        if (!Contains(key))
        {
            removedKeys.insert(key);
        }
    }

    return true;
}

bool AgentContext::SetUserInput(const QString &userInput)
{
    const QString normalizedUserInput = userInput.trimmed();

    if (normalizedUserInput.isEmpty())
    {
        return false;
    }

    return SetValue(USER_INPUT_KEY, normalizedUserInput);
}

QString AgentContext::GetUserInput() const
{
    QVariant value;

    if (!GetValue(USER_INPUT_KEY, value))
    {
        return QString();
    }

    return value.toString();
}

bool AgentContext::AppendExecutedNode(const QString &nodeName)
{
    const QString normalizedNodeName = nodeName.trimmed();

    if (normalizedNodeName.isEmpty())
    {
        return false;
    }

    QStringList executedNodes = GetExecutedNodes();
    executedNodes.append(normalizedNodeName);

    return SetValue(EXECUTED_NODES_KEY, executedNodes);
}

QStringList AgentContext::GetExecutedNodes() const
{
    QVariant value;

    if (!GetValue(EXECUTED_NODES_KEY, value))
    {
        return QStringList();
    }

    return value.toStringList();
}

} // namespace vpet
