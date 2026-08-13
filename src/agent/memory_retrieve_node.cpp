#include "vpet/agent/memory_retrieve_node.h"
#include "vpet/agent/agent_context_keys.h"

#include <QDebug>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace vpet
{

namespace
{

const QString TRIGGER_TYPE_VISION_MEMORY = QStringLiteral("vision");
const QString DEFAULT_PET_ID_MEMORY = QStringLiteral("default");
const QSet<QString> TOPIC_STOP_WORDS = {
    QStringLiteral("请问"),
    QStringLiteral("什么"),
    QStringLiteral("如何"),
    QStringLiteral("怎么"),
    QStringLiteral("可以"),
    QStringLiteral("这个"),
    QStringLiteral("那个")
};

/**
 * @brief 判断受限触发模式是否匹配当前提示
 * @param[in] pattern 字面模式或以 re: 开头的正则
 * @param[in] prompt 当前提示
 * @return 匹配返回 true
 */
bool TriggerMatchesPrompt(const QString &pattern, const QString &prompt)
{
    const QString normalizedPattern = pattern.trimmed();

    if (normalizedPattern.isEmpty())
    {
        return false;
    }

    if (!normalizedPattern.startsWith(QStringLiteral("re:"), Qt::CaseInsensitive))
    {
        return prompt.contains(normalizedPattern, Qt::CaseInsensitive);
    }

    if (normalizedPattern.mid(3).trimmed().isEmpty())
    {
        return false;
    }

    const QRegularExpression expression(normalizedPattern.mid(3),
                                        QRegularExpression::CaseInsensitiveOption
                                            | QRegularExpression::UseUnicodePropertiesOption);
    return expression.isValid() && expression.match(prompt).hasMatch();
}

/**
 * @brief 判断特殊记忆是否仍由当前提示触发
 * @param[in] entry 负面或流程记忆
 * @param[in] prompt 当前提示
 * @return 当前仍匹配返回 true
 */
bool SpecialMemoryMatchesPrompt(const MemoryEntry &entry, const QString &prompt)
{
    for (const QString &triggerPattern : entry.triggerPatterns)
    {
        if (TriggerMatchesPrompt(triggerPattern, prompt))
        {
            return true;
        }
    }

    return (entry.type == MemoryEntry::Type::Procedure)
           && TriggerMatchesPrompt(entry.procedure.trigger, prompt);
}

/**
 * @brief 提取用于主题相关性检查的中英文词元
 * @param[in] text 原始文本
 * @return 去重词元集合
 */
QSet<QString> ExtractTopicTokens(const QString &text)
{
    QSet<QString> tokens;
    const QString normalized = text.trimmed().toLower();

    for (int index = 0; index < normalized.size(); ++index)
    {
        const QChar character = normalized.at(index);

        if ((character.unicode() >= 0x4E00) && (character.unicode() <= 0x9FFF))
        {
            tokens.insert(QString(character));

            if (((index + 1) < normalized.size())
                && (normalized.at(index + 1).unicode() >= 0x4E00)
                && (normalized.at(index + 1).unicode() <= 0x9FFF))
            {
                tokens.insert(normalized.mid(index, 2));
            }

            continue;
        }

        if (!character.isLetterOrNumber())
        {
            continue;
        }

        const int start = index;

        while (((index + 1) < normalized.size()) && normalized.at(index + 1).isLetterOrNumber())
        {
            index += 1;
        }

        const QString token = normalized.mid(start, (index - start) + 1);

        if (token.size() >= 2)
        {
            tokens.insert(token);
        }
    }

    for (const QString &stopWord : TOPIC_STOP_WORDS)
    {
        tokens.remove(stopWord);

        for (const QChar &character : stopWord)
        {
            tokens.remove(QString(character));
        }
    }

    return tokens;
}

/**
 * @brief 计算两个话题词元集合的重合系数
 * @param[in] left 左词元集合
 * @param[in] right 右词元集合
 * @return 0.0 到 1.0 的重合率
 */
float TopicOverlap(const QSet<QString> &left, const QSet<QString> &right)
{
    if (left.isEmpty() || right.isEmpty())
    {
        return 0.0f;
    }

    const int denominator = qMin(left.size(), right.size());

    if (denominator <= 0)
    {
        return 0.0f;
    }

    return static_cast<float>((left & right).size())
           / static_cast<float>(denominator);
}

/**
 * @brief 判断延迟检索结果是否仍与当前话题相关
 * @param[in] previousQuery 产生结果的上一轮查询
 * @param[in] currentPrompt 当前轮提示
 * @param[in] entries 检索结果
 * @return 可安全注入返回 true
 */
bool IsResultRelevant(const QString &previousQuery,
                      const QString &currentPrompt,
                      const QVector<MemoryEntry> &entries)
{
    if (previousQuery.trimmed().isEmpty() || currentPrompt.trimmed().isEmpty())
    {
        return false;
    }

    const QSet<QString> previousTokens = ExtractTopicTokens(previousQuery);
    const QSet<QString> currentTokens = ExtractTopicTokens(currentPrompt);

    if (TopicOverlap(previousTokens, currentTokens) >= 0.3f)
    {
        return true;
    }

    for (const MemoryEntry &entry : entries)
    {
        if ((entry.type == MemoryEntry::Type::Negative)
            || (entry.type == MemoryEntry::Type::Procedure))
        {
            if (SpecialMemoryMatchesPrompt(entry, currentPrompt))
            {
                return true;
            }

            continue;
        }

        if (TopicOverlap(ExtractTopicTokens(entry.content), currentTokens) >= 0.3f)
        {
            return true;
        }
    }

    return false;
}

} // anonymous namespace

bool MemoryRetrieveNode::Execute(const _tagAgentDagNode &,
                                 AgentContext &context,
                                 MemoryService *service,
                                 const MemoryConfig &config,
                                 QString &)
{
    if ((service == nullptr) || !service->IsRunning())
    {
        return true;
    }

    QVariant triggerValue;
    const QString triggerType = context.GetValue(AgentContextKeys::RUNTIME_TRIGGER_TYPE,
                                                  triggerValue)
                                    ? triggerValue.toString().trimmed()
                                    : QString();

    if (triggerType.isEmpty() || (triggerType == TRIGGER_TYPE_VISION_MEMORY))
    {
        context.RemoveValue(AgentContextKeys::SEMANTIC_MEMORY_ENTRIES);
        context.RemoveValue(AgentContextKeys::SEMANTIC_MEMORY_PROMPT);
        return true;
    }

    const QString petId = ResolvePetId(context);
    const QString basePrompt = ReadPrompt(context);

    if (basePrompt.isEmpty())
    {
        return true;
    }

    MemoryService::_tagRetrieveResult result;

    if (service->TakeLatestReadyResult(petId, triggerType, result)
        && result.ok
        && !result.entries.isEmpty()
        && IsResultRelevant(result.query, basePrompt, result.entries))
    {
        const QString memorySection = MemoryService::BuildPromptSection(result.entries,
                                                                        config.maxResults,
                                                                        config.promptBudgetChars);

        if (!memorySection.isEmpty())
        {
            const QString finalPrompt = memorySection
                                        + QStringLiteral("\n\n")
                                        + basePrompt;

            context.SetValue(AgentContextKeys::SEMANTIC_TEXT_PROMPT, finalPrompt);
            context.SetValue(AgentContextKeys::NODE_INPUT_PROMPT, finalPrompt);
            context.SetValue(AgentContextKeys::NODE_OUTPUT_PROMPT, finalPrompt);
            context.SetValue(AgentContextKeys::PROMPT_TEXT, finalPrompt);
            context.SetValue(AgentContextKeys::SEMANTIC_MEMORY_PROMPT, memorySection);

            QVariantList entriesList;

            for (const MemoryEntry &entry : result.entries)
            {
                QVariantMap entryMap;
                entryMap.insert(QStringLiteral("id"), entry.id);
                entryMap.insert(QStringLiteral("content"), entry.content);
                entryMap.insert(QStringLiteral("type"), static_cast<int>(entry.type));
                entryMap.insert(QStringLiteral("scope"), static_cast<int>(entry.scope));
                entryMap.insert(QStringLiteral("tags"), entry.tags);
                entriesList.append(entryMap);
            }

            context.SetValue(AgentContextKeys::SEMANTIC_MEMORY_ENTRIES, entriesList);

            qDebug() << "[Memory] Injected" << entriesList.size()
                     << "memories into the prompt for trigger:" << triggerType;
        }
    }
    else
    {
        context.RemoveValue(AgentContextKeys::SEMANTIC_MEMORY_ENTRIES);
        context.RemoveValue(AgentContextKeys::SEMANTIC_MEMORY_PROMPT);
    }

    quint64 requestId = 0;

    if (!service->TryEnqueueRetrieve(petId, triggerType, basePrompt, requestId))
    {
        qDebug() << "[Memory] Retrieve task rejected (queue full or service unavailable).";
    }

    return true;
}

QString MemoryRetrieveNode::ReadPrompt(const AgentContext &context)
{
    QVariant promptValue;

    if (context.GetValue(AgentContextKeys::NODE_INPUT_PROMPT, promptValue)
        || context.GetValue(AgentContextKeys::SEMANTIC_TEXT_PROMPT, promptValue)
        || context.GetValue(AgentContextKeys::PROMPT_TEXT, promptValue))
    {
        const QString prompt = promptValue.toString().trimmed();

        if (!prompt.isEmpty())
        {
            return prompt;
        }
    }

    return context.GetUserInput().trimmed();
}

QString MemoryRetrieveNode::ResolvePetId(const AgentContext &context)
{
    QVariant petIdValue;
    QString petId = context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                        ? petIdValue.toString().trimmed()
                        : QString();

    if (petId.isEmpty())
    {
        petId = DEFAULT_PET_ID_MEMORY;
    }

    return petId;
}

} // namespace vpet
