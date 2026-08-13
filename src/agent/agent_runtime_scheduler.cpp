#include "vpet/agent/agent_runtime.h"
#include "agent_runtime_internal.h"
#include "vpet/llm/llm_client.h"
#include "vpet/web/web_research_engine.h"

#include <QDebug>
#include <QTimer>
#include <QVariant>

namespace vpet
{

using namespace AgentRuntimeInternal;

bool AgentRuntime::ExecuteNode(const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
{
    return m_nodeRegistry.Execute(node, context, errorMessage);
}

bool AgentRuntime::EnqueueInvocation(const AgentContext &context)
{
    return m_invocationQueue.Enqueue(context, m_sessionContext);
}

bool AgentRuntime::StartNextQueuedInvocation(QString &errorMessage)
{
    if (m_graphExecutor.IsActive() || m_invocationQueue.IsEmpty())
    {
        return true;
    }

    InvocationQueuePolicy::_tagEntry queuedInvocation;

    if (!m_invocationQueue.Dequeue(queuedInvocation))
    {
        return true;
    }
    m_contextWasQueued = false;
    m_context = m_sessionContext.Snapshot();

    if (!m_context.Overlay(queuedInvocation.local))
    {
        errorMessage = QStringLiteral("Agent runtime failed to restore queued invocation input.");
        m_context = m_sessionContext.Snapshot();
        return false;
    }

    for (const QString &removedKey : queuedInvocation.removedKeys)
    {
        m_context.RemoveValue(removedKey);
    }

    if (!m_graphExecutor.BeginInvocation(m_context,
                                         m_asyncBridge.HasPending(),
                                         errorMessage))
    {
        m_context = m_sessionContext.Snapshot();
        return false;
    }

    if (!m_graphExecutor.PumpReadyQueue(true,
                                        m_context,
                                        m_sessionContext,
                                        BuildGraphCallbacks(),
                                        errorMessage))
    {
        return false;
    }

    return true;
}

bool AgentRuntime::RegisterPendingNode(const _tagAgentDagNode &node,
                                       const AgentContext &context,
                                       quint64 invocationId,
                                       const QString &branchId,
                                       QString &errorMessage)
{
    QVariant requestIdValue;
    const QString nodeId = node.id.trimmed();
    const QString nodeType = node.type.trimmed();

    if (!m_graphExecutor.IsActiveInvocation(invocationId)
        || nodeId.isEmpty() || nodeType.isEmpty() || branchId.trimmed().isEmpty()
        || !context.GetValue(CONTEXT_KEY_RUNTIME_PENDING_REQUEST_ID, requestIdValue))
    {
        errorMessage = QStringLiteral("Agent runtime pending node state is invalid.");
        return false;
    }

    const int requestId = requestIdValue.toInt();
    QString clientType = ASYNC_CLIENT_TEXT;

    if (nodeType == NODE_TYPE_VISION_LLM)
    {
        clientType = ASYNC_CLIENT_VISION;
    }
    else if (nodeType == NODE_TYPE_WEB_RESEARCH)
    {
        clientType = ASYNC_CLIENT_WEB;
    }
    const QString pendingKey = m_asyncBridge.BuildRequestKey(clientType, requestId);
    const int timeoutMs = node.config.value(QStringLiteral("async_timeout_ms"))
                              .toInt(DEFAULT_ASYNC_TIMEOUT_MS);

    if ((requestId <= 0) || branchId.isEmpty() || pendingKey.isEmpty()
        || m_asyncBridge.ContainsPending(pendingKey)
        || (timeoutMs <= 0) || (timeoutMs > MAX_ASYNC_TIMEOUT_MS))
    {
        errorMessage = QStringLiteral("Agent runtime pending request is invalid, duplicated, or has an invalid timeout: %1")
                           .arg(requestId);
        return false;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;
    pendingRequest.requestId = requestId;
    pendingRequest.clientType = clientType;
    pendingRequest.invocationId = invocationId;
    pendingRequest.nodeId = nodeId;
    pendingRequest.branchId = branchId;
    pendingRequest.nodeType = nodeType;
    pendingRequest.context = context.Snapshot();
    if (!m_asyncBridge.AddPending(pendingKey, pendingRequest))
    {
        errorMessage = QStringLiteral("Agent runtime pending request is invalid, duplicated, or has an invalid timeout: %1")
                           .arg(requestId);
        return false;
    }

    QTimer::singleShot(timeoutMs, this, [this, pendingKey, requestId, invocationId]()
    {
        HandlePendingRequestTimeout(pendingKey, requestId, invocationId);
    });

    return true;
}

AgentGraphExecutor::_tagCallbacks AgentRuntime::BuildGraphCallbacks()
{
    AgentGraphExecutor::_tagCallbacks callbacks;
    callbacks.prepareInput = [this](AgentContext &context, QString &errorMessage)
    {
        return PrepareTextInputContext(context, errorMessage);
    };
    callbacks.executeNode = [this](const _tagAgentDagNode &node,
                                   AgentContext &context,
                                   QString &errorMessage)
    {
        return ExecuteNode(node, context, errorMessage);
    };
    callbacks.registerPending = [this](const _tagAgentDagNode &node,
                                       const AgentContext &context,
                                       quint64 invocationId,
                                       const QString &branchId,
                                       QString &errorMessage)
    {
        const bool registered = RegisterPendingNode(node,
                                                    context,
                                                    invocationId,
                                                    branchId,
                                                    errorMessage);

        if (registered)
        {
            qDebug() << "[Agent] Node is waiting for async response:" << node.id.trimmed();
        }

        return registered;
    };
    callbacks.hasPending = [this]()
    {
        return m_asyncBridge.HasPending();
    };
    callbacks.clearInput = [this](AgentContext &context)
    {
        ClearInvocationInputState(context);
    };
    callbacks.resetAfterFailure = [this](AgentContext &context)
    {
        ResetAsyncExecutionState(context);
    };
    callbacks.invocationCompleted = [this](AgentContext &context)
    {
        // Output emission is shared by synchronous and asynchronous invocation paths.
        // invocationCompleted runs only after the DAG invocation has finished.
        QVariant outputValue;

        if (context.GetValue(CONTEXT_KEY_OUTPUT_TEXT, outputValue))
        {
            const QString outputText = outputValue.toString().trimmed();

            if (!outputText.isEmpty())
            {
                int requestId = 0;
                QVariant requestIdValue;

                if (context.GetValue(CONTEXT_KEY_LLM_LAST_REQUEST_ID, requestIdValue)
                    && (requestIdValue.toInt() > 0))
                {
                    requestId = requestIdValue.toInt();
                }
                else if (context.GetValue(CONTEXT_KEY_RUNTIME_PENDING_REQUEST_ID, requestIdValue)
                         && (requestIdValue.toInt() > 0))
                {
                    requestId = requestIdValue.toInt();
                }
                else if (context.GetValue(CONTEXT_KEY_VISION_LLM_LAST_REQUEST_ID, requestIdValue)
                         && (requestIdValue.toInt() > 0))
                {
                    requestId = requestIdValue.toInt();
                }
                else
                {
                    // Synchronous graphs have no LLM request ID. Use the invocation ID
                    // so the UI can still correlate AgentOutputReady deterministically.
                    requestId = static_cast<int>(m_graphExecutor.GetLastCompletedInvocationId());

                    if (requestId <= 0)
                    {
                        requestId = 1;
                    }
                }

                emit LlmResponseReceived(requestId, outputText);
                EmitAgentOutputReady(requestId, outputText, context);
            }
        }

        if (!m_invocationQueue.IsEmpty())
        {
            QTimer::singleShot(0, this, [this]()
            {
                QString queuedErrorMessage;

                if (!StartNextQueuedInvocation(queuedErrorMessage)
                    && !queuedErrorMessage.isEmpty())
                {
                    emit LogMessage(queuedErrorMessage);
                }
            });
        }

        qDebug() << "[Agent] Ready queue execution finished.";
    };
    return callbacks;
}

QString AgentRuntime::BuildPendingRequestKey(const QString &clientType, int requestId) const
{
    return m_asyncBridge.BuildRequestKey(clientType, requestId);
}

bool AgentRuntime::ResumePendingNode(const QString &pendingKey,
                                     int requestId,
                                     const AgentContext &context,
                                     QString &errorMessage)
{
    if (pendingKey.trimmed().isEmpty() || (requestId <= 0) || !m_graphExecutor.IsActive()
        || !m_asyncBridge.ContainsPending(pendingKey))
    {
        errorMessage = QStringLiteral("Agent runtime has no matching pending request to resume.");
        return false;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;

    if (!m_asyncBridge.TakePending(pendingKey, pendingRequest))
    {
        errorMessage = QStringLiteral("Agent runtime has no matching pending request to resume.");
        return false;
    }

    if (!m_graphExecutor.IsActiveInvocation(pendingRequest.invocationId))
    {
        errorMessage = QStringLiteral("Agent runtime pending request belongs to an old invocation.");
        return false;
    }

    return m_graphExecutor.ResumePendingNode(pendingRequest.nodeId,
                                             pendingRequest.invocationId,
                                             context,
                                             m_context,
                                             m_sessionContext,
                                             BuildGraphCallbacks(),
                                             errorMessage);
}

void AgentRuntime::HandlePendingRequestTimeout(const QString &pendingKey,
                                               int requestId,
                                               quint64 invocationId)
{
    if (pendingKey.trimmed().isEmpty() || (requestId <= 0) || (invocationId == 0)
        || !m_graphExecutor.IsActiveInvocation(invocationId)
        || !m_asyncBridge.ContainsPending(pendingKey))
    {
        return;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;
    const QString failureSource = m_asyncBridge.GetPending(pendingKey, pendingRequest)
                                  ? ReadOutputSource(pendingRequest.context)
                                  : OUTPUT_SOURCE_USER_RESPONSE;
    ResetAsyncExecutionState(m_context);
    const QString message = QStringLiteral("Agent async request timed out.");
    emit LogMessage(QStringLiteral("%1 Request ID: %2").arg(message).arg(requestId));
    EmitAgentRequestFailed(requestId, message, 0, failureSource);
}

bool AgentRuntime::PrepareTextInputContext(AgentContext &context, QString &errorMessage)
{
    QVariant triggerTypeValue;
    const bool hasTriggerType = context.GetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE,
                                                  triggerTypeValue);
    const QString triggerType = hasTriggerType ? triggerTypeValue.toString().trimmed()
                                               : QString();
    const bool isUserTrigger = (triggerType == TRIGGER_TYPE_USER);
    const QString userInput = isUserTrigger ? context.GetUserInput().trimmed() : QString();
    const bool isInputAvailable = isUserTrigger && !userInput.isEmpty();

    if (isUserTrigger && !isInputAvailable)
    {
        errorMessage = QStringLiteral("Agent user-triggered execution has no user input.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_INPUT_AVAILABLE, isInputAvailable))
    {
        errorMessage = QStringLiteral("Agent failed to record input state.");
        return false;
    }

    context.RemoveValue(AgentContextKeys::LLM_LAST_RESPONSE);
    context.RemoveValue(CONTEXT_KEY_EMOTION_OUTPUT_TEXT);
    context.RemoveValue(CONTEXT_KEY_LLM_LAST_REQUEST_ID);
    context.RemoveValue(CONTEXT_KEY_NODE_INPUT_PROMPT);
    context.RemoveValue(CONTEXT_KEY_NODE_INPUT_TEXT_RESPONSE);
    context.RemoveValue(CONTEXT_KEY_NODE_OUTPUT_TEXT_FINAL);
    context.RemoveValue(CONTEXT_KEY_NODE_OUTPUT_TEXT_RESPONSE);
    context.RemoveValue(AgentContextKeys::NODE_OUTPUT_PROMPT);
    context.RemoveValue(CONTEXT_KEY_OUTPUT_TEXT);
    context.RemoveValue(CONTEXT_KEY_OUTPUT_PENDING);
    context.RemoveValue(CONTEXT_KEY_SEMANTIC_OUTPUT_SOURCE);
    context.RemoveValue(CONTEXT_KEY_SEMANTIC_TEXT_FINAL);
    context.RemoveValue(CONTEXT_KEY_SEMANTIC_TEXT_PROMPT);
    context.RemoveValue(CONTEXT_KEY_SEMANTIC_TEXT_RESPONSE);
    context.RemoveValue(AgentContextKeys::SEMANTIC_PROACTIVE_REASON);
    context.RemoveValue(AgentContextKeys::SEMANTIC_PROACTIVE_SHOULD_SPEAK);
    context.RemoveValue(AgentContextKeys::SEMANTIC_PROACTIVE_TOPIC);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_NEED_SEARCH);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_PLAN);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_QUERIES);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_EVIDENCE);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_UNSUPPORTED_CLAIMS);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_CONFLICTS);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_STATUS);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_CITATIONS);
    context.RemoveValue(AgentContextKeys::SEMANTIC_WEB_RESEARCH_ROUND_COUNT);
    context.RemoveValue(CONTEXT_KEY_PROMPT_TEXT);
    ClearAsyncPendingState(context);

    if (!isInputAvailable)
    {
        emit LogMessage(QStringLiteral("Agent execution has no user text input."));
        return true;
    }

    if (!context.SetValue(CONTEXT_KEY_PROMPT_TEXT, userInput))
    {
        errorMessage = QStringLiteral("Agent failed to write prompt text.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_SEMANTIC_TEXT_PROMPT, userInput))
    {
        errorMessage = QStringLiteral("Agent failed to write semantic prompt text.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_NODE_INPUT_PROMPT, userInput))
    {
        errorMessage = QStringLiteral("Agent failed to write node prompt input.");
        return false;
    }

    emit LogMessage(QStringLiteral("Agent text input prepared."));

    return true;
}

void AgentRuntime::ClearInvocationInputState(AgentContext &context)
{
    context.RemoveValue(AgentContextKeys::USER_INPUT);
    context.RemoveValue(CONTEXT_KEY_INPUT_AVAILABLE);
    context.RemoveValue(CONTEXT_KEY_NODE_INPUT_PROMPT);
    context.RemoveValue(CONTEXT_KEY_PROMPT_TEXT);
    context.RemoveValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE);
    context.RemoveValue(CONTEXT_KEY_SEMANTIC_TEXT_PROMPT);
}

void AgentRuntime::ClearAsyncPendingState(AgentContext &context)
{
    m_asyncBridge.ClearContextProtocol(context);
}

void AgentRuntime::ResetAsyncExecutionState(AgentContext &context)
{
    const bool shouldCancelWebResearch = (m_webResearchEngine != nullptr)
                                          && m_webResearchEngine->IsBusy();
    const QVector<AgentAsyncBridge::_tagPendingRequest> pendingRequests =
        m_asyncBridge.TakeAllPending();
    context.SetValue(CONTEXT_KEY_LLM_PENDING, false);
    context.SetValue(CONTEXT_KEY_VISION_LLM_PENDING, false);
    ClearAsyncPendingState(context);
    ClearInvocationInputState(context);
    m_graphExecutor.ClearInvocationState();

    if (shouldCancelWebResearch)
    {
        m_webResearchEngine->Cancel();
    }

    // Clear correlation before aborting: resulting callbacks must not resume the failed invocation.
    for (const AgentAsyncBridge::_tagPendingRequest &pendingRequest : pendingRequests)
    {
        if ((pendingRequest.clientType == ASYNC_CLIENT_TEXT) && (m_llmClient != nullptr))
        {
            m_llmClient->CancelRequest(pendingRequest.requestId);
        }
        else if ((pendingRequest.clientType == ASYNC_CLIENT_VISION)
                 && (m_visionLlmClient != nullptr))
        {
            m_visionLlmClient->CancelRequest(pendingRequest.requestId);
        }
    }

    context = m_sessionContext.Snapshot();

    if (!m_invocationQueue.IsEmpty())
    {
        QTimer::singleShot(0, this, [this]()
        {
            QString queuedErrorMessage;

            if (!StartNextQueuedInvocation(queuedErrorMessage) && !queuedErrorMessage.isEmpty())
            {
                emit LogMessage(queuedErrorMessage);
            }
        });
    }
}

bool AgentRuntime::SetAsyncPendingState(const _tagAgentDagNode &node,
                                        AgentContext &context,
                                        int requestId,
                                        QString &errorMessage)
{
    return m_asyncBridge.SetContextProtocol(node.id,
                                            node.type,
                                            requestId,
                                            context,
                                            errorMessage);
}

}
