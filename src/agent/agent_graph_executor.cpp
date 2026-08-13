#include "vpet/agent/agent_graph_executor.h"

#include "vpet/agent/agent_context_keys.h"

#include <QQueue>
#include <QStringList>
#include <QVariant>

namespace vpet
{

AgentGraphExecutor::AgentGraphExecutor()
    : m_dagGraph()
    , m_executionOrder()
    , m_invocationState()
    , m_nextInvocationId(0)
    , m_lastCompletedInvocationId(0)
{
}

bool AgentGraphExecutor::Load(const QString &configPath, QString &errorMessage)
{
    if (configPath.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime config path is empty.");
        return false;
    }

    if (!m_dagGraph.LoadFromJsonFile(configPath, errorMessage))
    {
        m_executionOrder.clear();
        return false;
    }

    if (!m_dagGraph.TopologicalSort(m_executionOrder, errorMessage))
    {
        m_executionOrder.clear();
        return false;
    }

    return true;
}

QVector<QString> AgentGraphExecutor::GetExecutionOrder() const
{
    return m_executionOrder;
}

bool AgentGraphExecutor::IsActive() const
{
    return m_invocationState.isActive;
}

bool AgentGraphExecutor::IsActiveInvocation(quint64 invocationId) const
{
    return (invocationId != 0)
           && m_invocationState.isActive
           && (m_invocationState.invocationId == invocationId);
}

quint64 AgentGraphExecutor::GetLastCompletedInvocationId() const
{
    return m_lastCompletedInvocationId;
}

bool AgentGraphExecutor::BeginInvocation(const AgentContext &context,
                                         bool hasPendingRequest,
                                         QString &errorMessage)
{
    if (m_executionOrder.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime is not loaded.");
        return false;
    }

    if (m_invocationState.isActive || hasPendingRequest)
    {
        errorMessage = QStringLiteral("Agent runtime invocation is already active.");
        return false;
    }

    const QVector<QString> allSourceNodes = m_dagGraph.GetSourceNodes();

    if (allSourceNodes.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime DAG has no source node.");
        return false;
    }

    ClearInvocationState();
    m_invocationState.remainingInDegree = m_dagGraph.GetInDegreeMap();
    const QVector<QString> nodeNames = m_dagGraph.GetNodeNames();

    if (m_invocationState.remainingInDegree.size() != nodeNames.size())
    {
        errorMessage = QStringLiteral("Agent runtime DAG invocation state is incomplete.");
        return false;
    }

    for (int nodeIndex = 0; nodeIndex < nodeNames.size(); ++nodeIndex)
    {
        m_invocationState.nodeDeclarationOrder.insert(nodeNames.at(nodeIndex), nodeIndex);
    }

    QVariant triggerValue;

    if (context.GetValue(AgentContextKeys::RUNTIME_TRIGGER_TYPE, triggerValue))
    {
        m_invocationState.trigger = triggerValue.toString().trimmed();
    }

    ++m_nextInvocationId;

    if (m_nextInvocationId == 0)
    {
        ++m_nextInvocationId;
    }

    m_invocationState.invocationId = m_nextInvocationId;
    QVector<QString> sourceNodes;

    if (!SelectSourceNodes(allSourceNodes, sourceNodes, errorMessage)
        || !BuildActiveSubgraph(sourceNodes, nodeNames, errorMessage)
        || !InitializeSourceBranches(sourceNodes, errorMessage))
    {
        ClearInvocationState();
        return false;
    }

    m_invocationState.isActive = true;
    return true;
}

bool AgentGraphExecutor::SelectSourceNodes(const QVector<QString> &allSourceNodes,
                                           QVector<QString> &sourceNodes,
                                           QString &errorMessage)
{
    if (allSourceNodes.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime DAG has no source node.");
        return false;
    }

    QVector<QString> matchingSourceNodes;
    bool hasDeclaredSourceTrigger = false;

    for (const QString &sourceNode : allSourceNodes)
    {
        _tagAgentDagNode sourceDefinition;

        if (!m_dagGraph.GetNode(sourceNode, sourceDefinition))
        {
            errorMessage = QStringLiteral("Agent runtime source node definition is missing: %1")
                               .arg(sourceNode);
            return false;
        }

        const QString sourceTrigger = sourceDefinition.config.value(QStringLiteral("trigger"))
                                          .toString()
                                          .trimmed();
        hasDeclaredSourceTrigger = hasDeclaredSourceTrigger || !sourceTrigger.isEmpty();

        if (!m_invocationState.trigger.isEmpty() && (sourceTrigger == m_invocationState.trigger))
        {
            matchingSourceNodes.append(sourceNode);
        }
    }

    if (hasDeclaredSourceTrigger && matchingSourceNodes.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime DAG has no source for trigger: %1").arg(
                           m_invocationState.trigger);
        return false;
    }

    sourceNodes = hasDeclaredSourceTrigger ? matchingSourceNodes : allSourceNodes;
    return true;
}

bool AgentGraphExecutor::BuildActiveSubgraph(const QVector<QString> &sourceNodes,
                                             const QVector<QString> &nodeNames,
                                             QString &errorMessage)
{
    if (sourceNodes.isEmpty() || nodeNames.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime DAG invocation state is incomplete.");
        return false;
    }

    QQueue<QString> reachableQueue;

    for (const QString &sourceNode : sourceNodes)
    {
        reachableQueue.enqueue(sourceNode);
    }

    while (!reachableQueue.isEmpty())
    {
        const QString nodeId = reachableQueue.dequeue();

        if (m_invocationState.activeNodeIds.contains(nodeId))
        {
            continue;
        }

        m_invocationState.activeNodeIds.insert(nodeId);
        QVector<QString> successors;

        if (!m_dagGraph.GetSuccessors(nodeId, successors))
        {
            errorMessage = QStringLiteral("Agent runtime reachable node lookup failed: %1").arg(nodeId);
            return false;
        }

        for (const QString &successor : successors)
        {
            reachableQueue.enqueue(successor);
        }
    }

    for (const QString &nodeId : nodeNames)
    {
        if (!m_invocationState.activeNodeIds.contains(nodeId))
        {
            m_invocationState.remainingInDegree.remove(nodeId);
            continue;
        }

        QVector<QString> predecessors;

        if (!m_dagGraph.GetPredecessors(nodeId, predecessors))
        {
            errorMessage = QStringLiteral("Agent runtime active node predecessors are missing: %1")
                               .arg(nodeId);
            return false;
        }

        int activeInDegree = 0;

        for (const QString &predecessor : predecessors)
        {
            if (m_invocationState.activeNodeIds.contains(predecessor))
            {
                ++activeInDegree;
            }
        }

        m_invocationState.remainingInDegree.insert(nodeId, activeInDegree);
    }

    return true;
}

bool AgentGraphExecutor::InitializeSourceBranches(const QVector<QString> &sourceNodes,
                                                  QString &errorMessage)
{
    if (sourceNodes.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime DAG has no source node.");
        return false;
    }

    for (const QString &sourceNode : sourceNodes)
    {
        _tagBranchState branch;
        _tagAgentDagNode sourceDefinition;

        if (!m_dagGraph.GetNode(sourceNode, sourceDefinition))
        {
            errorMessage = QStringLiteral("Agent runtime source node definition is missing: %1").arg(
                               sourceNode);
            return false;
        }

        branch.branchId = sourceNode;
        branch.sourceNodeId = sourceNode;
        branch.sourceTrigger = sourceDefinition.config.value(QStringLiteral("trigger"))
                                   .toString()
                                   .trimmed();
        m_invocationState.branches.insert(branch.branchId, branch);
        m_invocationState.nodeBranchIds.insert(sourceNode, branch.branchId);

        if (!EnqueueReadyNode(sourceNode, errorMessage))
        {
            return false;
        }
    }

    return true;
}

bool AgentGraphExecutor::ValidateCallbacks(const _tagCallbacks &callbacks,
                                           QString &errorMessage) const
{
    if (!callbacks.prepareInput || !callbacks.executeNode || !callbacks.registerPending
        || !callbacks.hasPending || !callbacks.clearInput || !callbacks.resetAfterFailure
        || !callbacks.invocationCompleted)
    {
        errorMessage = QStringLiteral("Agent runtime graph callbacks are invalid.");
        return false;
    }

    return true;
}

bool AgentGraphExecutor::PumpReadyQueue(bool shouldPrepareInput,
                                        AgentContext &context,
                                        AgentContext &sessionContext,
                                        const _tagCallbacks &callbacks,
                                        QString &errorMessage)
{
    if (!ValidateCallbacks(callbacks, errorMessage))
    {
        return false;
    }

    if (!m_invocationState.isActive)
    {
        errorMessage = QStringLiteral("Agent runtime invocation is not active.");
        return false;
    }

    if (shouldPrepareInput && !callbacks.prepareInput(context, errorMessage))
    {
        ClearInvocationState();
        callbacks.clearInput(context);
        return false;
    }

    if (shouldPrepareInput
        && !InitializeSourceContexts(context, sessionContext, errorMessage))
    {
        callbacks.resetAfterFailure(context);
        return false;
    }

    while (!m_invocationState.readyQueue.isEmpty())
    {
        const QString nodeId = m_invocationState.readyQueue.takeFirst();

        if (!ExecuteReadyNode(nodeId, context, sessionContext, callbacks, errorMessage))
        {
            return false;
        }
    }

    if (callbacks.hasPending())
    {
        return true;
    }

    if (m_invocationState.completedNodeIds.size() != m_invocationState.remainingInDegree.size())
    {
        errorMessage = QStringLiteral("Agent runtime invocation stopped before all nodes completed.");
        FailInvocation(QString(), false, context, callbacks, errorMessage);
        return false;
    }

    if (!CommitInvocationResult(context, sessionContext, errorMessage))
    {
        FailInvocation(QString(), false, context, callbacks, errorMessage);
        return false;
    }

    m_lastCompletedInvocationId = m_invocationState.invocationId;
    ClearInvocationState();
    callbacks.invocationCompleted(context);
    callbacks.clearInput(context);
    return true;
}

bool AgentGraphExecutor::InitializeSourceContexts(const AgentContext &context,
                                                  const AgentContext &sessionContext,
                                                  QString &errorMessage)
{
    for (const QString &nodeId : m_dagGraph.GetNodeNames())
    {
        if (!m_invocationState.activeNodeIds.contains(nodeId)
            || (m_invocationState.remainingInDegree.value(nodeId) != 0))
        {
            continue;
        }

        const QString branchId = m_invocationState.nodeBranchIds.value(nodeId);

        if (!m_invocationState.branches.contains(branchId))
        {
            errorMessage = QStringLiteral("Agent runtime source branch is missing: %1").arg(nodeId);
            return false;
        }

        _tagBranchState &branch = m_invocationState.branches[branchId];

        if (!context.BuildDelta(sessionContext, branch.local, branch.removedKeys))
        {
            errorMessage = QStringLiteral("Agent runtime failed to initialize source branch context: %1")
                               .arg(nodeId);
            return false;
        }
    }

    return true;
}

bool AgentGraphExecutor::ExecuteReadyNode(const QString &nodeId,
                                          AgentContext &context,
                                          const AgentContext &sessionContext,
                                          const _tagCallbacks &callbacks,
                                          QString &errorMessage)
{
    if (nodeId.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime dequeued node before its dependencies completed: %1")
                           .arg(nodeId);
        FailInvocation(nodeId, false, context, callbacks, errorMessage);
        return false;
    }

    if (!m_invocationState.remainingInDegree.contains(nodeId)
        || (m_invocationState.remainingInDegree.value(nodeId) != 0))
    {
        errorMessage = QStringLiteral("Agent runtime dequeued node before its dependencies completed: %1")
                           .arg(nodeId);
        FailInvocation(nodeId, false, context, callbacks, errorMessage);
        return false;
    }

    if (m_invocationState.completedNodeIds.contains(nodeId))
    {
        errorMessage = QStringLiteral("Agent runtime dequeued an already completed node: %1").arg(nodeId);
        FailInvocation(nodeId, false, context, callbacks, errorMessage);
        return false;
    }

    _tagAgentDagNode node;

    if (!m_dagGraph.GetNode(nodeId, node))
    {
        errorMessage = QStringLiteral("Agent runtime node definition is missing: %1").arg(nodeId);
        FailInvocation(nodeId, false, context, callbacks, errorMessage);
        return false;
    }

    AgentContext executionContext;

    if (!BuildExecutionView(nodeId, sessionContext, executionContext, errorMessage)
        || !callbacks.executeNode(node, executionContext, errorMessage))
    {
        FailInvocation(nodeId, true, context, callbacks, errorMessage);
        return false;
    }

    if (!SaveNodeResult(nodeId, sessionContext, executionContext, errorMessage))
    {
        FailInvocation(nodeId, true, context, callbacks, errorMessage);
        return false;
    }

    context = executionContext.Snapshot();
    QVariant pendingValue;

    if (executionContext.GetValue(AgentContextKeys::RUNTIME_PENDING, pendingValue)
        && pendingValue.toBool())
    {
        const QString branchId = m_invocationState.nodeBranchIds.value(nodeId);

        if (!callbacks.registerPending(node,
                                       executionContext,
                                       m_invocationState.invocationId,
                                       branchId,
                                       errorMessage))
        {
            FailInvocation(nodeId, false, context, callbacks, errorMessage);
            return false;
        }

        return true;
    }

    if (!CompleteNode(nodeId, errorMessage))
    {
        FailInvocation(nodeId, false, context, callbacks, errorMessage);
        return false;
    }

    return true;
}

void AgentGraphExecutor::FailInvocation(const QString &nodeId,
                                        bool markNodeFailure,
                                        AgentContext &context,
                                        const _tagCallbacks &callbacks,
                                        const QString &errorMessage)
{
    m_invocationState.hasFailed = true;
    m_invocationState.failureMessage = errorMessage;

    if (markNodeFailure && !nodeId.trimmed().isEmpty())
    {
        m_invocationState.nodeExecutionResults.insert(nodeId, false);
    }

    callbacks.resetAfterFailure(context);
}

bool AgentGraphExecutor::ResumePendingNode(const QString &nodeId,
                                           quint64 invocationId,
                                           const AgentContext &resumedContext,
                                           AgentContext &context,
                                           AgentContext &sessionContext,
                                           const _tagCallbacks &callbacks,
                                           QString &errorMessage)
{
    if (nodeId.trimmed().isEmpty() || !IsActiveInvocation(invocationId))
    {
        errorMessage = QStringLiteral("Agent runtime pending request belongs to an old invocation.");
        return false;
    }

    context = resumedContext.Snapshot();

    if (!SaveNodeResult(nodeId, sessionContext, resumedContext, errorMessage)
        || !CompleteNode(nodeId, errorMessage))
    {
        return false;
    }

    return PumpReadyQueue(false, context, sessionContext, callbacks, errorMessage);
}

bool AgentGraphExecutor::CompleteNode(const QString &nodeId, QString &errorMessage)
{
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime completed node id is empty.");
        return false;
    }

    if (!m_invocationState.isActive)
    {
        errorMessage = QStringLiteral("Agent runtime cannot complete a node without an active invocation.");
        return false;
    }

    if (!m_invocationState.remainingInDegree.contains(normalizedNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime completed node is missing from invocation: %1").arg(
                           normalizedNodeId);
        return false;
    }

    if (m_invocationState.completedNodeIds.contains(normalizedNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime node was completed more than once: %1").arg(
                           normalizedNodeId);
        return false;
    }

    QVector<QString> successors;

    if (!m_dagGraph.GetSuccessors(normalizedNodeId, successors))
    {
        errorMessage = QStringLiteral("Agent runtime completed node is not in DAG: %1").arg(
                           normalizedNodeId);
        return false;
    }

    m_invocationState.completedNodeIds.insert(normalizedNodeId);
    m_invocationState.nodeExecutionResults.insert(normalizedNodeId, true);

    for (const QString &successorId : successors)
    {
        if (!m_invocationState.remainingInDegree.contains(successorId))
        {
            errorMessage = QStringLiteral("Agent runtime successor is missing from invocation: %1").arg(
                               successorId);
            return false;
        }

        const int remainingInDegree = m_invocationState.remainingInDegree.value(successorId);

        if (remainingInDegree <= 0)
        {
            errorMessage = QStringLiteral("Agent runtime successor was completed more than once: %1").arg(
                               successorId);
            return false;
        }

        const int nextRemainingInDegree = remainingInDegree - 1;
        m_invocationState.remainingInDegree.insert(successorId, nextRemainingInDegree);

        if (nextRemainingInDegree != 0)
        {
            continue;
        }

        QVector<QString> predecessors;

        if (!m_dagGraph.GetPredecessors(successorId, predecessors))
        {
            errorMessage = QStringLiteral("Agent runtime successor predecessors are missing: %1").arg(
                               successorId);
            return false;
        }

        int activePredecessorCount = 0;

        for (const QString &predecessor : predecessors)
        {
            if (m_invocationState.activeNodeIds.contains(predecessor))
            {
                ++activePredecessorCount;
            }
        }

        if (activePredecessorCount > 1)
        {
            if (!CreateJoinBranch(successorId, errorMessage))
            {
                return false;
            }
        }
        else if (!CreateChildBranch(normalizedNodeId, successorId, errorMessage))
        {
            return false;
        }

        if (!EnqueueReadyNode(successorId, errorMessage))
        {
            return false;
        }
    }

    return true;
}

bool AgentGraphExecutor::EnqueueReadyNode(const QString &nodeId, QString &errorMessage)
{
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime ready node id is empty.");
        return false;
    }

    if (!m_invocationState.nodeDeclarationOrder.contains(normalizedNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime ready node is missing from declaration order: %1").arg(
                           normalizedNodeId);
        return false;
    }

    if (m_invocationState.readyQueue.contains(normalizedNodeId)
        || m_invocationState.completedNodeIds.contains(normalizedNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime ready node was queued more than once: %1").arg(
                           normalizedNodeId);
        return false;
    }

    const int declarationOrder = m_invocationState.nodeDeclarationOrder.value(normalizedNodeId);
    int insertionIndex = 0;

    while ((insertionIndex < m_invocationState.readyQueue.size())
           && (m_invocationState.nodeDeclarationOrder.value(
                   m_invocationState.readyQueue.at(insertionIndex)) < declarationOrder))
    {
        ++insertionIndex;
    }

    m_invocationState.readyQueue.insert(insertionIndex, normalizedNodeId);
    return true;
}

bool AgentGraphExecutor::BuildExecutionView(const QString &nodeId,
                                            const AgentContext &sessionContext,
                                            AgentContext &context,
                                            QString &errorMessage) const
{
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime execution view node id is empty.");
        return false;
    }

    const QString branchId = m_invocationState.nodeBranchIds.value(normalizedNodeId);

    if (branchId.isEmpty() || !m_invocationState.branches.contains(branchId))
    {
        errorMessage = QStringLiteral("Agent runtime execution branch is missing: %1").arg(normalizedNodeId);
        return false;
    }

    const _tagBranchState &branch = m_invocationState.branches.value(branchId);
    context = sessionContext.Snapshot();

    if (!context.Overlay(branch.local))
    {
        errorMessage = QStringLiteral("Agent runtime failed to overlay branch local context: %1").arg(
                           normalizedNodeId);
        return false;
    }

    for (const QString &removedKey : branch.removedKeys)
    {
        context.RemoveValue(removedKey);
    }

    return true;
}

bool AgentGraphExecutor::SaveNodeResult(const QString &nodeId,
                                        const AgentContext &sessionContext,
                                        const AgentContext &afterContext,
                                        QString &errorMessage)
{
    const QString normalizedNodeId = nodeId.trimmed();

    if (normalizedNodeId.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime node result id is empty.");
        return false;
    }

    const QString branchId = m_invocationState.nodeBranchIds.value(normalizedNodeId);

    if (branchId.isEmpty() || !m_invocationState.branches.contains(branchId))
    {
        errorMessage = QStringLiteral("Agent runtime node result branch is missing: %1").arg(
                           normalizedNodeId);
        return false;
    }

    _tagBranchState &branch = m_invocationState.branches[branchId];

    if (!afterContext.BuildDelta(sessionContext, branch.local, branch.removedKeys))
    {
        errorMessage = QStringLiteral("Agent runtime failed to save branch context delta: %1").arg(
                           normalizedNodeId);
        return false;
    }

    _tagNodeResult result;
    result.branchId = branchId;
    result.sourceNodeId = branch.sourceNodeId;
    result.sourceTrigger = branch.sourceTrigger;
    result.local = branch.local.Snapshot();
    result.removedKeys = branch.removedKeys;
    m_invocationState.nodeResults.insert(normalizedNodeId, result);
    return true;
}

bool AgentGraphExecutor::CreateChildBranch(const QString &parentNodeId,
                                           const QString &childNodeId,
                                           QString &errorMessage)
{
    const QString normalizedParentNodeId = parentNodeId.trimmed();
    const QString normalizedChildNodeId = childNodeId.trimmed();

    if (normalizedParentNodeId.isEmpty() || normalizedChildNodeId.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime child branch node id is empty.");
        return false;
    }

    if (m_invocationState.nodeBranchIds.contains(normalizedChildNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime child node already has a branch: %1").arg(
                           normalizedChildNodeId);
        return false;
    }

    if (!m_invocationState.nodeResults.contains(normalizedParentNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime parent node result is missing: %1").arg(
                           normalizedParentNodeId);
        return false;
    }

    const _tagNodeResult parentResult = m_invocationState.nodeResults.value(normalizedParentNodeId);
    _tagBranchState branch;
    branch.branchId = normalizedChildNodeId;
    branch.sourceNodeId = parentResult.sourceNodeId;
    branch.sourceTrigger = parentResult.sourceTrigger;
    branch.local = parentResult.local.Snapshot();
    branch.removedKeys = parentResult.removedKeys;
    m_invocationState.branches.insert(branch.branchId, branch);
    m_invocationState.nodeBranchIds.insert(normalizedChildNodeId, branch.branchId);
    return true;
}

bool AgentGraphExecutor::CreateJoinBranch(const QString &childNodeId, QString &errorMessage)
{
    const QString normalizedChildNodeId = childNodeId.trimmed();

    if (normalizedChildNodeId.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime join node id is empty.");
        return false;
    }

    if (m_invocationState.nodeBranchIds.contains(normalizedChildNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime join node already has a branch: %1").arg(
                           normalizedChildNodeId);
        return false;
    }

    QVector<QString> predecessors;
    _tagAgentDagNode joinNode;

    if (!m_dagGraph.GetPredecessors(normalizedChildNodeId, predecessors)
        || !m_dagGraph.GetNode(normalizedChildNodeId, joinNode))
    {
        errorMessage = QStringLiteral("Agent runtime join definition is invalid: %1").arg(
                           normalizedChildNodeId);
        return false;
    }

    QVector<QString> activePredecessors;

    for (const QString &predecessor : predecessors)
    {
        if (m_invocationState.activeNodeIds.contains(predecessor))
        {
            activePredecessors.append(predecessor);
        }
    }

    predecessors = activePredecessors;

    if (predecessors.size() < 2)
    {
        errorMessage = QStringLiteral("Agent runtime join has fewer than two active parents: %1").arg(
                           normalizedChildNodeId);
        return false;
    }

    const QJsonValue mergeValue = joinNode.config.value(QStringLiteral("merge"));

    if (!mergeValue.isUndefined() && !mergeValue.isObject())
    {
        errorMessage = QStringLiteral("Agent runtime join merge config must be an object: %1").arg(
                           normalizedChildNodeId);
        return false;
    }

    const QJsonObject mergeRules = mergeValue.toObject();

    if ((joinNode.type == AgentContextKeys::NODE_TYPE_LLM_CHAT) && mergeRules.isEmpty())
    {
        errorMessage = QStringLiteral(
                           "Agent runtime llm.chat has multiple active parents without an explicit merge policy: %1")
                           .arg(normalizedChildNodeId);
        return false;
    }

    QSet<QString> candidateKeys;

    for (const QString &predecessorId : predecessors)
    {
        if (!m_invocationState.nodeResults.contains(predecessorId))
        {
            errorMessage = QStringLiteral("Agent runtime join parent result is missing: %1 -> %2")
                               .arg(predecessorId, normalizedChildNodeId);
            return false;
        }

        const _tagNodeResult &parentResult = m_invocationState.nodeResults[predecessorId];

        for (const QString &key : parentResult.local.GetKeys())
        {
            candidateKeys.insert(key);
        }

        candidateKeys.unite(parentResult.removedKeys);
    }

    _tagBranchState branch;
    branch.branchId = normalizedChildNodeId;

    if (!m_invocationState.trigger.isEmpty()
        && !branch.local.SetValue(AgentContextKeys::RUNTIME_TRIGGER_TYPE, m_invocationState.trigger))
    {
        errorMessage = QStringLiteral("Agent runtime failed to restore join trigger: %1").arg(
                           normalizedChildNodeId);
        return false;
    }

    QStringList sortedKeys = candidateKeys.values();
    sortedKeys.sort();

    for (const QString &key : sortedKeys)
    {
        if (!MergeJoinKey(normalizedChildNodeId,
                          key,
                          predecessors,
                          mergeRules,
                          branch.local,
                          branch.removedKeys,
                          errorMessage))
        {
            return false;
        }
    }

    m_invocationState.branches.insert(branch.branchId, branch);
    m_invocationState.nodeBranchIds.insert(normalizedChildNodeId, branch.branchId);
    return true;
}

bool AgentGraphExecutor::MergeJoinKey(const QString &joinNodeId,
                                      const QString &key,
                                      const QVector<QString> &predecessors,
                                      const QJsonObject &mergeRules,
                                      AgentContext &local,
                                      QSet<QString> &removedKeys,
                                      QString &errorMessage)
{
    if (joinNodeId.trimmed().isEmpty() || key.trimmed().isEmpty() || predecessors.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime join key arguments are invalid.");
        return false;
    }

    if (key == AgentContextKeys::EXECUTED_NODES)
    {
        QStringList mergedExecutedNodes;

        for (const QString &predecessorId : predecessors)
        {
            QVariant value;

            if (!m_invocationState.nodeResults[predecessorId].local.GetValue(key, value))
            {
                continue;
            }

            for (const QString &executedNodeId : value.toStringList())
            {
                if (!mergedExecutedNodes.contains(executedNodeId))
                {
                    mergedExecutedNodes.append(executedNodeId);
                }
            }
        }

        return local.SetValue(key, mergedExecutedNodes);
    }

    if (key.startsWith(QStringLiteral("runtime.")))
    {
        return true;
    }

    QVector<QString> candidateParents;
    QVector<QVariant> candidateValues;
    QVector<bool> candidateRemovals;

    for (const QString &predecessorId : predecessors)
    {
        const _tagNodeResult &parentResult = m_invocationState.nodeResults[predecessorId];
        QVariant value;

        if (parentResult.local.GetValue(key, value))
        {
            candidateParents.append(predecessorId);
            candidateValues.append(value);
            candidateRemovals.append(false);
        }
        else if (parentResult.removedKeys.contains(key))
        {
            candidateParents.append(predecessorId);
            candidateValues.append(QVariant());
            candidateRemovals.append(true);
        }
    }

    if (candidateParents.isEmpty())
    {
        return true;
    }

    const QString strategy = mergeRules.value(key).toString().trimmed().toLower();

    if (!strategy.isEmpty()
        && (strategy != QStringLiteral("prefer_user"))
        && (strategy != QStringLiteral("prefer_vision"))
        && (strategy != QStringLiteral("concat")))
    {
        errorMessage = QStringLiteral("Agent runtime join strategy is invalid at node %1 for key %2: %3")
                           .arg(joinNodeId, key, strategy);
        return false;
    }

    QVector<int> selectedIndices;

    for (int candidateIndex = 0; candidateIndex < candidateParents.size(); ++candidateIndex)
    {
        if ((strategy == QStringLiteral("prefer_user"))
            || (strategy == QStringLiteral("prefer_vision")))
        {
            const QString preferredTrigger = (strategy == QStringLiteral("prefer_user"))
                                                 ? QStringLiteral("user")
                                                 : QStringLiteral("vision");
            const _tagNodeResult &parentResult =
                m_invocationState.nodeResults[candidateParents.at(candidateIndex)];

            if (parentResult.sourceTrigger != preferredTrigger)
            {
                continue;
            }
        }

        selectedIndices.append(candidateIndex);
    }

    if (selectedIndices.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime join strategy %1 found no matching parent for key %2 at node %3.")
                           .arg(strategy, key, joinNodeId);
        return false;
    }

    if (strategy == QStringLiteral("concat"))
    {
        QStringList values;

        for (const int selectedIndex : selectedIndices)
        {
            const QVariant &value = candidateValues.at(selectedIndex);

            if (candidateRemovals.at(selectedIndex)
                || ((value.metaType().id() != QMetaType::QString)
                    && (value.metaType().id() != QMetaType::QStringList)))
            {
                errorMessage = QStringLiteral("Agent runtime join concat requires string values for key %1 at node %2.")
                                   .arg(key, joinNodeId);
                return false;
            }

            values.append(value.metaType().id() == QMetaType::QStringList
                              ? value.toStringList()
                              : QStringList({value.toString()}));
        }

        return local.SetValue(key, values.join(QStringLiteral("\n")));
    }

    const int firstIndex = selectedIndices.first();
    const bool isRemoved = candidateRemovals.at(firstIndex);
    const QVariant selectedValue = candidateValues.at(firstIndex);

    for (const int selectedIndex : selectedIndices)
    {
        if ((candidateRemovals.at(selectedIndex) != isRemoved)
            || (!isRemoved && (candidateValues.at(selectedIndex) != selectedValue)))
        {
            errorMessage = QStringLiteral("Agent runtime join conflict at node %1 for key %2 from parents %3.")
                               .arg(joinNodeId, key, candidateParents.join(QStringLiteral(", ")));
            return false;
        }
    }

    if (isRemoved)
    {
        removedKeys.insert(key);
        return true;
    }

    return local.SetValue(key, selectedValue);
}

bool AgentGraphExecutor::CommitInvocationResult(AgentContext &context,
                                                AgentContext &sessionContext,
                                                QString &errorMessage)
{
    if (m_invocationState.completedNodeIds.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime invocation completed without nodes.");
        return false;
    }

    QString terminalNodeId;

    for (const QString &nodeId : m_dagGraph.GetNodeNames())
    {
        if (!m_invocationState.activeNodeIds.contains(nodeId))
        {
            continue;
        }

        QVector<QString> successors;

        if (!m_dagGraph.GetSuccessors(nodeId, successors))
        {
            errorMessage = QStringLiteral("Agent runtime terminal node lookup failed: %1").arg(nodeId);
            return false;
        }

        bool hasActiveSuccessor = false;

        for (const QString &successor : successors)
        {
            if (m_invocationState.activeNodeIds.contains(successor))
            {
                hasActiveSuccessor = true;
                break;
            }
        }

        if (!hasActiveSuccessor)
        {
            if (!terminalNodeId.isEmpty())
            {
                errorMessage = QStringLiteral(
                                   "Agent runtime has multiple terminal branches; P3 join merging is required.");
                return false;
            }

            terminalNodeId = nodeId;
        }
    }

    if (terminalNodeId.isEmpty() || !m_invocationState.nodeResults.contains(terminalNodeId))
    {
        errorMessage = QStringLiteral("Agent runtime terminal node result is missing.");
        return false;
    }

    const _tagNodeResult terminalResult = m_invocationState.nodeResults.value(terminalNodeId);
    AgentContext committedSessionContext = sessionContext.Snapshot();
    QVariant historyValue;

    if (terminalResult.local.GetValue(AgentContextKeys::CONVERSATION_HISTORY, historyValue)
        && !committedSessionContext.SetValue(AgentContextKeys::CONVERSATION_HISTORY, historyValue))
    {
        errorMessage = QStringLiteral("Agent runtime failed to commit conversation history.");
        return false;
    }

    if (terminalResult.removedKeys.contains(AgentContextKeys::CONVERSATION_HISTORY))
    {
        committedSessionContext.RemoveValue(AgentContextKeys::CONVERSATION_HISTORY);
    }

    const QStringList persistentKeys =
    {
        AgentContextKeys::PROACTIVE_LAST_SPOKEN_AT,
        AgentContextKeys::PROACTIVE_LAST_SUMMARY_HASH,
        AgentContextKeys::SEMANTIC_VISION_FRAME_HASH
    };

    for (const QString &key : persistentKeys)
    {
        QVariant value;

        if (terminalResult.local.GetValue(key, value)
            && !committedSessionContext.SetValue(key, value))
        {
            errorMessage = QStringLiteral("Agent runtime failed to commit persistent key: %1").arg(key);
            return false;
        }
    }

    AgentContext completedContext;

    if (!BuildExecutionView(terminalNodeId,
                            committedSessionContext,
                            completedContext,
                            errorMessage))
    {
        return false;
    }

    sessionContext = committedSessionContext;
    context = completedContext;
    return true;
}

void AgentGraphExecutor::ClearInvocationState()
{
    m_invocationState = _tagInvocationState();
}

} // namespace vpet
