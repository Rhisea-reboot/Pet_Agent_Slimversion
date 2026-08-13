#include "vpet/agent/agent_node_registry.h"

#include "vpet/agent/agent_context_keys.h"

#include <QVariant>

namespace vpet
{

bool AgentNodeRegistry::Register(const QString &nodeType, const NodeHandler &handler)
{
    const QString normalizedNodeType = nodeType.trimmed();

    if (normalizedNodeType.isEmpty() || !handler)
    {
        return false;
    }

    m_handlers.insert(normalizedNodeType, handler);
    return true;
}

bool AgentNodeRegistry::Execute(const _tagAgentDagNode &node,
                                AgentContext &context,
                                QString &errorMessage) const
{
    const QString nodeId = node.id.trimmed();
    const QString nodeType = node.type.trimmed();

    if (nodeId.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime node id is empty.");
        return false;
    }

    if (nodeType.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime node type is empty: %1").arg(nodeId);
        return false;
    }

    if (!context.AppendExecutedNode(nodeId))
    {
        errorMessage = QStringLiteral("Agent runtime failed to record executed node.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::RUNTIME_LAST_NODE_TYPE, nodeType))
    {
        errorMessage = QStringLiteral("Agent runtime failed to record node type: %1").arg(nodeId);
        return false;
    }

    if (!m_handlers.contains(nodeType))
    {
        errorMessage = QStringLiteral("Agent node handler is not registered: %1").arg(nodeType);
        return false;
    }

    const NodeHandler handler = m_handlers.value(nodeType);

    if (!handler)
    {
        errorMessage = QStringLiteral("Agent node handler is invalid: %1").arg(nodeType);
        return false;
    }

    if (!PrepareInputAliases(node, context))
    {
        errorMessage = QStringLiteral("Agent runtime failed to prepare node input aliases: %1").arg(nodeId);
        return false;
    }

    if (!handler(node, context, errorMessage))
    {
        return false;
    }

    if (!SyncOutputAliases(node, context))
    {
        errorMessage = QStringLiteral("Agent runtime failed to sync node output aliases: %1").arg(nodeId);
        return false;
    }

    return true;
}

bool AgentNodeRegistry::PrepareInputAliases(const _tagAgentDagNode &node,
                                            AgentContext &context) const
{
    const QString nodeType = node.type.trimmed();

    if (nodeType.isEmpty())
    {
        return false;
    }

    QVariant value;

    if (nodeType == AgentContextKeys::NODE_TYPE_LLM_CHAT)
    {
        if (context.GetValue(AgentContextKeys::NODE_INPUT_PROMPT, value))
        {
            return context.SetValue(AgentContextKeys::PROMPT_TEXT, value);
        }

        if (context.GetValue(AgentContextKeys::SEMANTIC_TEXT_PROMPT, value))
        {
            return context.SetValue(AgentContextKeys::NODE_INPUT_PROMPT, value)
                   && context.SetValue(AgentContextKeys::PROMPT_TEXT, value);
        }

        if (context.GetValue(AgentContextKeys::PROMPT_TEXT, value))
        {
            return context.SetValue(AgentContextKeys::NODE_INPUT_PROMPT, value)
                   && context.SetValue(AgentContextKeys::SEMANTIC_TEXT_PROMPT, value);
        }

        return true;
    }

    if (nodeType == AgentContextKeys::NODE_TYPE_EMOTION_REWRITE)
    {
        if (context.GetValue(AgentContextKeys::NODE_INPUT_TEXT_RESPONSE, value))
        {
            return context.SetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, value)
                   && context.SetValue(AgentContextKeys::LLM_LAST_RESPONSE, value);
        }

        if (context.GetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, value))
        {
            return context.SetValue(AgentContextKeys::NODE_INPUT_TEXT_RESPONSE, value)
                   && context.SetValue(AgentContextKeys::LLM_LAST_RESPONSE, value);
        }

        if (context.GetValue(AgentContextKeys::LLM_LAST_RESPONSE, value))
        {
            return context.SetValue(AgentContextKeys::NODE_INPUT_TEXT_RESPONSE, value)
                   && context.SetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, value);
        }

        return true;
    }

    if (nodeType == AgentContextKeys::NODE_TYPE_OUTPUT_FORMAT)
    {
        if (context.GetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, value)
            || context.GetValue(AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE, value)
            || context.GetValue(AgentContextKeys::EMOTION_OUTPUT_TEXT, value)
            || context.GetValue(AgentContextKeys::LLM_LAST_RESPONSE, value)
            || context.GetValue(AgentContextKeys::NODE_INPUT_TEXT_RESPONSE, value))
        {
            return context.SetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, value)
                   && context.SetValue(AgentContextKeys::NODE_INPUT_TEXT_RESPONSE, value);
        }
    }

    return true;
}

bool AgentNodeRegistry::SyncOutputAliases(const _tagAgentDagNode &node,
                                          AgentContext &context) const
{
    const QString nodeType = node.type.trimmed();

    if (nodeType.isEmpty())
    {
        return false;
    }

    QVariant value;

    if (nodeType == AgentContextKeys::NODE_TYPE_LLM_CHAT
        && context.GetValue(AgentContextKeys::LLM_LAST_RESPONSE, value))
    {
        return context.SetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, value)
               && context.SetValue(AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE, value);
    }

    if (nodeType == AgentContextKeys::NODE_TYPE_EMOTION_REWRITE
        && context.GetValue(AgentContextKeys::EMOTION_OUTPUT_TEXT, value))
    {
        return context.SetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, value)
               && context.SetValue(AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE, value);
    }

    if (nodeType == AgentContextKeys::NODE_TYPE_OUTPUT_FORMAT
        && context.GetValue(AgentContextKeys::OUTPUT_TEXT, value))
    {
        return context.SetValue(AgentContextKeys::SEMANTIC_TEXT_FINAL, value)
               && context.SetValue(AgentContextKeys::NODE_OUTPUT_TEXT_FINAL, value);
    }

    return true;
}

} // namespace vpet
