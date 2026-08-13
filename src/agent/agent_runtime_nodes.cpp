#include "vpet/agent/agent_runtime.h"
#include "agent_runtime_internal.h"
#include "vpet/agent/agent_output_policy.h"
#include "vpet/agent/emotion_rewrite_node.h"
#include "vpet/agent/memory_retrieve_node.h"
#include "vpet/agent/memory_store_node.h"
#include "vpet/agent/proactive_topic_node.h"
#include "vpet/agent/web_research_node.h"
#include "vpet/llm/llm_client.h"
#include "vpet/web/web_research_engine.h"

#include <QDebug>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTimer>
#include <QVariant>

namespace vpet
{

using namespace AgentRuntimeInternal;

bool AgentRuntime::ExecuteVisionInputNode(const _tagAgentDagNode &node,
                                          AgentContext &context,
                                          QString &errorMessage)
{
    if (node.id.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent vision input node id is empty.");
        return false;
    }

    QVariant visionBase64Value;
    QVariant visionFrameIdValue;
    QVariant visionWidthValue;
    QVariant visionHeightValue;
    QVariant visionModalityValue;

    const bool hasVisionBase64 = context.GetValue(CONTEXT_KEY_SEMANTIC_IMAGE_BASE64,
                                                   visionBase64Value)
                                   || context.GetValue(CONTEXT_KEY_VISION_LATEST_BASE64,
                                                       visionBase64Value);
    const bool hasVisionFrameId = context.GetValue(CONTEXT_KEY_SEMANTIC_VISION_FRAME_ID,
                                                    visionFrameIdValue)
                                    || context.GetValue(CONTEXT_KEY_VISION_LATEST_FRAME_ID,
                                                        visionFrameIdValue);
    const bool hasVisionWidth = context.GetValue(CONTEXT_KEY_SEMANTIC_IMAGE_WIDTH,
                                                  visionWidthValue)
                                || context.GetValue(CONTEXT_KEY_VISION_LATEST_WIDTH,
                                                    visionWidthValue);
    const bool hasVisionHeight = context.GetValue(CONTEXT_KEY_SEMANTIC_IMAGE_HEIGHT,
                                                   visionHeightValue)
                                 || context.GetValue(CONTEXT_KEY_VISION_LATEST_HEIGHT,
                                                     visionHeightValue);
    const bool hasVisionModality = context.GetValue(CONTEXT_KEY_SEMANTIC_IMAGE_MEDIA_TYPE,
                                                     visionModalityValue)
                                   || context.GetValue(CONTEXT_KEY_VISION_LATEST_MODALITY,
                                                       visionModalityValue);

    const bool hasInput = hasVisionBase64
                          && hasVisionFrameId
                          && hasVisionWidth
                          && hasVisionHeight
                          && hasVisionModality
                          && !visionBase64Value.toString().trimmed().isEmpty()
                          && (visionFrameIdValue.toInt() > 0)
                          && (visionWidthValue.toInt() > 0)
                          && (visionHeightValue.toInt() > 0)
                          && !visionModalityValue.toString().trimmed().isEmpty();

    if (!context.SetValue(CONTEXT_KEY_VISION_INPUT_READY, hasInput))
    {
        errorMessage = QStringLiteral("Agent vision input node failed to record readiness.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE,
                          hasInput ? QStringLiteral("ready") : QStringLiteral("waiting")))
    {
        errorMessage = QStringLiteral("Agent vision input node failed to record semantic state.");
        return false;
    }

    if (hasInput
        && (!context.SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_BASE64, visionBase64Value)
            || !context.SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_MEDIA_TYPE,
                                 ResolveVisionMediaType(visionModalityValue.toString()))
            || !context.SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_WIDTH, visionWidthValue)
            || !context.SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_HEIGHT, visionHeightValue)
            || !context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_FRAME_ID,
                                 visionFrameIdValue)))
    {
        errorMessage = QStringLiteral("Agent vision input node failed to sync semantic input.");
        return false;
    }

    if (hasInput)
    {
        emit LogMessage(QStringLiteral("Agent vision input is ready."));
    }
    else
    {
        emit LogMessage(QStringLiteral("Agent vision input node is waiting for perception data."));
    }

    return true;
}

bool AgentRuntime::ExecuteVisionLlmNode(const _tagAgentDagNode &node,
                                        AgentContext &context,
                                        QString &errorMessage)
{
    if (node.id.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent Vision LLM node id is empty.");
        return false;
    }

    QVariant visionStateValue;
    QVariant inputAvailableValue;

    if (context.GetValue(CONTEXT_KEY_INPUT_AVAILABLE, inputAvailableValue)
        && inputAvailableValue.toBool())
    {
        emit LogMessage(QStringLiteral("Agent Vision LLM node skipped during text input execution."));
        return true;
    }

    const bool hasSemanticState = context.GetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE,
                                                    visionStateValue);
    const bool hasSemanticReadyState = hasSemanticState
                                       && (visionStateValue.toString() == QStringLiteral("ready"));
    QVariant readyValue;
    const bool hasPrivateReadyState = context.GetValue(CONTEXT_KEY_VISION_INPUT_READY,
                                                        readyValue)
                                      && readyValue.toBool();

    if ((hasSemanticState && !hasSemanticReadyState)
        || (!hasSemanticState && !hasPrivateReadyState))
    {
        emit LogMessage(QStringLiteral("Agent Vision LLM node skipped because vision input is not ready."));
        return true;
    }

    QVariant imageValue;
    QVariant modalityValue;

    if (!context.GetValue(CONTEXT_KEY_SEMANTIC_IMAGE_BASE64, imageValue)
        && !context.GetValue(CONTEXT_KEY_VISION_LATEST_BASE64, imageValue))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node image data is missing.");
        return false;
    }

    if (!context.GetValue(CONTEXT_KEY_SEMANTIC_IMAGE_MEDIA_TYPE, modalityValue)
        && !context.GetValue(CONTEXT_KEY_VISION_LATEST_MODALITY, modalityValue))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node modality is missing.");
        return false;
    }

    const QByteArray base64Image = imageValue.toString().trimmed().toLatin1();
    const QString mediaType = ResolveVisionMediaType(modalityValue.toString());

    if (base64Image.isEmpty())
    {
        errorMessage = QStringLiteral("Agent Vision LLM node image data is empty.");
        return false;
    }

    if ((m_visionLlmClient == nullptr) || !m_visionLlmClient->IsConfigured())
    {
        errorMessage = QStringLiteral("Agent Vision LLM client is not configured.");
        return false;
    }

    const QString prompt = node.config.value(QStringLiteral("prompt")).toString().trimmed();
    const QString requestPrompt = prompt.isEmpty()
                                    ? QStringLiteral("请简洁准确地描述画面中与用户交互相关的内容。不要猜测看不到的信息。")
                                    : prompt;
    _tagVisionLlmRequestOptions options;
    const QJsonValue detailValue = node.config.value(QStringLiteral("detail"));

    if (!detailValue.isUndefined())
    {
        const QString detail = detailValue.toString().trimmed().toLower();

        if (detail == QStringLiteral("low"))
        {
            options.detailLevel = VISION_LLM_DETAIL_LEVEL::LOW;
        }
        else if (detail == QStringLiteral("high"))
        {
            options.detailLevel = VISION_LLM_DETAIL_LEVEL::HIGH;
        }
        else if (detail != QStringLiteral("auto"))
        {
            errorMessage = QStringLiteral("Agent Vision LLM node detail is invalid.");
            return false;
        }
    }

    const QJsonValue maxTokensValue = node.config.value(QStringLiteral("max_tokens"));

    if (!maxTokensValue.isUndefined())
    {
        if (!maxTokensValue.isDouble() || (maxTokensValue.toInt() < 1))
        {
            errorMessage = QStringLiteral("Agent Vision LLM node max_tokens is invalid.");
            return false;
        }

        options.maxTokens = maxTokensValue.toInt();
    }

    const int requestId = m_visionLlmClient->AnalyzeScreenshot(requestPrompt,
                                                                base64Image,
                                                                mediaType,
                                                                options);

    if (requestId <= 0)
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to send request.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_VISION_LLM_LAST_REQUEST_ID, requestId))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to record request ID.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_VISION_LLM_PENDING, true))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to record pending state.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING, true))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to record runtime pending state.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING_NODE_ID, node.id.trimmed()))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to record runtime pending node id.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING_NODE_TYPE, node.type.trimmed()))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to record runtime pending node type.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING_REQUEST_ID, requestId))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to record runtime pending request ID.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE,
                          QStringLiteral("processing")))
    {
        errorMessage = QStringLiteral("Agent Vision LLM node failed to record semantic state.");
        return false;
    }

    emit LogMessage(QStringLiteral("Agent Vision LLM node request sent: %1").arg(requestId));

    return true;
}

bool AgentRuntime::ParseLlmRequestOptions(const _tagAgentDagNode &node,
                                          _tagLlmRequestOptions &options,
                                          QString &errorMessage)
{
    const QJsonObject configObject = node.config;

    if (configObject.isEmpty())
    {
        return true;
    }

    const QJsonValue temperatureValue = configObject.value(QStringLiteral("temperature"));

    if (temperatureValue.isUndefined() == false)
    {
        if (!temperatureValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent LLM node temperature is not a number.");
            return false;
        }

        const double temperature = temperatureValue.toDouble();

        if ((temperature < LLM_TEMPERATURE_MIN) || (temperature > LLM_TEMPERATURE_MAX))
        {
            errorMessage = QStringLiteral("Agent LLM node temperature is outside the allowed range.");
            return false;
        }

        options.temperature = temperature;
    }

    const QJsonValue topPValue = configObject.value(QStringLiteral("top_p"));

    if (topPValue.isUndefined() == false)
    {
        if (!topPValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent LLM node top_p is not a number.");
            return false;
        }

        const double topP = topPValue.toDouble();

        if ((topP < LLM_TOP_P_MIN) || (topP > LLM_TOP_P_MAX))
        {
            errorMessage = QStringLiteral("Agent LLM node top_p is outside the allowed range.");
            return false;
        }

        options.topP = topP;
    }

    const QJsonValue frequencyPenaltyValue = configObject.value(QStringLiteral("frequency_penalty"));

    if (frequencyPenaltyValue.isUndefined() == false)
    {
        if (!frequencyPenaltyValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent LLM node frequency_penalty is not a number.");
            return false;
        }

        const double frequencyPenalty = frequencyPenaltyValue.toDouble();

        if ((frequencyPenalty < LLM_FREQUENCY_PENALTY_MIN)
            || (frequencyPenalty > LLM_FREQUENCY_PENALTY_MAX))
        {
            errorMessage = QStringLiteral("Agent LLM node frequency_penalty is outside the allowed range.");
            return false;
        }

        options.frequencyPenalty = frequencyPenalty;
    }

    const QJsonValue presencePenaltyValue = configObject.value(QStringLiteral("presence_penalty"));

    if (presencePenaltyValue.isUndefined() == false)
    {
        if (!presencePenaltyValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent LLM node presence_penalty is not a number.");
            return false;
        }

        const double presencePenalty = presencePenaltyValue.toDouble();

        if ((presencePenalty < LLM_PRESENCE_PENALTY_MIN)
            || (presencePenalty > LLM_PRESENCE_PENALTY_MAX))
        {
            errorMessage = QStringLiteral("Agent LLM node presence_penalty is outside the allowed range.");
            return false;
        }

        options.presencePenalty = presencePenalty;
    }

    const QJsonValue maxTokensValue = configObject.value(QStringLiteral("max_tokens"));

    if (maxTokensValue.isUndefined() == false)
    {
        if (!maxTokensValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent LLM node max_tokens is not a number.");
            return false;
        }

        const int maxTokens = maxTokensValue.toInt();

        if ((maxTokens < LLM_MAX_TOKENS_MIN) || (maxTokens > LLM_MAX_TOKENS_MAX))
        {
            errorMessage = QStringLiteral("Agent LLM node max_tokens is outside the allowed range.");
            return false;
        }

        options.maxTokens = maxTokens;
    }

    return true;
}

bool AgentRuntime::ExecuteLlmChatNode(const _tagAgentDagNode &node,
                                      AgentContext &context,
                                      QString &errorMessage)
{
    if (node.id.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent LLM node id is empty.");
        return false;
    }

    QVariant promptValue;

    if (!context.GetValue(CONTEXT_KEY_NODE_INPUT_PROMPT, promptValue)
        && !context.GetValue(CONTEXT_KEY_SEMANTIC_TEXT_PROMPT, promptValue)
        && !context.GetValue(CONTEXT_KEY_PROMPT_TEXT, promptValue))
    {
        emit LogMessage(QStringLiteral("Agent LLM node skipped because prompt is empty."));
        return true;
    }

    const QString promptText = promptValue.toString().trimmed();

    if (promptText.isEmpty())
    {
        emit LogMessage(QStringLiteral("Agent LLM node skipped because prompt text is empty."));
        return true;
    }

    if ((m_llmClient == nullptr) || !m_llmClient->IsConfigured())
    {
        errorMessage = QStringLiteral("Agent LLM client is not configured.");
        return false;
    }

    _tagLlmRequestOptions options;

    if (!ParseLlmRequestOptions(node, options, errorMessage))
    {
        return false;
    }

    const int requestId = m_llmClient->SendPrompt(promptText, options);

    if (requestId <= 0)
    {
        errorMessage = QStringLiteral("Agent LLM node failed to send request.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_LLM_LAST_REQUEST_ID, requestId))
    {
        errorMessage = QStringLiteral("Agent LLM node failed to record request ID.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_LLM_PENDING, true))
    {
        errorMessage = QStringLiteral("Agent LLM node failed to record pending state.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING, true))
    {
        errorMessage = QStringLiteral("Agent LLM node failed to record runtime pending state.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING_NODE_ID, node.id.trimmed()))
    {
        errorMessage = QStringLiteral("Agent LLM node failed to record runtime pending node id.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING_NODE_TYPE, node.type.trimmed()))
    {
        errorMessage = QStringLiteral("Agent LLM node failed to record runtime pending node type.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PENDING_REQUEST_ID, requestId))
    {
        errorMessage = QStringLiteral("Agent LLM node failed to record runtime pending request ID.");
        return false;
    }

    emit LogMessage(QStringLiteral("Agent LLM node request sent: %1 (temperature=%2, max_tokens=%3)")
                            .arg(requestId)
                            .arg(options.temperature)
                            .arg(options.maxTokens));

    return true;
}

bool AgentRuntime::ExecuteWebResearchNode(const _tagAgentDagNode &node,
                                          AgentContext &context,
                                          QString &errorMessage)
{
    if (m_webResearchEngine == nullptr)
    {
        errorMessage = QStringLiteral("Agent web research engine is not initialized.");
        return false;
    }

    _tagWebResearchRequest request;

    if (!WebResearchNode::BuildRequest(node, context, request, errorMessage))
    {
        return false;
    }

    m_webResearchStartInProgress = true;
    m_hasBufferedWebResearchCompletion = false;
    m_hasBufferedWebResearchFailure = false;
    m_bufferedWebResearchFailureId = -1;
    m_bufferedWebResearchFailureMessage.clear();
    m_bufferedWebResearchFailureStatusCode = 0;
    const int researchId = m_webResearchEngine->Start(request);
    m_webResearchStartInProgress = false;

    if (researchId <= 0)
    {
        m_hasBufferedWebResearchCompletion = false;
        m_hasBufferedWebResearchFailure = false;
        errorMessage = QStringLiteral("Agent web research node failed to start.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::WEB_RESEARCH_FAILURE_POLICY,
                          request.config.failurePolicy)
        || !context.SetValue(AgentContextKeys::WEB_RESEARCH_LAST_REQUEST_ID, researchId))
    {
        m_webResearchEngine->Cancel();
        errorMessage = QStringLiteral("Agent web research node failed to record request state.");
        return false;
    }

    if (!SetAsyncPendingState(node, context, researchId, errorMessage))
    {
        m_webResearchEngine->Cancel();
        m_hasBufferedWebResearchCompletion = false;
        m_hasBufferedWebResearchFailure = false;
        return false;
    }

    if (m_hasBufferedWebResearchCompletion
        && (m_bufferedWebResearchCompletion.researchId == researchId))
    {
        const _tagWebResearchResponse response = m_bufferedWebResearchCompletion;
        m_hasBufferedWebResearchCompletion = false;
        QTimer::singleShot(0, this, [this, response]()
        {
            OnWebResearchCompleted(response);
        });
    }
    else if (m_hasBufferedWebResearchFailure
             && (m_bufferedWebResearchFailureId == researchId))
    {
        const QString failureMessage = m_bufferedWebResearchFailureMessage;
        const int failureStatusCode = m_bufferedWebResearchFailureStatusCode;
        m_hasBufferedWebResearchFailure = false;
        QTimer::singleShot(0, this, [this, researchId, failureMessage, failureStatusCode]()
        {
            OnWebResearchFailed(researchId, failureMessage, failureStatusCode);
        });
    }
    else
    {
        m_hasBufferedWebResearchCompletion = false;
        m_hasBufferedWebResearchFailure = false;
    }

    emit LogMessage(QStringLiteral("Agent web research node started: %1").arg(researchId));
    return true;
}

bool AgentRuntime::ExecuteOutputFormatNode(const _tagAgentDagNode &node,
                                           AgentContext &context,
                                           QString &errorMessage)
{
    if (node.id.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent output node id is empty.");
        return false;
    }

    QVariant outputValue;

    if (!context.GetValue(CONTEXT_KEY_SEMANTIC_TEXT_FINAL, outputValue)
        && !context.GetValue(CONTEXT_KEY_SEMANTIC_TEXT_RESPONSE, outputValue)
        && !context.GetValue(CONTEXT_KEY_NODE_INPUT_TEXT_RESPONSE, outputValue)
        && !context.GetValue(CONTEXT_KEY_EMOTION_OUTPUT_TEXT, outputValue)
        && !context.GetValue(AgentContextKeys::LLM_LAST_RESPONSE, outputValue))
    {
        QVariant shouldSpeakValue;

        if (context.GetValue(AgentContextKeys::SEMANTIC_PROACTIVE_SHOULD_SPEAK,
                             shouldSpeakValue)
            && !shouldSpeakValue.toBool())
        {
            context.RemoveValue(CONTEXT_KEY_OUTPUT_PENDING);
            emit LogMessage(QStringLiteral("Agent output node completed without proactive speech."));
            return true;
        }

        if (!context.SetValue(CONTEXT_KEY_OUTPUT_PENDING, true))
        {
            errorMessage = QStringLiteral("Agent output node failed to record pending state.");
            return false;
        }

        emit LogMessage(QStringLiteral("Agent output node is waiting for LLM response."));
        return true;
    }

    const QString responseText = outputValue.toString().trimmed();

    if (responseText.isEmpty())
    {
        errorMessage = QStringLiteral("Agent output node received empty LLM response.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_OUTPUT_TEXT, responseText))
    {
        errorMessage = QStringLiteral("Agent output node failed to write output text.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_SEMANTIC_TEXT_FINAL, responseText))
    {
        errorMessage = QStringLiteral("Agent output node failed to write semantic final text.");
        return false;
    }

    if (!WriteOutputSource(context, errorMessage))
    {
        return false;
    }

    if (!AgentOutputPolicy::RecordVisionSpeech(context, errorMessage))
    {
        return false;
    }

    if (!AppendConversationHistory(context, responseText, errorMessage))
    {
        return false;
    }

    context.RemoveValue(CONTEXT_KEY_OUTPUT_PENDING);
    emit LogMessage(QStringLiteral("Agent output formatted."));

    return true;
}

bool AgentRuntime::WriteOutputSource(AgentContext &context, QString &errorMessage)
{
    QVariant triggerTypeValue;
    const bool hasTriggerType = context.GetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE,
                                                  triggerTypeValue);
    const QString triggerType = hasTriggerType ? triggerTypeValue.toString().trimmed()
                                               : QString();
    const QString outputSource = (triggerType == TRIGGER_TYPE_VISION)
                                 ? OUTPUT_SOURCE_VISION_PROACTIVE
                                 : OUTPUT_SOURCE_USER_RESPONSE;

    if (!context.SetValue(CONTEXT_KEY_SEMANTIC_OUTPUT_SOURCE, outputSource))
    {
        errorMessage = QStringLiteral("Agent output node failed to write output source.");
        return false;
    }

    return true;
}

QString AgentRuntime::ReadOutputSource(const AgentContext &context) const
{
    QVariant outputSourceValue;

    if (!context.GetValue(CONTEXT_KEY_SEMANTIC_OUTPUT_SOURCE, outputSourceValue))
    {
        QVariant triggerTypeValue;

        if (context.GetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE, triggerTypeValue)
            && (triggerTypeValue.toString().trimmed() == TRIGGER_TYPE_VISION))
        {
            return OUTPUT_SOURCE_VISION_PROACTIVE;
        }

        return OUTPUT_SOURCE_USER_RESPONSE;
    }

    const QString outputSource = outputSourceValue.toString().trimmed();

    if (outputSource == OUTPUT_SOURCE_VISION_PROACTIVE)
    {
        return OUTPUT_SOURCE_VISION_PROACTIVE;
    }

    return OUTPUT_SOURCE_USER_RESPONSE;
}

void AgentRuntime::EmitAgentOutputReady(int requestId,
                                        const QString &content,
                                        const AgentContext &context)
{
    if (requestId <= 0)
    {
        emit LogMessage(QStringLiteral("Agent output emission skipped because request ID is invalid."));
        return;
    }

    const QString normalizedContent = content.trimmed();

    if (normalizedContent.isEmpty())
    {
        emit LogMessage(QStringLiteral("Agent output emission skipped because content is empty."));
        return;
    }

    const QString outputSource = ReadOutputSource(context);

    m_latestSurfacedMemoryIds.clear();
    QVariant surfacedEntriesValue;

    if (context.GetValue(AgentContextKeys::SEMANTIC_MEMORY_ENTRIES, surfacedEntriesValue))
    {
        for (const QVariant &entryValue : surfacedEntriesValue.toList())
        {
            const QString memoryId = entryValue.toMap().value(QStringLiteral("id"))
                                         .toString().trimmed();

            if (!memoryId.isEmpty() && !m_latestSurfacedMemoryIds.contains(memoryId))
            {
                m_latestSurfacedMemoryIds.append(memoryId);
            }
        }
    }

    emit AgentOutputReady(requestId, normalizedContent, outputSource);
}

void AgentRuntime::EmitAgentRequestFailed(int requestId,
                                          const QString &message,
                                          int statusCode,
                                          const QString &source)
{
    const QString normalizedMessage = message.trimmed().isEmpty()
                                      ? QStringLiteral("Agent request failed.")
                                      : message.trimmed();
    const QString normalizedSource = (source.trimmed() == OUTPUT_SOURCE_VISION_PROACTIVE)
                                     ? OUTPUT_SOURCE_VISION_PROACTIVE
                                     : OUTPUT_SOURCE_USER_RESPONSE;
    emit LlmRequestFailed(requestId, normalizedMessage, statusCode);
    emit AgentRequestFailed(requestId, normalizedMessage, statusCode, normalizedSource);
}

bool AgentRuntime::AppendConversationHistory(AgentContext &context,
                                             const QString &outputText,
                                             QString &errorMessage)
{
    const QString normalizedUserInput = context.GetUserInput().trimmed();
    const QString normalizedOutputText = outputText.trimmed();

    if (normalizedOutputText.isEmpty())
    {
        errorMessage = QStringLiteral("Agent conversation history output is empty.");
        return false;
    }

    QVariant historyValue;
    QStringList conversationHistory;

    if (context.GetValue(CONTEXT_KEY_CONVERSATION_HISTORY, historyValue))
    {
        conversationHistory = historyValue.toStringList();
    }

    if (!normalizedUserInput.isEmpty())
    {
        conversationHistory.append(QStringLiteral("user: %1").arg(normalizedUserInput));
    }

    conversationHistory.append(QStringLiteral("assistant: %1").arg(normalizedOutputText));

    while (conversationHistory.size() > MAX_CONVERSATION_HISTORY_ITEMS)
    {
        conversationHistory.removeFirst();
    }

    if (!context.SetValue(CONTEXT_KEY_CONVERSATION_HISTORY, conversationHistory))
    {
        errorMessage = QStringLiteral("Agent failed to record conversation history.");
        return false;
    }

    return true;
}

bool AgentRuntime::ExecuteMemoryRetrieveNode(const _tagAgentDagNode &node,
                                             AgentContext &context,
                                             QString &errorMessage)
{
    return MemoryRetrieveNode::Execute(node,
                                       context,
                                       m_memoryService,
                                       m_memoryConfig,
                                       errorMessage);
}

bool AgentRuntime::ExecuteMemoryStoreNode(const _tagAgentDagNode &node,
                                          AgentContext &context,
                                          QString &errorMessage)
{
    if (!MemoryStoreNode::Execute(node,
                                  context,
                                  m_memoryService,
                                  m_memoryConfig,
                                  errorMessage))
    {
        return false;
    }

    if (!m_memoryConfig.automaticExtraction || (m_llmClient == nullptr)
        || (m_memoryService == nullptr) || !m_memoryService->IsRunning())
    {
        return true;
    }

    QVariant triggerValue;
    const QString triggerType = context.GetValue(AgentContextKeys::RUNTIME_TRIGGER_TYPE,
                                                 triggerValue)
                                    ? triggerValue.toString().trimmed()
                                    : QString();

    if (triggerType != AgentRuntimeInternal::TRIGGER_TYPE_USER)
    {
        return true;
    }

    QVariant finalValue;

    if (!context.GetValue(AgentContextKeys::SEMANTIC_TEXT_FINAL, finalValue))
    {
        return true;
    }

    QVariant petIdValue;
    QString petId = context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                        ? petIdValue.toString().trimmed()
                        : AgentRuntimeInternal::DEFAULT_PET_ID;

    QVariant entriesValue;
    QVector<MemoryEntry> knownEntries;

    if (context.GetValue(AgentContextKeys::SEMANTIC_MEMORY_ENTRIES, entriesValue))
    {
        const QVariantList entriesList = entriesValue.toList();

        for (const QVariant &entryValue : entriesList)
        {
            const QVariantMap entryMap = entryValue.toMap();
            MemoryEntry entry;
            entry.id = entryMap.value(QStringLiteral("id")).toString();
            entry.content = entryMap.value(QStringLiteral("content")).toString();

            if (!entry.id.isEmpty() && !entry.content.isEmpty())
            {
                knownEntries.append(entry);
            }
        }
    }

    m_memoryConsolidator.SubmitTurn(*m_llmClient,
                                    *m_memoryService,
                                    context.GetUserInput(),
                                    finalValue.toString(),
                                    petId,
                                    knownEntries,
                                    m_memoryConfig.consolidationMaxCandidates);

    return true;
}

bool AgentRuntime::ExecutePassThroughNode(const _tagAgentDagNode &node,
                                          AgentContext &context,
                                          QString &errorMessage)
{
    const QString nodeId = node.id.trimmed();
    const QString nodeType = node.type.trimmed();

    if (nodeId.isEmpty() || nodeType.isEmpty())
    {
        errorMessage = QStringLiteral("Agent pass-through node is invalid.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_RUNTIME_PASS_THROUGH_PREFIX + nodeId, nodeType))
    {
        errorMessage = QStringLiteral("Agent pass-through node failed to write context: %1").arg(
                           nodeId);
        return false;
    }

    emit LogMessage(QStringLiteral("Agent pass-through node executed: %1 (%2)").arg(
                        nodeId,
                        nodeType));

    return true;
}

void AgentRuntime::RegisterDefaultNodeHandlers()
{
    RegisterNodeHandler(NODE_TYPE_USER_INPUT,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecutePassThroughNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_VISION_INPUT,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecuteVisionInputNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_VISION_LLM,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecuteVisionLlmNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_LLM_CHAT,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecuteLlmChatNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_WEB_RESEARCH,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecuteWebResearchNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_MEMORY_RETRIEVE,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecuteMemoryRetrieveNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_MEMORY_STORE,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecuteMemoryStoreNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_EMOTION_REWRITE,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        int pendingRequestId = -1;

        if (!EmotionRewriteNode::Execute(node,
                                         context,
                                         m_llmClient,
                                         pendingRequestId,
                                         errorMessage))
        {
            return false;
        }

        if (pendingRequestId <= 0)
        {
            return true;
        }

        return SetAsyncPendingState(node, context, pendingRequestId, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_OUTPUT_FORMAT,
                        [this](const _tagAgentDagNode &node,
                               AgentContext &context,
                               QString &errorMessage)
    {
        return ExecuteOutputFormatNode(node, context, errorMessage);
    });

    RegisterNodeHandler(NODE_TYPE_PROACTIVE_TOPIC,
                        [](const _tagAgentDagNode &node,
                           AgentContext &context,
                           QString &errorMessage)
    {
        return ProactiveTopicNode::Execute(node, context, errorMessage);
    });
}
}
