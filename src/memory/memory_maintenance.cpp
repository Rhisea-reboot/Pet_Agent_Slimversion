#include "vpet/memory/memory_maintenance.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vpet
{

namespace
{

constexpr qint64 MILLISECONDS_PER_HOUR = 60LL * 60LL * 1000LL;
constexpr double HOURS_PER_DAY = 24.0;
constexpr double NATURAL_LOG_OF_TWO = 0.6931471805599453;
constexpr float HELPFUL_CONFIDENCE_INCREMENT = 0.05f;
constexpr float UNHELPFUL_CONFIDENCE_DECREMENT = 0.1f;
constexpr int MAX_INFERRED_TAGS_PER_ENTRY = 2;
constexpr int MAX_MAINTENANCE_HITS = 6;
const QString GAP_FILE_NAME = QStringLiteral("gaps.json");
const QString CLUSTER_DIRECTORY_NAME = QStringLiteral("clusters");
const QString CLUSTER_FILE_NAME = QStringLiteral("cluster_metadata.json");

/**
 * @brief 提取中英文相似度词元
 * @param[in] text 原始文本
 * @return 去重词元集合
 */
QSet<QString> SimilarityTokens(const QString &text)
{
    QSet<QString> tokens;
    const QString normalized = text.trimmed().toLower();
    int index = 0;

    while (index < normalized.size())
    {
        const QChar character = normalized.at(index);

        if ((character.unicode() >= 0x4E00) && (character.unicode() <= 0x9FFF))
        {
            tokens.insert(QString(character));
            index += 1;
            continue;
        }

        if (character.isLetterOrNumber() || (character == QLatin1Char('_')))
        {
            const int start = index;

            while ((index < normalized.size())
                   && (normalized.at(index).isLetterOrNumber()
                       || (normalized.at(index) == QLatin1Char('_'))))
            {
                index += 1;
            }

            const QString token = normalized.mid(start, index - start);

            if (token.size() >= 2)
            {
                tokens.insert(token);
            }

            continue;
        }

        index += 1;
    }

    return tokens;
}

/**
 * @brief 计算记忆文本的保守 Jaccard 相似度
 * @param[in] left 左文本
 * @param[in] right 右文本
 * @return 0.0 到 1.0 的相似度
 */
float MemoryTextSimilarity(const QString &left, const QString &right)
{
    if (left.trimmed().compare(right.trimmed(), Qt::CaseInsensitive) == 0)
    {
        return 1.0f;
    }

    const QSet<QString> leftTokens = SimilarityTokens(left);
    const QSet<QString> rightTokens = SimilarityTokens(right);

    if (leftTokens.isEmpty() || rightTokens.isEmpty())
    {
        return 0.0f;
    }

    const int unionSize = (leftTokens | rightTokens).size();

    if (unionSize <= 0)
    {
        return 0.0f;
    }

    return static_cast<float>((leftTokens & rightTokens).size())
           / static_cast<float>(unionSize);
}

/**
 * @brief 判断两个条目是否属于同一逻辑作用域
 * @param[in] left 左条目
 * @param[in] right 右条目
 * @return 可合并返回 true
 */
bool SameLogicalScope(const MemoryEntry &left, const MemoryEntry &right)
{
    return (left.scope == right.scope)
           && ((left.scope == MemoryEntry::Scope::Global) || (left.petId == right.petId));
}

/**
 * @brief 判断左条目是否应成为重复合并的保留项
 * @param[in] left 左条目
 * @param[in] right 右条目
 * @return 左条目优先返回 true
 */
bool PreferAsSurvivor(const MemoryEntry &left, const MemoryEntry &right)
{
    if (left.strength != right.strength)
    {
        return left.strength > right.strength;
    }

    if (left.trustScore != right.trustScore)
    {
        return left.trustScore > right.trustScore;
    }

    if (left.createdAt != right.createdAt)
    {
        return left.createdAt < right.createdAt;
    }

    return left.id < right.id;
}

/**
 * @brief 返回记忆类型和来源对应的半衰期天数
 * @param[in] entry 记忆条目
 * @return 半衰期天数
 */
double HalfLifeDays(const MemoryEntry &entry)
{
    if (entry.provenance == MemoryEntry::Provenance::Inferred)
    {
        return 7.0;
    }

    switch (entry.type)
    {
        case MemoryEntry::Type::Preference:
            return 90.0;
        case MemoryEntry::Type::Procedure:
            return 60.0;
        case MemoryEntry::Type::Correction:
        case MemoryEntry::Type::Negative:
            return 365.0;
        case MemoryEntry::Type::Fact:
            return 30.0;
    }

    return 30.0;
}

/**
 * @brief 构造无向端点键
 * @param[in] leftId 左端点 ID
 * @param[in] rightId 右端点 ID
 * @return 规范化端点键
 */
QString EndpointKey(const QString &leftId, const QString &rightId)
{
    if (leftId <= rightId)
    {
        return leftId + QStringLiteral("|") + rightId;
    }

    return rightId + QStringLiteral("|") + leftId;
}

/**
 * @brief 原子写入 JSON 文档
 * @param[in] filePath 文件路径
 * @param[in] document JSON 文档
 * @param[out] errorMessage 错误描述
 * @return 写入成功返回 true
 */
bool SaveJsonDocument(const QString &filePath,
                      const QJsonDocument &document,
                      QString &errorMessage)
{
    QSaveFile saveFile(filePath);

    if (!saveFile.open(QIODevice::WriteOnly))
    {
        errorMessage = QStringLiteral("Failed to open memory maintenance file for writing.");
        return false;
    }

    const QByteArray data = document.toJson(QJsonDocument::Indented);

    if (saveFile.write(data) != data.size())
    {
        saveFile.cancelWriting();
        errorMessage = QStringLiteral("Failed to write memory maintenance data.");
        return false;
    }

    if (!saveFile.commit())
    {
        errorMessage = QStringLiteral("Failed to commit memory maintenance file.");
        return false;
    }

    return true;
}

} // anonymous namespace

bool MemoryMaintenance::SetConfig(const _tagMemoryMaintenanceConfig &config,
                                  QString &errorMessage)
{
    if (config.decayIntervalHours <= 0)
    {
        errorMessage = QStringLiteral("Memory maintenance decay interval must be positive.");
        return false;
    }

    if (config.clusterUpdateRetrievals <= 0)
    {
        errorMessage = QStringLiteral("Memory maintenance cluster interval must be positive.");
        return false;
    }

    if ((config.maxGapRecords <= 0) || (config.maxGapRecords > 4096))
    {
        errorMessage = QStringLiteral("Memory maintenance max gap records must be 1 to 4096.");
        return false;
    }

    if ((config.inferredTagMinSupport < 2) || (config.inferredTagMinSupport > 32))
    {
        errorMessage = QStringLiteral("Memory maintenance tag support must be 2 to 32.");
        return false;
    }

    if ((config.deepMaintenanceRetrievals <= 0)
        || (config.deepMaintenanceRetrievals > 100000))
    {
        errorMessage = QStringLiteral("Memory deep maintenance interval is invalid.");
        return false;
    }

    if ((config.duplicateSimilarityThreshold < 0.90f)
        || (config.duplicateSimilarityThreshold > 1.0f))
    {
        errorMessage = QStringLiteral("Memory duplicate similarity threshold is invalid.");
        return false;
    }

    if ((config.weakConfidenceThreshold < 0.0f)
        || (config.weakConfidenceThreshold > 0.25f)
        || (config.weakStrengthLimit > 100))
    {
        errorMessage = QStringLiteral("Memory weak pruning thresholds are invalid.");
        return false;
    }

    if ((config.relatedInitialWeight <= 0.0f)
        || (config.relatedInitialWeight > config.maxRelatedWeight)
        || (config.relatedWeightIncrement <= 0.0f)
        || (config.maxRelatedWeight > 1.0f))
    {
        errorMessage = QStringLiteral("Memory maintenance related edge weights are invalid.");
        return false;
    }

    m_config = config;
    return true;
}

bool MemoryMaintenance::SetMemoryDir(const QString &memoryDir, QString &errorMessage)
{
    const QString normalizedDir = memoryDir.trimmed();

    if (normalizedDir.isEmpty())
    {
        errorMessage = QStringLiteral("Memory maintenance directory is empty.");
        return false;
    }

    if (!QDir().mkpath(normalizedDir))
    {
        errorMessage = QStringLiteral("Failed to create memory maintenance directory.");
        return false;
    }

    m_memoryDir = QDir(normalizedDir).absolutePath();
    return true;
}

bool MemoryMaintenance::ApplyConfidenceDecay(MemoryGraph &graph, qint64 nowMs) const
{
    if (!m_config.enabled || (nowMs <= 0))
    {
        return false;
    }

    bool changed = false;
    const qint64 intervalMs = static_cast<qint64>(m_config.decayIntervalHours)
                              * MILLISECONDS_PER_HOUR;

    for (MemoryEntry entry : graph.AllEntries())
    {
        if (!entry.active)
        {
            continue;
        }

        qint64 decayStartMs = entry.confidenceUpdatedAt;

        if (decayStartMs <= 0)
        {
            decayStartMs = (entry.updatedAt > 0) ? entry.updatedAt : entry.createdAt;
        }

        if ((decayStartMs <= 0) || (nowMs <= decayStartMs)
            || ((nowMs - decayStartMs) < intervalMs))
        {
            continue;
        }

        const double elapsedDays = static_cast<double>(nowMs - decayStartMs)
                                   / static_cast<double>(MILLISECONDS_PER_HOUR)
                                   / HOURS_PER_DAY;
        const double decayFactor = std::exp((-NATURAL_LOG_OF_TWO * elapsedDays)
                                            / HalfLifeDays(entry));
        entry.confidence = static_cast<float>(qBound(0.0,
                                                     static_cast<double>(entry.confidence)
                                                         * decayFactor,
                                                     1.0));
        entry.confidenceUpdatedAt = nowMs;

        QString updateError;

        if (graph.UpdateEntry(entry, updateError))
        {
            changed = true;
        }
    }

    return changed;
}

bool MemoryMaintenance::OnRetrieved(MemoryGraph &graph,
                                    const QVector<MemoryEntry> &entries,
                                    const QString &query,
                                    const QString &petId,
                                    const QString &triggerType,
                                    qint64 nowMs,
                                    QStringList &removedIds,
                                    QString &errorMessage)
{
    if (!m_config.enabled || (nowMs <= 0))
    {
        return false;
    }

    m_retrievalCount += 1;
    bool changed = false;

    if (entries.isEmpty())
    {
        RecordGap(query, petId, triggerType, nowMs, errorMessage);
    }
    else
    {
        for (const MemoryEntry &retrievedEntry : entries)
        {
            MemoryEntry entry;

            if (!graph.GetEntry(retrievedEntry.id, entry) || !entry.active)
            {
                continue;
            }

            entry.lastAccessed = nowMs;

            if (entry.accessCount < std::numeric_limits<quint32>::max())
            {
                entry.accessCount += 1;
            }

            QString updateError;

            if (graph.UpdateEntry(entry, updateError))
            {
                changed = true;
            }
        }

        changed = DiscoverLinks(graph, entries) || changed;
        changed = InferTags(graph, entries) || changed;
    }

    if ((m_retrievalCount % static_cast<quint64>(m_config.clusterUpdateRetrievals)) == 0)
    {
        QString clusterError;

        if (!RebuildClusters(graph, nowMs, clusterError) && errorMessage.isEmpty())
        {
            errorMessage = clusterError;
        }
    }

    if ((m_retrievalCount % static_cast<quint64>(m_config.deepMaintenanceRetrievals)) == 0)
    {
        changed = RunDeepConsolidation(graph, nowMs, removedIds) || changed;

        QString clusterError;

        if (!RebuildClusters(graph, nowMs, clusterError) && errorMessage.isEmpty())
        {
            errorMessage = clusterError;
        }
    }

    return changed;
}

bool MemoryMaintenance::RunDeepConsolidation(MemoryGraph &graph,
                                             qint64 nowMs,
                                             QStringList &removedIds) const
{
    if (!m_config.enabled || (nowMs <= 0))
    {
        return false;
    }

    bool changed = false;
    QVector<MemoryEntry> entries = graph.AllEntries();
    QSet<QString> conflictIds;

    for (const MemoryEdge &edge : graph.AllEdges())
    {
        if (edge.active && (edge.type == MemoryEdge::Type::Conflict))
        {
            conflictIds.insert(edge.sourceId);
            conflictIds.insert(edge.targetId);
        }
    }

    for (int leftIndex = 0; leftIndex < entries.size(); ++leftIndex)
    {
        if (!entries.at(leftIndex).active || conflictIds.contains(entries.at(leftIndex).id))
        {
            continue;
        }

        for (int rightIndex = leftIndex + 1; rightIndex < entries.size(); ++rightIndex)
        {
            if (!entries.at(rightIndex).active
                || conflictIds.contains(entries.at(rightIndex).id)
                || (entries.at(leftIndex).type != entries.at(rightIndex).type)
                || (entries.at(leftIndex).type == MemoryEntry::Type::Procedure)
                || (entries.at(leftIndex).type == MemoryEntry::Type::Negative)
                || !SameLogicalScope(entries.at(leftIndex), entries.at(rightIndex))
                || (MemoryTextSimilarity(entries.at(leftIndex).content,
                                         entries.at(rightIndex).content)
                    < m_config.duplicateSimilarityThreshold))
            {
                continue;
            }

            const bool leftSurvives = PreferAsSurvivor(entries.at(leftIndex),
                                                       entries.at(rightIndex));
            const int survivorIndex = leftSurvives ? leftIndex : rightIndex;
            const int duplicateIndex = leftSurvives ? rightIndex : leftIndex;
            MemoryEntry survivor = entries.at(survivorIndex);
            const MemoryEntry duplicate = entries.at(duplicateIndex);

            survivor.strength = qMax(survivor.strength, duplicate.strength);

            if (survivor.strength < std::numeric_limits<quint32>::max())
            {
                survivor.strength += 1;
            }

            survivor.accessCount = qMax(survivor.accessCount, duplicate.accessCount);
            survivor.confidence = qMax(survivor.confidence, duplicate.confidence);
            survivor.trustScore = qMax(survivor.trustScore, duplicate.trustScore);
            survivor.lastAccessed = qMax(survivor.lastAccessed, duplicate.lastAccessed);
            survivor.updatedAt = nowMs;

            for (const QString &tag : duplicate.tags)
            {
                if (!survivor.tags.contains(tag))
                {
                    survivor.tags.append(tag);
                }
            }

            QString updateError;

            if (!graph.UpdateEntry(survivor, updateError)
                || !graph.AddEdge(survivor.id,
                                  duplicate.id,
                                  MemoryEdge::Type::Supersedes,
                                  QString(),
                                  1.0f,
                                  updateError)
                || !graph.SoftDeleteEntry(duplicate.id))
            {
                continue;
            }

            entries[survivorIndex] = survivor;
            entries[duplicateIndex].active = false;
            removedIds.append(duplicate.id);
            changed = true;

            if (!leftSurvives)
            {
                break;
            }
        }
    }

    for (MemoryEntry entry : graph.AllEntries())
    {
        if (!entry.active
            || conflictIds.contains(entry.id)
            || (entry.confidence >= m_config.weakConfidenceThreshold)
            || (entry.strength > m_config.weakStrengthLimit))
        {
            continue;
        }

        if (graph.SoftDeleteEntry(entry.id))
        {
            removedIds.append(entry.id);
            changed = true;
        }
    }

    QVector<QString> propagationEdgesToRemove;

    for (const MemoryEdge &edge : graph.AllEdges())
    {
        if ((edge.type == MemoryEdge::Type::Supersedes)
            || (edge.type == MemoryEdge::Type::Conflict))
        {
            continue;
        }

        MemoryEntry sourceEntry;
        MemoryEntry targetEntry;

        if (!graph.GetEntry(edge.sourceId, sourceEntry)
            || !graph.GetEntry(edge.targetId, targetEntry)
            || !sourceEntry.active
            || !targetEntry.active)
        {
            propagationEdgesToRemove.append(edge.id);
        }
    }

    for (const QString &edgeId : propagationEdgesToRemove)
    {
        QString removeError;

        if (graph.RemoveEdge(edgeId, removeError))
        {
            changed = true;
        }
    }

    removedIds.removeDuplicates();
    return changed;
}

bool MemoryMaintenance::ApplyFeedback(MemoryGraph &graph,
                                      const QStringList &memoryIds,
                                      const QString &petId,
                                      bool helpful,
                                      qint64 nowMs) const
{
    if (!m_config.enabled || memoryIds.isEmpty() || (nowMs <= 0))
    {
        return false;
    }

    bool changed = false;
    QSet<QString> visitedIds;

    for (const QString &rawMemoryId : memoryIds)
    {
        const QString memoryId = rawMemoryId.trimmed();

        if (memoryId.isEmpty() || visitedIds.contains(memoryId))
        {
            continue;
        }

        visitedIds.insert(memoryId);
        MemoryEntry entry;

        if (!graph.GetEntry(memoryId, entry) || !entry.active)
        {
            continue;
        }

        if ((entry.scope == MemoryEntry::Scope::Pet) && (entry.petId != petId))
        {
            continue;
        }

        if (helpful)
        {
            if (entry.strength < std::numeric_limits<quint32>::max())
            {
                entry.strength += 1;
            }

            entry.confidence = qMin(1.0f,
                                    entry.confidence + HELPFUL_CONFIDENCE_INCREMENT);
        }
        else
        {
            entry.confidence = qMax(0.0f,
                                    entry.confidence - UNHELPFUL_CONFIDENCE_DECREMENT);
        }

        entry.confidenceUpdatedAt = nowMs;
        entry.updatedAt = nowMs;
        QString updateError;

        if (graph.UpdateEntry(entry, updateError))
        {
            changed = true;
        }
    }

    return changed;
}

float MemoryMaintenance::RetrievalWeight(const MemoryEntry &entry, qint64 nowMs)
{
    const float confidence = qBound(0.0f, entry.confidence, 1.0f);
    const float trustScore = qBound(0.0f, entry.trustScore, 1.0f);
    const float accessBoost = 1.0f
                              + (0.1f * std::log(static_cast<float>(entry.accessCount) + 1.0f));
    float recencyBoost = 1.0f;

    if ((entry.lastAccessed > 0) && (nowMs > entry.lastAccessed))
    {
        const double elapsedHours = static_cast<double>(nowMs - entry.lastAccessed)
                                    / static_cast<double>(MILLISECONDS_PER_HOUR);
        recencyBoost += static_cast<float>(0.5 * std::exp(-elapsedHours / HOURS_PER_DAY));
    }

    return confidence * trustScore * accessBoost * recencyBoost;
}

bool MemoryMaintenance::DiscoverLinks(MemoryGraph &graph,
                                      const QVector<MemoryEntry> &entries) const
{
    if (entries.size() < 2)
    {
        return false;
    }

    QHash<QString, MemoryEdge> relatedEdges;

    for (const MemoryEdge &edge : graph.AllEdges())
    {
        if (edge.active && (edge.type == MemoryEdge::Type::Related))
        {
            relatedEdges.insert(EndpointKey(edge.sourceId, edge.targetId), edge);
        }
    }

    bool changed = false;

    const int entryLimit = qMin(MAX_MAINTENANCE_HITS, static_cast<int>(entries.size()));

    for (int leftIndex = 0; leftIndex < entryLimit; ++leftIndex)
    {
        for (int rightIndex = leftIndex + 1; rightIndex < entryLimit; ++rightIndex)
        {
            const MemoryEntry &leftEntry = entries.at(leftIndex);
            const MemoryEntry &rightEntry = entries.at(rightIndex);

            if (leftEntry.id.isEmpty() || rightEntry.id.isEmpty())
            {
                continue;
            }

            const QString endpointKey = EndpointKey(leftEntry.id, rightEntry.id);
            const auto edgeIt = relatedEdges.constFind(endpointKey);
            QString edgeError;

            if (edgeIt == relatedEdges.constEnd())
            {
                if (graph.AddEdge(leftEntry.id,
                                  rightEntry.id,
                                  MemoryEdge::Type::Related,
                                  QString(),
                                  m_config.relatedInitialWeight,
                                  edgeError))
                {
                    changed = true;
                }

                continue;
            }

            const float updatedWeight = qMin(m_config.maxRelatedWeight,
                                             edgeIt.value().weight
                                                 + m_config.relatedWeightIncrement);

            if ((updatedWeight > edgeIt.value().weight)
                && graph.SetEdgeWeight(edgeIt.value().id, updatedWeight, edgeError))
            {
                changed = true;
            }
        }
    }

    return changed;
}

bool MemoryMaintenance::InferTags(MemoryGraph &graph,
                                  const QVector<MemoryEntry> &entries) const
{
    if (entries.size() < m_config.inferredTagMinSupport)
    {
        return false;
    }

    bool changed = false;

    const int entryLimit = qMin(MAX_MAINTENANCE_HITS, static_cast<int>(entries.size()));

    for (int targetIndex = 0; targetIndex < entryLimit; ++targetIndex)
    {
        const MemoryEntry &target = entries.at(targetIndex);
        QHash<QString, int> supportCounts;

        for (int sourceIndex = 0; sourceIndex < entryLimit; ++sourceIndex)
        {
            const MemoryEntry &source = entries.at(sourceIndex);
            if (source.id == target.id)
            {
                continue;
            }

            if ((source.scope != target.scope)
                || ((target.scope == MemoryEntry::Scope::Pet)
                    && (source.petId != target.petId)))
            {
                continue;
            }

            for (const QString &rawTag : source.tags)
            {
                const QString tag = rawTag.trimmed();

                if (!tag.isEmpty() && !target.tags.contains(tag))
                {
                    supportCounts[tag] += 1;
                }
            }
        }

        QVector<QString> supportedTags;

        for (auto it = supportCounts.constBegin(); it != supportCounts.constEnd(); ++it)
        {
            if (it.value() >= m_config.inferredTagMinSupport)
            {
                supportedTags.append(it.key());
            }
        }

        std::stable_sort(supportedTags.begin(), supportedTags.end(),
                         [&supportCounts](const QString &left, const QString &right)
        {
            if (supportCounts.value(left) != supportCounts.value(right))
            {
                return supportCounts.value(left) > supportCounts.value(right);
            }

            return left < right;
        });

        const int tagLimit = qMin(MAX_INFERRED_TAGS_PER_ENTRY,
                                  static_cast<int>(supportedTags.size()));

        for (int tagIndex = 0; tagIndex < tagLimit; ++tagIndex)
        {
            QString tagError;

            if (graph.AddTag(target.id, supportedTags.at(tagIndex), tagError))
            {
                changed = true;
            }
        }
    }

    return changed;
}

bool MemoryMaintenance::RecordGap(const QString &query,
                                  const QString &petId,
                                  const QString &triggerType,
                                  qint64 nowMs,
                                  QString &errorMessage) const
{
    if (m_memoryDir.isEmpty() || query.trimmed().isEmpty())
    {
        return false;
    }

    const QString filePath = QDir(m_memoryDir).filePath(GAP_FILE_NAME);
    QJsonArray gaps;

    if (QFileInfo::exists(filePath))
    {
        QFile file(filePath);

        if (!file.open(QIODevice::ReadOnly))
        {
            errorMessage = QStringLiteral("Failed to read memory gap file.");
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument existingDocument = QJsonDocument::fromJson(file.readAll(), &parseError);

        if ((parseError.error != QJsonParseError::NoError) || !existingDocument.isObject())
        {
            const QString backupPath = filePath
                                       + QStringLiteral(".corrupt.")
                                       + QString::number(nowMs);
            QFile::copy(filePath, backupPath);
            errorMessage = QStringLiteral("Memory gap file is corrupt; backup retained.");
        }
        else
        {
            gaps = existingDocument.object().value(QStringLiteral("gaps")).toArray();
        }
    }

    const QByteArray queryHash = QCryptographicHash::hash(query.trimmed().toUtf8(),
                                                          QCryptographicHash::Sha256)
                                     .toHex();
    bool found = false;

    for (int index = 0; index < gaps.size(); ++index)
    {
        QJsonObject gap = gaps.at(index).toObject();

        if ((gap.value(QStringLiteral("queryHash")).toString().toLatin1() != queryHash)
            || (gap.value(QStringLiteral("petId")).toString() != petId)
            || (gap.value(QStringLiteral("triggerType")).toString() != triggerType))
        {
            continue;
        }

        gap.insert(QStringLiteral("lastSeenAt"), static_cast<double>(nowMs));
        gap.insert(QStringLiteral("count"), gap.value(QStringLiteral("count")).toInt(0) + 1);
        gaps.replace(index, gap);
        found = true;
        break;
    }

    if (!found)
    {
        QJsonObject gap;
        gap.insert(QStringLiteral("queryHash"), QString::fromLatin1(queryHash));
        gap.insert(QStringLiteral("petId"), petId);
        gap.insert(QStringLiteral("triggerType"), triggerType);
        gap.insert(QStringLiteral("firstSeenAt"), static_cast<double>(nowMs));
        gap.insert(QStringLiteral("lastSeenAt"), static_cast<double>(nowMs));
        gap.insert(QStringLiteral("count"), 1);
        gaps.append(gap);
    }

    while (gaps.size() > m_config.maxGapRecords)
    {
        gaps.removeAt(0);
    }

    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("schemaVersion"), 1);
    rootObject.insert(QStringLiteral("gaps"), gaps);
    rootObject.insert(QStringLiteral("updatedAt"), static_cast<double>(nowMs));
    return SaveJsonDocument(filePath, QJsonDocument(rootObject), errorMessage);
}

bool MemoryMaintenance::RebuildClusters(const MemoryGraph &graph,
                                        qint64 nowMs,
                                        QString &errorMessage) const
{
    if (m_memoryDir.isEmpty())
    {
        errorMessage = QStringLiteral("Memory cluster directory is unavailable.");
        return false;
    }

    QHash<QString, QSet<QString>> adjacency;

    for (const MemoryEdge &edge : graph.AllEdges())
    {
        if (!edge.active || (edge.type != MemoryEdge::Type::Related))
        {
            continue;
        }

        MemoryEntry sourceEntry;
        MemoryEntry targetEntry;

        if (!graph.GetEntry(edge.sourceId, sourceEntry)
            || !graph.GetEntry(edge.targetId, targetEntry)
            || !sourceEntry.active
            || !targetEntry.active)
        {
            continue;
        }

        adjacency[edge.sourceId].insert(edge.targetId);
        adjacency[edge.targetId].insert(edge.sourceId);
    }

    QSet<QString> visited;
    QJsonArray clusters;
    QStringList rootIds = adjacency.keys();
    rootIds.sort();

    for (const QString &rootId : rootIds)
    {
        if (visited.contains(rootId))
        {
            continue;
        }

        QStringList frontier = { rootId };
        QStringList memberIds;
        visited.insert(rootId);

        while (!frontier.isEmpty())
        {
            const QString currentId = frontier.takeFirst();
            memberIds.append(currentId);
            QStringList neighborIds = adjacency.value(currentId).values();
            neighborIds.sort();

            for (const QString &neighborId : neighborIds)
            {
                if (!visited.contains(neighborId))
                {
                    visited.insert(neighborId);
                    frontier.append(neighborId);
                }
            }
        }

        if (memberIds.size() < 2)
        {
            continue;
        }

        memberIds.sort();
        QJsonArray members;

        for (const QString &memberId : memberIds)
        {
            members.append(memberId);
        }

        const QByteArray clusterHash = QCryptographicHash::hash(
            memberIds.join(QLatin1Char('|')).toUtf8(), QCryptographicHash::Sha256).toHex();
        QJsonObject cluster;
        cluster.insert(QStringLiteral("id"),
                       QStringLiteral("cluster_%1").arg(QString::fromLatin1(clusterHash.left(16))));
        cluster.insert(QStringLiteral("memberIds"), members);
        cluster.insert(QStringLiteral("size"), memberIds.size());
        clusters.append(cluster);
    }

    const QString clusterDir = QDir(m_memoryDir).filePath(CLUSTER_DIRECTORY_NAME);

    if (!QDir().mkpath(clusterDir))
    {
        errorMessage = QStringLiteral("Failed to create memory cluster directory.");
        return false;
    }

    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("schemaVersion"), 1);
    rootObject.insert(QStringLiteral("clusters"), clusters);
    rootObject.insert(QStringLiteral("updatedAt"), static_cast<double>(nowMs));
    const QString filePath = QDir(clusterDir).filePath(CLUSTER_FILE_NAME);
    return SaveJsonDocument(filePath, QJsonDocument(rootObject), errorMessage);
}

} // namespace vpet
