#include "vpet/agent/proactive_topic_node.h"
#include "vpet/agent/agent_context_keys.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonValue>
#include <QVariant>

namespace vpet
{

namespace
{

const QString DEFAULT_INSTRUCTION = QStringLiteral(
    "请根据画面中最值得关注的新内容，以桌宠口吻自然地说一句简短中文。"
    "不要描述整个画面，不要提及截图或图像识别，也不要虚构看不到的信息。");

/**
 * @brief 记录主动发话决策
 * @param[in,out] context 运行时上下文
 * @param[in] shouldSpeak 是否应继续生成主动发话
 * @param[in] reason 决策原因
 * @param[out] errorMessage 错误描述
 * @return 写入成功返回 true
 */
bool RecordDecision(AgentContext &context,
                    bool shouldSpeak,
                    const QString &reason,
                    QString &errorMessage)
{
    if (reason.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Proactive topic decision reason is empty.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::SEMANTIC_PROACTIVE_SHOULD_SPEAK,
                          shouldSpeak)
        || !context.SetValue(AgentContextKeys::SEMANTIC_PROACTIVE_REASON,
                             reason))
    {
        errorMessage = QStringLiteral("Proactive topic node failed to record decision.");
        return false;
    }

    return true;
}

} // anonymous namespace

bool ProactiveTopicNode::Execute(const _tagAgentDagNode &node,
                                 AgentContext &context,
                                 QString &errorMessage)
{
    if (node.id.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Proactive topic node id is empty.");
        return false;
    }

    QVariant existingPromptValue;

    if (context.GetValue(AgentContextKeys::SEMANTIC_TEXT_PROMPT, existingPromptValue)
        && !existingPromptValue.toString().trimmed().isEmpty())
    {
        return RecordDecision(context,
                              false,
                              QStringLiteral("user_prompt_available"),
                              errorMessage);
    }

    const bool isEnabled = node.config.value(QStringLiteral("enabled")).toBool(true);

    if (!isEnabled)
    {
        return RecordDecision(context,
                              false,
                              QStringLiteral("disabled"),
                              errorMessage);
    }

    QVariant visionSummaryValue;

    if (!context.GetValue(AgentContextKeys::SEMANTIC_VISION_SUMMARY,
                          visionSummaryValue))
    {
        return RecordDecision(context,
                              false,
                              QStringLiteral("vision_summary_missing"),
                              errorMessage);
    }

    const QString visionSummary = visionSummaryValue.toString().trimmed();

    if (visionSummary.isEmpty())
    {
        return RecordDecision(context,
                              false,
                              QStringLiteral("vision_summary_empty"),
                              errorMessage);
    }

    const QString summaryHash = QString::fromLatin1(
        QCryptographicHash::hash(visionSummary.toUtf8(), QCryptographicHash::Sha256).toHex());
    QString suppressionReason;

    if (!IsSpeechAllowed(node, context, summaryHash, suppressionReason))
    {
        return RecordDecision(context, false, suppressionReason, errorMessage);
    }

    QString instruction = node.config.value(QStringLiteral("instruction")).toString().trimmed();

    if (instruction.isEmpty())
    {
        instruction = DEFAULT_INSTRUCTION;
    }

    const QString prompt = BuildPrompt(instruction, visionSummary);

    if (prompt.isEmpty())
    {
        errorMessage = QStringLiteral("Proactive topic node generated an empty prompt.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::SEMANTIC_PROACTIVE_TOPIC, visionSummary)
        || !context.SetValue(AgentContextKeys::SEMANTIC_PROACTIVE_SUMMARY_HASH, summaryHash)
        || !context.SetValue(AgentContextKeys::SEMANTIC_PROACTIVE_SHOULD_SPEAK, true)
        || !context.SetValue(AgentContextKeys::SEMANTIC_PROACTIVE_REASON,
                             QStringLiteral("vision_summary_available"))
        || !context.SetValue(AgentContextKeys::SEMANTIC_TEXT_PROMPT, prompt)
        || !context.SetValue(AgentContextKeys::NODE_OUTPUT_PROMPT, prompt))
    {
        errorMessage = QStringLiteral("Proactive topic node failed to write output.");
        return false;
    }

    return true;
}

bool ProactiveTopicNode::IsSpeechAllowed(const _tagAgentDagNode &node,
                                         const AgentContext &context,
                                         const QString &summaryHash,
                                         QString &reason)
{
    const qint64 minimumIntervalMs = qMax<qint64>(
        0, node.config.value(QStringLiteral("min_interval_ms")).toInteger(30000));
    const qint64 dedupWindowMs = qMax<qint64>(
        0, node.config.value(QStringLiteral("dedup_window_ms")).toInteger(300000));
    const qint64 nowMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QVariant lastSpokenValue;
    QVariant lastHashValue;

    if (context.GetValue(AgentContextKeys::PROACTIVE_LAST_SPOKEN_AT, lastSpokenValue)
        && (nowMs - lastSpokenValue.toLongLong()) < minimumIntervalMs)
    {
        reason = QStringLiteral("cooldown");
        return false;
    }

    if (context.GetValue(AgentContextKeys::PROACTIVE_LAST_SUMMARY_HASH, lastHashValue)
        && context.GetValue(AgentContextKeys::PROACTIVE_LAST_SPOKEN_AT, lastSpokenValue)
        && (lastHashValue.toString() == summaryHash)
        && (nowMs - lastSpokenValue.toLongLong()) < dedupWindowMs)
    {
        reason = QStringLiteral("duplicate_summary");
        return false;
    }

    reason.clear();
    return true;
}

QString ProactiveTopicNode::BuildPrompt(const QString &instruction,
                                        const QString &visionSummary)
{
    const QString normalizedInstruction = instruction.trimmed();
    const QString normalizedVisionSummary = visionSummary.trimmed();

    if (normalizedInstruction.isEmpty() || normalizedVisionSummary.isEmpty())
    {
        return QString();
    }

    QString prompt;
    prompt += normalizedInstruction;
    prompt += QStringLiteral("\n\n当前画面摘要：\n");
    prompt += normalizedVisionSummary;
    prompt += QStringLiteral("\n\n只输出最终要说的话，不要解释。");

    return prompt;
}

} // namespace vpet
