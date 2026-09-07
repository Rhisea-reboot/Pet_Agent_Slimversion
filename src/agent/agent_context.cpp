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

    if (normalizedKey.isEmpty())
    {
        value.clear();
        return false;
    }

    const auto it = m_values.constFind(normalizedKey);
    if (it == m_values.constEnd())
    {
        value.clear();
        return false;
    }

    value = it.value();
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
    if (this == &overlay)
    {
        return true;
    }

    for (auto it = overlay.m_values.constBegin(); it != overlay.m_values.constEnd(); ++it)
    {
        if (!it.value().isValid())
        {
            return false;
        }

        m_values.insert(it.key(), it.value());
    }

    return true;
}

bool AgentContext::BuildDelta(const AgentContext &base,
                              AgentContext &delta,
                              QSet<QString> &removedKeys) const
{
    // 如果存在别名（delta 与 this 或 base 是同一个对象），保留原先行为与语义（先 Clear 再按旧逻辑通过快照 keys 提取）
    if ((&delta == this) || (&delta == &base))
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

    delta.Clear();
    removedKeys.clear();

    for (auto it = m_values.constBegin(); it != m_values.constEnd(); ++it)
    {
        const QString &key = it.key();
        const QVariant &currentValue = it.value();

        const auto baseIt = base.m_values.constFind(key);
        if ((baseIt == base.m_values.constEnd()) || (currentValue != baseIt.value()))
        {
            if (!delta.SetValue(key, currentValue))
            {
                return false;
            }
        }
    }

    for (auto it = base.m_values.constBegin(); it != base.m_values.constEnd(); ++it)
    {
        const QString &key = it.key();
        if (!m_values.contains(key))
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
