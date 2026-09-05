#include "vpet/agent/tool_loop_node.h"
#include "vpet/agent/agent_context_keys.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>
#include <QVariant>

namespace vpet
{

namespace
{

constexpr int MIN_MAX_ROUNDS = 1;
constexpr int MAX_MAX_ROUNDS = 16;
constexpr int DEFAULT_MAX_ROUNDS = 4;
constexpr qint64 MIN_TIME_BUDGET_MS = 0;
constexpr qint64 MAX_TIME_BUDGET_MS = 600000;
constexpr qint64 DEFAULT_TIME_BUDGET_MS = 20000;
constexpr int MIN_MAX_TOOL_CALLS = 1;
constexpr int MAX_MAX_TOOL_CALLS = 64;
constexpr int DEFAULT_MAX_TOOL_CALLS = 12;
constexpr int MIN_TOOL_TIMEOUT_MS = 100;
constexpr int MAX_TOOL_TIMEOUT_MS = 120000;
constexpr int DEFAULT_TOOL_TIMEOUT_MS = 10000;

const QString PERMISSION_AUTO_READONLY = QStringLiteral("auto_readonly");
const QString PERMISSION_ALLOW_ALL = QStringLiteral("allow_all");
const QString PERMISSION_ASK = QStringLiteral("ask");
const QString BUDGET_ANSWER_WITH_CONTEXT = QStringLiteral("answer_with_context");
const QString BUDGET_END = QStringLiteral("end");
const QString FALLBACK_PLAIN_CHAT = QStringLiteral("plain_chat");
const QString FALLBACK_FAIL = QStringLiteral("fail");

/**
 * @brief 校验枚举配置值。
 * @param[in] value 配置值。
 * @param[in] allowed 允许值列表。
 * @param[in] key 配置键名。
 * @param[out] normalized 归一化值。
 * @param[out] errorMessage 错误描述。
 * @return 合法返回 true。
 */
bool ValidateEnumValue(const QJsonValue &value,
                       const QStringList &allowed,
                       const QString &key,
                       QString &normalized,
                       QString &errorMessage)
{
    if (value.isUndefined())
    {
        return true;
    }

    normalized = value.toString().trimmed().toLower();

    if (value.isString() && allowed.contains(normalized))
    {
        return true;
    }

    errorMessage = QStringLiteral("Agent tool.loop node config '%1' must be one of: %2.")
                       .arg(key, allowed.join(QStringLiteral(", ")));
    return false;
}

} // anonymous namespace

bool ToolLoopNode::ParseConfig(const _tagAgentDagNode &node,
                               const ToolRegistry &registry,
                               _tagToolLoopConfig &config,
                               QString &errorMessage)
{
    config = _tagToolLoopConfig();
    const QJsonObject configObject = node.config;
    for (const auto &key : {"max_rounds", "time_budget_ms", "max_tool_calls", "tool_timeout_ms"}) {
        const QJsonValue value = configObject.value(QLatin1String(key));
        if (value.isUndefined()) continue;
        const double number = value.toDouble(-1);
        if (!value.isDouble() || number < 0 || number > 600000 || number != value.toInt(-1)) {
            errorMessage = QStringLiteral("Tool loop budgets must be bounded nonnegative integers.");
            return false;
        }
    }

    const QJsonValue toolsValue = configObject.value(QStringLiteral("tools"));

    if (!toolsValue.isUndefined())
    {
        if (!toolsValue.isArray())
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'tools' must be an array.");
            return false;
        }

        const QJsonArray toolsArray = toolsValue.toArray();
        QStringList toolNames;

        for (const QJsonValue &toolValue : toolsArray)
        {
            const QString toolName = toolValue.toString().trimmed();

            if (toolName.isEmpty())
            {
                errorMessage = QStringLiteral("Tool names must be nonempty strings.");
                return false;
            }

            if (!registry.Contains(toolName))
            {
                errorMessage = QStringLiteral("Agent tool.loop node references unknown tool '%1'.")
                                   .arg(toolName);
                return false;
            }

            if (!toolNames.contains(toolName))
            {
                toolNames.append(toolName);
            }
        }

        config.tools = toolNames;
    }

    const QJsonValue maxRoundsValue = configObject.value(QStringLiteral("max_rounds"));

    if (!maxRoundsValue.isUndefined())
    {
        if (!maxRoundsValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'max_rounds' must be a number.");
            return false;
        }

        const int maxRounds = maxRoundsValue.toInt();

        if ((maxRounds < MIN_MAX_ROUNDS) || (maxRounds > MAX_MAX_ROUNDS))
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'max_rounds' is outside the allowed range.");
            return false;
        }

        config.maxRounds = maxRounds;
    }

    const QJsonValue timeBudgetValue = configObject.value(QStringLiteral("time_budget_ms"));

    if (!timeBudgetValue.isUndefined())
    {
        if (!timeBudgetValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'time_budget_ms' must be a number.");
            return false;
        }

        const qint64 timeBudgetMs = static_cast<qint64>(timeBudgetValue.toDouble());

        if ((timeBudgetMs < MIN_TIME_BUDGET_MS) || (timeBudgetMs > MAX_TIME_BUDGET_MS))
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'time_budget_ms' is outside the allowed range.");
            return false;
        }

        config.timeBudgetMs = timeBudgetMs;
    }

    const QJsonValue maxToolCallsValue = configObject.value(QStringLiteral("max_tool_calls"));

    if (!maxToolCallsValue.isUndefined())
    {
        if (!maxToolCallsValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'max_tool_calls' must be a number.");
            return false;
        }

        const int maxToolCalls = maxToolCallsValue.toInt();

        if ((maxToolCalls < MIN_MAX_TOOL_CALLS) || (maxToolCalls > MAX_MAX_TOOL_CALLS))
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'max_tool_calls' is outside the allowed range.");
            return false;
        }

        config.maxToolCalls = maxToolCalls;
    }

    const QJsonValue toolTimeoutValue = configObject.value(QStringLiteral("tool_timeout_ms"));

    if (!toolTimeoutValue.isUndefined())
    {
        if (!toolTimeoutValue.isDouble())
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'tool_timeout_ms' must be a number.");
            return false;
        }

        const int toolTimeoutMs = toolTimeoutValue.toInt();

        if ((toolTimeoutMs < MIN_TOOL_TIMEOUT_MS) || (toolTimeoutMs > MAX_TOOL_TIMEOUT_MS))
        {
            errorMessage = QStringLiteral("Agent tool.loop node config 'tool_timeout_ms' is outside the allowed range.");
            return false;
        }

        config.toolTimeoutMs = toolTimeoutMs;
    }

    QString permissionValue;

    if (!ValidateEnumValue(configObject.value(QStringLiteral("permission")),
                           {PERMISSION_AUTO_READONLY, PERMISSION_ALLOW_ALL, PERMISSION_ASK},
                           QStringLiteral("permission"), permissionValue, errorMessage))
    {
        return false;
    }

    if (!permissionValue.isEmpty())
    {
        config.permission = permissionValue;
    }

    QString budgetPolicyValue;

    if (!ValidateEnumValue(configObject.value(QStringLiteral("on_budget_exhausted")),
                           {BUDGET_ANSWER_WITH_CONTEXT, BUDGET_END},
                           QStringLiteral("on_budget_exhausted"), budgetPolicyValue,
                           errorMessage))
    {
        return false;
    }

    if (!budgetPolicyValue.isEmpty())
    {
        config.onBudgetExhausted = budgetPolicyValue;
    }

    QString fallbackValue;

    if (!ValidateEnumValue(configObject.value(QStringLiteral("fallback")),
                           {FALLBACK_PLAIN_CHAT, FALLBACK_FAIL},
                           QStringLiteral("fallback"), fallbackValue, errorMessage))
    {
        return false;
    }

    if (!fallbackValue.isEmpty())
    {
        config.fallback = fallbackValue;
    }

    return true;
}

bool ToolLoopNode::BuildRequest(const _tagAgentDagNode &node,
                                const AgentContext &context,
                                const ToolRegistry &registry,
                                _tagToolLoopRequest &request,
                                QString &errorMessage)
{
    request = _tagToolLoopRequest();

    if (!ParseConfig(node, registry, request.config, errorMessage))
    {
        return false;
    }

    QVariant promptValue;

    if (context.GetValue(AgentContextKeys::NODE_INPUT_PROMPT, promptValue)
        || context.GetValue(AgentContextKeys::SEMANTIC_TEXT_PROMPT, promptValue)
        || context.GetValue(AgentContextKeys::PROMPT_TEXT, promptValue))
    {
        request.promptText = promptValue.toString().trimmed();
    }

    if (request.promptText.isEmpty())
    {
        request.promptText = context.GetUserInput().trimmed();
    }

    if (request.promptText.isEmpty())
    {
        errorMessage = QStringLiteral("Agent tool.loop node prompt is empty.");
        return false;
    }

    QVariant historyValue;

    if (context.GetValue(AgentContextKeys::CONVERSATION_HISTORY, historyValue))
    {
        request.conversationHistory = historyValue.toStringList();
    }

    return true;
}

bool ToolLoopNode::Complete(const _tagToolLoopResult &result,
                            AgentContext &context,
                            QString &errorMessage)
{
    if (!context.SetValue(AgentContextKeys::SEMANTIC_TOOL_CALLS,
                          QString::fromUtf8(QJsonDocument(result.toolCallsAudit)
                                                .toJson(QJsonDocument::Compact)))
        || !context.SetValue(AgentContextKeys::SEMANTIC_TOOL_TRACE,
                             result.trace.join(QStringLiteral("\n"))))
    {
        errorMessage = QStringLiteral("Agent tool.loop node failed to write tool audit keys.");
        return false;
    }

    if (result.ok && !result.textOutput.trimmed().isEmpty())
    {
        if (!context.SetValue(AgentContextKeys::SEMANTIC_TEXT_RESPONSE, result.textOutput)
            || !context.SetValue(AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE,
                                 result.textOutput))
        {
            errorMessage = QStringLiteral("Agent tool.loop node failed to write final response.");
            return false;
        }
    }

    return true;
}

} // namespace vpet
