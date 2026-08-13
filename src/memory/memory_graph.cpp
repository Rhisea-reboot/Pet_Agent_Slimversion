#include "vpet/memory/memory_graph.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QDebug>
#include <QSet>

#include <algorithm>

namespace vpet
{

namespace
{

constexpr int MAX_TRIGGER_PATTERN_CHARS = 128;

/**
 * @brief 判断条目是否对指定作用域可见
 * @param[in] entry 记忆条目
 * @param[in] scope 查询作用域
 * @param[in] petId 宠物 ID
 * @return 可见返回 true
 */
bool IsEntryVisible(const MemoryEntry &entry,
                    MemoryEntry::Scope scope,
                    const QString &petId)
{
    if (!entry.active)
    {
        return false;
    }

    if (entry.scope == MemoryEntry::Scope::Global)
    {
        return true;
    }

    return (scope == MemoryEntry::Scope::Pet) && (entry.petId == petId);
}

/**
 * @brief 判断单个受限触发模式是否匹配上下文
 * @param[in] pattern 原始模式；以 re: 开头时按正则处理
 * @param[in] contextText 已标准化上下文
 * @return 匹配返回 true
 */
bool TriggerPatternMatches(const QString &pattern, const QString &contextText)
{
    const QString normalizedPattern = pattern.trimmed();

    if (normalizedPattern.isEmpty() || (normalizedPattern.size() > MAX_TRIGGER_PATTERN_CHARS))
    {
        return false;
    }

    if (!normalizedPattern.startsWith(QStringLiteral("re:"), Qt::CaseInsensitive))
    {
        return contextText.contains(normalizedPattern, Qt::CaseInsensitive);
    }

    if (normalizedPattern.mid(3).trimmed().isEmpty())
    {
        return false;
    }

    const QRegularExpression expression(normalizedPattern.mid(3),
                                        QRegularExpression::CaseInsensitiveOption
                                            | QRegularExpression::UseUnicodePropertiesOption);
    return expression.isValid() && expression.match(contextText).hasMatch();
}

/**
 * @brief 校验记忆类型特有的结构字段
 * @param[in] entry 记忆条目
 * @param[out] errorMessage 错误描述
 * @return 结构有效返回 true
 */
bool ValidateEntryShape(const MemoryEntry &entry, QString &errorMessage)
{
    for (const QString &triggerPattern : entry.triggerPatterns)
    {
        const QString normalizedPattern = triggerPattern.trimmed();

        if (normalizedPattern.isEmpty() || (normalizedPattern.size() > MAX_TRIGGER_PATTERN_CHARS))
        {
            errorMessage = QStringLiteral("Memory trigger pattern is invalid.");
            return false;
        }

        if (normalizedPattern.startsWith(QStringLiteral("re:"), Qt::CaseInsensitive))
        {
            if (normalizedPattern.mid(3).trimmed().isEmpty())
            {
                errorMessage = QStringLiteral("Memory trigger regular expression is empty.");
                return false;
            }

            const QRegularExpression expression(
                normalizedPattern.mid(3),
                QRegularExpression::CaseInsensitiveOption
                    | QRegularExpression::UseUnicodePropertiesOption);

            if (!expression.isValid())
            {
                errorMessage = QStringLiteral("Memory trigger regular expression is invalid.");
                return false;
            }
        }
    }

    if (entry.type != MemoryEntry::Type::Procedure)
    {
        return true;
    }

    if (entry.procedure.steps.isEmpty())
    {
        errorMessage = QStringLiteral("Procedure memory requires at least one step.");
        return false;
    }

    for (const QString &step : entry.procedure.steps)
    {
        if (step.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Procedure memory contains an empty step.");
            return false;
        }
    }

    return true;
}

/**
 * @brief 将关键词与查询文本规范化为小写
 * @param[in] text 原始文本
 * @return 小写文本
 */
QString NormalizeText(const QString &text)
{
    return text.trimmed().toLower();
}

/**
 * @brief 将边类型转换为字符串（供复合键使用）
 */
QString EdgeTypeToString(MemoryEdge::Type type)
{
    switch (type)
    {
        case MemoryEdge::Type::Explicit:
            return QStringLiteral("explicit");
        case MemoryEdge::Type::Related:
            return QStringLiteral("related");
        case MemoryEdge::Type::TagShared:
            return QStringLiteral("tag_shared");
        case MemoryEdge::Type::Supersedes:
            return QStringLiteral("supersedes");
        case MemoryEdge::Type::Conflict:
            return QStringLiteral("conflict");
    }

    return QStringLiteral("explicit");
}

/**
 * @brief 条目是否通过作用域过滤
 * @param[in] entry 条目
 * @param[in] scope 作用域
 * @param[in] petId 宠物 ID
 * @return 通过过滤返回 true
 */
bool PassesScopeFilter(const MemoryEntry &entry,
                       MemoryEntry::Scope scope,
                       const QString &petId)
{
    if (scope == MemoryEntry::Scope::Global)
    {
        return entry.scope == MemoryEntry::Scope::Global;
    }

    return (entry.scope == MemoryEntry::Scope::Global) || (entry.petId == petId);
}

/**
 * @brief 按访问热度与最近访问排序的比较器（稳定排序用）
 */
bool HotterEntryFirst(const MemoryEntry &lhs, const MemoryEntry &rhs)
{
    if (lhs.accessCount != rhs.accessCount)
    {
        return lhs.accessCount > rhs.accessCount;
    }

    if (lhs.lastAccessed != rhs.lastAccessed)
    {
        return lhs.lastAccessed > rhs.lastAccessed;
    }

    return lhs.id < rhs.id;
}

} // anonymous namespace

bool MemoryGraph::AddEntry(const MemoryEntry &entry, QString &errorMessage)
{
    if (entry.id.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Memory entry id is empty.");
        return false;
    }

    if (entry.content.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Memory entry content is empty.");
        return false;
    }

    if (m_entries.contains(entry.id))
    {
        errorMessage = QStringLiteral("Memory entry id already exists: %1").arg(entry.id);
        return false;
    }

    if (!ValidateEntryShape(entry, errorMessage))
    {
        return false;
    }

    MemoryEntry storedEntry = entry;
    storedEntry.content = entry.content.trimmed();
    storedEntry.petId = entry.petId.trimmed();

    if (storedEntry.scope == MemoryEntry::Scope::Global)
    {
        storedEntry.petId.clear();
    }

    storedEntry.tags = entry.tags;
    storedEntry.tags.removeAll(QString());

    m_entries.insert(storedEntry.id, storedEntry);
    m_insertionOrder.append(storedEntry.id);
    RebuildIndexes(storedEntry);
    SyncTagEdges(storedEntry);

    return true;
}

bool MemoryGraph::GetEntry(const QString &id, MemoryEntry &entry) const
{
    const auto it = m_entries.constFind(id);

    if (it == m_entries.constEnd())
    {
        return false;
    }

    entry = it.value();
    return true;
}

bool MemoryGraph::SoftDeleteEntry(const QString &id)
{
    auto it = m_entries.find(id);

    if (it == m_entries.constEnd())
    {
        return false;
    }

    if (!it.value().active)
    {
        return true;
    }

    it.value().active = false;
    it.value().updatedAt = QDateTime::currentMSecsSinceEpoch();
    RemoveIndexes(it.value());
    RemoveTagEdgesOfEntry(it.value().id);
    return true;
}

bool MemoryGraph::UpdateEntry(const MemoryEntry &entry, QString &errorMessage)
{
    auto it = m_entries.find(entry.id);

    if (it == m_entries.constEnd())
    {
        errorMessage = QStringLiteral("Memory entry not found: %1").arg(entry.id);
        return false;
    }

    if (!ValidateEntryShape(entry, errorMessage))
    {
        return false;
    }

    RemoveIndexes(it.value());
    RemoveTagEdgesOfEntry(entry.id);

    MemoryEntry updatedEntry = entry;
    updatedEntry.content = entry.content.trimmed();
    updatedEntry.petId = entry.petId.trimmed();

    if (updatedEntry.scope == MemoryEntry::Scope::Global)
    {
        updatedEntry.petId.clear();
    }

    updatedEntry.tags = entry.tags;
    updatedEntry.tags.removeAll(QString());

    it.value() = updatedEntry;
    RebuildIndexes(updatedEntry);
    SyncTagEdges(updatedEntry);

    return true;
}

bool MemoryGraph::AddTag(const QString &id, const QString &tag, QString &errorMessage)
{
    auto it = m_entries.find(id);

    if (it == m_entries.constEnd())
    {
        errorMessage = QStringLiteral("Memory entry not found: %1").arg(id);
        return false;
    }

    const QString normalizedTag = tag.trimmed();

    if (normalizedTag.isEmpty())
    {
        errorMessage = QStringLiteral("Memory tag is empty.");
        return false;
    }

    if (it.value().tags.contains(normalizedTag))
    {
        return true;
    }

    RemoveIndexes(it.value());
    it.value().tags.append(normalizedTag);
    RebuildIndexes(it.value());
    it.value().updatedAt = QDateTime::currentMSecsSinceEpoch();
    SyncTagEdges(it.value());

    return true;
}

bool MemoryGraph::RemoveTag(const QString &id, const QString &tag, QString &errorMessage)
{
    auto it = m_entries.find(id);

    if (it == m_entries.constEnd())
    {
        errorMessage = QStringLiteral("Memory entry not found: %1").arg(id);
        return false;
    }

    const QString normalizedTag = tag.trimmed();

    if (!it.value().tags.contains(normalizedTag))
    {
        return true;
    }

    RemoveIndexes(it.value());
    it.value().tags.removeOne(normalizedTag);
    RebuildIndexes(it.value());
    it.value().updatedAt = QDateTime::currentMSecsSinceEpoch();
    RemoveTagEdgesOfEntry(it.value().id, normalizedTag);
    SyncTagEdges(it.value());

    return true;
}

QVector<MemoryEntry> MemoryGraph::ListEntries(MemoryEntry::Scope scope,
                                              const QString &petId) const
{
    QVector<MemoryEntry> result;

    for (const QString &id : m_insertionOrder)
    {
        const MemoryEntry &entry = m_entries.value(id);

        if (!entry.active)
        {
            continue;
        }

        if (scope == MemoryEntry::Scope::Global)
        {
            if (entry.scope != MemoryEntry::Scope::Global)
            {
                continue;
            }
        }
        else if ((entry.scope != MemoryEntry::Scope::Global) && (entry.petId != petId))
        {
            continue;
        }

        result.append(entry);
    }

    return result;
}

QVector<MemoryEntry> MemoryGraph::SearchByTags(const QStringList &tags,
                                               MemoryEntry::Scope scope,
                                               const QString &petId,
                                               int maxResults) const
{
    if (tags.isEmpty())
    {
        return {};
    }

    QSet<QString> matchedIds;

    for (const QString &tag : tags)
    {
        const QString normalizedTag = NormalizeText(tag);

        if (normalizedTag.isEmpty())
        {
            continue;
        }

        const auto it = m_tagIndex.constFind(normalizedTag);

        if (it != m_tagIndex.constEnd())
        {
            matchedIds.unite(it.value());
        }
    }

    QVector<MemoryEntry> result;

    for (const QString &id : m_insertionOrder)
    {
        const MemoryEntry &entry = m_entries.value(id);

        if (!matchedIds.contains(id) || !entry.active)
        {
            continue;
        }

        if (scope == MemoryEntry::Scope::Global)
        {
            if (entry.scope != MemoryEntry::Scope::Global)
            {
                continue;
            }
        }
        else if ((entry.scope != MemoryEntry::Scope::Global) && (entry.petId != petId))
        {
            continue;
        }

        result.append(entry);
    }

    std::stable_sort(result.begin(), result.end(), [](const MemoryEntry &lhs, const MemoryEntry &rhs)
    {
        if (lhs.accessCount != rhs.accessCount)
        {
            return lhs.accessCount > rhs.accessCount;
        }

        if (lhs.lastAccessed != rhs.lastAccessed)
        {
            return lhs.lastAccessed > rhs.lastAccessed;
        }

        return lhs.id < rhs.id;
    });

    if (result.size() > maxResults)
    {
        result.resize(maxResults);
    }

    return result;
}

QVector<MemoryEntry> MemoryGraph::SearchByKeywords(const QString &query,
                                                   MemoryEntry::Scope scope,
                                                   const QString &petId,
                                                   int maxResults) const
{
    const QString normalizedQuery = NormalizeText(query);

    if (normalizedQuery.isEmpty())
    {
        return {};
    }

    const QStringList queryKeywords = ExtractKeywords(normalizedQuery);
    const bool hasQueryKeywords = !queryKeywords.isEmpty();

    if (!hasQueryKeywords && normalizedQuery.isEmpty())
    {
        return {};
    }

    struct _tagScoredEntry
    {
        MemoryEntry entry;
        int score = 0;
    };

    QVector<_tagScoredEntry> scored;

    for (const QString &id : m_insertionOrder)
    {
        const MemoryEntry &entry = m_entries.value(id);

        if (!entry.active)
        {
            continue;
        }

        if (scope == MemoryEntry::Scope::Global)
        {
            if (entry.scope != MemoryEntry::Scope::Global)
            {
                continue;
            }
        }
        else if ((entry.scope != MemoryEntry::Scope::Global) && (entry.petId != petId))
        {
            continue;
        }

        const int score = hasQueryKeywords
                              ? ScoreEntry(entry, queryKeywords, normalizedQuery)
                              : (NormalizeText(entry.content).contains(normalizedQuery) ? 1 : 0);

        if (score <= 0)
        {
            continue;
        }

        _tagScoredEntry scoredEntry;
        scoredEntry.entry = entry;
        scoredEntry.score = score;
        scored.append(scoredEntry);
    }

    std::stable_sort(scored.begin(), scored.end(),
                     [](const _tagScoredEntry &lhs, const _tagScoredEntry &rhs)
    {
        if (lhs.score != rhs.score)
        {
            return lhs.score > rhs.score;
        }

        if (lhs.entry.accessCount != rhs.entry.accessCount)
        {
            return lhs.entry.accessCount > rhs.entry.accessCount;
        }

        if (lhs.entry.lastAccessed != rhs.entry.lastAccessed)
        {
            return lhs.entry.lastAccessed > rhs.entry.lastAccessed;
        }

        return lhs.entry.id < rhs.entry.id;
    });

    QVector<MemoryEntry> result;

    for (const _tagScoredEntry &scoredEntry : scored)
    {
        result.append(scoredEntry.entry);

        if (result.size() >= maxResults)
        {
            break;
        }
    }

    return result;
}

QVector<MemoryEntry> MemoryGraph::MatchTriggeredEntries(const QString &contextText,
                                                         MemoryEntry::Scope scope,
                                                         const QString &petId,
                                                         int maxResults) const
{
    QVector<MemoryEntry> results;
    const QString normalizedContext = contextText.trimmed();

    if (normalizedContext.isEmpty() || (maxResults <= 0))
    {
        return results;
    }

    QHash<QString, int> matchCounts;

    for (const QString &entryId : m_insertionOrder)
    {
        const auto entryIt = m_entries.constFind(entryId);

        if (entryIt == m_entries.constEnd()
            || !IsEntryVisible(entryIt.value(), scope, petId)
            || ((entryIt.value().type != MemoryEntry::Type::Negative)
                && (entryIt.value().type != MemoryEntry::Type::Procedure)))
        {
            continue;
        }

        int matchCount = 0;

        for (const QString &pattern : entryIt.value().triggerPatterns)
        {
            if (TriggerPatternMatches(pattern, normalizedContext))
            {
                matchCount += 1;
            }
        }

        if ((matchCount == 0)
            && (entryIt.value().type == MemoryEntry::Type::Procedure)
            && TriggerPatternMatches(entryIt.value().procedure.trigger, normalizedContext))
        {
            matchCount = 1;
        }

        if (matchCount > 0)
        {
            results.append(entryIt.value());
            matchCounts.insert(entryIt.value().id, matchCount);
        }
    }

    std::stable_sort(results.begin(), results.end(),
                     [&matchCounts](const MemoryEntry &left, const MemoryEntry &right)
    {
        if (left.type != right.type)
        {
            return left.type == MemoryEntry::Type::Negative;
        }

        if (matchCounts.value(left.id) != matchCounts.value(right.id))
        {
            return matchCounts.value(left.id) > matchCounts.value(right.id);
        }

        if (left.updatedAt != right.updatedAt)
        {
            return left.updatedAt > right.updatedAt;
        }

        return left.id < right.id;
    });

    if (results.size() > maxResults)
    {
        results.resize(maxResults);
    }

    return results;
}

int MemoryGraph::ActiveEntryCount() const
{
    int count = 0;

    for (const MemoryEntry &entry : m_entries)
    {
        if (entry.active)
        {
            count += 1;
        }
    }

    return count;
}

int MemoryGraph::EntryCount() const
{
    return m_entries.size();
}

QStringList MemoryGraph::AllTags() const
{
    struct _tagTagCount
    {
        QString name;
        int count = 0;
    };

    QVector<_tagTagCount> tagCounts;

    for (auto it = m_tagIndex.constBegin(); it != m_tagIndex.constEnd(); ++it)
    {
        _tagTagCount tagCount;
        tagCount.name = it.key();
        tagCount.count = it.value().size();
        tagCounts.append(tagCount);
    }

    std::stable_sort(tagCounts.begin(), tagCounts.end(),
                     [](const _tagTagCount &lhs, const _tagTagCount &rhs)
    {
        if (lhs.count != rhs.count)
        {
            return lhs.count > rhs.count;
        }

        return lhs.name < rhs.name;
    });

    QStringList result;

    for (const _tagTagCount &tagCount : tagCounts)
    {
        result.append(tagCount.name);
    }

    return result;
}

QVector<MemoryEntry> MemoryGraph::AllEntries() const
{
    QVector<MemoryEntry> result;

    for (const QString &id : m_insertionOrder)
    {
        result.append(m_entries.value(id));
    }

    return result;
}

bool MemoryGraph::LoadEntries(const QVector<MemoryEntry> &entries, QString &errorMessage)
{
    Clear();

    for (const MemoryEntry &entry : entries)
    {
        if (entry.id.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Memory entry id is empty.");
            return false;
        }

        if (!ValidateEntryShape(entry, errorMessage))
        {
            Clear();
            return false;
        }

        if (m_entries.contains(entry.id))
        {
            errorMessage = QStringLiteral("Memory entry id is duplicated: %1").arg(entry.id);
            Clear();
            return false;
        }

        MemoryEntry storedEntry = entry;
        storedEntry.content = entry.content.trimmed();
        storedEntry.petId = entry.petId.trimmed();

        if (storedEntry.scope == MemoryEntry::Scope::Global)
        {
            storedEntry.petId.clear();
        }
        storedEntry.tags = entry.tags;
        storedEntry.tags.removeAll(QString());

        m_entries.insert(storedEntry.id, storedEntry);
        m_insertionOrder.append(storedEntry.id);
        RebuildIndexes(storedEntry);
        SyncTagEdges(storedEntry);
    }

    return true;
}

void MemoryGraph::Clear()
{
    m_entries.clear();
    m_insertionOrder.clear();
    m_tagIndex.clear();
    m_keywordIndex.clear();
    m_edges.clear();
    m_adjacency.clear();
}

bool MemoryGraph::AddEdge(const QString &sourceId,
                          const QString &targetId,
                          MemoryEdge::Type type,
                          const QString &tag,
                          float weight,
                          QString &errorMessage)
{
    if (sourceId.trimmed().isEmpty() || targetId.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Memory edge endpoint id is empty.");
        return false;
    }

    if (sourceId == targetId)
    {
        errorMessage = QStringLiteral("Memory edge endpoints must differ.");
        return false;
    }

    if (!m_entries.contains(sourceId) || !m_entries.contains(targetId))
    {
        errorMessage = QStringLiteral("Memory edge endpoint does not exist.");
        return false;
    }

    if (!m_entries.value(sourceId).active || !m_entries.value(targetId).active)
    {
        errorMessage = QStringLiteral("Memory edge endpoint is inactive.");
        return false;
    }

    if ((type == MemoryEdge::Type::TagShared) && tag.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Memory edge tag is empty for TagShared edge.");
        return false;
    }

    if (weight <= 0.0f)
    {
        errorMessage = QStringLiteral("Memory edge weight must be positive.");
        return false;
    }

    MemoryEdge edge;
    edge.id = MakeEdgeKey(sourceId, targetId, type, tag);
    edge.sourceId = sourceId;
    edge.targetId = targetId;
    edge.type = type;
    edge.tag = tag;
    edge.weight = weight;
    edge.createdAt = QDateTime::currentMSecsSinceEpoch();

    if (m_edges.contains(edge.id))
    {
        return true;
    }

    m_edges.insert(edge.id, edge);
    IndexEdge(edge);
    return true;
}

bool MemoryGraph::RemoveEdge(const QString &edgeId, QString &errorMessage)
{
    const auto it = m_edges.find(edgeId);

    if (it == m_edges.end())
    {
        errorMessage = QStringLiteral("Memory edge not found: %1").arg(edgeId);
        return false;
    }

    const MemoryEdge edge = it.value();
    m_edges.erase(it);
    UnindexEdge(edge);
    return true;
}

bool MemoryGraph::SetEdgeWeight(const QString &edgeId,
                                float weight,
                                QString &errorMessage)
{
    if (weight <= 0.0f)
    {
        errorMessage = QStringLiteral("Memory edge weight must be positive.");
        return false;
    }

    auto it = m_edges.find(edgeId);

    if (it == m_edges.end())
    {
        errorMessage = QStringLiteral("Memory edge not found: %1").arg(edgeId);
        return false;
    }

    it.value().weight = weight;
    return true;
}

QVector<MemoryEdge> MemoryGraph::AllEdges() const
{
    QVector<MemoryEdge> result;

    for (auto it = m_edges.constBegin(); it != m_edges.constEnd(); ++it)
    {
        result.append(it.value());
    }

    std::stable_sort(result.begin(), result.end(),
                     [](const MemoryEdge &lhs, const MemoryEdge &rhs)
    {
        return lhs.id < rhs.id;
    });

    return result;
}

bool MemoryGraph::LoadEdges(const QVector<MemoryEdge> &edges, QString &errorMessage)
{
    for (const MemoryEdge &edge : edges)
    {
        if (edge.id.trimmed().isEmpty()
            || edge.sourceId.trimmed().isEmpty()
            || edge.targetId.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Memory edge id or endpoint is empty.");
            return false;
        }

        if (m_edges.contains(edge.id))
        {
            errorMessage = QStringLiteral("Memory edge id is duplicated: %1").arg(edge.id);
            return false;
        }

        if (!m_entries.contains(edge.sourceId) || !m_entries.contains(edge.targetId))
        {
            errorMessage = QStringLiteral("Memory edge endpoint is missing: %1").arg(edge.id);
            return false;
        }

        MemoryEdge storedEdge = edge;
        storedEdge.sourceId = edge.sourceId.trimmed();
        storedEdge.targetId = edge.targetId.trimmed();
        storedEdge.tag = edge.tag.trimmed();

        if (storedEdge.weight <= 0.0f)
        {
            errorMessage = QStringLiteral("Memory edge weight is invalid: %1").arg(storedEdge.id);
            return false;
        }

        if ((storedEdge.type == MemoryEdge::Type::TagShared) && storedEdge.tag.isEmpty())
        {
            errorMessage = QStringLiteral("Memory TagShared edge has no tag: %1").arg(storedEdge.id);
            return false;
        }

        const QString expectedId = MakeEdgeKey(storedEdge.sourceId,
                                               storedEdge.targetId,
                                               storedEdge.type,
                                               storedEdge.tag);

        if (storedEdge.id != expectedId)
        {
            errorMessage = QStringLiteral("Memory edge id does not match its fields: %1")
                               .arg(storedEdge.id);
            return false;
        }

        m_edges.insert(storedEdge.id, storedEdge);
        IndexEdge(storedEdge);
    }

    errorMessage.clear();
    return true;
}

QVector<MemoryEntry> MemoryGraph::ActiveEntriesByIds(const QSet<QString> &ids,
                                                     MemoryEntry::Scope scope,
                                                     const QString &petId) const
{
    QVector<MemoryEntry> result;

    for (const QString &id : m_insertionOrder)
    {
        if (!ids.contains(id))
        {
            continue;
        }

        const MemoryEntry &entry = m_entries.value(id);

        if (!entry.active)
        {
            continue;
        }

        if (!PassesScopeFilter(entry, scope, petId))
        {
            continue;
        }

        result.append(entry);
    }

    return result;
}

QVector<_tagGraphHit> MemoryGraph::ExpandByEdges(const QHash<QString, float> &seedScores,
                                                 MemoryEntry::Scope scope,
                                                 const QString &petId,
                                                 int maxDepth,
                                                 float decayPerHop) const
{
    QVector<_tagGraphHit> hits;

    if (seedScores.isEmpty() || (maxDepth <= 0) || (decayPerHop <= 0.0f))
    {
        return hits;
    }

    constexpr int MAX_EXPANDED_ENTRIES = 256;

    QHash<QString, float> bestScores;
    QSet<QString> visited;
    QHash<QString, float> frontier = seedScores;

    for (int hop = 1; hop <= maxDepth; ++hop)
    {
        QHash<QString, float> nextFrontier;

        for (auto it = frontier.constBegin(); it != frontier.constEnd(); ++it)
        {
            const QString &currentId = it.key();
            const float currentScore = it.value();

            for (const QString &edgeId : m_adjacency.value(currentId))
            {
                if (visited.size() >= MAX_EXPANDED_ENTRIES)
                {
                    break;
                }

                const auto edgeIt = m_edges.constFind(edgeId);

                if (edgeIt == m_edges.constEnd())
                {
                    continue;
                }

                const MemoryEdge &edge = edgeIt.value();

                if (!edge.active)
                {
                    continue;
                }

                const QString neighborId = (edge.sourceId == currentId)
                                               ? edge.targetId
                                               : edge.sourceId;

                if (seedScores.contains(neighborId) || visited.contains(neighborId))
                {
                    continue;
                }

                const auto neighborIt = m_entries.constFind(neighborId);

                if (neighborIt == m_entries.constEnd())
                {
                    continue;
                }

                const MemoryEntry &neighbor = neighborIt.value();

                if (!neighbor.active)
                {
                    continue;
                }

                if (!PassesScopeFilter(neighbor, scope, petId))
                {
                    continue;
                }

                const float propagatedScore = currentScore * edge.weight * decayPerHop;
                const auto bestIt = bestScores.constFind(neighborId);

                if ((bestIt == bestScores.constEnd())
                    || (propagatedScore > bestIt.value()))
                {
                    bestScores.insert(neighborId, propagatedScore);
                }

                visited.insert(neighborId);
                nextFrontier.insert(neighborId, propagatedScore);
            }
        }

        frontier = nextFrontier;

        if (frontier.isEmpty())
        {
            break;
        }
    }

    for (auto it = bestScores.constBegin(); it != bestScores.constEnd(); ++it)
    {
        _tagGraphHit hit;
        hit.entry = m_entries.value(it.key());
        hit.score = it.value();
        hits.append(hit);
    }

    std::stable_sort(hits.begin(), hits.end(),
                     [](const _tagGraphHit &lhs, const _tagGraphHit &rhs)
    {
        if (lhs.score != rhs.score)
        {
            return lhs.score > rhs.score;
        }

        return HotterEntryFirst(lhs.entry, rhs.entry);
    });

    return hits;
}

QString MemoryGraph::MakeEdgeKey(const QString &sourceId,
                                 const QString &targetId,
                                 MemoryEdge::Type type,
                                 const QString &tag)
{
    const bool sourceFirst = (sourceId <= targetId);
    const QString first = sourceFirst ? sourceId : targetId;
    const QString second = sourceFirst ? targetId : sourceId;

    return first
           + QStringLiteral("|")
           + second
           + QStringLiteral("|")
           + EdgeTypeToString(type)
           + QStringLiteral("|")
           + tag.trimmed();
}

void MemoryGraph::SyncTagEdges(const MemoryEntry &entry)
{
    QString edgeError;

    for (const QString &tag : entry.tags)
    {
        const QString normalizedTag = NormalizeText(tag);

        if (normalizedTag.isEmpty())
        {
            continue;
        }

        const auto tagIt = m_tagIndex.constFind(normalizedTag);

        if (tagIt == m_tagIndex.constEnd())
        {
            continue;
        }

        for (const QString &otherId : tagIt.value())
        {
            if (otherId == entry.id)
            {
                continue;
            }

            const auto otherIt = m_entries.constFind(otherId);

            if (otherIt == m_entries.constEnd() || !otherIt.value().active)
            {
                continue;
            }

            edgeError.clear();
            AddEdge(entry.id, otherId, MemoryEdge::Type::TagShared, normalizedTag, 1.0f, edgeError);
        }
    }
}

void MemoryGraph::RemoveTagEdgesOfEntry(const QString &entryId, const QString &tag)
{
    QVector<QString> edgesToRemove;

    for (auto it = m_edges.constBegin(); it != m_edges.constEnd(); ++it)
    {
        const MemoryEdge &edge = it.value();

        if (edge.type != MemoryEdge::Type::TagShared)
        {
            continue;
        }

        if ((edge.sourceId != entryId) && (edge.targetId != entryId))
        {
            continue;
        }

        if (!tag.isEmpty() && (edge.tag != tag))
        {
            continue;
        }

        edgesToRemove.append(edge.id);
    }

    for (const QString &edgeId : edgesToRemove)
    {
        const auto edgeIt = m_edges.find(edgeId);

        if (edgeIt == m_edges.end())
        {
            continue;
        }

        const MemoryEdge edge = edgeIt.value();
        m_edges.erase(edgeIt);
        UnindexEdge(edge);
    }
}

void MemoryGraph::IndexEdge(const MemoryEdge &edge)
{
    m_adjacency[edge.sourceId].append(edge.id);
    m_adjacency[edge.targetId].append(edge.id);
}

void MemoryGraph::UnindexEdge(const MemoryEdge &edge)
{
    const QStringList endpointIds = { edge.sourceId, edge.targetId };

    for (const QString &endpointId : endpointIds)
    {
        auto it = m_adjacency.find(endpointId);

        if (it == m_adjacency.end())
        {
            continue;
        }

        it.value().removeOne(edge.id);

        if (it.value().isEmpty())
        {
            m_adjacency.erase(it);
        }
    }
}

QStringList MemoryGraph::ExtractKeywords(const QString &text)
{
    QStringList keywords;
    QSet<QString> seen;

    const auto appendKeyword = [&keywords, &seen](const QString &token)
    {
        const QString normalizedToken = NormalizeText(token);

        if (!normalizedToken.isEmpty() && !seen.contains(normalizedToken))
        {
            seen.insert(normalizedToken);
            keywords.append(normalizedToken);
        }
    };

    const auto isHan = [](const QChar &ch)
    {
        return (ch.unicode() >= 0x4E00) && (ch.unicode() <= 0x9FFF);
    };

    const int length = text.size();
    int index = 0;

    while (index < length)
    {
        const QChar current = text.at(index);

        if (isHan(current))
        {
            int end = index;

            while ((end < length) && isHan(text.at(end)))
            {
                end += 1;
            }

            const QString hanSegment = text.mid(index, end - index);

            for (int offset = 0; offset < hanSegment.size(); ++offset)
            {
                appendKeyword(hanSegment.mid(offset, 1));
            }

            index = end;
        }
        else if (current.isLetterOrNumber() || (current == QLatin1Char('_')))
        {
            int end = index;

            while ((end < length)
                   && (text.at(end).isLetterOrNumber() || (text.at(end) == QLatin1Char('_'))))
            {
                end += 1;
            }

            const QString token = text.mid(index, end - index);

            if (token.size() >= 2)
            {
                appendKeyword(token);
            }

            index = end;
        }
        else
        {
            index += 1;
        }
    }

    return keywords;
}

void MemoryGraph::RebuildIndexes(const MemoryEntry &entry)
{
    if (!entry.active)
    {
        return;
    }

    const QStringList keywords = ExtractKeywords(entry.content);

    for (const QString &keyword : keywords)
    {
        m_keywordIndex[keyword].insert(entry.id);
    }

    for (const QString &tag : entry.tags)
    {
        const QString normalizedTag = NormalizeText(tag);

        if (!normalizedTag.isEmpty())
        {
            m_tagIndex[normalizedTag].insert(entry.id);
        }
    }
}

void MemoryGraph::RemoveIndexes(const MemoryEntry &entry)
{
    const QStringList keywords = ExtractKeywords(entry.content);

    for (const QString &keyword : keywords)
    {
        auto it = m_keywordIndex.find(keyword);

        if (it == m_keywordIndex.end())
        {
            continue;
        }

        it.value().remove(entry.id);

        if (it.value().isEmpty())
        {
            m_keywordIndex.erase(it);
        }
    }

    for (const QString &tag : entry.tags)
    {
        const QString normalizedTag = NormalizeText(tag);
        auto it = m_tagIndex.find(normalizedTag);

        if (it == m_tagIndex.end())
        {
            continue;
        }

        it.value().remove(entry.id);

        if (it.value().isEmpty())
        {
            m_tagIndex.erase(it);
        }
    }
}

int MemoryGraph::ScoreEntry(const MemoryEntry &entry,
                            const QStringList &queryKeywords,
                            const QString &query) const
{
    if (queryKeywords.isEmpty() || query.isEmpty())
    {
        return 0;
    }

    int score = 0;
    const QString normalizedContent = NormalizeText(entry.content);

    for (const QString &keyword : queryKeywords)
    {
        if (m_keywordIndex.value(keyword).contains(entry.id))
        {
            score += 2;
        }
        else if (normalizedContent.contains(keyword))
        {
            score += 1;
        }
    }

    if (score <= 0)
    {
        return 0;
    }

    for (const QString &tag : entry.tags)
    {
        if (normalizedContent.contains(NormalizeText(tag)))
        {
            score += 1;
        }
    }

    return score;
}

} // namespace vpet
