#include "vpet/agent/agent_async_bridge.h"

#include "vpet/agent/agent_context_keys.h"

namespace vpet
{

bool AgentAsyncBridge::HasPending() const
{
    return !m_pendingRequests.isEmpty();
}

bool AgentAsyncBridge::HasRequests() const
{
    return !m_pendingRequests.isEmpty() || !m_directRequestIds.isEmpty();
}

void AgentAsyncBridge::ClearPending()
{
    m_pendingRequests.clear();
}

QVector<AgentAsyncBridge::_tagPendingRequest> AgentAsyncBridge::TakeAllPending()
{
    QVector<_tagPendingRequest> pendingRequests;
    pendingRequests.reserve(m_pendingRequests.size());

    for (auto iterator = m_pendingRequests.cbegin(); iterator != m_pendingRequests.cend(); ++iterator)
    {
        pendingRequests.append(iterator.value());
    }

    m_pendingRequests.clear();
    return pendingRequests;
}

QString AgentAsyncBridge::BuildRequestKey(const QString &clientType, int requestId) const
{
    const QString normalizedClientType = clientType.trimmed().toLower();

    if (normalizedClientType.isEmpty() || (requestId <= 0))
    {
        return QString();
    }

    return QStringLiteral("%1:%2").arg(normalizedClientType).arg(requestId);
}

bool AgentAsyncBridge::AddPending(const QString &key, const _tagPendingRequest &request)
{
    const QString normalizedKey = key.trimmed();

    if (normalizedKey.isEmpty() || (request.requestId <= 0)
        || m_pendingRequests.contains(normalizedKey))
    {
        return false;
    }

    m_pendingRequests.insert(normalizedKey, request);
    return true;
}

bool AgentAsyncBridge::ContainsPending(const QString &key) const
{
    return !key.trimmed().isEmpty() && m_pendingRequests.contains(key);
}

bool AgentAsyncBridge::GetPending(const QString &key, _tagPendingRequest &request) const
{
    if (!ContainsPending(key))
    {
        return false;
    }

    request = m_pendingRequests.value(key);
    return true;
}

bool AgentAsyncBridge::TakePending(const QString &key, _tagPendingRequest &request)
{
    if (!ContainsPending(key))
    {
        return false;
    }

    request = m_pendingRequests.take(key);
    return true;
}

bool AgentAsyncBridge::AddDirectRequest(int requestId)
{
    if (requestId <= 0)
    {
        return false;
    }

    m_directRequestIds.insert(requestId);
    return true;
}

bool AgentAsyncBridge::TakeDirectRequest(int requestId)
{
    return (requestId > 0) && m_directRequestIds.remove(requestId);
}

void AgentAsyncBridge::ClearContextProtocol(AgentContext &context) const
{
    context.RemoveValue(AgentContextKeys::RUNTIME_PENDING);
    context.RemoveValue(AgentContextKeys::RUNTIME_PENDING_NODE_ID);
    context.RemoveValue(AgentContextKeys::RUNTIME_PENDING_NODE_TYPE);
    context.RemoveValue(AgentContextKeys::RUNTIME_PENDING_REQUEST_ID);
}

bool AgentAsyncBridge::SetContextProtocol(const QString &nodeId,
                                          const QString &nodeType,
                                          int requestId,
                                          AgentContext &context,
                                          QString &errorMessage) const
{
    if (nodeId.trimmed().isEmpty() || nodeType.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent async pending node is invalid.");
        return false;
    }

    if (requestId <= 0)
    {
        errorMessage = QStringLiteral("Agent async pending request ID is invalid.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::RUNTIME_PENDING, true))
    {
        errorMessage = QStringLiteral("Agent failed to record async pending state.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::RUNTIME_PENDING_NODE_ID, nodeId.trimmed()))
    {
        errorMessage = QStringLiteral("Agent failed to record async pending node id.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::RUNTIME_PENDING_NODE_TYPE, nodeType.trimmed()))
    {
        errorMessage = QStringLiteral("Agent failed to record async pending node type.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, requestId))
    {
        errorMessage = QStringLiteral("Agent failed to record async pending request ID.");
        return false;
    }

    return true;
}

} // namespace vpet
