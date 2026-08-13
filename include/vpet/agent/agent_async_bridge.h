#ifndef VPET_AGENT_AGENT_ASYNC_BRIDGE_H
#define VPET_AGENT_AGENT_ASYNC_BRIDGE_H

#include "vpet/agent/agent_context.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace vpet
{

/**
 * @brief Owns asynchronous request correlation and context protocol operations.
 */
class AgentAsyncBridge
{
public:
    struct _tagPendingRequest
    {
        int requestId = -1;
        QString clientType;
        quint64 invocationId = 0;
        QString nodeId;
        QString branchId;
        QString nodeType;
        AgentContext context;
    };

    /** @brief Returns whether any continuation is pending. @return true when pending. */
    bool HasPending() const;
    /** @brief Returns whether any continuation or direct request is pending. @return true when pending. */
    bool HasRequests() const;
    /** @brief Clears all pending continuations. */
    void ClearPending();
    /** @brief Takes and clears every pending continuation. @return Pending request snapshots. */
    QVector<_tagPendingRequest> TakeAllPending();
    /** @brief Builds a client-scoped request key. @param[in] clientType Client type. @param[in] requestId Request ID. @return Correlation key. */
    QString BuildRequestKey(const QString &clientType, int requestId) const;
    /** @brief Adds a continuation. @param[in] key Correlation key. @param[in] request Request data. @return true when added. */
    bool AddPending(const QString &key, const _tagPendingRequest &request);
    /** @brief Tests for a continuation. @param[in] key Correlation key. @return true when found. */
    bool ContainsPending(const QString &key) const;
    /** @brief Reads a continuation without removing it. @param[in] key Correlation key. @param[out] request Request data. @return true when found. */
    bool GetPending(const QString &key, _tagPendingRequest &request) const;
    /** @brief Removes a continuation. @param[in] key Correlation key. @param[out] request Request data. @return true when found. */
    bool TakePending(const QString &key, _tagPendingRequest &request);
    /** @brief Adds a direct request ID. @param[in] requestId Request ID. @return true when valid. */
    bool AddDirectRequest(int requestId);
    /** @brief Removes a direct request ID. @param[in] requestId Request ID. @return true when found. */
    bool TakeDirectRequest(int requestId);
    /** @brief Clears runtime pending keys. @param[in,out] context Context to update. */
    void ClearContextProtocol(AgentContext &context) const;
    /** @brief Writes runtime pending keys. @param[in] nodeId Node ID. @param[in] nodeType Node type. @param[in] requestId Request ID. @param[in,out] context Context to update. @param[out] errorMessage Failure description. @return true on success. */
    bool SetContextProtocol(const QString &nodeId,
                            const QString &nodeType,
                            int requestId,
                            AgentContext &context,
                            QString &errorMessage) const;

private:
    QHash<QString, _tagPendingRequest> m_pendingRequests;
    QSet<int> m_directRequestIds;
};

} // namespace vpet

#endif // VPET_AGENT_AGENT_ASYNC_BRIDGE_H
