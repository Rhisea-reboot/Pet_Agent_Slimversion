#ifndef VPET_AGENT_AGENT_GRAPH_EXECUTOR_H
#define VPET_AGENT_AGENT_GRAPH_EXECUTOR_H

#include "vpet/agent/agent_context.h"
#include "vpet/agent/agent_dag_graph.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

namespace vpet
{

/**
 * @brief Owns the DAG and executes one invocation at a time.
 */
class AgentGraphExecutor
{
public:
    /**
     * @brief Runtime operations required while pumping the graph.
     */
    struct _tagCallbacks
    {
        std::function<bool(AgentContext &, QString &)> prepareInput; ///< Prepares invocation input.
        std::function<bool(const _tagAgentDagNode &, AgentContext &, QString &)> executeNode; ///< Dispatches one node.
        std::function<bool(const _tagAgentDagNode &,
                           const AgentContext &,
                           quint64,
                           const QString &,
                           QString &)> registerPending; ///< Registers an async continuation.
        std::function<bool()> hasPending; ///< Tests for pending continuations.
        std::function<void(AgentContext &)> clearInput; ///< Clears consumed invocation input.
        std::function<void(AgentContext &)> resetAfterFailure; ///< Resets facade state after failure.
        std::function<void(AgentContext &)> invocationCompleted; ///< Reports successful completion.
    };

    /**
     * @brief Constructs an empty graph executor.
     */
    AgentGraphExecutor();

    /**
     * @brief Loads and topologically sorts a DAG.
     * @param[in] configPath DAG configuration path.
     * @param[out] errorMessage Failure description.
     * @return true when loading and sorting succeed.
     */
    bool Load(const QString &configPath, QString &errorMessage);

    /**
     * @brief Returns the loaded topological execution order.
     * @return Node IDs in topological order.
     */
    QVector<QString> GetExecutionOrder() const;

    /**
     * @brief Returns whether an invocation is active.
     * @return true when an invocation is active.
     */
    bool IsActive() const;

    /**
     * @brief Returns whether the supplied ID is the active invocation.
     * @param[in] invocationId Invocation ID to test.
     * @return true when the ID identifies the active invocation.
     */
    bool IsActiveInvocation(quint64 invocationId) const;

    /**
     * @brief Returns the most recently completed invocation ID.
     * @return Last completed invocation ID; 0 when none completed yet.
     */
    quint64 GetLastCompletedInvocationId() const;

    /**
     * @brief Initializes graph state for a context trigger.
     * @param[in] context Invocation input context.
     * @param[in] hasPendingRequest Whether an async request is already pending.
     * @param[out] errorMessage Failure description.
     * @return true when initialization succeeds.
     */
    bool BeginInvocation(const AgentContext &context,
                         bool hasPendingRequest,
                         QString &errorMessage);

    /**
     * @brief Pumps all currently ready nodes.
     * @param[in] shouldPrepareInput Whether input preparation is required.
     * @param[in,out] context Facade-visible invocation context.
     * @param[in,out] sessionContext Persistent session context.
     * @param[in] callbacks Runtime operation callbacks.
     * @param[out] errorMessage Failure description.
     * @return true when pumping succeeds or pauses for async work.
     */
    bool PumpReadyQueue(bool shouldPrepareInput,
                        AgentContext &context,
                        AgentContext &sessionContext,
                        const _tagCallbacks &callbacks,
                        QString &errorMessage);

    /**
     * @brief Resumes a completed asynchronous node and continues pumping.
     * @param[in] nodeId Pending node ID.
     * @param[in] invocationId Pending request invocation ID.
     * @param[in] resumedContext Context containing the async result.
     * @param[in,out] context Facade-visible invocation context.
     * @param[in,out] sessionContext Persistent session context.
     * @param[in] callbacks Runtime operation callbacks.
     * @param[out] errorMessage Failure description.
     * @return true when resumption succeeds.
     */
    bool ResumePendingNode(const QString &nodeId,
                           quint64 invocationId,
                           const AgentContext &resumedContext,
                           AgentContext &context,
                           AgentContext &sessionContext,
                           const _tagCallbacks &callbacks,
                           QString &errorMessage);

    /**
     * @brief Clears the current invocation scheduling state.
     */
    void ClearInvocationState();

private:
    struct _tagBranchState
    {
        QString branchId;             ///< Unique branch ID.
        QString sourceNodeId;         ///< Source node that created the branch.
        QString sourceTrigger;        ///< Source trigger declaration.
        AgentContext local;           ///< Branch-local context delta.
        QSet<QString> removedKeys;    ///< Keys removed relative to the session base.
    };

    struct _tagNodeResult
    {
        QString branchId;             ///< Result branch ID.
        QString sourceNodeId;         ///< Result source node ID.
        QString sourceTrigger;        ///< Result source trigger.
        AgentContext local;           ///< Completed branch-local context delta.
        QSet<QString> removedKeys;    ///< Completed branch removed keys.
    };

    struct _tagInvocationState
    {
        quint64 invocationId = 0;                         ///< Active invocation ID.
        bool isActive = false;                            ///< Whether an invocation is active.
        bool hasFailed = false;                           ///< Whether the invocation failed.
        QString trigger;                                  ///< Invocation trigger.
        QString failureMessage;                           ///< Last scheduling failure.
        QHash<QString, int> remainingInDegree;            ///< Remaining active dependencies.
        QHash<QString, int> nodeDeclarationOrder;         ///< Stable declaration ordering.
        QVector<QString> readyQueue;                      ///< Declaration-ordered ready nodes.
        QSet<QString> completedNodeIds;                   ///< Completed node IDs.
        QHash<QString, bool> nodeExecutionResults;        ///< Node success flags.
        QHash<QString, _tagBranchState> branches;         ///< Branch states by ID.
        QHash<QString, QString> nodeBranchIds;            ///< Node to branch mapping.
        QHash<QString, _tagNodeResult> nodeResults;       ///< Completed node results.
        QSet<QString> activeNodeIds;                      ///< Trigger-selected active nodes.
    };

    /**
     * @brief Validates callbacks required by graph pumping.
     * @param[in] callbacks Runtime operation callbacks.
     * @param[out] errorMessage Failure description.
     * @return true when all callbacks are present.
     */
    bool ValidateCallbacks(const _tagCallbacks &callbacks, QString &errorMessage) const;

    /**
     * @brief Selects source nodes matching the invocation trigger.
     * @param[in] allSourceNodes All DAG source nodes.
     * @param[out] sourceNodes Selected source nodes.
     * @param[out] errorMessage Failure description.
     * @return true when source selection succeeds.
     */
    bool SelectSourceNodes(const QVector<QString> &allSourceNodes,
                           QVector<QString> &sourceNodes,
                           QString &errorMessage);

    /**
     * @brief Builds the active subgraph and recalculates active in-degrees.
     * @param[in] sourceNodes Selected source nodes.
     * @param[in] nodeNames All declared node IDs.
     * @param[out] errorMessage Failure description.
     * @return true when the active subgraph is built.
     */
    bool BuildActiveSubgraph(const QVector<QString> &sourceNodes,
                             const QVector<QString> &nodeNames,
                             QString &errorMessage);

    /**
     * @brief Creates source branches and queues source nodes.
     * @param[in] sourceNodes Selected source nodes.
     * @param[out] errorMessage Failure description.
     * @return true when all source branches are initialized.
     */
    bool InitializeSourceBranches(const QVector<QString> &sourceNodes,
                                  QString &errorMessage);

    /**
     * @brief Initializes source branch deltas from the invocation input.
     * @param[in] context Invocation input context.
     * @param[in] sessionContext Persistent session context.
     * @param[out] errorMessage Failure description.
     * @return true when all source deltas are initialized.
     */
    bool InitializeSourceContexts(const AgentContext &context,
                                  const AgentContext &sessionContext,
                                  QString &errorMessage);

    /**
     * @brief Executes one ready node.
     * @param[in] nodeId Ready node ID.
     * @param[in,out] context Facade-visible invocation context.
     * @param[in] sessionContext Persistent session context.
     * @param[in] callbacks Runtime operation callbacks.
     * @param[out] errorMessage Failure description.
     * @return true when execution succeeds or registers async work.
     */
    bool ExecuteReadyNode(const QString &nodeId,
                          AgentContext &context,
                          const AgentContext &sessionContext,
                          const _tagCallbacks &callbacks,
                          QString &errorMessage);

    /**
     * @brief Records a graph failure and invokes runtime cleanup.
     * @param[in] nodeId Optional failed node ID.
     * @param[in] markNodeFailure Whether to record node failure.
     * @param[in,out] context Facade-visible invocation context.
     * @param[in] callbacks Runtime operation callbacks.
     * @param[in] errorMessage Failure description.
     */
    void FailInvocation(const QString &nodeId,
                        bool markNodeFailure,
                        AgentContext &context,
                        const _tagCallbacks &callbacks,
                        const QString &errorMessage);

    /**
     * @brief Marks a node complete and schedules newly ready successors.
     * @param[in] nodeId Completed node ID.
     * @param[out] errorMessage Failure description.
     * @return true when graph state is updated.
     */
    bool CompleteNode(const QString &nodeId, QString &errorMessage);

    /**
     * @brief Inserts a ready node in declaration order.
     * @param[in] nodeId Ready node ID.
     * @param[out] errorMessage Failure description.
     * @return true when inserted.
     */
    bool EnqueueReadyNode(const QString &nodeId, QString &errorMessage);

    /**
     * @brief Builds a branch execution view over the session context.
     * @param[in] nodeId Node ID.
     * @param[in] sessionContext Session base context.
     * @param[out] context Execution view.
     * @param[out] errorMessage Failure description.
     * @return true when the view is built.
     */
    bool BuildExecutionView(const QString &nodeId,
                            const AgentContext &sessionContext,
                            AgentContext &context,
                            QString &errorMessage) const;

    /**
     * @brief Saves a node result as its branch delta.
     * @param[in] nodeId Node ID.
     * @param[in] sessionContext Session base context.
     * @param[in] afterContext Executed node context.
     * @param[out] errorMessage Failure description.
     * @return true when saved.
     */
    bool SaveNodeResult(const QString &nodeId,
                        const AgentContext &sessionContext,
                        const AgentContext &afterContext,
                        QString &errorMessage);

    /**
     * @brief Creates a child branch from a completed parent result.
     * @param[in] parentNodeId Parent node ID.
     * @param[in] childNodeId Child node ID.
     * @param[out] errorMessage Failure description.
     * @return true when created.
     */
    bool CreateChildBranch(const QString &parentNodeId,
                           const QString &childNodeId,
                           QString &errorMessage);

    /**
     * @brief Creates a merged branch for a fan-in node.
     * @param[in] childNodeId Fan-in node ID.
     * @param[out] errorMessage Failure description.
     * @return true when created.
     */
    bool CreateJoinBranch(const QString &childNodeId, QString &errorMessage);

    /**
     * @brief Merges one context key according to join rules.
     * @param[in] joinNodeId Join node ID.
     * @param[in] key Context key.
     * @param[in] predecessors Active predecessor IDs.
     * @param[in] mergeRules Per-key merge rules.
     * @param[in,out] local Join branch local context.
     * @param[in,out] removedKeys Join branch removed keys.
     * @param[out] errorMessage Failure description.
     * @return true when the key is merged.
     */
    bool MergeJoinKey(const QString &joinNodeId,
                      const QString &key,
                      const QVector<QString> &predecessors,
                      const QJsonObject &mergeRules,
                      AgentContext &local,
                      QSet<QString> &removedKeys,
                      QString &errorMessage);

    /**
     * @brief Commits persistent terminal results to the session context.
     * @param[in,out] context Facade-visible invocation context.
     * @param[in,out] sessionContext Persistent session context.
     * @param[out] errorMessage Failure description.
     * @return true when committed.
     */
    bool CommitInvocationResult(AgentContext &context,
                                AgentContext &sessionContext,
                                QString &errorMessage);

private:
    AgentDagGraph m_dagGraph;                 ///< Loaded DAG structure.
    QVector<QString> m_executionOrder;        ///< Loaded topological order.
    _tagInvocationState m_invocationState;    ///< Current graph invocation state.
    quint64 m_nextInvocationId;               ///< Next invocation sequence value.
    quint64 m_lastCompletedInvocationId;      ///< Most recently completed invocation ID.
};

} // namespace vpet

#endif // VPET_AGENT_AGENT_GRAPH_EXECUTOR_H
