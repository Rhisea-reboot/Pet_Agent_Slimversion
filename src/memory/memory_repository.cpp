#include "vpet/memory/memory_repository.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace vpet
{

namespace
{

const QString GRAPH_FILE_NAME = QStringLiteral("graph.json");
const QString MEMORY_DIR_NAME = QStringLiteral("memory");
const QString DEFAULT_PET_ID = QStringLiteral("default");

/**
 * @brief 校验 JSON 数组中的所有元素均为字符串
 * @param[in] array JSON 数组
 * @return 全部为字符串返回 true
 */
bool IsStringArray(const QJsonArray &array)
{
    for (const QJsonValue &value : array)
    {
        if (!value.isString())
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 校验条目 JSON 的枚举和结构字段
 * @param[in] object 条目 JSON
 * @param[out] errorMessage 错误描述
 * @return 结构有效返回 true
 */
bool ValidateEntryJson(const QJsonObject &object, QString &errorMessage)
{
    const QString type = object.value(QStringLiteral("type")).toString();
    const QString scope = object.value(QStringLiteral("scope")).toString();
    const QString provenance = object.value(QStringLiteral("provenance")).toString();
    const QSet<QString> validTypes = {
        QStringLiteral("fact"),
        QStringLiteral("preference"),
        QStringLiteral("procedure"),
        QStringLiteral("correction"),
        QStringLiteral("negative")
    };
    const QSet<QString> validScopes = {
        QStringLiteral("global"),
        QStringLiteral("pet")
    };
    const QSet<QString> validProvenance = {
        QStringLiteral("user_stated"),
        QStringLiteral("user_corrected"),
        QStringLiteral("observed"),
        QStringLiteral("extracted"),
        QStringLiteral("inferred")
    };

    const QJsonArray tags = object.value(QStringLiteral("tags")).toArray();

    if (!object.value(QStringLiteral("id")).isString()
        || !object.value(QStringLiteral("content")).isString()
        || !validTypes.contains(type)
        || !validScopes.contains(scope)
        || !validProvenance.contains(provenance)
        || !object.value(QStringLiteral("tags")).isArray()
        || !IsStringArray(tags))
    {
        errorMessage = QStringLiteral("Memory entry JSON has invalid required fields.");
        return false;
    }

    if (object.contains(QStringLiteral("triggerPatterns"))
        && (!object.value(QStringLiteral("triggerPatterns")).isArray()
            || !IsStringArray(object.value(QStringLiteral("triggerPatterns")).toArray())))
    {
        errorMessage = QStringLiteral("Memory entry triggerPatterns must be an array.");
        return false;
    }

    if (type == QStringLiteral("procedure"))
    {
        const QJsonValue procedureValue = object.value(QStringLiteral("procedure"));

        if (!procedureValue.isObject())
        {
            errorMessage = QStringLiteral("Procedure memory JSON must contain an object.");
            return false;
        }

        const QJsonObject procedureObject = procedureValue.toObject();
        const QJsonArray steps = procedureObject.value(QStringLiteral("steps")).toArray();
        const QJsonArray prerequisites = procedureObject.value(
            QStringLiteral("prerequisites")).toArray();
        const QJsonArray warnings = procedureObject.value(QStringLiteral("warnings")).toArray();

        if (!procedureObject.value(QStringLiteral("steps")).isArray()
            || !procedureObject.value(QStringLiteral("prerequisites")).isArray()
            || !procedureObject.value(QStringLiteral("warnings")).isArray()
            || !IsStringArray(steps)
            || !IsStringArray(prerequisites)
            || !IsStringArray(warnings))
        {
            errorMessage = QStringLiteral("Procedure memory list fields must be arrays.");
            return false;
        }
    }

    const QStringList numericFields = {
        QStringLiteral("createdAt"),
        QStringLiteral("updatedAt"),
        QStringLiteral("lastAccessed"),
        QStringLiteral("confidenceUpdatedAt"),
        QStringLiteral("accessCount"),
        QStringLiteral("strength"),
        QStringLiteral("confidence"),
        QStringLiteral("trustScore")
    };

    for (const QString &fieldName : numericFields)
    {
        if (object.contains(fieldName) && !object.value(fieldName).isDouble())
        {
            errorMessage = QStringLiteral("Memory entry numeric field is invalid: %1")
                               .arg(fieldName);
            return false;
        }
    }

    if (object.contains(QStringLiteral("active"))
        && !object.value(QStringLiteral("active")).isBool())
    {
        errorMessage = QStringLiteral("Memory entry active field must be boolean.");
        return false;
    }

    return true;
}

/**
 * @brief 校验边 JSON 的枚举和结构字段
 * @param[in] object 边 JSON
 * @param[out] errorMessage 错误描述
 * @return 结构有效返回 true
 */
bool ValidateEdgeJson(const QJsonObject &object, QString &errorMessage)
{
    const QString type = object.value(QStringLiteral("type")).toString();
    const QSet<QString> validTypes = {
        QStringLiteral("explicit"),
        QStringLiteral("related"),
        QStringLiteral("tag_shared"),
        QStringLiteral("supersedes"),
        QStringLiteral("conflict")
    };

    if (!object.value(QStringLiteral("id")).isString()
        || !object.value(QStringLiteral("sourceId")).isString()
        || !object.value(QStringLiteral("targetId")).isString()
        || !validTypes.contains(type)
        || !object.value(QStringLiteral("weight")).isDouble())
    {
        errorMessage = QStringLiteral("Memory edge JSON has invalid required fields.");
        return false;
    }

    return true;
}

/**
 * @brief 将记忆类型转换为 JSON 字符串
 */
QString TypeToString(MemoryEntry::Type type)
{
    switch (type)
    {
        case MemoryEntry::Type::Fact:
            return QStringLiteral("fact");
        case MemoryEntry::Type::Preference:
            return QStringLiteral("preference");
        case MemoryEntry::Type::Procedure:
            return QStringLiteral("procedure");
        case MemoryEntry::Type::Correction:
            return QStringLiteral("correction");
        case MemoryEntry::Type::Negative:
            return QStringLiteral("negative");
    }

    return QStringLiteral("fact");
}

/**
 * @brief 将 JSON 字符串解析为记忆类型
 */
MemoryEntry::Type TypeFromString(const QString &value)
{
    if (value == QStringLiteral("preference"))
    {
        return MemoryEntry::Type::Preference;
    }

    if (value == QStringLiteral("correction"))
    {
        return MemoryEntry::Type::Correction;
    }

    if (value == QStringLiteral("procedure"))
    {
        return MemoryEntry::Type::Procedure;
    }

    if (value == QStringLiteral("negative"))
    {
        return MemoryEntry::Type::Negative;
    }

    return MemoryEntry::Type::Fact;
}

/**
 * @brief 将作用域转换为 JSON 字符串
 */
QString ScopeToString(MemoryEntry::Scope scope)
{
    return (scope == MemoryEntry::Scope::Global) ? QStringLiteral("global")
                                                 : QStringLiteral("pet");
}

/**
 * @brief 将 JSON 字符串解析为作用域
 */
MemoryEntry::Scope ScopeFromString(const QString &value)
{
    return (value == QStringLiteral("global")) ? MemoryEntry::Scope::Global
                                               : MemoryEntry::Scope::Pet;
}

/**
 * @brief 将来源转换为 JSON 字符串
 */
QString ProvenanceToString(MemoryEntry::Provenance provenance)
{
    switch (provenance)
    {
        case MemoryEntry::Provenance::UserCorrected:
            return QStringLiteral("user_corrected");
        case MemoryEntry::Provenance::Observed:
            return QStringLiteral("observed");
        case MemoryEntry::Provenance::Extracted:
            return QStringLiteral("extracted");
        case MemoryEntry::Provenance::Inferred:
            return QStringLiteral("inferred");
        case MemoryEntry::Provenance::UserStated:
            return QStringLiteral("user_stated");
    }

    return QStringLiteral("user_stated");
}

/**
 * @brief 将 JSON 字符串解析为来源
 */
MemoryEntry::Provenance ProvenanceFromString(const QString &value)
{
    if (value == QStringLiteral("user_corrected"))
    {
        return MemoryEntry::Provenance::UserCorrected;
    }

    if (value == QStringLiteral("observed"))
    {
        return MemoryEntry::Provenance::Observed;
    }

    if (value == QStringLiteral("extracted"))
    {
        return MemoryEntry::Provenance::Extracted;
    }

    if (value == QStringLiteral("inferred"))
    {
        return MemoryEntry::Provenance::Inferred;
    }

    return MemoryEntry::Provenance::UserStated;
}

/**
 * @brief 将条目序列化为 JSON 对象
 */
QJsonObject EntryToJson(const MemoryEntry &entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), entry.id);
    object.insert(QStringLiteral("content"), entry.content);
    object.insert(QStringLiteral("category"), entry.category);
    object.insert(QStringLiteral("type"), TypeToString(entry.type));
    object.insert(QStringLiteral("scope"), ScopeToString(entry.scope));
    object.insert(QStringLiteral("provenance"), ProvenanceToString(entry.provenance));
    object.insert(QStringLiteral("petId"), entry.petId);

    QJsonArray tagArray;

    for (const QString &tag : entry.tags)
    {
        tagArray.append(tag);
    }

    object.insert(QStringLiteral("tags"), tagArray);

    QJsonArray triggerPatternArray;

    for (const QString &triggerPattern : entry.triggerPatterns)
    {
        triggerPatternArray.append(triggerPattern);
    }

    object.insert(QStringLiteral("triggerPatterns"), triggerPatternArray);

    if (entry.type == MemoryEntry::Type::Procedure)
    {
        QJsonObject procedureObject;
        procedureObject.insert(QStringLiteral("name"), entry.procedure.name);
        procedureObject.insert(QStringLiteral("trigger"), entry.procedure.trigger);

        QJsonArray stepArray;

        for (const QString &step : entry.procedure.steps)
        {
            stepArray.append(step);
        }

        procedureObject.insert(QStringLiteral("steps"), stepArray);

        QJsonArray prerequisiteArray;

        for (const QString &prerequisite : entry.procedure.prerequisites)
        {
            prerequisiteArray.append(prerequisite);
        }

        procedureObject.insert(QStringLiteral("prerequisites"), prerequisiteArray);

        QJsonArray warningArray;

        for (const QString &warning : entry.procedure.warnings)
        {
            warningArray.append(warning);
        }

        procedureObject.insert(QStringLiteral("warnings"), warningArray);
        object.insert(QStringLiteral("procedure"), procedureObject);
    }

    object.insert(QStringLiteral("createdAt"), entry.createdAt);
    object.insert(QStringLiteral("updatedAt"), entry.updatedAt);
    object.insert(QStringLiteral("lastAccessed"), entry.lastAccessed);
    object.insert(QStringLiteral("confidenceUpdatedAt"), entry.confidenceUpdatedAt);
    object.insert(QStringLiteral("accessCount"), static_cast<double>(entry.accessCount));
    object.insert(QStringLiteral("strength"), static_cast<double>(entry.strength));
    object.insert(QStringLiteral("confidence"), static_cast<double>(entry.confidence));
    object.insert(QStringLiteral("trustScore"), static_cast<double>(entry.trustScore));
    object.insert(QStringLiteral("active"), entry.active);
    return object;
}

/**
 * @brief 将 JSON 对象解析为条目；未知字段忽略、缺失字段使用默认值
 */
MemoryEntry EntryFromJson(const QJsonObject &object)
{
    MemoryEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString().trimmed();
    entry.content = object.value(QStringLiteral("content")).toString().trimmed();
    entry.category = object.value(QStringLiteral("category")).toString();
    entry.type = TypeFromString(object.value(QStringLiteral("type")).toString());
    entry.scope = ScopeFromString(object.value(QStringLiteral("scope")).toString());
    entry.provenance = ProvenanceFromString(
        object.value(QStringLiteral("provenance")).toString());
    entry.petId = object.value(QStringLiteral("petId")).toString();

    const QJsonArray tagArray = object.value(QStringLiteral("tags")).toArray();

    for (const QJsonValue &tagValue : tagArray)
    {
        const QString tag = tagValue.toString().trimmed();

        if (!tag.isEmpty())
        {
            entry.tags.append(tag);
        }
    }

    const QJsonArray triggerPatternArray = object.value(QStringLiteral("triggerPatterns")).toArray();

    for (const QJsonValue &triggerPatternValue : triggerPatternArray)
    {
        const QString triggerPattern = triggerPatternValue.toString().trimmed();

        if (!triggerPattern.isEmpty() && !entry.triggerPatterns.contains(triggerPattern))
        {
            entry.triggerPatterns.append(triggerPattern);
        }
    }

    if (entry.type == MemoryEntry::Type::Procedure)
    {
        const QJsonObject procedureObject = object.value(QStringLiteral("procedure")).toObject();
        entry.procedure.name = procedureObject.value(QStringLiteral("name")).toString().trimmed();
        entry.procedure.trigger = procedureObject.value(QStringLiteral("trigger")).toString().trimmed();

        for (const QJsonValue &stepValue : procedureObject.value(QStringLiteral("steps")).toArray())
        {
            const QString step = stepValue.toString().trimmed();

            if (!step.isEmpty())
            {
                entry.procedure.steps.append(step);
            }
        }

        for (const QJsonValue &prerequisiteValue :
             procedureObject.value(QStringLiteral("prerequisites")).toArray())
        {
            const QString prerequisite = prerequisiteValue.toString().trimmed();

            if (!prerequisite.isEmpty())
            {
                entry.procedure.prerequisites.append(prerequisite);
            }
        }

        for (const QJsonValue &warningValue :
             procedureObject.value(QStringLiteral("warnings")).toArray())
        {
            const QString warning = warningValue.toString().trimmed();

            if (!warning.isEmpty())
            {
                entry.procedure.warnings.append(warning);
            }
        }
    }

    entry.createdAt = static_cast<qint64>(object.value(QStringLiteral("createdAt")).toDouble());
    entry.updatedAt = static_cast<qint64>(object.value(QStringLiteral("updatedAt")).toDouble());
    entry.lastAccessed = static_cast<qint64>(
        object.value(QStringLiteral("lastAccessed")).toDouble());
    entry.confidenceUpdatedAt = static_cast<qint64>(
        object.value(QStringLiteral("confidenceUpdatedAt")).toDouble());
    entry.accessCount = static_cast<quint32>(
        object.value(QStringLiteral("accessCount")).toDouble());
    entry.strength = static_cast<quint32>(object.value(QStringLiteral("strength")).toDouble());
    entry.confidence = static_cast<float>(object.value(QStringLiteral("confidence")).toDouble(1.0));
    entry.trustScore = static_cast<float>(object.value(QStringLiteral("trustScore")).toDouble(1.0));
    entry.active = object.value(QStringLiteral("active")).toBool(true);

    if ((entry.scope == MemoryEntry::Scope::Pet) && entry.petId.isEmpty())
    {
        entry.petId = DEFAULT_PET_ID;
    }

    if (entry.scope == MemoryEntry::Scope::Global)
    {
        entry.petId.clear();
    }

    return entry;
}

/**
 * @brief 将边序列化为 JSON 对象
 */
QJsonObject EdgeToJson(const MemoryEdge &edge)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), edge.id);
    object.insert(QStringLiteral("sourceId"), edge.sourceId);
    object.insert(QStringLiteral("targetId"), edge.targetId);
    QString type;

    switch (edge.type)
    {
        case MemoryEdge::Type::Explicit:
            type = QStringLiteral("explicit");
            break;
        case MemoryEdge::Type::Related:
            type = QStringLiteral("related");
            break;
        case MemoryEdge::Type::TagShared:
            type = QStringLiteral("tag_shared");
            break;
        case MemoryEdge::Type::Supersedes:
            type = QStringLiteral("supersedes");
            break;
        case MemoryEdge::Type::Conflict:
            type = QStringLiteral("conflict");
            break;
    }

    object.insert(QStringLiteral("type"), type);
    object.insert(QStringLiteral("tag"), edge.tag);
    object.insert(QStringLiteral("weight"), static_cast<double>(edge.weight));
    object.insert(QStringLiteral("createdAt"),
                  static_cast<double>(edge.createdAt));
    object.insert(QStringLiteral("active"), edge.active);
    return object;
}

/**
 * @brief 将 JSON 对象解析为边；未知字段忽略、缺失字段使用默认值
 */
MemoryEdge EdgeFromJson(const QJsonObject &object)
{
    MemoryEdge edge;
    edge.id = object.value(QStringLiteral("id")).toString().trimmed();
    edge.sourceId = object.value(QStringLiteral("sourceId")).toString().trimmed();
    edge.targetId = object.value(QStringLiteral("targetId")).toString().trimmed();
    const QString type = object.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("explicit"))
    {
        edge.type = MemoryEdge::Type::Explicit;
    }
    else if (type == QStringLiteral("related"))
    {
        edge.type = MemoryEdge::Type::Related;
    }
    else if (type == QStringLiteral("supersedes"))
    {
        edge.type = MemoryEdge::Type::Supersedes;
    }
    else if (type == QStringLiteral("conflict"))
    {
        edge.type = MemoryEdge::Type::Conflict;
    }
    else
    {
        edge.type = MemoryEdge::Type::TagShared;
    }
    edge.tag = object.value(QStringLiteral("tag")).toString().trimmed();
    edge.weight = static_cast<float>(object.value(QStringLiteral("weight")).toDouble(1.0));
    edge.createdAt = static_cast<qint64>(object.value(QStringLiteral("createdAt")).toDouble());
    edge.active = object.value(QStringLiteral("active")).toBool(true);
    return edge;
}

} // anonymous namespace

bool MemoryRepository::SetDataDir(const QString &dataDir, QString &errorMessage)
{
    QString resolvedDir = dataDir.trimmed();

    if (resolvedDir.isEmpty())
    {
        resolvedDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }

    if (resolvedDir.isEmpty())
    {
        errorMessage = QStringLiteral("Memory data directory is unavailable.");
        return false;
    }

    const QString memoryDir = QDir(resolvedDir).filePath(MEMORY_DIR_NAME);

    if (!QDir().mkpath(memoryDir))
    {
        errorMessage = QStringLiteral("Failed to create memory data directory: %1")
                            .arg(memoryDir);
        return false;
    }

    m_dataDir = QDir(resolvedDir).absolutePath();
    return true;
}

QString MemoryRepository::MemoryDir() const
{
    return QDir(m_dataDir).filePath(MEMORY_DIR_NAME);
}

QString MemoryRepository::GraphFilePath() const
{
    return QDir(MemoryDir()).filePath(GRAPH_FILE_NAME);
}

MemoryRepository::_tagLoadResult MemoryRepository::Load(MemoryGraph &graph)
{
    _tagLoadResult result;
    const QString graphPath = GraphFilePath();

    if (!QFileInfo::exists(graphPath))
    {
        graph.Clear();
        result.ok = true;
        result.detail = QStringLiteral("Memory graph file does not exist yet.");
        return result;
    }

    QFile graphFile(graphPath);

    if (!graphFile.open(QIODevice::ReadOnly))
    {
        graph.Clear();
        result.recovered = true;
        result.detail = QStringLiteral("Failed to open memory graph file for reading.");
        return result;
    }

    const QByteArray jsonData = graphFile.readAll();
    graphFile.close();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(jsonData, &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        const QString backupPath = graphPath
                                   + QStringLiteral(".corrupt.")
                                   + QString::number(QDateTime::currentMSecsSinceEpoch());
        QFile::copy(graphPath, backupPath);
        graph.Clear();
        result.recovered = true;
        result.detail = QStringLiteral("Memory graph JSON is corrupt; backup kept at %1.")
                            .arg(backupPath);
        result.backupPath = backupPath;
        return result;
    }

    const QJsonObject rootObject = document.object();
    const int schemaVersion = rootObject.value(QStringLiteral("schemaVersion")).toInt(0);

    if ((schemaVersion < FIRST_SUPPORTED_SCHEMA_VERSION)
        || (schemaVersion > CURRENT_SCHEMA_VERSION))
    {
        const QString backupPath = graphPath
                                   + QStringLiteral(".corrupt.")
                                   + QString::number(QDateTime::currentMSecsSinceEpoch());
        QFile::copy(graphPath, backupPath);
        graph.Clear();
        result.recovered = true;
        result.detail = QStringLiteral(
                            "Memory graph schema version %1 is unsupported; backup kept at %2.")
                            .arg(schemaVersion)
                            .arg(backupPath);
        result.backupPath = backupPath;
        return result;
    }

    const QJsonValue entriesValue = rootObject.value(QStringLiteral("entries"));

    if (!entriesValue.isArray())
    {
        const QString backupPath = graphPath
                                   + QStringLiteral(".corrupt.")
                                   + QString::number(QDateTime::currentMSecsSinceEpoch());
        QFile::copy(graphPath, backupPath);
        graph.Clear();
        result.recovered = true;
        result.detail = QStringLiteral("Memory graph entries field is missing or invalid; backup kept at %1.")
                            .arg(backupPath);
        result.backupPath = backupPath;
        return result;
    }

    const QJsonArray entriesArray = entriesValue.toArray();

    QVector<MemoryEntry> entries;

    for (const QJsonValue &entryValue : entriesArray)
    {
        if (!entryValue.isObject())
        {
            const QString backupPath = graphPath
                                       + QStringLiteral(".corrupt.")
                                       + QString::number(QDateTime::currentMSecsSinceEpoch());
            QFile::copy(graphPath, backupPath);
            graph.Clear();
            result.recovered = true;
            result.detail = QStringLiteral("Memory graph contains an invalid entry; backup kept at %1.")
                                .arg(backupPath);
            result.backupPath = backupPath;
            return result;
        }

        QString jsonError;

        if (!ValidateEntryJson(entryValue.toObject(), jsonError))
        {
            const QString backupPath = graphPath
                                       + QStringLiteral(".corrupt.")
                                       + QString::number(QDateTime::currentMSecsSinceEpoch());
            QFile::copy(graphPath, backupPath);
            graph.Clear();
            result.recovered = true;
            result.detail = QStringLiteral("%1 Backup kept at %2.")
                                .arg(jsonError)
                                .arg(backupPath);
            result.backupPath = backupPath;
            return result;
        }

        MemoryEntry entry = EntryFromJson(entryValue.toObject());
        QString privacyCategory;

        if (!ValidateEntry(entry, privacyCategory))
        {
            const QString backupPath = graphPath
                                       + QStringLiteral(".corrupt.")
                                       + QString::number(QDateTime::currentMSecsSinceEpoch());
            QFile::copy(graphPath, backupPath);
            graph.Clear();
            result.recovered = true;
            result.detail = QStringLiteral("Memory graph entry failed privacy validation (%1); backup kept at %2.")
                                .arg(privacyCategory)
                                .arg(backupPath);
            result.backupPath = backupPath;
            return result;
        }

        entries.append(entry);
    }

    QString loadError;

    if (!graph.LoadEntries(entries, loadError))
    {
        const QString backupPath = graphPath
                                   + QStringLiteral(".corrupt.")
                                   + QString::number(QDateTime::currentMSecsSinceEpoch());
        QFile::copy(graphPath, backupPath);
        graph.Clear();
        result.recovered = true;
        result.detail = QStringLiteral("Memory graph load failed: %1; backup kept at %2.")
                            .arg(loadError)
                            .arg(backupPath);
        result.backupPath = backupPath;
        return result;
    }

    if (schemaVersion >= 2)
    {
        const QJsonValue edgesValue = rootObject.value(QStringLiteral("edges"));

        if (!edgesValue.isArray())
        {
            const QString backupPath = graphPath
                                       + QStringLiteral(".corrupt.")
                                       + QString::number(QDateTime::currentMSecsSinceEpoch());
            QFile::copy(graphPath, backupPath);
            graph.Clear();
            result.recovered = true;
            result.detail = QStringLiteral("Memory graph edges field is missing or invalid; backup kept at %1.")
                                .arg(backupPath);
            result.backupPath = backupPath;
            return result;
        }

        const QJsonArray edgesArray = edgesValue.toArray();
        QVector<MemoryEdge> edges;

        for (const QJsonValue &edgeValue : edgesArray)
        {
            if (!edgeValue.isObject())
            {
                const QString backupPath = graphPath
                                           + QStringLiteral(".corrupt.")
                                           + QString::number(QDateTime::currentMSecsSinceEpoch());
                QFile::copy(graphPath, backupPath);
                graph.Clear();
                result.recovered = true;
                result.detail = QStringLiteral("Memory graph contains an invalid edge; backup kept at %1.")
                                    .arg(backupPath);
                result.backupPath = backupPath;
                return result;
            }

            QString jsonError;

            if (!ValidateEdgeJson(edgeValue.toObject(), jsonError))
            {
                const QString backupPath = graphPath
                                           + QStringLiteral(".corrupt.")
                                           + QString::number(QDateTime::currentMSecsSinceEpoch());
                QFile::copy(graphPath, backupPath);
                graph.Clear();
                result.recovered = true;
                result.detail = QStringLiteral("%1 Backup kept at %2.")
                                    .arg(jsonError)
                                    .arg(backupPath);
                result.backupPath = backupPath;
                return result;
            }

            edges.append(EdgeFromJson(edgeValue.toObject()));
        }

        if (!graph.LoadEdges(edges, loadError))
        {
            const QString backupPath = graphPath
                                       + QStringLiteral(".corrupt.")
                                       + QString::number(QDateTime::currentMSecsSinceEpoch());
            QFile::copy(graphPath, backupPath);
            graph.Clear();
            result.recovered = true;
            result.detail = QStringLiteral("Memory graph edge load failed: %1; backup kept at %2.")
                                .arg(loadError)
                                .arg(backupPath);
            result.backupPath = backupPath;
            return result;
        }
    }

    result.ok = true;
    result.detail = QStringLiteral("Memory graph loaded: %1 entries, %2 edges.")
                        .arg(graph.ActiveEntryCount())
                        .arg(graph.AllEdges().size());
    return result;
}

bool MemoryRepository::Save(const MemoryGraph &graph, QString &errorMessage) const
{
    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("schemaVersion"), CURRENT_SCHEMA_VERSION);

    QJsonArray entriesArray;

    for (const MemoryEntry &entry : graph.AllEntries())
    {
        entriesArray.append(EntryToJson(entry));
    }

    rootObject.insert(QStringLiteral("entries"), entriesArray);

    QJsonArray edgesArray;

    for (const MemoryEdge &edge : graph.AllEdges())
    {
        edgesArray.append(EdgeToJson(edge));
    }

    rootObject.insert(QStringLiteral("edges"), edgesArray);
    rootObject.insert(QStringLiteral("updatedAt"),
                      static_cast<double>(QDateTime::currentMSecsSinceEpoch()));

    QSaveFile saveFile(GraphFilePath());

    if (!saveFile.open(QIODevice::WriteOnly))
    {
        errorMessage = QStringLiteral("Failed to open memory graph for writing.");
        return false;
    }

    const QByteArray jsonData = QJsonDocument(rootObject).toJson(QJsonDocument::Indented);

    if (saveFile.write(jsonData) != jsonData.size())
    {
        saveFile.cancelWriting();
        errorMessage = QStringLiteral("Failed to write memory graph data.");
        return false;
    }

    if (!saveFile.commit())
    {
        errorMessage = QStringLiteral("Failed to commit memory graph file.");
        return false;
    }

    return true;
}

bool MemoryRepository::Export(const MemoryGraph &graph,
                              const QString &filePath,
                              QString &errorMessage) const
{
    const QString normalizedPath = filePath.trimmed();

    if (normalizedPath.isEmpty())
    {
        errorMessage = QStringLiteral("Memory export path is empty.");
        return false;
    }

    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("schemaVersion"), CURRENT_SCHEMA_VERSION);

    QJsonArray entriesArray;

    for (const MemoryEntry &entry : graph.AllEntries())
    {
        entriesArray.append(EntryToJson(entry));
    }

    rootObject.insert(QStringLiteral("entries"), entriesArray);

    QJsonArray edgesArray;

    for (const MemoryEdge &edge : graph.AllEdges())
    {
        edgesArray.append(EdgeToJson(edge));
    }

    rootObject.insert(QStringLiteral("edges"), edgesArray);
    rootObject.insert(QStringLiteral("updatedAt"),
                      static_cast<double>(QDateTime::currentMSecsSinceEpoch()));

    QSaveFile saveFile(normalizedPath);

    if (!saveFile.open(QIODevice::WriteOnly))
    {
        errorMessage = QStringLiteral("Failed to open memory export file.");
        return false;
    }

    const QByteArray jsonData = QJsonDocument(rootObject).toJson(QJsonDocument::Indented);

    if (saveFile.write(jsonData) != jsonData.size())
    {
        saveFile.cancelWriting();
        errorMessage = QStringLiteral("Failed to write memory export file.");
        return false;
    }

    if (!saveFile.commit())
    {
        errorMessage = QStringLiteral("Failed to commit memory export file.");
        return false;
    }

    return true;
}

bool MemoryRepository::Import(const QString &filePath,
                              MemoryGraph &graph,
                              QString &errorMessage) const
{
    const QString normalizedPath = filePath.trimmed();

    if (normalizedPath.isEmpty())
    {
        errorMessage = QStringLiteral("Memory import path is empty.");
        return false;
    }

    QFile importFile(normalizedPath);

    if (!importFile.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("Failed to open memory import file.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(importFile.readAll(), &parseError);

    if ((parseError.error != QJsonParseError::NoError) || !document.isObject())
    {
        errorMessage = QStringLiteral("Memory import JSON is invalid.");
        return false;
    }

    const QJsonObject rootObject = document.object();
    const int schemaVersion = rootObject.value(QStringLiteral("schemaVersion")).toInt(0);

    if ((schemaVersion < FIRST_SUPPORTED_SCHEMA_VERSION)
        || (schemaVersion > CURRENT_SCHEMA_VERSION))
    {
        errorMessage = QStringLiteral("Memory import schema version is unsupported.");
        return false;
    }

    if (!rootObject.contains(QStringLiteral("entries"))
        || !rootObject.value(QStringLiteral("entries")).isArray())
    {
        errorMessage = QStringLiteral("Memory import entries field is missing.");
        return false;
    }

    QVector<MemoryEntry> entries;

    for (const QJsonValue &entryValue : rootObject.value(QStringLiteral("entries")).toArray())
    {
        if (!entryValue.isObject())
        {
            errorMessage = QStringLiteral("Memory import contains an invalid entry.");
            return false;
        }

        QString jsonError;

        if (!ValidateEntryJson(entryValue.toObject(), jsonError))
        {
            errorMessage = jsonError;
            return false;
        }

        MemoryEntry entry = EntryFromJson(entryValue.toObject());
        QString privacyCategory;

        if (!ValidateEntry(entry, privacyCategory))
        {
            errorMessage = QStringLiteral("Memory import entry rejected: %1")
                               .arg(privacyCategory);
            return false;
        }

        entries.append(entry);
    }

    MemoryGraph importedGraph;
    QString graphError;

    if (!importedGraph.LoadEntries(entries, graphError))
    {
        errorMessage = QStringLiteral("Memory import entries are invalid: %1").arg(graphError);
        return false;
    }

    if (schemaVersion >= 2)
    {
        const QJsonValue edgesValue = rootObject.value(QStringLiteral("edges"));

        if (!edgesValue.isArray())
        {
            errorMessage = QStringLiteral("Memory import edges field is missing or invalid.");
            return false;
        }

        QVector<MemoryEdge> edges;

        for (const QJsonValue &edgeValue : edgesValue.toArray())
        {
            if (!edgeValue.isObject())
            {
                errorMessage = QStringLiteral("Memory import contains an invalid edge.");
                return false;
            }

            QString jsonError;

            if (!ValidateEdgeJson(edgeValue.toObject(), jsonError))
            {
                errorMessage = jsonError;
                return false;
            }

            edges.append(EdgeFromJson(edgeValue.toObject()));
        }

        if (!importedGraph.LoadEdges(edges, graphError))
        {
            errorMessage = QStringLiteral("Memory import edges are invalid: %1").arg(graphError);
            return false;
        }
    }

    graph = std::move(importedGraph);
    return true;
}

bool MemoryRepository::ValidateContent(const QString &content, QString &errorCategory)
{
    if (content.isEmpty())
    {
        errorCategory = QStringLiteral("empty");
        return false;
    }

    if (content.size() > MAX_MEMORY_CONTENT_CHARS)
    {
        errorCategory = QStringLiteral("too_long");
        return false;
    }

    static const QVector<QPair<QString, QString>> patterns =
    {
        { QStringLiteral("credential"),
          QStringLiteral("(?i)\\b(api[_-]?key|access[_-]?token|client[_-]?secret|"
                         "password|passwd|secret|bearer\\s+[a-z0-9._-]+)\\b") },
        { QStringLiteral("private_key"),
          QStringLiteral("-----BEGIN\\s+(RSA|EC|OPENSSH|DSA|PGP|PRIVATE)\\s+KEY-----") },
        { QStringLiteral("jwt"),
          QStringLiteral("eyJ[a-zA-Z0-9_-]{8,}\\.[a-zA-Z0-9_-]{8,}\\.[a-zA-Z0-9_-]{8,}") },
        { QStringLiteral("env_file"),
          QStringLiteral("(?m)^\\s*[A-Z_][A-Z0-9_]*\\s*=\\s*\\S{8,}\\s*$") },
        { QStringLiteral("id_card"),
          QStringLiteral("\\b(?:\\d{17}[\\dXx]|\\d{15})\\b") },
        { QStringLiteral("bank_card"),
          QStringLiteral("\\b\\d{16,19}\\b") }
    };

    for (const auto &patternEntry : patterns)
    {
        const QRegularExpression pattern(patternEntry.second);

        if (pattern.match(content).hasMatch())
        {
            errorCategory = patternEntry.first;
            return false;
        }
    }

    return true;
}

bool MemoryRepository::ValidateEntry(const MemoryEntry &entry, QString &errorCategory)
{
    if (!ValidateContent(entry.content, errorCategory))
    {
        return false;
    }

    QStringList optionalFields;
    optionalFields.append(entry.category);
    optionalFields.append(entry.tags);
    optionalFields.append(entry.triggerPatterns);

    if (entry.type == MemoryEntry::Type::Procedure)
    {
        optionalFields.append(entry.procedure.name);
        optionalFields.append(entry.procedure.trigger);
        optionalFields.append(entry.procedure.steps);
        optionalFields.append(entry.procedure.prerequisites);
        optionalFields.append(entry.procedure.warnings);
    }

    int totalChars = entry.content.size();

    for (const QString &field : optionalFields)
    {
        totalChars += field.size();

        if (totalChars > MAX_MEMORY_CONTENT_CHARS)
        {
            errorCategory = QStringLiteral("too_long");
            return false;
        }

        if (!field.trimmed().isEmpty() && !ValidateContent(field, errorCategory))
        {
            return false;
        }
    }

    return true;
}

} // namespace vpet
