#include "vpet/agent/agent_output_policy.h"
#include "vpet/agent/agent_context_keys.h"

#include <QDateTime>

namespace vpet
{

bool AgentOutputPolicy::RecordVisionSpeech(AgentContext &context, QString &errorMessage)
{
    QVariant triggerValue;

    if (!context.GetValue(AgentContextKeys::RUNTIME_TRIGGER_TYPE, triggerValue)
        || (triggerValue.toString().trimmed() != QStringLiteral("vision")))
    {
        return true;
    }

    QVariant summaryHashValue;

    if (context.GetValue(AgentContextKeys::SEMANTIC_PROACTIVE_SUMMARY_HASH,
                         summaryHashValue)
        && !context.SetValue(AgentContextKeys::PROACTIVE_LAST_SUMMARY_HASH,
                             summaryHashValue))
    {
        errorMessage = QStringLiteral("Agent output policy failed to record proactive summary hash.");
        return false;
    }

    if (!context.SetValue(AgentContextKeys::PROACTIVE_LAST_SPOKEN_AT,
                          QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()))
    {
        errorMessage = QStringLiteral("Agent output policy failed to record proactive speech time.");
        return false;
    }

    return true;
}

} // namespace vpet
