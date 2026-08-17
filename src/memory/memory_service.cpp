#include "vpet/memory/memory_service.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QThread>

#include <algorithm>

namespace vpet
{

namespace
{

constexpr int DEFAULT_SHUTDOWN_TIMEOUT_MS = 2000;
constexpr int WORKER_SEARCH_LIMIT = 32; ///< worker 检索上限；组装时再按配置截断
constexpr int GRAPH_EXPAND_MAX_DEPTH = 2; ///< 图 BFS 最大传播深度
constexpr float GRAPH_EXPAND_DECAY_PER_HOP = 0.5f; ///< 每跳得分衰减
constexpr qint64 EMBEDDER_RETRY_INTERVAL_MS = 60000; ///< 加载失败后的重试间隔
constexpr float CONSOLIDATION_DUPLICATE_SIMILARITY = 0.80f;
const QString VECTORS_DB_FILE_NAME = QStringLiteral("vectors.sqlite3"); ///< 向量库文件名
const QString DEFAULT_QUERY_INSTRUCTION =
    QStringLiteral("为这个句子生成表示以用于检索相关文章："); ///< BGE 查询指令默认值

/**
 * @brief 将记忆类型转换为字符串（供配置与日志使用）
 */
QString ActionToString(MemoryService::Action action)
{
    switch (action)
    {
        case MemoryService::Action::Retrieve:
            return QStringLiteral("retrieve");
        case MemoryService::Action::Store:
            return QStringLiteral("store");
        case MemoryService::Action::Forget:
            return QStringLiteral("forget");
        case MemoryService::Action::Tag:
            return QStringLiteral("tag");
        case MemoryService::Action::List:
            return QStringLiteral("list");
        case MemoryService::Action::Consolidate:
            return QStringLiteral("consolidate");
        case MemoryService::Action::Feedback:
            return QStringLiteral("feedback");
        case MemoryService::Action::Update:
            return QStringLiteral("update");
        case MemoryService::Action::Export:
            return QStringLiteral("export");
        case MemoryService::Action::Import:
            return QStringLiteral("import");
    }

    return QStringLiteral("unknown");
}

/**
 * @brief 读取 JSON 整数字段
 */
int ReadInt(const QJsonObject &object, const QString &key, int defaultValue)
{
    const QJsonValue value = object.value(key);

    if (value.isUndefined() || !value.isDouble())
    {
        return defaultValue;
    }

    return value.toInt(defaultValue);
}

/**
 * @brief 读取 JSON 浮点字段
 * @param[in] object JSON 对象
 * @param[in] key 字段名
 * @param[in] defaultValue 默认值
 * @return 字段值或默认值
 */
float ReadFloat(const QJsonObject &object, const QString &key, float defaultValue)
{
    const QJsonValue value = object.value(key);

    if (value.isUndefined() || !value.isDouble())
    {
        return defaultValue;
    }

    return static_cast<float>(value.toDouble(defaultValue));
}

/**
 * @brief 解析并校验检索后维护配置
 * @param[in] rootObject 记忆配置根对象
 * @param[out] config 输出维护配置
 * @param[out] errorMessage 错误描述
 * @return 配置有效返回 true
 */
bool ParseMaintenanceConfig(const QJsonObject &rootObject,
                            _tagMemoryMaintenanceConfig &config,
                            QString &errorMessage)
{
    const QJsonValue maintenanceValue = rootObject.value(QStringLiteral("maintenance"));

    if (maintenanceValue.isUndefined())
    {
        return true;
    }

    if (!maintenanceValue.isObject())
    {
        errorMessage = QStringLiteral("Memory maintenance config must be an object.");
        return false;
    }

    const QJsonObject maintenanceObject = maintenanceValue.toObject();
    const QJsonValue enabledValue = maintenanceObject.value(QStringLiteral("enabled"));

    if (!enabledValue.isUndefined() && !enabledValue.isBool())
    {
        errorMessage = QStringLiteral("Memory maintenance enabled must be a boolean.");
        return false;
    }

    config.enabled = enabledValue.toBool(true);
    config.decayIntervalHours = ReadInt(maintenanceObject,
                                        QStringLiteral("decay_interval_hours"),
                                        24);
    config.clusterUpdateRetrievals = ReadInt(maintenanceObject,
                                             QStringLiteral("cluster_update_retrievals"),
                                             20);
    config.maxGapRecords = ReadInt(maintenanceObject,
                                   QStringLiteral("max_gap_records"),
                                   128);
    config.inferredTagMinSupport = ReadInt(maintenanceObject,
                                           QStringLiteral("inferred_tag_min_support"),
                                           2);
    config.deepMaintenanceRetrievals = ReadInt(maintenanceObject,
                                                QStringLiteral("deep_maintenance_retrievals"),
                                                50);
    config.duplicateSimilarityThreshold = ReadFloat(
        maintenanceObject,
        QStringLiteral("duplicate_similarity_threshold"),
        0.95f);
    config.weakConfidenceThreshold = ReadFloat(maintenanceObject,
                                                QStringLiteral("weak_confidence_threshold"),
                                                0.05f);
    config.weakStrengthLimit = static_cast<quint32>(ReadInt(
        maintenanceObject,
        QStringLiteral("weak_strength_limit"),
        1));
    config.relatedInitialWeight = ReadFloat(maintenanceObject,
                                            QStringLiteral("related_initial_weight"),
                                            0.4f);
    config.relatedWeightIncrement = ReadFloat(maintenanceObject,
                                              QStringLiteral("related_weight_increment"),
                                              0.1f);
    config.maxRelatedWeight = ReadFloat(maintenanceObject,
                                        QStringLiteral("max_related_weight"),
                                        1.0f);

    MemoryMaintenance validator;
    return validator.SetConfig(config, errorMessage);
}

} // anonymous namespace

MemoryService::MemoryService(QObject *parent)
    : QObject(parent)
    , m_workerThread(nullptr)
    , m_queueCapacity(0)
{
    m_nextRequestId.storeRelease(1);
}

MemoryService::~MemoryService()
{
    Shutdown(DEFAULT_SHUTDOWN_TIMEOUT_MS);
}

bool MemoryService::ParseConfig(const QString &configPath,
                                MemoryConfig &config,
                                QString &errorMessage)
{
    QFile configFile(configPath);

    if (!configFile.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("Failed to open memory config: %1").arg(configPath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(configFile.readAll(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = QStringLiteral("Memory config JSON is invalid: %1").arg(configPath);
        return false;
    }

    const QJsonObject rootObject = document.object();

    if (rootObject.contains(QStringLiteral("enabled")))
    {
        const QJsonValue enabledValue = rootObject.value(QStringLiteral("enabled"));

        if (!enabledValue.isBool())
        {
            errorMessage = QStringLiteral("Memory config enabled must be a boolean.");
            return false;
        }

        config.enabled = enabledValue.toBool();
    }

    config.dataDir = rootObject.value(QStringLiteral("data_dir")).toString().trimmed();

    const int queueCapacity = ReadInt(rootObject, QStringLiteral("queue_capacity"), 64);

    if (queueCapacity <= 0)
    {
        errorMessage = QStringLiteral("Memory config queue_capacity must be positive.");
        return false;
    }

    config.queueCapacity = queueCapacity;

    const int maxResults = ReadInt(rootObject, QStringLiteral("max_results"), 6);

    if (maxResults <= 0)
    {
        errorMessage = QStringLiteral("Memory config max_results must be positive.");
        return false;
    }

    config.maxResults = maxResults;

    const int promptBudgetChars = ReadInt(rootObject,
                                          QStringLiteral("prompt_budget_chars"),
                                          1200);

    if (promptBudgetChars <= 0)
    {
        errorMessage = QStringLiteral("Memory config prompt_budget_chars must be positive.");
        return false;
    }

    config.promptBudgetChars = promptBudgetChars;
    config.defaultScope = rootObject.value(QStringLiteral("default_scope")).toString().trimmed();

    if (config.defaultScope.isEmpty())
    {
        config.defaultScope = QStringLiteral("pet");
    }

    if ((config.defaultScope != QStringLiteral("pet"))
        && (config.defaultScope != QStringLiteral("global")))
    {
        errorMessage = QStringLiteral("Memory config default_scope must be pet or global.");
        return false;
    }

    const QJsonValue automaticValue = rootObject.value(QStringLiteral("automatic_extraction"));

    if (automaticValue.isBool())
    {
        config.automaticExtraction = automaticValue.toBool();
    }

    const int consolidationMaxCandidates = ReadInt(rootObject,
                                                   QStringLiteral("consolidation_max_candidates"),
                                                   4);

    if ((consolidationMaxCandidates <= 0) || (consolidationMaxCandidates > 8))
    {
        errorMessage = QStringLiteral("Memory config consolidation_max_candidates must be 1 to 8.");
        return false;
    }

    config.consolidationMaxCandidates = consolidationMaxCandidates;

    if (!ParseMaintenanceConfig(rootObject, config.maintenance, errorMessage))
    {
        return false;
    }

    const QJsonValue embeddingValue = rootObject.value(QStringLiteral("embedding"));

    if (embeddingValue.isObject())
    {
        const QJsonObject embeddingObject = embeddingValue.toObject();
        EmbeddingConfig &embedding = config.embedding;
        embedding.enabled = embeddingObject.value(QStringLiteral("enabled")).toBool(false);
        embedding.backend = embeddingObject.value(QStringLiteral("backend"))
                                .toString(QStringLiteral("local_onnx"))
                                .trimmed();
        embedding.model = embeddingObject.value(QStringLiteral("model")).toString().trimmed();
        embedding.modelDir = embeddingObject.value(QStringLiteral("model_dir")).toString().trimmed();
        embedding.onnxModel = embeddingObject.value(QStringLiteral("onnx_model"))
                                  .toString(QStringLiteral("model.onnx"))
                                  .trimmed();
        embedding.tokenizerFile = embeddingObject.value(QStringLiteral("tokenizer_file"))
                                      .toString(QStringLiteral("tokenizer.json"))
                                      .trimmed();
        embedding.vectorStore = embeddingObject.value(QStringLiteral("vector_store"))
                                    .toString(QStringLiteral("sqlite"))
                                    .trimmed();
        embedding.vectorDb = embeddingObject.value(QStringLiteral("vector_db")).toString().trimmed();

        const int maxSequenceLength = ReadInt(embeddingObject,
                                              QStringLiteral("max_sequence_length"),
                                              512);

        if (maxSequenceLength <= 0)
        {
            errorMessage = QStringLiteral("Memory config max_sequence_length must be positive.");
            return false;
        }

        embedding.maxSequenceLength = maxSequenceLength;
        embedding.device = embeddingObject.value(QStringLiteral("device"))
                               .toString(QStringLiteral("cpu"))
                               .trimmed();
        embedding.queryInstruction = embeddingObject.value(QStringLiteral("query_instruction"))
                                         .toString(DEFAULT_QUERY_INSTRUCTION)
                                         .trimmed();

        if (embedding.queryInstruction.isEmpty())
        {
            embedding.queryInstruction = DEFAULT_QUERY_INSTRUCTION;
        }

        if (embedding.enabled && (embedding.backend != QStringLiteral("local_onnx")))
        {
            errorMessage = QStringLiteral("Memory config embedding backend must be local_onnx.");
            return false;
        }

        if (embedding.enabled && embedding.model.isEmpty())
        {
            errorMessage = QStringLiteral("Memory config embedding model must not be empty.");
            return false;
        }

        if (embedding.enabled && (embedding.device != QStringLiteral("cpu")))
        {
            errorMessage = QStringLiteral("Memory config embedding device must be cpu.");
            return false;
        }

        if (embedding.enabled
            && (embedding.vectorStore != QStringLiteral("sqlite")))
        {
            errorMessage = QStringLiteral("Memory config embedding vector_store must be sqlite.");
            return false;
        }
    }

    return true;
}

QString MemoryService::BuildPromptSection(const QVector<MemoryEntry> &entries,
                                          int maxResults,
                                          int budgetChars)
{
    if (entries.isEmpty() || maxResults <= 0 || budgetChars <= 0)
    {
        return QString();
    }

    const int entryLimit = static_cast<int>(qMin(entries.size(), maxResults));
    QStringList lines;
    int usedChars = 0;
    const QString header = QStringLiteral("[长期记忆]");

    for (int index = 0; index < entryLimit; ++index)
    {
        const MemoryEntry &entry = entries.at(index);
        QString line;

        if ((entry.type == MemoryEntry::Type::Procedure) && !entry.procedure.steps.isEmpty())
        {
            line = QStringLiteral("- [流程] %1：%2")
                       .arg(entry.procedure.name.isEmpty() ? entry.content : entry.procedure.name,
                            entry.procedure.steps.join(QStringLiteral("；")));
        }
        else if (entry.type == MemoryEntry::Type::Negative)
        {
            line = QStringLiteral("- [避免] %1").arg(entry.content);
        }
        else
        {
            line = QStringLiteral("- %1").arg(entry.content);
        }
        const int candidateChars = header.size() + 1 + usedChars + line.size() + 1;

        if (candidateChars > budgetChars)
        {
            continue;
        }

        lines.append(line);
        usedChars += line.size() + 1;
    }

    if (lines.isEmpty())
    {
        return QString();
    }

    QString result = header + QStringLiteral("\n") + lines.join(QStringLiteral("\n"));
    const int totalChars = usedChars + header.size() + 1;

    if (totalChars > budgetChars)
    {
        return QString();
    }

    return result;
}

bool MemoryService::SetEmbeddingConfig(const EmbeddingConfig &config, QString &errorMessage)
{
    QMutexLocker locker(&m_queueMutex);

    if (m_isRunning)
    {
        errorMessage = QStringLiteral("Embedding config must be set before Start.");
        return false;
    }

    if (config.enabled && (config.backend != QStringLiteral("local_onnx")))
    {
        errorMessage = QStringLiteral("Unsupported embedding backend: %1").arg(config.backend);
        return false;
    }

    m_embeddingConfig = config;
    m_embeddingConfigured = true;
    return true;
}

bool MemoryService::SetMaintenanceConfig(const _tagMemoryMaintenanceConfig &config,
                                         QString &errorMessage)
{
    QMutexLocker locker(&m_queueMutex);

    if (m_isRunning)
    {
        errorMessage = QStringLiteral("Memory maintenance config must be set before Start.");
        return false;
    }

    return m_maintenance.SetConfig(config, errorMessage);
}

void MemoryService::InstallEmbedderForTest(std::unique_ptr<MemoryEmbedder> embedder)
{
    QMutexLocker locker(&m_queueMutex);

    if (m_isRunning)
    {
        return;
    }

    m_embedder = std::move(embedder);
    m_embeddingConfigured = true;
    m_embeddingConfig.enabled = true;
}

bool MemoryService::Start(const QString &dataDir,
                          int queueCapacity,
                          QString &errorMessage)
{
    QMutexLocker locker(&m_queueMutex);

    if (m_isRunning)
    {
        errorMessage = QStringLiteral("Memory service is already running.");
        return false;
    }

    if (queueCapacity <= 0)
    {
        errorMessage = QStringLiteral("Memory service queue capacity must be positive.");
        return false;
    }

    if (m_workerThread != nullptr)
    {
        errorMessage = QStringLiteral("Memory service worker state is invalid.");
        return false;
    }

    m_embeddingEnabled = false;
    m_vectorDbPath.clear();

    if (!m_repository.SetDataDir(dataDir, errorMessage))
    {
        return false;
    }

    if (!m_maintenance.SetMemoryDir(m_repository.MemoryDir(), errorMessage))
    {
        return false;
    }

    const MemoryRepository::_tagLoadResult loadResult = m_repository.Load(m_graph);

    if (!loadResult.ok)
    {
        qWarning() << "[Memory]" << loadResult.detail;
        m_graph.Clear();
    }
    else if (!loadResult.detail.isEmpty())
    {
        qDebug() << "[Memory]" << loadResult.detail;
    }

    if (m_embeddingConfigured && m_embeddingConfig.enabled)
    {
        if (m_embedder == nullptr)
        {
            m_embedder = std::make_unique<EmbeddingClient>(
                m_embeddingConfig,
                [this](const QString &message) { EmitLogMessage(message); });
        }

        QString vectorDbPath = m_embeddingConfig.vectorDb.trimmed();

        if (vectorDbPath.isEmpty())
        {
            vectorDbPath = QDir(m_repository.MemoryDir()).filePath(VECTORS_DB_FILE_NAME);
        }
        else if (QDir::isRelativePath(vectorDbPath))
        {
            vectorDbPath = QDir(m_repository.MemoryDir()).filePath(vectorDbPath);
        }

        m_vectorDbPath = vectorDbPath;
        m_embeddingEnabled = true;
    }

    m_queueCapacity = queueCapacity;
    m_taskQueue.clear();
    m_vectorBackfillComplete = false;
    m_shuttingDown = false;
    m_isRunning = true;

    m_workerThread = QThread::create([this]() { WorkerLoop(); });
    m_workerThread->setObjectName(QStringLiteral("memory-worker"));
    m_workerThread->start();

    locker.unlock();
    emit LogMessage(QStringLiteral("Memory service started."));
    return true;
}

bool MemoryService::IsRunning() const
{
    QMutexLocker locker(&m_queueMutex);
    return m_isRunning;
}

void MemoryService::Shutdown(int timeoutMs)
{
    QThread *workerThread = nullptr;

    {
        QMutexLocker locker(&m_queueMutex);

        if (!m_isRunning || (m_workerThread == nullptr))
        {
            return;
        }

        // Stop admission first. The worker drains the already accepted queue and
        // returns only after the current task reaches its normal commit boundary.
        m_shuttingDown = true;
        m_isRunning = false;
        workerThread = m_workerThread;
        m_queueNotEmpty.wakeAll();
    }

    if (!workerThread->wait(timeoutMs))
    {
        qWarning() << "[Memory] Worker shutdown exceeded" << timeoutMs
                   << "ms; waiting for the current task to finish safely.";
        // Never force-stop or delete a live worker: memory graph and SQLite writes
        // must finish before their owning objects are destroyed.
        workerThread->wait();
    }

    {
        QMutexLocker locker(&m_queueMutex);
        if (m_workerThread == workerThread)
        {
            m_workerThread = nullptr;
        }
    }

    delete workerThread;
    emit LogMessage(QStringLiteral("Memory service stopped."));
}

bool MemoryService::TryEnqueueRetrieve(const QString &petId,
                                       const QString &triggerType,
                                       const QString &query,
                                       quint64 &requestId)
{
    if (query.trimmed().isEmpty())
    {
        return false;
    }

    _tagTask task;
    task.action = Action::Retrieve;
    task.petId = petId;
    task.triggerType = triggerType;
    task.query = query.trimmed();
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueStore(const QString &petId,
                                    const QString &triggerType,
                                    const MemoryEntry &entry,
                                    quint64 &requestId,
                                    QString &rejectCategory)
{
    QString category;

    if (!MemoryRepository::ValidateEntry(entry, category))
    {
        rejectCategory = category;
        EmitLogMessage(QStringLiteral("Memory store rejected by privacy filter: %1")
                           .arg(category));
        return false;
    }

    _tagTask task;
    task.action = Action::Store;
    task.petId = petId;
    task.triggerType = triggerType;
    task.entry = entry;
    task.entry.content = entry.content.trimmed();
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueConsolidation(
    const QString &petId,
    const QString &triggerType,
    const QVector<_tagMemoryConsolidationCandidate> &candidates,
    quint64 &requestId)
{
    if (candidates.isEmpty() || (candidates.size() > 8))
    {
        return false;
    }

    for (const _tagMemoryConsolidationCandidate &candidate : candidates)
    {
        QString rejectCategory;

        if (!MemoryRepository::ValidateEntry(candidate.entry, rejectCategory))
        {
            EmitLogMessage(QStringLiteral("Memory consolidation rejected by validation: %1")
                               .arg(rejectCategory));
            return false;
        }
    }

    _tagTask task;
    task.action = Action::Consolidate;
    task.petId = petId;
    task.triggerType = triggerType;
    task.candidates = candidates;
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueForget(const QString &petId,
                                     const QString &triggerType,
                                     const QString &memoryId,
                                     quint64 &requestId)
{
    if (memoryId.trimmed().isEmpty())
    {
        return false;
    }

    _tagTask task;
    task.action = Action::Forget;
    task.petId = petId;
    task.triggerType = triggerType;
    task.memoryId = memoryId.trimmed();
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueForgetByKeyword(const QString &petId,
                                              const QString &triggerType,
                                              const QString &query,
                                              quint64 &requestId)
{
    if (query.trimmed().isEmpty())
    {
        return false;
    }

    _tagTask task;
    task.action = Action::Forget;
    task.petId = petId;
    task.triggerType = triggerType;
    task.query = query.trimmed();
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueTag(const QString &petId,
                                  const QString &triggerType,
                                  const QString &memoryId,
                                  const QStringList &tags,
                                  quint64 &requestId)
{
    if (memoryId.trimmed().isEmpty())
    {
        return false;
    }

    QStringList normalizedTags;

    for (const QString &rawTag : tags)
    {
        const QString tag = rawTag.trimmed();

        if (tag.isEmpty())
        {
            continue;
        }

        QString rejectCategory;

        if (!MemoryRepository::ValidateContent(tag, rejectCategory))
        {
            EmitLogMessage(QStringLiteral("Memory tag rejected by privacy filter: %1")
                               .arg(rejectCategory));
            return false;
        }

        if (!normalizedTags.contains(tag))
        {
            normalizedTags.append(tag);
        }
    }

    _tagTask task;
    task.action = Action::Tag;
    task.petId = petId;
    task.triggerType = triggerType;
    task.memoryId = memoryId.trimmed();
    task.tags = normalizedTags;
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueList(const QString &petId,
                                   const QString &triggerType,
                                   MemoryEntry::Scope scope,
                                   quint64 &requestId)
{
    _tagTask task;
    task.action = Action::List;
    task.petId = petId;
    task.triggerType = triggerType;
    task.scope = scope;
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueFeedback(const QString &petId,
                                       const QString &triggerType,
                                       const QStringList &memoryIds,
                                       bool helpful,
                                       quint64 &requestId)
{
    QStringList normalizedIds;

    for (const QString &rawMemoryId : memoryIds)
    {
        const QString memoryId = rawMemoryId.trimmed();

        if (!memoryId.isEmpty() && !normalizedIds.contains(memoryId))
        {
            normalizedIds.append(memoryId);
        }
    }

    if (petId.trimmed().isEmpty() || triggerType.trimmed().isEmpty()
        || normalizedIds.isEmpty())
    {
        return false;
    }

    _tagTask task;
    task.action = Action::Feedback;
    task.petId = petId.trimmed();
    task.triggerType = triggerType.trimmed();
    task.memoryIds = normalizedIds;
    task.helpful = helpful;
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueUpdate(const QString &petId,
                                     const QString &triggerType,
                                     const MemoryEntry &entry,
                                     quint64 &requestId,
                                     QString &rejectCategory)
{
    QString category;

    if (entry.id.trimmed().isEmpty()
        || !MemoryRepository::ValidateEntry(entry, category))
    {
        rejectCategory = category;
        return false;
    }

    _tagTask task;
    task.action = Action::Update;
    task.petId = petId.trimmed();
    task.triggerType = triggerType.trimmed();
    task.entry = entry;
    task.entry.id = entry.id.trimmed();
    task.entry.content = entry.content.trimmed();
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueExport(const QString &petId,
                                     const QString &triggerType,
                                     const QString &filePath,
                                     quint64 &requestId)
{
    if (petId.trimmed().isEmpty() || triggerType.trimmed().isEmpty()
        || filePath.trimmed().isEmpty())
    {
        return false;
    }

    _tagTask task;
    task.action = Action::Export;
    task.petId = petId.trimmed();
    task.triggerType = triggerType.trimmed();
    task.filePath = filePath.trimmed();
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TryEnqueueImport(const QString &petId,
                                     const QString &triggerType,
                                     const QString &filePath,
                                     quint64 &requestId)
{
    if (petId.trimmed().isEmpty() || triggerType.trimmed().isEmpty()
        || filePath.trimmed().isEmpty())
    {
        return false;
    }

    _tagTask task;
    task.action = Action::Import;
    task.petId = petId.trimmed();
    task.triggerType = triggerType.trimmed();
    task.filePath = filePath.trimmed();
    return EnqueueTask(task, &requestId);
}

bool MemoryService::TakeLatestReadyResult(const QString &petId,
                                          const QString &triggerType,
                                          _tagRetrieveResult &result)
{
    QMutexLocker locker(&m_mailboxMutex);
    const auto it = m_mailbox.find(MailboxKey(petId, triggerType));

    if (it == m_mailbox.end())
    {
        return false;
    }

    result = it.value();
    m_mailbox.erase(it);
    return true;
}

bool MemoryService::HasReadyResult(const QString &petId, const QString &triggerType) const
{
    QMutexLocker locker(&m_mailboxMutex);
    return m_mailbox.contains(MailboxKey(petId, triggerType));
}

int MemoryService::PendingCount() const
{
    QMutexLocker locker(&m_queueMutex);
    return m_taskQueue.size();
}

int MemoryService::QueueCapacity() const
{
    QMutexLocker locker(&m_queueMutex);
    return m_queueCapacity;
}

void MemoryService::EmitLogMessage(const QString &message)
{
    emit LogMessage(message);
}

void MemoryService::WorkerLoop()
{
    if (m_embeddingEnabled)
    {
        m_vectorStore = std::make_unique<VectorStore>();
        QString vectorStoreError;

        if (!m_vectorStore->Open(m_vectorDbPath, vectorStoreError))
        {
            EmitLogMessage(QStringLiteral("Vector store unavailable: %1; embedding disabled.")
                               .arg(vectorStoreError));
            m_vectorStore.reset();
            m_embeddingEnabled = false;
        }
        else
        {
            EmitLogMessage(QStringLiteral("Vector store ready: %1").arg(m_vectorDbPath));
        }
    }

    for (;;)
    {
        _tagTask task;
        bool hasTask = false;

        {
            QMutexLocker locker(&m_queueMutex);

            while (m_taskQueue.isEmpty() && !m_shuttingDown)
            {
                m_queueNotEmpty.wait(&m_queueMutex);
            }

            if (!m_taskQueue.isEmpty())
            {
                task = m_taskQueue.dequeue();
                hasTask = true;
            }
        }

        if (!hasTask)
        {
            if (m_vectorStore != nullptr)
            {
                m_vectorStore->Close();
                m_vectorStore.reset();
            }

            return;
        }

        ProcessOneTask(task);
    }
}

bool MemoryService::EnqueueTask(const _tagTask &task, quint64 *requestId)
{
    _tagTask queuedTask = task;
    QString rejectionMessage;

    {
        QMutexLocker locker(&m_queueMutex);

        // Shutdown() changes this state under the same lock, so admission and
        // the worker's empty-queue exit decision are linearized together.
        if (!m_isRunning || m_shuttingDown)
        {
            rejectionMessage = QStringLiteral("Memory task rejected: service is not running.");
        }
        else if (m_taskQueue.size() >= m_queueCapacity)
        {
            rejectionMessage = QStringLiteral("Memory task queue is full; task rejected (%1).")
                                   .arg(ActionToString(task.action));
        }
        else
        {
            queuedTask.requestId = m_nextRequestId.fetchAndAddRelaxed(1);
            m_taskQueue.enqueue(queuedTask);
            m_queueNotEmpty.wakeOne();
        }
    }

    if (!rejectionMessage.isEmpty())
    {
        EmitLogMessage(rejectionMessage);
        return false;
    }

    if (requestId != nullptr)
    {
        *requestId = queuedTask.requestId;
    }

    return true;
}

void MemoryService::ProcessOneTask(const _tagTask &task)
{
    switch (task.action)
    {
        case Action::Retrieve:
        {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            bool graphChanged = m_maintenance.ApplyConfidenceDecay(m_graph, now);
            _tagRetrieveResult result;
            result.requestId = task.requestId;
            result.petId = task.petId;
            result.triggerType = task.triggerType;
            result.query = task.query;
            result.entries = CascadeRetrieve(task.query, task.petId, WORKER_SEARCH_LIMIT);
            result.ok = true;

            QString maintenanceError;
            QStringList removedIds;
            graphChanged = m_maintenance.OnRetrieved(m_graph,
                                                     result.entries,
                                                     task.query,
                                                     task.petId,
                                                     task.triggerType,
                                                     now,
                                                     removedIds,
                                                     maintenanceError)
                           || graphChanged;

            if (!maintenanceError.isEmpty())
            {
                EmitLogMessage(QStringLiteral("Memory maintenance warning: %1")
                                   .arg(maintenanceError));
            }

            if (graphChanged)
            {
                QString saveError;

                if (!m_repository.Save(m_graph, saveError))
                {
                    qWarning() << "[Memory]" << saveError;
                }
            }

            for (const QString &removedId : removedIds)
            {
                RemoveVector(removedId);
            }

            {
                QMutexLocker locker(&m_mailboxMutex);
                const QString key = MailboxKey(task.petId, task.triggerType);
                const auto it = m_mailbox.constFind(key);

                if ((it == m_mailbox.constEnd())
                    || (result.requestId >= it.value().requestId))
                {
                    m_mailbox.insert(key, result);
                }
            }
            break;
        }
        case Action::Store:
        {
            MemoryEntry entry = task.entry;

            if (entry.id.trimmed().isEmpty())
            {
                entry.id = QStringLiteral("mem_%1").arg(task.requestId);
            }

            if (entry.createdAt <= 0)
            {
                entry.createdAt = QDateTime::currentMSecsSinceEpoch();
            }

            entry.updatedAt = QDateTime::currentMSecsSinceEpoch();
            entry.lastAccessed = entry.updatedAt;

            if (entry.scope == MemoryEntry::Scope::Global)
            {
                entry.petId.clear();
            }
            else
            {
                entry.petId = task.petId;
            }

            QString storeError;

            if (m_graph.AddEntry(entry, storeError))
            {
                QString saveError;

                if (!m_repository.Save(m_graph, saveError))
                {
                    qWarning() << "[Memory]" << saveError;
                }

                if (!TryWriteVector(entry.id, entry.content))
                {
                    m_vectorBackfillComplete = false;
                }
            }
            else
            {
                qWarning() << "[Memory] Store rejected:" << storeError;
            }
            break;
        }
        case Action::Forget:
        {
            QVector<QString> removedIds;
            bool changed = false;

            if (!task.memoryId.isEmpty())
            {
                MemoryEntry targetEntry;

                if (m_graph.GetEntry(task.memoryId, targetEntry)
                    && ((targetEntry.scope == MemoryEntry::Scope::Global)
                        || (targetEntry.petId == task.petId))
                    && m_graph.SoftDeleteEntry(task.memoryId))
                {
                    changed = true;
                    removedIds.append(task.memoryId);
                }
            }
            else
            {
                const QVector<MemoryEntry> matches = m_graph.SearchByKeywords(task.query,
                                                                              MemoryEntry::Scope::Pet,
                                                                              task.petId,
                                                                              8);

                for (const MemoryEntry &match : matches)
                {
                    if (m_graph.SoftDeleteEntry(match.id))
                    {
                        changed = true;
                        removedIds.append(match.id);
                    }
                }
            }

            if (changed)
            {
                QString saveError;

                if (!m_repository.Save(m_graph, saveError))
                {
                    qWarning() << "[Memory]" << saveError;
                }

                for (const QString &removedId : removedIds)
                {
                    RemoveVector(removedId);
                }
            }

            break;
        }
        case Action::Tag:
        {
            MemoryEntry entry;
            QString tagError;

            if (m_graph.GetEntry(task.memoryId, entry)
                && ((entry.scope == MemoryEntry::Scope::Global)
                    || (entry.petId == task.petId)))
            {
                bool changed = false;

                if (task.tags.isEmpty() && !entry.tags.isEmpty())
                {
                    entry.tags.clear();
                    changed = true;
                }
                else
                {
                    for (const QString &tag : task.tags)
                    {
                        if (!entry.tags.contains(tag))
                        {
                            entry.tags.append(tag);
                            changed = true;
                        }
                    }
                }

                if (changed)
                {
                    entry.updatedAt = QDateTime::currentMSecsSinceEpoch();

                    if (m_graph.UpdateEntry(entry, tagError))
                    {
                        QString saveError;

                        if (!m_repository.Save(m_graph, saveError))
                        {
                            qWarning() << "[Memory]" << saveError;
                        }
                    }
                }
            }
            break;
        }
        case Action::List:
        {
            _tagRetrieveResult result;
            result.requestId = task.requestId;
            result.petId = task.petId;
            result.triggerType = task.triggerType;
            result.entries = m_graph.ListEntries(task.scope, task.petId);

            QSet<QString> conflictIds;

            for (const MemoryEdge &edge : m_graph.AllEdges())
            {
                if (edge.active && (edge.type == MemoryEdge::Type::Conflict))
                {
                    conflictIds.insert(edge.sourceId);
                    conflictIds.insert(edge.targetId);
                }
            }

            for (MemoryEntry &entry : result.entries)
            {
                entry.hasConflict = conflictIds.contains(entry.id);
            }

            result.ok = true;

            {
                QMutexLocker locker(&m_mailboxMutex);
                const QString key = MailboxKey(task.petId, task.triggerType);
                const auto it = m_mailbox.constFind(key);

                if ((it == m_mailbox.constEnd())
                    || (result.requestId >= it.value().requestId))
                {
                    m_mailbox.insert(key, result);
                }
            }
            break;
        }
        case Action::Consolidate:
        {
            ConsolidateCandidates(task);
            break;
        }
        case Action::Feedback:
        {
            if (m_maintenance.ApplyFeedback(m_graph,
                                            task.memoryIds,
                                            task.petId,
                                            task.helpful,
                                            QDateTime::currentMSecsSinceEpoch()))
            {
                QString saveError;

                if (!m_repository.Save(m_graph, saveError))
                {
                    qWarning() << "[Memory]" << saveError;
                }
            }

            break;
        }
        case Action::Update:
        {
            MemoryEntry existingEntry;

            if (!m_graph.GetEntry(task.entry.id, existingEntry)
                || ((existingEntry.scope == MemoryEntry::Scope::Pet)
                    && (existingEntry.petId != task.petId)))
            {
                EmitLogMessage(QStringLiteral("Memory update target is unavailable."));
                break;
            }

            MemoryEntry updatedEntry = task.entry;
            updatedEntry.createdAt = existingEntry.createdAt;
            updatedEntry.updatedAt = QDateTime::currentMSecsSinceEpoch();
            updatedEntry.lastAccessed = existingEntry.lastAccessed;
            updatedEntry.confidenceUpdatedAt = existingEntry.confidenceUpdatedAt;
            updatedEntry.accessCount = existingEntry.accessCount;
            updatedEntry.strength = existingEntry.strength;
            updatedEntry.confidence = existingEntry.confidence;
            updatedEntry.trustScore = existingEntry.trustScore;
            updatedEntry.active = existingEntry.active;

            if (updatedEntry.scope == MemoryEntry::Scope::Global)
            {
                updatedEntry.petId.clear();
            }
            else
            {
                updatedEntry.petId = task.petId;
            }

            QString updateError;

            if (!m_graph.UpdateEntry(updatedEntry, updateError))
            {
                EmitLogMessage(QStringLiteral("Memory update failed: %1").arg(updateError));
                break;
            }

            QString saveError;

            if (!m_repository.Save(m_graph, saveError))
            {
                EmitLogMessage(QStringLiteral("Memory update could not be saved: %1")
                                   .arg(saveError));
                break;
            }

            RemoveVector(updatedEntry.id);

            if (!TryWriteVector(updatedEntry.id, updatedEntry.content))
            {
                m_vectorBackfillComplete = false;
            }
            EmitLogMessage(QStringLiteral("Memory update completed."));
            break;
        }
        case Action::Export:
        {
            QString operationError;
            const bool ok = m_repository.Export(m_graph, task.filePath, operationError);
            EmitLogMessage(ok
                               ? QStringLiteral("Memory export completed.")
                               : QStringLiteral("Memory export failed: %1").arg(operationError));
            break;
        }
        case Action::Import:
        {
            MemoryGraph importedGraph;
            QString operationError;

            if (!m_repository.Import(task.filePath, importedGraph, operationError))
            {
                EmitLogMessage(QStringLiteral("Memory import failed: %1").arg(operationError));
                break;
            }

            QString saveError;

            if (!m_repository.Save(importedGraph, saveError))
            {
                EmitLogMessage(QStringLiteral("Imported memory could not be committed: %1")
                                   .arg(saveError));
                break;
            }

            m_graph = std::move(importedGraph);
            m_vectorBackfillComplete = false;

            if ((m_vectorStore != nullptr) && m_vectorStore->IsOpen()
                && (m_embedder != nullptr))
            {
                QString vectorError;
                m_vectorStore->ClearAll(vectorError);

                if (m_embedder->IsReady())
                {
                    for (const MemoryEntry &entry : m_graph.AllEntries())
                    {
                        if (entry.active)
                        {
                            TryWriteVector(entry.id, entry.content);
                        }
                    }
                }
            }

            EmitLogMessage(QStringLiteral("Memory import completed."));
            break;
        }
    }
}

QString MemoryService::MailboxKey(const QString &petId, const QString &triggerType)
{
    return petId + QStringLiteral("|") + triggerType;
}

void MemoryService::EnsureEmbedderReady()
{
    if (!m_embeddingEnabled || (m_embedder == nullptr) || m_embedder->IsReady())
    {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if ((m_lastEmbedderAttemptMs > 0)
        && ((now - m_lastEmbedderAttemptMs) < EMBEDDER_RETRY_INTERVAL_MS))
    {
        return;
    }

    m_lastEmbedderAttemptMs = now;

    auto *client = dynamic_cast<EmbeddingClient *>(m_embedder.get());

    if (client == nullptr)
    {
        return;
    }

    if (client->Reload())
    {
        emit LogMessage(QStringLiteral("Embedding model loaded after retry."));
    }
    else
    {
        const QString diagnostic = client->LastError().trimmed();
        const QString category = diagnostic.isEmpty()
                                     ? QStringLiteral("unknown")
                                     : diagnostic.section(QLatin1Char(':'), 0, 0);
        emit LogMessage(QStringLiteral("Embedding model unavailable (%1); "
                                       "fallback to keyword retrieval.")
                            .arg(category));
    }
}

void MemoryService::BackfillMissingVectors()
{
    if (m_vectorBackfillComplete || !m_embeddingEnabled
        || (m_embedder == nullptr) || !m_embedder->IsReady()
        || (m_vectorStore == nullptr) || !m_vectorStore->IsOpen())
    {
        return;
    }

    bool allSucceeded = true;

    for (const MemoryEntry &entry : m_graph.AllEntries())
    {
        if (!entry.active)
        {
            continue;
        }

        QString storedModelId;
        int storedDimension = 0;
        QVector<float> storedVector;
        QString vectorError;
        const bool hasVector = m_vectorStore->Get(entry.id,
                                                   storedModelId,
                                                   storedDimension,
                                                   storedVector,
                                                   vectorError);

        if (hasVector
            && (storedModelId == m_embedder->ModelId())
            && (storedDimension == m_embedder->Dimension()))
        {
            continue;
        }

        if (!TryWriteVector(entry.id, entry.content))
        {
            allSucceeded = false;
        }
    }

    m_vectorBackfillComplete = allSucceeded;

    if (allSucceeded)
    {
        EmitLogMessage(QStringLiteral("Memory vector backfill completed."));
    }
}

QVector<MemoryEntry> MemoryService::CascadeRetrieve(const QString &query,
                                                    const QString &petId,
                                                    int maxResults)
{
    QVector<MemoryEntry> hits = m_graph.MatchTriggeredEntries(query,
                                                               MemoryEntry::Scope::Pet,
                                                               petId,
                                                               maxResults);
    QSet<QString> triggeredIds;

    for (const MemoryEntry &triggeredEntry : hits)
    {
        triggeredIds.insert(triggeredEntry.id);
    }

    QVector<MemoryEntry> semanticHits;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, float> seedScores;

    if (m_embeddingEnabled && (m_embedder != nullptr))
    {
        EnsureEmbedderReady();
        BackfillMissingVectors();

        if (m_embedder->IsReady())
        {
            QVector<float> queryVector;

            if (m_embedder->EmbedQuery(query, queryVector))
            {
                QVector<VectorStore::_tagVectorHit> vectorHits;
                QString vectorError;

                if ((m_vectorStore != nullptr)
                    && m_vectorStore->QueryTopK(m_embedder->ModelId(),
                                                queryVector,
                                                WORKER_SEARCH_LIMIT,
                                                vectorHits,
                                                vectorError))
                {
                    QSet<QString> seedIds;

                    for (const VectorStore::_tagVectorHit &vectorHit : vectorHits)
                    {
                        if (vectorHit.score <= 0.0f)
                        {
                            continue;
                        }

                        seedIds.insert(vectorHit.entryId);
                        seedScores.insert(vectorHit.entryId, vectorHit.score);
                    }

                    semanticHits = m_graph.ActiveEntriesByIds(seedIds,
                                                              MemoryEntry::Scope::Pet,
                                                              petId);
                }
                else if (!vectorError.isEmpty())
                {
                    qWarning() << "[Memory] Vector query failed:" << vectorError;
                }
            }
        }
    }

    if (semanticHits.isEmpty())
    {
        semanticHits = m_graph.SearchByKeywords(query,
                                                MemoryEntry::Scope::Pet,
                                                petId,
                                                maxResults);
        seedScores.clear();

        for (int index = 0; index < semanticHits.size(); ++index)
        {
            const float baseScore = static_cast<float>(maxResults - index);
            seedScores.insert(semanticHits.at(index).id, baseScore);
        }
    }

    if (semanticHits.isEmpty())
    {
        return hits;
    }

    const QVector<_tagGraphHit> graphHits = m_graph.ExpandByEdges(seedScores,
                                                                  MemoryEntry::Scope::Pet,
                                                                  petId,
                                                                  GRAPH_EXPAND_MAX_DEPTH,
                                                                  GRAPH_EXPAND_DECAY_PER_HOP);

    struct _tagScoredEntry
    {
        MemoryEntry entry;
        float score = 0.0f;
    };

    QVector<_tagScoredEntry> scored;
    scored.reserve(semanticHits.size() + graphHits.size());

    for (const MemoryEntry &entry : semanticHits)
    {
        if (triggeredIds.contains(entry.id))
        {
            continue;
        }

        _tagScoredEntry scoredEntry;
        scoredEntry.entry = entry;
        scoredEntry.score = seedScores.value(entry.id, 0.0f)
                            * MemoryMaintenance::RetrievalWeight(entry, now);
        scored.append(scoredEntry);
    }

    for (const _tagGraphHit &graphHit : graphHits)
    {
        if (triggeredIds.contains(graphHit.entry.id))
        {
            continue;
        }

        _tagScoredEntry scoredEntry;
        scoredEntry.entry = graphHit.entry;
        scoredEntry.score = graphHit.score
                            * MemoryMaintenance::RetrievalWeight(graphHit.entry, now);
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

    for (const _tagScoredEntry &scoredEntry : scored)
    {
        hits.append(scoredEntry.entry);

        if (hits.size() >= maxResults)
        {
            break;
        }
    }

    return hits;
}

bool MemoryService::TryWriteVector(const QString &entryId, const QString &content)
{
    if (!m_embeddingEnabled || (m_embedder == nullptr) || (m_vectorStore == nullptr))
    {
        return false;
    }

    EnsureEmbedderReady();

    if (!m_embedder->IsReady() || !m_vectorStore->IsOpen())
    {
        return false;
    }

    QVector<float> vector;

    if (!m_embedder->EmbedDocument(content, vector))
    {
        return false;
    }

    QString vectorError;

    if (!m_vectorStore->Upsert(entryId,
                               m_embedder->ModelId(),
                               m_embedder->Dimension(),
                               vector,
                               vectorError))
    {
        qWarning() << "[Memory]" << vectorError;
        return false;
    }

    return true;
}

void MemoryService::RemoveVector(const QString &entryId)
{
    if ((m_vectorStore == nullptr) || !m_vectorStore->IsOpen())
    {
        return;
    }

    QString vectorError;

    if (!m_vectorStore->Remove(entryId, vectorError))
    {
        qWarning() << "[Memory]" << vectorError;
    }
}

void MemoryService::ConsolidateCandidates(const _tagTask &task)
{
    bool changed = false;
    QVector<MemoryEntry> newEntries;
    QVector<QString> supersededIds;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (int index = 0; index < task.candidates.size(); ++index)
    {
        const _tagMemoryConsolidationCandidate &candidate = task.candidates.at(index);
        QString rejectCategory;

        if (!MemoryRepository::ValidateEntry(candidate.entry, rejectCategory))
        {
            EmitLogMessage(QStringLiteral("Memory consolidation candidate discarded: %1")
                               .arg(rejectCategory));
            continue;
        }

        MemoryEntry entry = candidate.entry;
        entry.content = entry.content.trimmed();

        if (entry.scope == MemoryEntry::Scope::Global)
        {
            entry.petId.clear();
        }
        else if (entry.petId.trimmed().isEmpty())
        {
            entry.petId = task.petId;
        }
        else
        {
            entry.petId = entry.petId.trimmed();
        }
        entry.id.clear();
        entry.createdAt = now;
        entry.updatedAt = now;
        entry.lastAccessed = now;
        entry.provenance = MemoryEntry::Provenance::Extracted;

        MemoryEntry relatedEntry;

        if (candidate.relation != MEMORY_CONSOLIDATION_RELATION::NONE)
        {
            if (candidate.relatedMemoryId.trimmed().isEmpty()
                || !m_graph.GetEntry(candidate.relatedMemoryId, relatedEntry)
                || !relatedEntry.active || (relatedEntry.scope != entry.scope)
                || ((entry.scope == MemoryEntry::Scope::Pet)
                    && (relatedEntry.petId != entry.petId)))
            {
                EmitLogMessage(QStringLiteral("Memory consolidation relation target is unavailable."));
                continue;
            }
        }
        else if (!candidate.relatedMemoryId.trimmed().isEmpty())
        {
            EmitLogMessage(QStringLiteral("Memory consolidation relation target is unexpected."));
            continue;
        }

        float similarity = 0.0f;
        const MemoryEntry similarEntry = FindSimilarActiveEntry(entry, similarity);

        if ((candidate.relation == MEMORY_CONSOLIDATION_RELATION::NONE)
            && !similarEntry.id.isEmpty()
            && (similarity >= CONSOLIDATION_DUPLICATE_SIMILARITY))
        {
            MemoryEntry strengthenedEntry = similarEntry;
            strengthenedEntry.strength += 1;
            strengthenedEntry.confidence = qMax(strengthenedEntry.confidence, entry.confidence);
            strengthenedEntry.trustScore = qMax(strengthenedEntry.trustScore, entry.trustScore);
            strengthenedEntry.updatedAt = now;

            for (const QString &tag : entry.tags)
            {
                if (!strengthenedEntry.tags.contains(tag))
                {
                    strengthenedEntry.tags.append(tag);
                }
            }

            QString updateError;

            if (m_graph.UpdateEntry(strengthenedEntry, updateError))
            {
                changed = true;
            }
            else
            {
                qWarning() << "[Memory] Consolidation strengthen failed:" << updateError;
            }

            continue;
        }

        entry.id = QStringLiteral("mem_consolidated_%1_%2").arg(task.requestId).arg(index);
        QString addError;

        if (!m_graph.AddEntry(entry, addError))
        {
            qWarning() << "[Memory] Consolidation store failed:" << addError;
            continue;
        }

        changed = true;
        newEntries.append(entry);

        if (candidate.relatedMemoryId.trimmed().isEmpty())
        {
            continue;
        }

        MemoryEdge::Type edgeType = MemoryEdge::Type::Explicit;

        if (candidate.relation == MEMORY_CONSOLIDATION_RELATION::SUPERSEDES)
        {
            edgeType = MemoryEdge::Type::Supersedes;
        }
        else if (candidate.relation == MEMORY_CONSOLIDATION_RELATION::CONFLICTS)
        {
            edgeType = MemoryEdge::Type::Conflict;
        }
        else
        {
            continue;
        }

        QString edgeError;

        if (!m_graph.AddEdge(entry.id,
                             relatedEntry.id,
                             edgeType,
                             QString(),
                             1.0f,
                             edgeError))
        {
            qWarning() << "[Memory] Consolidation relation failed:" << edgeError;
            continue;
        }

        if (candidate.relation == MEMORY_CONSOLIDATION_RELATION::SUPERSEDES)
        {
            if (m_graph.SoftDeleteEntry(relatedEntry.id))
            {
                supersededIds.append(relatedEntry.id);
            }
        }
    }

    if (!changed)
    {
        return;
    }

    QString saveError;

    if (!m_repository.Save(m_graph, saveError))
    {
        qWarning() << "[Memory]" << saveError;
        return;
    }

    for (const MemoryEntry &entry : newEntries)
    {
        if (!TryWriteVector(entry.id, entry.content))
        {
            m_vectorBackfillComplete = false;
        }
    }

    for (const QString &supersededId : supersededIds)
    {
        RemoveVector(supersededId);
    }
}

MemoryEntry MemoryService::FindSimilarActiveEntry(const MemoryEntry &entry, float &similarity) const
{
    MemoryEntry bestEntry;
    similarity = 0.0f;
    const QVector<MemoryEntry> visibleEntries = m_graph.ListEntries(entry.scope, entry.petId);

    for (const MemoryEntry &existingEntry : visibleEntries)
    {
        if ((existingEntry.scope != entry.scope)
            || (existingEntry.petId != entry.petId)
            || (existingEntry.type != entry.type))
        {
            continue;
        }

        const float candidateSimilarity = ContentSimilarity(entry.content, existingEntry.content);

        if (candidateSimilarity > similarity)
        {
            similarity = candidateSimilarity;
            bestEntry = existingEntry;
        }
    }

    return bestEntry;
}

float MemoryService::ContentSimilarity(const QString &left, const QString &right)
{
    const auto tokenize = [](const QString &text)
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
    };

    const QSet<QString> leftTokens = tokenize(left);
    const QSet<QString> rightTokens = tokenize(right);

    if (leftTokens.isEmpty() || rightTokens.isEmpty())
    {
        return 0.0f;
    }

    QSet<QString> unionTokens = leftTokens;
    unionTokens.unite(rightTokens);

    if (unionTokens.isEmpty())
    {
        return 0.0f;
    }

    QSet<QString> intersectionTokens = leftTokens;
    intersectionTokens.intersect(rightTokens);
    return static_cast<float>(intersectionTokens.size())
           / static_cast<float>(unionTokens.size());
}

} // namespace vpet
