#ifndef VPET_AGENT_AGENT_NODE_REGISTRY_H
#define VPET_AGENT_AGENT_NODE_REGISTRY_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"

#include <QHash>
#include <QString>

#include <functional>

namespace vpet
{

/**
 * @brief Owns Agent node handlers and semantic alias dispatch.
 */
class AgentNodeRegistry
{
public:
    using NodeHandler = std::function<bool(const _tagAgentDagNode &, AgentContext &, QString &)>;

    /**
     * @brief Registers a node handler.
     * @param[in] nodeType Node type key.
     * @param[in] handler Handler implementation.
     * @return true when the handler is valid and registered.
     */
    bool Register(const QString &nodeType, const NodeHandler &handler);

    /**
     * @brief Executes a node through its registered handler and alias protocol.
     * @param[in] node Node definition.
     * @param[in,out] context Invocation context.
     * @param[out] errorMessage Failure description.
     * @return true when dispatch succeeds.
     */
    bool Execute(const _tagAgentDagNode &node,
                 AgentContext &context,
                 QString &errorMessage) const;

private:
    /**
     * @brief Prepares semantic aliases consumed by a node.
     * @param[in] node Node definition.
     * @param[in,out] context Invocation context.
     * @return true when aliases are synchronized.
     */
    bool PrepareInputAliases(const _tagAgentDagNode &node, AgentContext &context) const;

    /**
     * @brief Synchronizes semantic aliases produced by a node.
     * @param[in] node Node definition.
     * @param[in,out] context Invocation context.
     * @return true when aliases are synchronized.
     */
    bool SyncOutputAliases(const _tagAgentDagNode &node, AgentContext &context) const;

private:
    QHash<QString, NodeHandler> m_handlers; ///< Node type to handler mapping.
};

} // namespace vpet

#endif // VPET_AGENT_AGENT_NODE_REGISTRY_H
