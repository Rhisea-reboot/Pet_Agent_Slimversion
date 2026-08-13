#include "vpet/agent/memory_store_node.h"
#include "vpet/agent/agent_context_keys.h"

#include <QDebug>
#include <QRegularExpression>
#include <QStringList>

namespace vpet
{

namespace
{

const QString ACTION_REMEMBER = QStringLiteral("remember");
const QString ACTION_FORGET = QStringLiteral("forget");
const QString TYPE_FACT = QStringLiteral("fact");
const QString TYPE_PREFERENCE = QStringLiteral("preference");
const QString TYPE_PROCEDURE = QStringLiteral("procedure");
const QString TYPE_CORRECTION = QStringLiteral("correction");
const QString TYPE_NEGATIVE = QStringLiteral("negative");
const QString SCOPE_GLOBAL = QStringLiteral("global");
const QString SCOPE_PET = QStringLiteral("pet");
const QString TRIGGER_TYPE_USER_MEMORY = QStringLiteral("user");
const QString DEFAULT_PET_ID_MEMORY = QStringLiteral("default");

/**
 * @brief 去除命令前缀与常见引导符号
 */
QString StripPrefix(const QString &text, const QStringList &prefixes, bool &matched)
{
    const QString normalized = text.trimmed();

    for (const QString &prefix : prefixes)
    {
        if (normalized.startsWith(prefix))
        {
            matched = true;
            QString remainder = normalized.mid(prefix.size()).trimmed();
            remainder.remove(QRegularExpression(QStringLiteral("^[：:，,。.!！\\s]+")));
            remainder.remove(QRegularExpression(QStringLiteral("[。.!！]+$")));
            return remainder;
        }
    }

    matched = false;
    return normalized;
}

/**
 * @brief 判断文本是否为疑问句（不触发记忆命令）
 */
bool IsInterrogative(const QString &text)
{
    if (text.endsWith(QStringLiteral("?"))
        || text.endsWith(QStringLiteral("？"))
        || text.endsWith(QStringLiteral("吗"))
        || text.endsWith(QStringLiteral("呢"))
        || text.endsWith(QStringLiteral("么")))
    {
        return true;
    }

    return false;
}

/**
 * @brief 根据内容推断记忆类型
 */
QString InferType(const QString &content)
{
    if (content.contains(QStringLiteral("不要"))
        || content.contains(QStringLiteral("不喜欢"))
        || content.contains(QStringLiteral("讨厌"))
        || content.contains(QStringLiteral("别")))
    {
        return TYPE_NEGATIVE;
    }

    if (content.contains(QStringLiteral("喜欢"))
        || content.contains(QStringLiteral("希望"))
        || content.contains(QStringLiteral("想要"))
        || content.contains(QStringLiteral("愿意"))
        || content.contains(QStringLiteral("介意"))
        || content.contains(QStringLiteral("偏好")))
    {
        return TYPE_PREFERENCE;
    }

    return TYPE_FACT;
}

/**
 * @brief 从负面记忆正文提取上下文触发模式
 * @param[in] content 负面记忆正文
 * @return 去重后的触发模式
 */
QStringList ExtractNegativeTriggers(const QString &content)
{
    QStringList triggers;
    const QRegularExpression contextExpression(
        QStringLiteral("(?:在|当|如果|用户在)([^，,。；;]{1,24}?)(?:时|的时候|期间|中)"));
    const QRegularExpressionMatch match = contextExpression.match(content);

    if (match.hasMatch())
    {
        const QString trigger = match.captured(1).trimmed();

        if (!trigger.isEmpty())
        {
            triggers.append(trigger);
        }
    }

    if (triggers.isEmpty())
    {
        for (const QString &rawToken : content.split(
                 QRegularExpression(QStringLiteral("[，,。；;\\s]+")),
                 Qt::SkipEmptyParts))
        {
            const QString token = rawToken.trimmed();

            if ((token.size() >= 2) && !triggers.contains(token))
            {
                triggers.append(token);
            }
        }
    }

    return triggers;
}

/**
 * @brief 解析流程记忆的分号分隔字段
 * @param[in] text 流程正文
 * @param[out] fields 字段映射
 * @return 至少包含步骤字段返回 true
 */
bool ParseProcedureFields(const QString &text, QVariantMap &fields)
{
    for (const QString &rawField : text.split(QRegularExpression(QStringLiteral("[；;]")),
                                              Qt::SkipEmptyParts))
    {
        const int separatorIndex = rawField.indexOf(QRegularExpression(QStringLiteral("[:：=]")));

        if (separatorIndex <= 0)
        {
            continue;
        }

        const QString key = rawField.left(separatorIndex).trimmed();
        const QString value = rawField.mid(separatorIndex + 1).trimmed();

        if (!key.isEmpty() && !value.isEmpty())
        {
            fields.insert(key, value);
        }
    }

    return fields.contains(QStringLiteral("步骤")) || fields.contains(QStringLiteral("steps"));
}

/**
 * @brief 解析流程的有序列表字段
 * @param[in] value 列表文本
 * @return 非空条目列表
 */
QStringList SplitProcedureList(const QString &value)
{
    QStringList result;

    for (const QString &rawValue : value.split(QRegularExpression(QStringLiteral("[、,，|]")),
                                               Qt::SkipEmptyParts))
    {
        const QString normalizedValue = rawValue.trimmed();

        if (!normalizedValue.isEmpty())
        {
            result.append(normalizedValue);
        }
    }

    return result;
}

} // anonymous namespace

bool MemoryStoreNode::Execute(const _tagAgentDagNode &,
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

    if (triggerType != TRIGGER_TYPE_USER_MEMORY)
    {
        return true;
    }

    QVariant petIdValue;
    QString petId = context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                        ? petIdValue.toString().trimmed()
                        : QString();

    if (petId.isEmpty())
    {
        petId = DEFAULT_PET_ID_MEMORY;
    }

    QVariant intentValue;
    QVariantMap intent;

    if (context.GetValue(AgentContextKeys::MEMORY_STORE_INTENT, intentValue))
    {
        intent = intentValue.toMap();
    }

    if (intent.isEmpty())
    {
        intent = QVariantMap();

        if (!ParseCommand(context.GetUserInput().trimmed(), intent))
        {
            return true;
        }

        context.SetValue(AgentContextKeys::MEMORY_STORE_INTENT, intent);
    }

    const QString action = intent.value(QStringLiteral("action")).toString();

    if (action == ACTION_FORGET)
    {
        const QString query = intent.value(QStringLiteral("content")).toString().trimmed();
        quint64 requestId = 0;

        if (!query.isEmpty())
        {
            service->TryEnqueueForgetByKeyword(petId, triggerType, query, requestId);
        }
    }
    else if (action == ACTION_REMEMBER)
    {
        MemoryEntry entry;
        QString entryError;

        if (BuildEntry(intent, petId, config.defaultScope, entry, entryError))
        {
            QString rejectCategory;
            quint64 requestId = 0;

            if (!service->TryEnqueueStore(petId,
                                          triggerType,
                                          entry,
                                          requestId,
                                          rejectCategory))
            {
                qDebug() << "[Memory] Store rejected by privacy filter:" << rejectCategory;
            }
        }
        else
        {
            qDebug() << "[Memory] Store entry build failed:" << entryError;
        }
    }

    return true;
}

bool MemoryStoreNode::ParseCommand(const QString &userInput, QVariantMap &intent)
{
    const QString normalizedInput = userInput.trimmed();

    if (normalizedInput.isEmpty() || IsInterrogative(normalizedInput))
    {
        return false;
    }

    bool matched = false;
    QString content;

    content = StripPrefix(normalizedInput,
                          { QStringLiteral("请记住流程"),
                            QStringLiteral("记住流程") },
                          matched);

    if (matched)
    {
        QVariantMap procedureFields;

        if (!ParseProcedureFields(content, procedureFields))
        {
            return false;
        }

        intent.insert(QStringLiteral("action"), ACTION_REMEMBER);
        intent.insert(QStringLiteral("content"), content);
        intent.insert(QStringLiteral("type"), TYPE_PROCEDURE);
        intent.insert(QStringLiteral("procedure"), procedureFields);
        intent.insert(QStringLiteral("scope"), QString());
        return true;
    }

    content = StripPrefix(normalizedInput,
                          { QStringLiteral("请记住"),
                            QStringLiteral("帮我记住"),
                            QStringLiteral("以后记得"),
                            QStringLiteral("记住一下"),
                            QStringLiteral("记住"),
                            QStringLiteral("以后请") },
                          matched);

    if (matched)
    {
        content = content.trimmed();

        if (content.isEmpty())
        {
            return false;
        }

        intent.insert(QStringLiteral("action"), ACTION_REMEMBER);
        intent.insert(QStringLiteral("content"), content);
        intent.insert(QStringLiteral("type"), InferType(content));
        intent.insert(QStringLiteral("scope"),
                      content.startsWith(QStringLiteral("全局"))
                          ? SCOPE_GLOBAL
                          : QString());
        return true;
    }

    content = StripPrefix(normalizedInput,
                          { QStringLiteral("以后不要再"),
                            QStringLiteral("以后不要"),
                            QStringLiteral("以后别再"),
                            QStringLiteral("不要再"),
                            QStringLiteral("以后不再"),
                            QStringLiteral("别再") },
                          matched);

    if (matched)
    {
        content = content.trimmed();

        if (content.isEmpty())
        {
            return false;
        }

        intent.insert(QStringLiteral("action"), ACTION_REMEMBER);
        intent.insert(QStringLiteral("content"), content);
        intent.insert(QStringLiteral("type"), TYPE_NEGATIVE);
        intent.insert(QStringLiteral("scope"), QString());
        return true;
    }

    content = StripPrefix(normalizedInput,
                          { QStringLiteral("更正一下"),
                            QStringLiteral("纠正一下"),
                            QStringLiteral("更正"),
                            QStringLiteral("纠正") },
                          matched);

    if (matched)
    {
        content = content.trimmed();

        if (content.isEmpty())
        {
            return false;
        }

        intent.insert(QStringLiteral("action"), ACTION_REMEMBER);
        intent.insert(QStringLiteral("content"), content);
        intent.insert(QStringLiteral("type"), TYPE_CORRECTION);
        intent.insert(QStringLiteral("scope"), QString());
        return true;
    }

    content = StripPrefix(normalizedInput,
                          { QStringLiteral("删除记忆"),
                            QStringLiteral("删掉记忆"),
                            QStringLiteral("不记得了"),
                            QStringLiteral("忘记"),
                            QStringLiteral("忘了"),
                            QStringLiteral("忘掉") },
                          matched);

    if (matched)
    {
        content = content.trimmed();

        if (content.isEmpty())
        {
            return false;
        }

        intent.insert(QStringLiteral("action"), ACTION_FORGET);
        intent.insert(QStringLiteral("content"), content);
        return true;
    }

    return false;
}

bool MemoryStoreNode::BuildEntry(const QVariantMap &intent,
                                 const QString &petId,
                                 const QString &defaultScope,
                                 MemoryEntry &entry,
                                 QString &errorMessage)
{
    if (intent.value(QStringLiteral("action")).toString() != ACTION_REMEMBER)
    {
        errorMessage = QStringLiteral("Memory store intent action is not remember.");
        return false;
    }

    const QString content = intent.value(QStringLiteral("content")).toString().trimmed();

    if (content.isEmpty())
    {
        errorMessage = QStringLiteral("Memory store intent content is empty.");
        return false;
    }

    entry = MemoryEntry();
    entry.content = content;
    entry.petId = petId;

    const QString type = intent.value(QStringLiteral("type")).toString();

    if (type == TYPE_PREFERENCE)
    {
        entry.type = MemoryEntry::Type::Preference;
    }
    else if (type == TYPE_CORRECTION)
    {
        entry.type = MemoryEntry::Type::Correction;
        entry.provenance = MemoryEntry::Provenance::UserCorrected;
    }
    else if (type == TYPE_PROCEDURE)
    {
        entry.type = MemoryEntry::Type::Procedure;
    }
    else if (type == TYPE_NEGATIVE)
    {
        entry.type = MemoryEntry::Type::Negative;
    }
    else
    {
        entry.type = MemoryEntry::Type::Fact;
    }

    const QString scope = intent.value(QStringLiteral("scope")).toString();
    const bool isGlobal = (scope == SCOPE_GLOBAL)
                          || (scope.isEmpty() && (defaultScope == SCOPE_GLOBAL));
    entry.scope = isGlobal ? MemoryEntry::Scope::Global : MemoryEntry::Scope::Pet;

    entry.category = intent.value(QStringLiteral("category")).toString();

    const QVariantList triggerValues = intent.value(QStringLiteral("trigger_patterns")).toList();

    for (const QVariant &triggerValue : triggerValues)
    {
        const QString trigger = triggerValue.toString().trimmed();

        if (!trigger.isEmpty() && !entry.triggerPatterns.contains(trigger))
        {
            entry.triggerPatterns.append(trigger);
        }
    }

    if (entry.type == MemoryEntry::Type::Negative && entry.triggerPatterns.isEmpty())
    {
        entry.triggerPatterns = ExtractNegativeTriggers(content);
    }

    if (entry.type == MemoryEntry::Type::Procedure)
    {
        const QVariantMap procedure = intent.value(QStringLiteral("procedure")).toMap();
        entry.procedure.name = procedure.value(QStringLiteral("name")).toString().trimmed();
        entry.procedure.trigger = procedure.value(QStringLiteral("trigger")).toString().trimmed();
        entry.procedure.steps = procedure.value(QStringLiteral("steps")).toStringList();
        entry.procedure.prerequisites = procedure.value(QStringLiteral("prerequisites"))
                                            .toStringList();
        entry.procedure.warnings = procedure.value(QStringLiteral("warnings")).toStringList();

        if (entry.procedure.steps.isEmpty())
        {
            entry.procedure.steps = SplitProcedureList(
                procedure.value(QStringLiteral("步骤")).toString());
        }

        if (entry.procedure.prerequisites.isEmpty())
        {
            entry.procedure.prerequisites = SplitProcedureList(
                procedure.value(QStringLiteral("前置")).toString());
        }

        if (entry.procedure.warnings.isEmpty())
        {
            entry.procedure.warnings = SplitProcedureList(
                procedure.value(QStringLiteral("警告")).toString());
        }

        if (entry.procedure.name.isEmpty())
        {
            entry.procedure.name = procedure.value(QStringLiteral("名称")).toString().trimmed();
        }

        if (entry.procedure.trigger.isEmpty())
        {
            entry.procedure.trigger = procedure.value(QStringLiteral("触发")).toString().trimmed();
        }

        if (entry.procedure.steps.isEmpty())
        {
            errorMessage = QStringLiteral("Procedure memory requires at least one step.");
            return false;
        }

        if (!entry.procedure.trigger.isEmpty() && !entry.triggerPatterns.contains(entry.procedure.trigger))
        {
            entry.triggerPatterns.append(entry.procedure.trigger);
        }
    }

    return true;
}

} // namespace vpet
