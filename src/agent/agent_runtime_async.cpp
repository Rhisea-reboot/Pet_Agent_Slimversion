#include "vpet/agent/agent_runtime.h"
#include "agent_runtime_internal.h"
#include "vpet/agent/emotion_rewrite_node.h"
#include "vpet/agent/web_research_node.h"
#include "vpet/llm/llm_client.h"
#include "vpet/web/web_research_engine.h"

#include <QDebug>
#include <QTimer>
#include <QVariant>

namespace vpet
{

using namespace AgentRuntimeInternal;

bool AgentRuntime::CancelUserRequest(int requestId)
{
    if ((m_llmClient == nullptr) || (requestId <= 0))
    {
        return false;
    }

    const bool cancelled = m_llmClient->CancelRequest(requestId);

    if (m_streamingLlmRequests.contains(requestId))
    {
        m_sentenceSplitter.Reset(requestId);
        m_streamingLlmRequests.remove(requestId);
    }

    if (cancelled)
    {
        m_cancelledUserRequests.insert(requestId);
    }

    return cancelled;
}

void AgentRuntime::CancelActiveStreaming()
{
    const QSet<int> streamingRequests = m_streamingLlmRequests;

    for (const int requestId : streamingRequests)
    {
        CancelUserRequest(requestId);
    }
}

bool AgentRuntime::SendUserInputToLlm(const QString &userInput, QString &errorMessage)
{
    const QString normalizedUserInput = userInput.trimmed();

    if (normalizedUserInput.isEmpty())
    {
        errorMessage = QStringLiteral("Agent LLM user input is empty.");
        return false;
    }

    if ((m_llmClient == nullptr) || !m_llmClient->IsConfigured())
    {
        errorMessage = QStringLiteral("Agent LLM client is not configured.");
        return false;
    }

    const int requestId = m_llmClient->SendPrompt(normalizedUserInput);

    if (requestId <= 0)
    {
        errorMessage = QStringLiteral("Agent failed to send LLM request.");
        return false;
    }

    m_asyncBridge.AddDirectRequest(requestId);

    emit LogMessage(QStringLiteral("Agent LLM request sent: %1").arg(requestId));

    return true;
}

void AgentRuntime::OnLlmChatDelta(int requestId, const QString &delta)
{
    if (!m_streamingLlmRequests.contains(requestId))
    {
        return;
    }

    m_sentenceSplitter.AppendDelta(requestId, delta);
}

void AgentRuntime::OnLlmChatStreamFinished(int requestId, const QString &fullContent)
{
    if (!m_streamingLlmRequests.contains(requestId))
    {
        return;
    }

    (void)fullContent;
    m_sentenceSplitter.FinalizeStream(requestId);
    emit StreamResponseFinished(requestId);
}

void AgentRuntime::OnLlmChatCompleted(int requestId, const QString &content)
{
    m_streamingLlmRequests.remove(requestId);

    if (requestId <= 0)
    {
        emit LogMessage(QStringLiteral("Agent LLM response ignored because request ID is invalid."));
        return;
    }

    if (m_memoryConsolidator.HandleLlmCompleted(requestId, content))
    {
        emit LogMessage(QStringLiteral("Memory consolidation response processed: %1")
                            .arg(requestId));
        return;
    }

    if (m_asyncBridge.TakeDirectRequest(requestId))
    {
        if (content.trimmed().isEmpty())
        {
            EmitAgentRequestFailed(requestId,
                                   QStringLiteral("Agent LLM response is empty."),
                                   0,
                                   OUTPUT_SOURCE_USER_RESPONSE);
            return;
        }

        m_context.SetValue(AgentContextKeys::LLM_LAST_RESPONSE, content);
        m_context.SetValue(CONTEXT_KEY_OUTPUT_TEXT, content);
        m_context.SetValue(CONTEXT_KEY_SEMANTIC_TEXT_FINAL, content);
        m_context.SetValue(CONTEXT_KEY_NODE_OUTPUT_TEXT_FINAL, content);
        m_context.SetValue(CONTEXT_KEY_LLM_PENDING, false);
        ClearAsyncPendingState(m_context);
        ClearInvocationInputState(m_context);
        emit LlmResponseReceived(requestId, content);
        EmitAgentOutputReady(requestId, content, m_context);
        return;
    }

    const QString pendingKey = BuildPendingRequestKey(ASYNC_CLIENT_TEXT, requestId);

    if (pendingKey.isEmpty() || !m_asyncBridge.ContainsPending(pendingKey))
    {
        emit LogMessage(QStringLiteral("Agent LLM response ignored because request ID is unknown."));
        return;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;

    if (!m_asyncBridge.GetPending(pendingKey, pendingRequest))
    {
        emit LogMessage(QStringLiteral("Agent LLM response ignored because request ID is unknown."));
        return;
    }
    AgentContext callbackContext = pendingRequest.context.Snapshot();
    const QString failureSource = ReadOutputSource(callbackContext);
    QString errorMessage;

    if (content.trimmed().isEmpty())
    {
        ResetAsyncExecutionState(m_context);
        EmitAgentRequestFailed(requestId,
                                QStringLiteral("Agent LLM response is empty."),
                                0,
                                failureSource);
        return;
    }

    if (pendingRequest.nodeType == NODE_TYPE_EMOTION_REWRITE)
    {
        if (!EmotionRewriteNode::Complete(requestId, content, callbackContext, errorMessage))
        {
            ResetAsyncExecutionState(m_context);
            EmitAgentRequestFailed(requestId, errorMessage, 0, failureSource);
            return;
        }

        QVariant emotionOutputValue;

        if (callbackContext.GetValue(CONTEXT_KEY_EMOTION_OUTPUT_TEXT, emotionOutputValue))
        {
            callbackContext.SetValue(CONTEXT_KEY_SEMANTIC_TEXT_RESPONSE, emotionOutputValue);
            callbackContext.SetValue(CONTEXT_KEY_NODE_OUTPUT_TEXT_RESPONSE, emotionOutputValue);
        }
    }
    else if (!callbackContext.SetValue(AgentContextKeys::LLM_LAST_RESPONSE, content)
             || !callbackContext.SetValue(CONTEXT_KEY_LLM_LAST_REQUEST_ID, requestId)
             || !callbackContext.SetValue(CONTEXT_KEY_SEMANTIC_TEXT_RESPONSE, content)
             || !callbackContext.SetValue(CONTEXT_KEY_NODE_OUTPUT_TEXT_RESPONSE, content))
    {
        ResetAsyncExecutionState(m_context);
        EmitAgentRequestFailed(requestId,
                               QStringLiteral("Agent failed to store LLM response."),
                               0,
                               failureSource);
        return;
    }

    callbackContext.SetValue(CONTEXT_KEY_LLM_PENDING, false);
    ClearAsyncPendingState(callbackContext);

    if (!ResumePendingNode(pendingKey, requestId, callbackContext, errorMessage))
    {
        ResetAsyncExecutionState(m_context);
        EmitAgentRequestFailed(requestId, errorMessage, 0, failureSource);
        return;
    }

    // invocationCompleted emits AgentOutputReady after the DAG invocation finishes.
}

void AgentRuntime::OnLlmChatFailed(int requestId, const QString &message, int statusCode)
{
    if (m_cancelledUserRequests.remove(requestId))
    {
        emit LogMessage(QStringLiteral("Agent LLM request cancelled by user: %1").arg(requestId));

        if (m_streamingLlmRequests.contains(requestId))
        {
            m_sentenceSplitter.Reset(requestId);
            m_streamingLlmRequests.remove(requestId);
        }

        if (m_asyncBridge.TakeDirectRequest(requestId))
        {
            return;
        }

        const QString pendingKey = BuildPendingRequestKey(ASYNC_CLIENT_TEXT, requestId);

        if (m_asyncBridge.ContainsPending(pendingKey))
        {
            ResetAsyncExecutionState(m_context);
        }

        return;
    }

    if (m_streamingLlmRequests.contains(requestId))
    {
        m_sentenceSplitter.FinalizeStream(requestId);
        m_streamingLlmRequests.remove(requestId);
        emit StreamResponseFinished(requestId);
    }

    if (m_memoryConsolidator.HandleLlmFailed(requestId))
    {
        emit LogMessage(QStringLiteral("Memory consolidation request failed: %1")
                            .arg(requestId));
        return;
    }

    if (m_asyncBridge.TakeDirectRequest(requestId))
    {
        EmitAgentRequestFailed(requestId, message, statusCode, OUTPUT_SOURCE_USER_RESPONSE);
        return;
    }

    const QString pendingKey = BuildPendingRequestKey(ASYNC_CLIENT_TEXT, requestId);

    if (pendingKey.isEmpty() || !m_asyncBridge.ContainsPending(pendingKey))
    {
        emit LogMessage(QStringLiteral("Agent LLM failure ignored because request ID is unknown."));
        return;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;
    const QString failureSource = m_asyncBridge.GetPending(pendingKey, pendingRequest)
                                  ? ReadOutputSource(pendingRequest.context)
                                  : OUTPUT_SOURCE_USER_RESPONSE;
    ResetAsyncExecutionState(m_context);
    EmitAgentRequestFailed(requestId, message, statusCode, failureSource);
}

void AgentRuntime::OnVisionAnalysisCompleted(int requestId, const QString &content)
{
    const QString pendingKey = BuildPendingRequestKey(ASYNC_CLIENT_VISION, requestId);

    if (pendingKey.isEmpty() || !m_asyncBridge.ContainsPending(pendingKey))
    {
        emit LogMessage(QStringLiteral(
                            "Agent Vision LLM response ignored because pending state does not match."));
        return;
    }

    const QString normalizedContent = content.trimmed();

    if (normalizedContent.isEmpty())
    {
        ResetAsyncExecutionState(m_context);
        m_context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE, QStringLiteral("error"));
        emit LogMessage(QStringLiteral("Agent Vision LLM response is empty."));
        EmitAgentRequestFailed(requestId,
                               QStringLiteral("Agent Vision LLM response is empty."),
                               0,
                               OUTPUT_SOURCE_VISION_PROACTIVE);
        return;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;

    if (!m_asyncBridge.GetPending(pendingKey, pendingRequest))
    {
        emit LogMessage(QStringLiteral(
                            "Agent Vision LLM response ignored because pending state does not match."));
        return;
    }

    if (pendingRequest.nodeType != NODE_TYPE_VISION_LLM)
    {
        emit LogMessage(QStringLiteral(
                            "Agent Vision LLM response ignored because pending node type does not match."));
        return;
    }

    AgentContext callbackContext = pendingRequest.context.Snapshot();

    if (!callbackContext.SetValue(CONTEXT_KEY_VISION_ANALYSIS, normalizedContent))
    {
        ResetAsyncExecutionState(m_context);
        m_context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE, QStringLiteral("error"));
        emit LogMessage(QStringLiteral("Agent failed to record Vision LLM analysis."));
        EmitAgentRequestFailed(requestId,
                               QStringLiteral("Agent failed to record Vision LLM analysis."),
                               0,
                               OUTPUT_SOURCE_VISION_PROACTIVE);
        return;
    }

    if (!callbackContext.SetValue(CONTEXT_KEY_SEMANTIC_VISION_SUMMARY, normalizedContent))
    {
        ResetAsyncExecutionState(m_context);
        m_context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE, QStringLiteral("error"));
        emit LogMessage(QStringLiteral("Agent failed to record semantic vision summary."));
        EmitAgentRequestFailed(requestId,
                               QStringLiteral("Agent failed to record semantic vision summary."),
                               0,
                               OUTPUT_SOURCE_VISION_PROACTIVE);
        return;
    }

    if (!callbackContext.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE,
                                  QStringLiteral("analyzed")))
    {
        ResetAsyncExecutionState(m_context);
        m_context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE, QStringLiteral("error"));
        emit LogMessage(QStringLiteral("Agent failed to record completed semantic vision state."));
        EmitAgentRequestFailed(requestId,
                               QStringLiteral("Agent failed to record completed semantic vision state."),
                               0,
                               OUTPUT_SOURCE_VISION_PROACTIVE);
        return;
    }

    if (!callbackContext.SetValue(CONTEXT_KEY_VISION_LLM_PENDING, false))
    {
        ResetAsyncExecutionState(m_context);
        m_context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE, QStringLiteral("error"));
        emit LogMessage(QStringLiteral("Agent failed to clear Vision LLM pending state."));
        EmitAgentRequestFailed(requestId,
                               QStringLiteral("Agent failed to clear Vision LLM pending state."),
                               0,
                               OUTPUT_SOURCE_VISION_PROACTIVE);
        return;
    }

    ClearAsyncPendingState(callbackContext);
    emit LogMessage(QStringLiteral("Agent Vision LLM analysis completed."));

    QString errorMessage;

    if (!ResumePendingNode(pendingKey, requestId, callbackContext, errorMessage))
    {
        ResetAsyncExecutionState(m_context);
        emit LogMessage(errorMessage);
        EmitAgentRequestFailed(requestId, errorMessage, 0, OUTPUT_SOURCE_VISION_PROACTIVE);
    }
}

void AgentRuntime::OnVisionAnalysisFailed(int requestId, const QString &message, int statusCode)
{
    const QString pendingKey = BuildPendingRequestKey(ASYNC_CLIENT_VISION, requestId);

    if (pendingKey.isEmpty() || !m_asyncBridge.ContainsPending(pendingKey))
    {
        emit LogMessage(QStringLiteral(
                            "Agent Vision LLM failure ignored because request ID does not match pending node."));
        return;
    }

    const QString normalizedMessage = message.trimmed();
    const QString outputMessage = normalizedMessage.isEmpty()
                                  ? QStringLiteral("Agent Vision LLM request failed.")
                                  : normalizedMessage;

    ResetAsyncExecutionState(m_context);
    m_context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE, QStringLiteral("error"));

    emit LogMessage(QStringLiteral("Agent Vision LLM request failed: %1 %2").arg(
                        requestId).arg(outputMessage));
    EmitAgentRequestFailed(requestId, outputMessage, statusCode, OUTPUT_SOURCE_VISION_PROACTIVE);
}

void AgentRuntime::OnWebResearchCompleted(const _tagWebResearchResponse &response)
{
    if (m_webResearchStartInProgress)
    {
        m_bufferedWebResearchCompletion = response;
        m_hasBufferedWebResearchCompletion = true;
        return;
    }

    const int researchId = response.researchId;
    const QString pendingKey = BuildPendingRequestKey(ASYNC_CLIENT_WEB, researchId);

    if ((researchId <= 0) || pendingKey.isEmpty()
        || !m_asyncBridge.ContainsPending(pendingKey))
    {
        emit LogMessage(QStringLiteral("Agent web research completion ignored because request is unknown."));
        return;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;

    if (!m_asyncBridge.GetPending(pendingKey, pendingRequest)
        || (pendingRequest.clientType != ASYNC_CLIENT_WEB)
        || (pendingRequest.nodeType != NODE_TYPE_WEB_RESEARCH)
        || (pendingRequest.requestId != researchId))
    {
        emit LogMessage(QStringLiteral("Agent web research completion ignored because correlation does not match."));
        return;
    }

    AgentContext callbackContext = pendingRequest.context.Snapshot();
    QString errorMessage;

    if (!WebResearchNode::Complete(response, callbackContext, errorMessage))
    {
        ResetAsyncExecutionState(m_context);
        emit LogMessage(errorMessage);
        return;
    }

    ClearAsyncPendingState(callbackContext);

    if (!ResumePendingNode(pendingKey, researchId, callbackContext, errorMessage))
    {
        ResetAsyncExecutionState(m_context);
        emit LogMessage(errorMessage);
    }
}

void AgentRuntime::OnWebResearchFailed(int researchId,
                                       const QString &message,
                                       int statusCode)
{
    if (m_webResearchStartInProgress)
    {
        m_bufferedWebResearchFailureId = researchId;
        m_bufferedWebResearchFailureMessage = message;
        m_bufferedWebResearchFailureStatusCode = statusCode;
        m_hasBufferedWebResearchFailure = true;
        return;
    }

    (void)statusCode;
    const QString pendingKey = BuildPendingRequestKey(ASYNC_CLIENT_WEB, researchId);

    if ((researchId <= 0) || pendingKey.isEmpty()
        || !m_asyncBridge.ContainsPending(pendingKey))
    {
        emit LogMessage(QStringLiteral("Agent web research failure ignored because request is unknown."));
        return;
    }

    AgentAsyncBridge::_tagPendingRequest pendingRequest;

    if (!m_asyncBridge.GetPending(pendingKey, pendingRequest)
        || (pendingRequest.clientType != ASYNC_CLIENT_WEB)
        || (pendingRequest.nodeType != NODE_TYPE_WEB_RESEARCH))
    {
        emit LogMessage(QStringLiteral("Agent web research failure ignored because correlation does not match."));
        return;
    }

    AgentContext callbackContext = pendingRequest.context.Snapshot();
    QVariant policyValue;
    const QString failurePolicy = callbackContext.GetValue(
                                      AgentContextKeys::WEB_RESEARCH_FAILURE_POLICY,
                                      policyValue)
                                      ? policyValue.toString().trimmed().toLower()
                                      : QStringLiteral("continue");

    if (failurePolicy == QStringLiteral("fail"))
    {
        const QString failureSource = ReadOutputSource(callbackContext);
        ResetAsyncExecutionState(m_context);
        emit LogMessage(QStringLiteral("Agent web research failed with fail policy."));
        EmitAgentRequestFailed(researchId, message, statusCode, failureSource);
        return;
    }

    QString errorMessage;

    if (!WebResearchNode::CompleteFailure(message, callbackContext, errorMessage))
    {
        const QString failureSource = ReadOutputSource(callbackContext);
        ResetAsyncExecutionState(m_context);
        emit LogMessage(errorMessage);
        EmitAgentRequestFailed(researchId, errorMessage, statusCode, failureSource);
        return;
    }

    ClearAsyncPendingState(callbackContext);

    if (!ResumePendingNode(pendingKey, researchId, callbackContext, errorMessage))
    {
        const QString failureSource = ReadOutputSource(callbackContext);
        ResetAsyncExecutionState(m_context);
        emit LogMessage(errorMessage);
        EmitAgentRequestFailed(researchId, errorMessage, statusCode, failureSource);
    }
}
}
