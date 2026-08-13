#include "vpet/memory/memory_consolidator.h"

#include "vpet/llm/llm_client.h"
#include "vpet/memory/memory_repository.h"
#include "vpet/memory/memory_service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace vpet
{

namespace
{

const QString CONSOLIDATION_TRIGGER_TYPE = QStringLiteral("user");
const QString TYPE_FACT = QStringLiteral("fact");
const QString TYPE_PREFERENCE = QStringLiteral("preference");
const QString TYPE_CORRECTION = QStringLiteral("correction");
const QString TYPE_NEGATIVE = QStringLiteral("negative");
const QString SCOPE_GLOBAL = QStringLiteral("global");
const QString SCOPE_PET = QStringLiteral("pet");
const QString RELATION_NONE = QStringLiteral("none");
const QString RELATION_SUPERSEDES = QStringLiteral("supersedes");
const QString RELATION_CONFLICTS = QStringLiteral("conflicts");
constexpr int MAX_TAG_COUNT = 8;
constexpr int MAX_TAG_CHARS = 48;

/**
 * @brief 构造只允许 JSON 的巩固系统提示词
 * @param[in] knownEntries 本轮已注入且可引用的记忆
 * @param[in] maxCandidates 最大候选数量
 * @return 系统提示词
 */
QString BuildSystemPrompt(const QVector<MemoryEntry> &knownEntries, int maxCandidates)
{
    QJsonArray knownMemoryArray;

    for (const MemoryEntry &entry : knownEntries)
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), entry.id);
        object.insert(QStringLiteral("content"), entry.content);
        knownMemoryArray.append(object);
    }

    const QString knownMemoryJson = QString::fromUtf8(
        QJsonDocument(knownMemoryArray).toJson(QJsonDocument::Compact));

    return QStringLiteral(
               "You extract only durable user memories from the current turn. "
               "Return exactly one JSON object and no Markdown. "
               "Use {\"candidates\":[]} when there is no durable memory. "
               "Each candidate must contain content, type, scope, tags, confidence, relation, "
               "and related_memory_id. Allowed type values are fact, preference, correction, negative. "
               "Allowed scope values are pet, global. Confidence is a number from 0.0 to 1.0. "
               "Allowed relation values are none, supersedes, conflicts. "
               "Use a non-none relation only when related_memory_id exactly matches a supplied known ID. "
               "Use supersedes only for a clearly confirmed replacement; use conflicts when incompatible "
               "but not confirmed. Never extract credentials, secrets, financial identifiers, government IDs, "
               "or temporary conversational details. Maximum candidates: %1. Known memories: %2")
        .arg(maxCandidates)
        .arg(knownMemoryJson);
}

/**
 * @brief 将候选类型字符串转换为枚举
 * @param[in] type 类型字符串
 * @param[out] result 输出枚举
 * @return 类型有效返回 true
 */
bool ParseType(const QString &type, MemoryEntry::Type &result)
{
    if (type == TYPE_FACT)
    {
        result = MemoryEntry::Type::Fact;
        return true;
    }

    if (type == TYPE_PREFERENCE)
    {
        result = MemoryEntry::Type::Preference;
        return true;
    }

    if (type == TYPE_CORRECTION)
    {
        result = MemoryEntry::Type::Correction;
        return true;
    }

    if (type == TYPE_NEGATIVE)
    {
        result = MemoryEntry::Type::Negative;
        return true;
    }

    return false;
}

/**
 * @brief 将关系字符串转换为枚举
 * @param[in] relation 关系字符串
 * @param[out] result 输出枚举
 * @return 关系有效返回 true
 */
bool ParseRelation(const QString &relation, MEMORY_CONSOLIDATION_RELATION &result)
{
    if (relation == RELATION_NONE)
    {
        result = MEMORY_CONSOLIDATION_RELATION::NONE;
        return true;
    }

    if (relation == RELATION_SUPERSEDES)
    {
        result = MEMORY_CONSOLIDATION_RELATION::SUPERSEDES;
        return true;
    }

    if (relation == RELATION_CONFLICTS)
    {
        result = MEMORY_CONSOLIDATION_RELATION::CONFLICTS;
        return true;
    }

    return false;
}

/**
 * @brief 获取数组字段的非空、去重标签
 * @param[in] object 候选 JSON 对象
 * @param[out] tags 输出标签
 * @return 标签字段满足 schema 返回 true
 */
bool ParseTags(const QJsonObject &object, QStringList &tags)
{
    const QJsonValue tagsValue = object.value(QStringLiteral("tags"));

    if (!tagsValue.isArray())
    {
        return false;
    }

    const QJsonArray tagArray = tagsValue.toArray();

    if (tagArray.size() > MAX_TAG_COUNT)
    {
        return false;
    }

    QSet<QString> seenTags;

    for (const QJsonValue &tagValue : tagArray)
    {
        if (!tagValue.isString())
        {
            return false;
        }

        const QString tag = tagValue.toString().trimmed();

        if (tag.isEmpty() || (tag.size() > MAX_TAG_CHARS) || seenTags.contains(tag))
        {
            return false;
        }

        seenTags.insert(tag);
        tags.append(tag);
    }

    return true;
}

} // anonymous namespace

bool MemoryConsolidator::SubmitTurn(LlmClient &client,
                                    MemoryService &service,
                                    const QString &userInput,
                                    const QString &assistantOutput,
                                    const QString &petId,
                                    const QVector<MemoryEntry> &knownEntries,
                                    int maxCandidates)
{
    const QString normalizedInput = userInput.trimmed();
    const QString normalizedOutput = assistantOutput.trimmed();
    const QString normalizedPetId = petId.trimmed();

    if (!service.IsRunning() || !client.IsConfigured() || normalizedInput.isEmpty()
        || normalizedOutput.isEmpty() || normalizedPetId.isEmpty() || (maxCandidates <= 0)
        || (maxCandidates > 8))
    {
        return false;
    }

    QString rejectCategory;

    if (!MemoryRepository::ValidateContent(normalizedInput, rejectCategory)
        || !MemoryRepository::ValidateContent(normalizedOutput, rejectCategory))
    {
        service.EmitLogMessage(QStringLiteral("Memory consolidation skipped by privacy filter: %1")
                                   .arg(rejectCategory));
        return false;
    }

    QVector<QString> knownEntryIds;
    knownEntryIds.reserve(knownEntries.size());

    for (const MemoryEntry &entry : knownEntries)
    {
        if (!entry.id.trimmed().isEmpty())
        {
            knownEntryIds.append(entry.id);
        }
    }

    _tagLlmMessage systemMessage;
    systemMessage.role = LLM_MESSAGE_ROLE::SYSTEM;
    systemMessage.content = BuildSystemPrompt(knownEntries, maxCandidates);

    _tagLlmMessage userMessage;
    userMessage.role = LLM_MESSAGE_ROLE::USER;
    userMessage.content = QStringLiteral("Current user input:\n%1\n\nCurrent assistant response:\n%2")
                              .arg(normalizedInput, normalizedOutput);

    _tagLlmRequestOptions options;
    options.temperature = 0.0;
    options.maxTokens = 600;
    const int requestId = client.SendChat({ systemMessage, userMessage }, options);

    if (requestId <= 0)
    {
        return false;
    }

    _tagPendingConsolidation pending;
    pending.service = &service;
    pending.petId = normalizedPetId;
    pending.triggerType = CONSOLIDATION_TRIGGER_TYPE;
    pending.knownEntryIds = knownEntryIds;
    pending.maxCandidates = maxCandidates;
    m_pendingRequests.insert(requestId, pending);
    return true;
}

bool MemoryConsolidator::HandleLlmCompleted(int requestId, const QString &response)
{
    const auto pendingIt = m_pendingRequests.find(requestId);

    if (pendingIt == m_pendingRequests.end())
    {
        return false;
    }

    const _tagPendingConsolidation pending = pendingIt.value();
    m_pendingRequests.erase(pendingIt);

    if ((pending.service == nullptr) || !pending.service->IsRunning())
    {
        return true;
    }

    QVector<_tagMemoryConsolidationCandidate> candidates;
    QString errorCategory;

    if (!ParseCandidates(response,
                         pending.petId,
                         pending.knownEntryIds,
                         pending.maxCandidates,
                         candidates,
                         errorCategory))
    {
        pending.service->EmitLogMessage(QStringLiteral("Memory consolidation response discarded: %1")
                                            .arg(errorCategory));
        return true;
    }

    if (candidates.isEmpty())
    {
        return true;
    }

    quint64 requestIdForService = 0;

    if (!pending.service->TryEnqueueConsolidation(pending.petId,
                                                  pending.triggerType,
                                                  candidates,
                                                  requestIdForService))
    {
        pending.service->EmitLogMessage(QStringLiteral("Memory consolidation queue rejected."));
    }

    return true;
}

bool MemoryConsolidator::HandleLlmFailed(int requestId)
{
    return m_pendingRequests.remove(requestId) > 0;
}

bool MemoryConsolidator::ParseCandidates(
    const QString &response,
    const QString &petId,
    const QVector<QString> &knownEntryIds,
    int maxCandidates,
    QVector<_tagMemoryConsolidationCandidate> &candidates,
    QString &errorCategory)
{
    candidates.clear();
    errorCategory.clear();

    if (response.trimmed().isEmpty() || petId.trimmed().isEmpty() || (maxCandidates <= 0)
        || (maxCandidates > 8))
    {
        errorCategory = QStringLiteral("invalid_input");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response.toUtf8(), &parseError);

    if ((parseError.error != QJsonParseError::NoError) || !document.isObject())
    {
        errorCategory = QStringLiteral("invalid_json");
        return false;
    }

    const QJsonObject root = document.object();

    if ((root.size() != 1) || !root.contains(QStringLiteral("candidates"))
        || !root.value(QStringLiteral("candidates")).isArray())
    {
        errorCategory = QStringLiteral("invalid_root_schema");
        return false;
    }

    const QJsonArray candidateArray = root.value(QStringLiteral("candidates")).toArray();

    if (candidateArray.size() > maxCandidates)
    {
        errorCategory = QStringLiteral("candidate_limit");
        return false;
    }

    QSet<QString> knownIds;

    for (const QString &knownEntryId : knownEntryIds)
    {
        knownIds.insert(knownEntryId);
    }
    QSet<QString> seenContents;

    for (const QJsonValue &candidateValue : candidateArray)
    {
        if (!candidateValue.isObject())
        {
            errorCategory = QStringLiteral("invalid_candidate_schema");
            return false;
        }

        const QJsonObject object = candidateValue.toObject();
        const QSet<QString> requiredKeys = { QStringLiteral("content"),
                                             QStringLiteral("type"),
                                             QStringLiteral("scope"),
                                             QStringLiteral("tags"),
                                             QStringLiteral("confidence"),
                                             QStringLiteral("relation"),
                                             QStringLiteral("related_memory_id") };

        if ((object.size() != requiredKeys.size()))
        {
            errorCategory = QStringLiteral("invalid_candidate_schema");
            return false;
        }

        for (const QString &key : requiredKeys)
        {
            if (!object.contains(key))
            {
                errorCategory = QStringLiteral("invalid_candidate_schema");
                return false;
            }
        }

        if (!object.value(QStringLiteral("content")).isString()
            || !object.value(QStringLiteral("type")).isString()
            || !object.value(QStringLiteral("scope")).isString()
            || !object.value(QStringLiteral("confidence")).isDouble()
            || !object.value(QStringLiteral("relation")).isString()
            || !object.value(QStringLiteral("related_memory_id")).isString())
        {
            errorCategory = QStringLiteral("invalid_candidate_types");
            return false;
        }

        _tagMemoryConsolidationCandidate candidate;
        candidate.entry.content = object.value(QStringLiteral("content")).toString().trimmed();
        const QString normalizedContent = candidate.entry.content.toLower();

        if (seenContents.contains(normalizedContent))
        {
            errorCategory = QStringLiteral("duplicate_candidate");
            return false;
        }

        QString rejectCategory;

        if (!MemoryRepository::ValidateEntry(candidate.entry, rejectCategory))
        {
            errorCategory = rejectCategory;
            return false;
        }

        if (!ParseType(object.value(QStringLiteral("type")).toString(), candidate.entry.type))
        {
            errorCategory = QStringLiteral("invalid_type");
            return false;
        }

        const QString scope = object.value(QStringLiteral("scope")).toString();

        if (scope == SCOPE_GLOBAL)
        {
            candidate.entry.scope = MemoryEntry::Scope::Global;
        }
        else if (scope == SCOPE_PET)
        {
            candidate.entry.scope = MemoryEntry::Scope::Pet;
        }
        else
        {
            errorCategory = QStringLiteral("invalid_scope");
            return false;
        }

        if (!ParseTags(object, candidate.entry.tags))
        {
            errorCategory = QStringLiteral("invalid_tags");
            return false;
        }

        const double confidence = object.value(QStringLiteral("confidence")).toDouble();

        if ((confidence < 0.0) || (confidence > 1.0))
        {
            errorCategory = QStringLiteral("invalid_confidence");
            return false;
        }

        candidate.entry.confidence = static_cast<float>(confidence);
        candidate.entry.trustScore = 0.7f;
        candidate.entry.provenance = MemoryEntry::Provenance::Extracted;
        candidate.entry.petId = (candidate.entry.scope == MemoryEntry::Scope::Global)
                                    ? QString()
                                    : petId.trimmed();
        candidate.relatedMemoryId = object.value(QStringLiteral("related_memory_id"))
                                      .toString()
                                      .trimmed();

        if (!ParseRelation(object.value(QStringLiteral("relation")).toString(), candidate.relation))
        {
            errorCategory = QStringLiteral("invalid_relation");
            return false;
        }

        if (candidate.relation == MEMORY_CONSOLIDATION_RELATION::NONE)
        {
            if (!candidate.relatedMemoryId.isEmpty())
            {
                errorCategory = QStringLiteral("unexpected_relation_target");
                return false;
            }
        }
        else if (candidate.relatedMemoryId.isEmpty() || !knownIds.contains(candidate.relatedMemoryId))
        {
            errorCategory = QStringLiteral("invalid_relation_target");
            return false;
        }

        seenContents.insert(normalizedContent);
        candidates.append(candidate);
    }

    return true;
}

} // namespace vpet
