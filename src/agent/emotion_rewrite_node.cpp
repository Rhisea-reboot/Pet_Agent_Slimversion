#include "vpet/agent/emotion_rewrite_node.h"
#include "vpet/agent/agent_context_keys.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

namespace vpet
{

namespace
{

const QString &CONTEXT_KEY_CONVERSATION_HISTORY = AgentContextKeys::CONVERSATION_HISTORY;
const QString &CONTEXT_KEY_EMOTION_LAST_REQUEST_ID = AgentContextKeys::EMOTION_LAST_REQUEST_ID;
const QString &CONTEXT_KEY_EMOTION_OUTPUT_TEXT = AgentContextKeys::EMOTION_OUTPUT_TEXT;
const QString &CONTEXT_KEY_EMOTION_PET = AgentContextKeys::EMOTION_PET;
const QString &CONTEXT_KEY_EMOTION_PROMPT_TEXT = AgentContextKeys::EMOTION_PROMPT_TEXT;
const QString &CONTEXT_KEY_EMOTION_RAW_RESPONSE = AgentContextKeys::EMOTION_RAW_RESPONSE;
const QString &CONTEXT_KEY_EMOTION_SOURCE_TEXT = AgentContextKeys::EMOTION_SOURCE_TEXT;
const QString &CONTEXT_KEY_EMOTION_USER = AgentContextKeys::EMOTION_USER;
const QString &CONTEXT_KEY_LLM_LAST_RESPONSE = AgentContextKeys::LLM_LAST_RESPONSE;
const QString &CONTEXT_KEY_NODE_INPUT_TEXT_RESPONSE = AgentContextKeys::NODE_INPUT_TEXT_RESPONSE;
const QString &CONTEXT_KEY_SEMANTIC_TEXT_RESPONSE = AgentContextKeys::SEMANTIC_TEXT_RESPONSE;
constexpr int MAX_HISTORY_LINES = 60;

} // anonymous namespace

bool EmotionRewriteNode::Execute(const _tagAgentDagNode &node,
                                 AgentContext &context,
                                 LlmClient *llmClient,
                                 int &pendingRequestId,
                                 QString &errorMessage)
{
    pendingRequestId = -1;

    if (node.id.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Emotion rewrite node id is empty.");
        return false;
    }

    if (llmClient == nullptr)
    {
        errorMessage = QStringLiteral("Emotion rewrite node LLM client is not initialized.");
        return false;
    }

    QVariant responseValue;

    if (!context.GetValue(CONTEXT_KEY_NODE_INPUT_TEXT_RESPONSE, responseValue)
        && !context.GetValue(CONTEXT_KEY_SEMANTIC_TEXT_RESPONSE, responseValue)
        && !context.GetValue(CONTEXT_KEY_LLM_LAST_RESPONSE, responseValue))
    {
        return true;
    }

    const QString sourceText = responseValue.toString().trimmed();

    if (sourceText.isEmpty())
    {
        errorMessage = QStringLiteral("Emotion rewrite node input is empty.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_EMOTION_SOURCE_TEXT, sourceText))
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to record source text.");
        return false;
    }

    if (!HasConversationContext(context))
    {
        if (!context.SetValue(CONTEXT_KEY_EMOTION_USER, QStringLiteral("neutral")))
        {
            errorMessage = QStringLiteral("Emotion rewrite node failed to record user emotion.");
            return false;
        }

        if (!context.SetValue(CONTEXT_KEY_EMOTION_PET, QStringLiteral("calm")))
        {
            errorMessage = QStringLiteral("Emotion rewrite node failed to record pet emotion.");
            return false;
        }

        if (!context.SetValue(CONTEXT_KEY_EMOTION_OUTPUT_TEXT, sourceText))
        {
            errorMessage = QStringLiteral("Emotion rewrite node failed to record output text.");
            return false;
        }

        return true;
    }

    const QString promptText = BuildPrompt(context, sourceText);

    if (!context.SetValue(CONTEXT_KEY_EMOTION_PROMPT_TEXT, promptText))
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to record prompt text.");
        return false;
    }

    QVector<_tagLlmMessage> messages;
    _tagLlmMessage message;
    message.role = LLM_MESSAGE_ROLE::USER;
    message.content = promptText;
    messages.append(message);

    const int requestId = llmClient->SendChat(messages);

    if (requestId <= 0)
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to send LLM request.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_EMOTION_LAST_REQUEST_ID, requestId))
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to record request ID.");
        return false;
    }

    pendingRequestId = requestId;

    return true;
}

bool EmotionRewriteNode::Complete(int requestId,
                                  const QString &content,
                                  AgentContext &context,
                                  QString &errorMessage)
{
    if (requestId <= 0)
    {
        errorMessage = QStringLiteral("Emotion rewrite request ID is invalid.");
        return false;
    }

    QVariant storedRequestValue;

    if (!context.GetValue(CONTEXT_KEY_EMOTION_LAST_REQUEST_ID, storedRequestValue))
    {
        errorMessage = QStringLiteral("Emotion rewrite request ID is missing.");
        return false;
    }

    if (storedRequestValue.toInt() != requestId)
    {
        errorMessage = QStringLiteral("Emotion rewrite request ID does not match.");
        return false;
    }

    const QString normalizedContent = content.trimmed();

    if (normalizedContent.isEmpty())
    {
        errorMessage = QStringLiteral("Emotion rewrite response is empty.");
        return false;
    }

    QString userEmotion;
    QString petEmotion;
    QString rewriteText;

    if (!ParseResponse(normalizedContent,
                       userEmotion,
                       petEmotion,
                       rewriteText,
                       errorMessage))
    {
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_EMOTION_USER, userEmotion))
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to record user emotion.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_EMOTION_PET, petEmotion))
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to record pet emotion.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_EMOTION_RAW_RESPONSE, normalizedContent))
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to record raw response.");
        return false;
    }

    if (!context.SetValue(CONTEXT_KEY_EMOTION_OUTPUT_TEXT, rewriteText))
    {
        errorMessage = QStringLiteral("Emotion rewrite node failed to record output text.");
        return false;
    }

    return true;
}

bool EmotionRewriteNode::HasConversationContext(const AgentContext &context)
{
    QVariant historyValue;

    if (!context.GetValue(CONTEXT_KEY_CONVERSATION_HISTORY, historyValue))
    {
        return false;
    }

    return !historyValue.toStringList().isEmpty();
}

QString EmotionRewriteNode::BuildPrompt(const AgentContext &context, const QString &sourceText)
{
    QString prompt;
    prompt += QStringLiteral("你是桌宠情感模块。\n");
    prompt += QStringLiteral("请基于最近对话历史、当前用户输入和原始回复，判断用户情绪、桌宠情绪，并生成适合桌宠口吻的最终回复。\n");
    prompt += QStringLiteral("要求：只输出严格 JSON，不要输出多余解释。JSON 字段必须包含 user_emotion, pet_emotion, rewrite。\n");
    prompt += QStringLiteral("user_emotion 和 pet_emotion 使用简短英文标签。rewrite 为最终要输出的中文文本。\n");
    prompt += QStringLiteral("如果没有足够上下文，请将 rewrite 保持为原始回复或轻微润色。\n\n");
    prompt += QStringLiteral("当前用户输入：");
    prompt += context.GetUserInput().trimmed();
    prompt += QStringLiteral("\n\n");
    prompt += QStringLiteral("最近对话历史：\n");
    prompt += BuildHistoryText(context);
    prompt += QStringLiteral("\n\n");
    prompt += QStringLiteral("原始回复：");
    prompt += sourceText.trimmed();

    return prompt;
}

bool EmotionRewriteNode::ParseResponse(const QString &content,
                                       QString &userEmotion,
                                       QString &petEmotion,
                                       QString &rewriteText,
                                       QString &errorMessage)
{
    const int jsonStart = content.indexOf(QChar('{'));
    const int jsonEnd = content.lastIndexOf(QChar('}'));

    if ((jsonStart < 0) || (jsonEnd <= jsonStart))
    {
        userEmotion = QStringLiteral("neutral");
        petEmotion = QStringLiteral("calm");
        rewriteText = content.trimmed();
        return !rewriteText.isEmpty();
    }

    const QByteArray jsonBytes = content.mid(jsonStart, jsonEnd - jsonStart + 1).toUtf8();
    const QJsonDocument document = QJsonDocument::fromJson(jsonBytes);

    if (!document.isObject())
    {
        errorMessage = QStringLiteral("Emotion rewrite response JSON is invalid.");
        return false;
    }

    const QJsonObject object = document.object();
    userEmotion = object.value(QStringLiteral("user_emotion")).toString().trimmed();
    petEmotion = object.value(QStringLiteral("pet_emotion")).toString().trimmed();
    rewriteText = object.value(QStringLiteral("rewrite")).toString().trimmed();

    if (userEmotion.isEmpty())
    {
        userEmotion = QStringLiteral("neutral");
    }

    if (petEmotion.isEmpty())
    {
        petEmotion = QStringLiteral("calm");
    }

    if (rewriteText.isEmpty())
    {
        errorMessage = QStringLiteral("Emotion rewrite response rewrite text is empty.");
        return false;
    }

    return true;
}

QString EmotionRewriteNode::BuildHistoryText(const AgentContext &context)
{
    QVariant historyValue;

    if (!context.GetValue(CONTEXT_KEY_CONVERSATION_HISTORY, historyValue))
    {
        return QString();
    }

    const QStringList historyLines = historyValue.toStringList();

    if (historyLines.isEmpty())
    {
        return QString();
    }

    QString historyText;
    const int startIndex = qMax(0, historyLines.size() - MAX_HISTORY_LINES);

    for (int index = startIndex; index < historyLines.size(); ++index)
    {
        historyText += historyLines.at(index);
        historyText += QStringLiteral("\n");
    }

    return historyText.trimmed();
}

} // namespace vpet
