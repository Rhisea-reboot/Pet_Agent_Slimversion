#include "vpet/agent/agent_runtime.h"
#include "agent_runtime_internal.h"
#include "vpet/llm/llm_client.h"
#include "vpet/web/web_research_engine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSize>
#include <QStringList>
#include <QVariant>

namespace vpet
{

using namespace AgentRuntimeInternal;

AgentRuntime::AgentRuntime(QObject *parent)
    : AgentRuntime(nullptr, nullptr, parent)
{
}
AgentRuntime::AgentRuntime(WebResearchEngine *webResearchEngine, QObject *parent)
    : AgentRuntime(webResearchEngine, nullptr, parent)
{
}
AgentRuntime::AgentRuntime(WebResearchEngine *webResearchEngine,
                           MemoryService *memoryService,
                           QObject *parent)
    : QObject(parent)
    , m_context()
    , m_sessionContext()
    , m_llmClient(new LlmClient(this))
    , m_visionLlmClient(new VisionLlmClient(this))
    , m_webResearchEngine(webResearchEngine != nullptr
                              ? webResearchEngine
                              : new WebResearchEngine(nullptr, this))
    , m_memoryService(memoryService != nullptr ? memoryService : new MemoryService(this))
    , m_ownsMemoryService(memoryService == nullptr)
    , m_memoryConfig()
    , m_memoryConfigLoaded(false)
    , m_nodeRegistry()
    , m_graphExecutor()
    , m_asyncBridge()
    , m_invocationQueue()
    , m_lastPerceptionFrameHash()
    , m_isLoaded(false)
    , m_contextWasQueued(false)
    , m_webResearchStartInProgress(false)
    , m_hasBufferedWebResearchCompletion(false)
    , m_hasBufferedWebResearchFailure(false)
    , m_bufferedWebResearchCompletion()
    , m_bufferedWebResearchFailureId(-1)
    , m_bufferedWebResearchFailureMessage()
    , m_bufferedWebResearchFailureStatusCode(0)
{
    connect(m_llmClient, &LlmClient::ChatCompleted,
            this, &AgentRuntime::OnLlmChatCompleted);
    connect(m_llmClient, &LlmClient::ChatFailed,
            this, &AgentRuntime::OnLlmChatFailed);
    connect(m_visionLlmClient, &VisionLlmClient::AnalysisCompleted,
            this, &AgentRuntime::OnVisionAnalysisCompleted);
    connect(m_visionLlmClient, &VisionLlmClient::AnalysisFailed,
            this, &AgentRuntime::OnVisionAnalysisFailed);
    connect(m_webResearchEngine, &WebResearchEngine::Completed,
            this, &AgentRuntime::OnWebResearchCompleted);
    connect(m_webResearchEngine, &WebResearchEngine::Failed,
            this, &AgentRuntime::OnWebResearchFailed);
    connect(m_memoryService, &MemoryService::LogMessage,
            this, &AgentRuntime::LogMessage);

    RegisterDefaultNodeHandlers();
}

AgentRuntime::~AgentRuntime()
{
    // QObject parent-child ownership releases the client instances.
}

bool AgentRuntime::Load(const QString &configPath, QString &errorMessage)
{
    if (configPath.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime config path is empty.");
        return false;
    }

    if (m_graphExecutor.IsActive() || HasPendingAsyncRequest() || !m_invocationQueue.IsEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime cannot load a DAG during an active invocation.");
        return false;
    }

    if (!m_graphExecutor.Load(configPath, errorMessage))
    {
        m_isLoaded = false;
        return false;
    }

    m_isLoaded = true;

    qDebug() << "[Agent] DAG loaded:" << configPath;
    qDebug() << "[Agent] Topological order:" << m_graphExecutor.GetExecutionOrder();

    return true;
}

bool AgentRuntime::Execute(QString &errorMessage)
{
    if (!m_isLoaded)
    {
        errorMessage = QStringLiteral("Agent runtime is not loaded.");
        return false;
    }

    if (m_graphExecutor.GetExecutionOrder().isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime execution order is empty.");
        return false;
    }

    if (m_graphExecutor.IsActive() || HasPendingAsyncRequest() || !m_invocationQueue.IsEmpty())
    {
        if (m_contextWasQueued)
        {
            m_contextWasQueued = false;
            return true;
        }

        QVariant triggerValue;

        if (!m_context.GetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE, triggerValue)
            || triggerValue.toString().trimmed().isEmpty())
        {
            errorMessage = QStringLiteral(
                "Agent runtime cannot queue an execution without a trigger while another invocation is active.");
            return false;
        }

        if (!EnqueueInvocation(m_context))
        {
            errorMessage = QStringLiteral("Agent runtime failed to queue invocation.");
            return false;
        }

        m_context = m_sessionContext.Snapshot();
        m_contextWasQueued = true;
        return true;
    }

    QVariant triggerValue;

    if (!m_context.GetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE, triggerValue)
        || triggerValue.toString().trimmed().isEmpty())
    {
        m_context = m_sessionContext.Snapshot();
    }

    if (!m_graphExecutor.BeginInvocation(m_context,
                                         m_asyncBridge.HasPending(),
                                         errorMessage))
    {
        return false;
    }

    return m_graphExecutor.PumpReadyQueue(true,
                                          m_context,
                                          m_sessionContext,
                                          BuildGraphCallbacks(),
                                          errorMessage);
}

bool AgentRuntime::ExecuteWithUserInput(const QString &userInput, QString &errorMessage)
{
    const QString normalizedUserInput = userInput.trimmed();

    m_contextWasQueued = false;

    if (normalizedUserInput.isEmpty())
    {
        errorMessage = QStringLiteral("Agent runtime user input is empty.");
        return false;
    }

    m_context = m_sessionContext.Snapshot();

    if (!m_context.SetUserInput(normalizedUserInput))
    {
        errorMessage = QStringLiteral("Agent runtime failed to set user input.");
        return false;
    }

    if (!m_context.SetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE, TRIGGER_TYPE_USER))
    {
        m_context.RemoveValue(AgentContextKeys::USER_INPUT);
        errorMessage = QStringLiteral("Agent runtime failed to set user trigger type.");
        return false;
    }

    if (m_isLoaded
        && (m_graphExecutor.IsActive()
            || HasPendingAsyncRequest()
            || !m_invocationQueue.IsEmpty()))
    {
        if (!EnqueueInvocation(m_context))
        {
            errorMessage = QStringLiteral("Agent runtime failed to queue user invocation.");
            return false;
        }

        m_contextWasQueued = true;
        m_context = m_sessionContext.Snapshot();
        return true;
    }

    if (m_isLoaded)
    {
        if (!Execute(errorMessage))
        {
            ClearInvocationInputState(m_context);
            return false;
        }

        return true;
    }

    emit LogMessage(QStringLiteral("Agent DAG is not loaded. Voice text will use direct LLM fallback."));

    if (!PrepareTextInputContext(m_context, errorMessage))
    {
        ClearInvocationInputState(m_context);
        return false;
    }

    if (!SendUserInputToLlm(normalizedUserInput, errorMessage))
    {
        ClearInvocationInputState(m_context);
        return false;
    }

    return true;
}

bool AgentRuntime::LoadLlmConfig(const QString &configPath, QString &errorMessage)
{
    if (configPath.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent LLM config path is empty.");
        return false;
    }

    if (m_llmClient == nullptr)
    {
        errorMessage = QStringLiteral("Agent LLM client is not initialized.");
        return false;
    }

    if (!m_llmClient->LoadConfig(configPath))
    {
        errorMessage = QStringLiteral("Agent failed to load LLM config.");
        return false;
    }

    emit LogMessage(QStringLiteral("Agent LLM config loaded: %1").arg(configPath));

    return true;
}

bool AgentRuntime::LoadVisionLlmConfig(const QString &configPath, QString &errorMessage)
{
    if (configPath.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent Vision LLM config path is empty.");
        return false;
    }

    if (m_visionLlmClient == nullptr)
    {
        errorMessage = QStringLiteral("Agent Vision LLM client is not initialized.");
        return false;
    }

    if (!m_visionLlmClient->LoadConfig(configPath))
    {
        errorMessage = QStringLiteral("Agent failed to load Vision LLM config.");
        return false;
    }

    emit LogMessage(QStringLiteral("Agent Vision LLM config loaded: %1").arg(configPath));

    return true;
}

bool AgentRuntime::LoadWebSearchConfig(const QString &configPath, QString &errorMessage)
{
    if (configPath.trimmed().isEmpty() || (m_webResearchEngine == nullptr))
    {
        errorMessage = QStringLiteral("Agent web search config input is invalid.");
        return false;
    }

    if (!m_webResearchEngine->LoadClientConfig(configPath, errorMessage))
    {
        return false;
    }

    emit LogMessage(QStringLiteral("Agent web search config loaded: %1").arg(configPath));
    return true;
}

bool AgentRuntime::UpdatePerceptionFrame(const QByteArray &encodedData,
                                          int frameId,
                                          const QSize &frameSize,
                                          const QString &modality,
                                          QString &errorMessage)
{
    const QString normalizedModality = modality.trimmed();
    const QString frameHash = QString::fromLatin1(
        QCryptographicHash::hash(encodedData, QCryptographicHash::Sha256).toHex());

    m_contextWasQueued = false;

    if (encodedData.isEmpty())
    {
        errorMessage = QStringLiteral("Agent perception frame data is empty.");
        return false;
    }

    QVariant previousFrameHashValue;

    if ((m_lastPerceptionFrameHash == frameHash)
        || (m_context.GetValue(AgentContextKeys::SEMANTIC_VISION_FRAME_HASH,
                               previousFrameHashValue)
            && (previousFrameHashValue.toString() == frameHash)))
    {
        emit LogMessage(QStringLiteral("Agent perception frame skipped because content is unchanged."));
        return true;
    }

    const bool mustQueuePerception = m_graphExecutor.IsActive()
                                     || HasPendingAsyncRequest()
                                     || !m_invocationQueue.IsEmpty();
    AgentContext queuedPerceptionContext;
    AgentContext *perceptionContext = &m_context;

    if (mustQueuePerception)
    {
        // Do not overwrite an invocation context that an asynchronous callback will resume.
        queuedPerceptionContext = m_sessionContext.Snapshot();
        perceptionContext = &queuedPerceptionContext;
    }
    else
    {
        m_context = m_sessionContext.Snapshot();
    }

    if (frameId <= 0)
    {
        errorMessage = QStringLiteral("Agent perception frame ID is invalid.");
        return false;
    }

    if (!frameSize.isValid())
    {
        errorMessage = QStringLiteral("Agent perception frame size is invalid.");
        return false;
    }

    if (normalizedModality.isEmpty())
    {
        errorMessage = QStringLiteral("Agent perception modality is empty.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_VISION_AVAILABLE, true))
    {
        errorMessage = QStringLiteral("Agent failed to record vision availability.");
        return false;
    }

    perceptionContext->RemoveValue(CONTEXT_KEY_VISION_ANALYSIS);
    perceptionContext->RemoveValue(CONTEXT_KEY_SEMANTIC_VISION_SUMMARY);
    perceptionContext->RemoveValue(CONTEXT_KEY_VISION_LLM_LAST_REQUEST_ID);
    perceptionContext->RemoveValue(CONTEXT_KEY_VISION_LLM_PENDING);

    const QString base64Image = QString::fromLatin1(encodedData);
    const QString mediaType = ResolveVisionMediaType(normalizedModality);

    if (!perceptionContext->SetValue(CONTEXT_KEY_VISION_LATEST_BASE64, base64Image)
        || !perceptionContext->SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_BASE64, base64Image)
        || !perceptionContext->SetValue(AgentContextKeys::SEMANTIC_VISION_FRAME_HASH, frameHash))
    {
        errorMessage = QStringLiteral("Agent failed to record latest vision frame.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_VISION_LATEST_FRAME_ID, frameId)
        || !perceptionContext->SetValue(CONTEXT_KEY_SEMANTIC_VISION_FRAME_ID, frameId))
    {
        errorMessage = QStringLiteral("Agent failed to record latest vision frame ID.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_VISION_LATEST_WIDTH, frameSize.width())
        || !perceptionContext->SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_WIDTH, frameSize.width()))
    {
        errorMessage = QStringLiteral("Agent failed to record latest vision frame width.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_VISION_LATEST_HEIGHT, frameSize.height())
        || !perceptionContext->SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_HEIGHT, frameSize.height()))
    {
        errorMessage = QStringLiteral("Agent failed to record latest vision frame height.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_VISION_LATEST_MODALITY, normalizedModality))
    {
        errorMessage = QStringLiteral("Agent failed to record latest vision modality.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_SEMANTIC_IMAGE_MEDIA_TYPE, mediaType)
        || !perceptionContext->SetValue(CONTEXT_KEY_SEMANTIC_VISION_STATE,
                                         QStringLiteral("available")))
    {
        errorMessage = QStringLiteral("Agent failed to record semantic vision state.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_VISION_UPDATED_AT,
                                     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)))
    {
        errorMessage = QStringLiteral("Agent failed to record latest vision timestamp.");
        return false;
    }

    if (!perceptionContext->SetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE, TRIGGER_TYPE_VISION))
    {
        errorMessage = QStringLiteral("Agent failed to record vision trigger type.");
        return false;
    }

    perceptionContext->RemoveValue(AgentContextKeys::USER_INPUT);

    if (mustQueuePerception)
    {
        if (!EnqueueInvocation(*perceptionContext))
        {
            errorMessage = QStringLiteral("Agent runtime failed to queue vision invocation.");
            return false;
        }

        m_contextWasQueued = true;
    }

    m_lastPerceptionFrameHash = frameHash;

    emit LogMessage(QStringLiteral("Agent perception frame updated: %1").arg(frameId));

    return true;
}

bool AgentRuntime::LoadDefaultLlmConfig(QString &errorMessage)
{
    const QString configPath = FindDefaultLlmConfigPath();

    if (configPath.isEmpty())
    {
        errorMessage = QStringLiteral("llm_config.json not found.");
        return false;
    }

    return LoadLlmConfig(configPath, errorMessage);
}

bool AgentRuntime::LoadDefaultVisionLlmConfig(QString &errorMessage)
{
    const QString configPath = FindDefaultVisionLlmConfigPath();

    if (configPath.isEmpty())
    {
        errorMessage = QStringLiteral("vision_llm_config.json not found.");
        return false;
    }

    return LoadVisionLlmConfig(configPath, errorMessage);
}

bool AgentRuntime::LoadDefaultWebSearchConfig(QString &errorMessage)
{
    const QString configPath = FindDefaultWebSearchConfigPath();

    if (configPath.isEmpty())
    {
        errorMessage = QStringLiteral("web_search_config.json not found.");
        return false;
    }

    return LoadWebSearchConfig(configPath, errorMessage);
}

bool AgentRuntime::LoadMemoryConfig(const QString &configPath, QString &errorMessage)
{
    if (configPath.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Agent memory config path is empty.");
        return false;
    }

    MemoryConfig config;

    if (!MemoryService::ParseConfig(configPath, config, errorMessage))
    {
        return false;
    }

    m_memoryConfig = config;
    m_memoryConfigLoaded = true;

    if (!config.enabled)
    {
        emit LogMessage(QStringLiteral("Memory service disabled by config: %1").arg(configPath));
        return true;
    }

    if (m_memoryService == nullptr)
    {
        errorMessage = QStringLiteral("Agent memory service is not initialized.");
        return false;
    }

    if (!m_memoryService->SetEmbeddingConfig(config.embedding, errorMessage))
    {
        qWarning() << "[Agent]" << errorMessage
                   << "Memory nodes will be skipped without affecting replies.";
        return false;
    }

    if (!m_memoryService->SetMaintenanceConfig(config.maintenance, errorMessage))
    {
        qWarning() << "[Agent]" << errorMessage
                   << "Memory nodes will be skipped without affecting replies.";
        return false;
    }

    if (!m_memoryService->Start(config.dataDir, config.queueCapacity, errorMessage))
    {
        qWarning() << "[Agent]" << errorMessage
                   << "Memory nodes will be skipped without affecting replies.";
        return false;
    }

    emit LogMessage(QStringLiteral("Memory config loaded: %1").arg(configPath));
    return true;
}

bool AgentRuntime::LoadDefaultMemoryConfig(QString &errorMessage)
{
    const QString configPath = FindDefaultMemoryConfigPath();

    if (configPath.isEmpty())
    {
        errorMessage = QStringLiteral("memory_config.json not found.");
        return false;
    }

    return LoadMemoryConfig(configPath, errorMessage);
}

void AgentRuntime::ShutdownMemory()
{
    if ((m_memoryService != nullptr) && m_memoryService->IsRunning())
    {
        m_memoryService->Shutdown(MEMORY_SHUTDOWN_TIMEOUT_MS);
    }
}

bool AgentRuntime::IsMemoryEnabled() const
{
    return (m_memoryService != nullptr) && m_memoryService->IsRunning();
}

bool AgentRuntime::SubmitMemoryFeedback(const QStringList &memoryIds,
                                        bool helpful,
                                        quint64 &requestId)
{
    if (!IsMemoryEnabled() || memoryIds.isEmpty())
    {
        return false;
    }

    QVariant petIdValue;
    const QString petId = m_context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                              ? petIdValue.toString().trimmed()
                              : AgentRuntimeInternal::DEFAULT_PET_ID;
    QVariant triggerValue;
    const QString triggerType = m_context.GetValue(AgentContextKeys::RUNTIME_TRIGGER_TYPE,
                                                    triggerValue)
                                    ? triggerValue.toString().trimmed()
                                    : AgentRuntimeInternal::TRIGGER_TYPE_USER;
    return m_memoryService->TryEnqueueFeedback(petId,
                                               triggerType,
                                               memoryIds,
                                               helpful,
                                               requestId);
}

QStringList AgentRuntime::GetLatestSurfacedMemoryIds() const
{
    return m_latestSurfacedMemoryIds;
}

bool AgentRuntime::RequestMemoryList(quint64 &requestId)
{
    if (!IsMemoryEnabled())
    {
        return false;
    }

    QVariant petIdValue;
    const QString petId = m_context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                              ? petIdValue.toString().trimmed()
                              : AgentRuntimeInternal::DEFAULT_PET_ID;
    return m_memoryService->TryEnqueueList(petId,
                                           QStringLiteral("memory.management"),
                                           MemoryEntry::Scope::Pet,
                                           requestId);
}

bool AgentRuntime::TakeMemoryListResult(QVector<MemoryEntry> &entries)
{
    if (!IsMemoryEnabled())
    {
        return false;
    }

    QVariant petIdValue;
    const QString petId = m_context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                              ? petIdValue.toString().trimmed()
                              : AgentRuntimeInternal::DEFAULT_PET_ID;
    MemoryService::_tagRetrieveResult result;

    if (!m_memoryService->TakeLatestReadyResult(petId,
                                                QStringLiteral("memory.management"),
                                                result)
        || !result.ok)
    {
        return false;
    }

    entries = result.entries;
    return true;
}

bool AgentRuntime::UpdateMemory(const MemoryEntry &entry,
                                quint64 &requestId,
                                QString &errorMessage)
{
    if (!IsMemoryEnabled())
    {
        errorMessage = QStringLiteral("Memory service is unavailable.");
        return false;
    }

    QVariant petIdValue;
    const QString petId = m_context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                              ? petIdValue.toString().trimmed()
                              : AgentRuntimeInternal::DEFAULT_PET_ID;
    QString rejectCategory;

    if (!m_memoryService->TryEnqueueUpdate(petId,
                                           QStringLiteral("memory.management"),
                                           entry,
                                           requestId,
                                           rejectCategory))
    {
        errorMessage = rejectCategory.isEmpty()
                           ? QStringLiteral("Memory update could not be queued.")
                           : QStringLiteral("Memory update rejected: %1").arg(rejectCategory);
        return false;
    }

    return true;
}

bool AgentRuntime::ForgetMemory(const QString &memoryId, quint64 &requestId)
{
    if (!IsMemoryEnabled())
    {
        return false;
    }

    QVariant petIdValue;
    const QString petId = m_context.GetValue(AgentContextKeys::PET_ID, petIdValue)
                              ? petIdValue.toString().trimmed()
                              : AgentRuntimeInternal::DEFAULT_PET_ID;
    return m_memoryService->TryEnqueueForget(petId,
                                             QStringLiteral("memory.management"),
                                             memoryId,
                                             requestId);
}

bool AgentRuntime::ExportMemory(const QString &filePath, quint64 &requestId)
{
    if (!IsMemoryEnabled())
    {
        return false;
    }

    return m_memoryService->TryEnqueueExport(AgentRuntimeInternal::DEFAULT_PET_ID,
                                             QStringLiteral("memory.management"),
                                             filePath,
                                             requestId);
}

bool AgentRuntime::ImportMemory(const QString &filePath, quint64 &requestId)
{
    if (!IsMemoryEnabled())
    {
        return false;
    }

    return m_memoryService->TryEnqueueImport(AgentRuntimeInternal::DEFAULT_PET_ID,
                                             QStringLiteral("memory.management"),
                                             filePath,
                                             requestId);
}

bool AgentRuntime::IsLlmConfigured() const
{
    if (m_llmClient == nullptr)
    {
        return false;
    }

    return m_llmClient->IsConfigured();
}

bool AgentRuntime::IsVisionLlmConfigured() const
{
    if (m_visionLlmClient == nullptr)
    {
        return false;
    }

    return m_visionLlmClient->IsConfigured();
}

bool AgentRuntime::SetActiveVisionLlmProfile(VISION_LLM_MODEL_PROFILE profile)
{
    if (m_visionLlmClient == nullptr)
    {
        return false;
    }

    return m_visionLlmClient->SetActiveProfile(profile);
}

VISION_LLM_MODEL_PROFILE AgentRuntime::GetActiveVisionLlmProfile() const
{
    if (m_visionLlmClient == nullptr)
    {
        return VISION_LLM_MODEL_PROFILE::GPT;
    }

    return m_visionLlmClient->GetActiveProfile();
}

bool AgentRuntime::HasPendingAsyncRequest() const
{
    return m_asyncBridge.HasRequests();
}

bool AgentRuntime::Start(const QString &configPath, QString &errorMessage)
{
    if (!Load(configPath, errorMessage))
    {
        return false;
    }

    QVariant triggerValue;

    if (!m_context.GetValue(CONTEXT_KEY_RUNTIME_TRIGGER_TYPE, triggerValue)
        || triggerValue.toString().trimmed().isEmpty())
    {
        return true;
    }

    return Execute(errorMessage);
}

QVector<QString> AgentRuntime::GetExecutionOrder() const
{
    return m_graphExecutor.GetExecutionOrder();
}

AgentContext &AgentRuntime::GetContext()
{
    return m_context;
}

const AgentContext &AgentRuntime::GetContext() const
{
    return m_context;
}

void AgentRuntime::SetContext(const AgentContext &context)
{
    m_sessionContext = context.Snapshot();
    m_context = context.Snapshot();
}

bool AgentRuntime::RegisterNodeHandler(const QString &nodeType, const NodeHandler &handler)
{
    return m_nodeRegistry.Register(nodeType, handler);
}


QString AgentRuntime::FindDefaultLlmConfigPath() const
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths =
    {
        exeDir + QStringLiteral("/") + LLM_CONFIG_FILE_NAME,
        QDir::currentPath() + QStringLiteral("/") + LLM_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../") + LLM_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../../") + LLM_CONFIG_FILE_NAME
    };

    for (const QString &candidatePath : candidatePaths)
    {
        if (QFileInfo::exists(candidatePath))
        {
            return QFileInfo(candidatePath).absoluteFilePath();
        }
    }

    return QString();
}
QString AgentRuntime::FindDefaultVisionLlmConfigPath() const
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths =
    {
        exeDir + QStringLiteral("/") + VISION_LLM_CONFIG_FILE_NAME,
        QDir::currentPath() + QStringLiteral("/") + VISION_LLM_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../") + VISION_LLM_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../../") + VISION_LLM_CONFIG_FILE_NAME
    };

    for (const QString &candidatePath : candidatePaths)
    {
        if (QFileInfo::exists(candidatePath))
        {
            return QFileInfo(candidatePath).absoluteFilePath();
        }
    }

    return QString();
}
QString AgentRuntime::FindDefaultWebSearchConfigPath() const
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths =
    {
        exeDir + QStringLiteral("/") + WEB_SEARCH_CONFIG_FILE_NAME,
        QDir::currentPath() + QStringLiteral("/") + WEB_SEARCH_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../") + WEB_SEARCH_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../../") + WEB_SEARCH_CONFIG_FILE_NAME
    };

    for (const QString &candidatePath : candidatePaths)
    {
        if (QFileInfo::exists(candidatePath))
        {
            return QFileInfo(candidatePath).absoluteFilePath();
        }
    }

    return QString();
}
QString AgentRuntime::FindDefaultMemoryConfigPath() const
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths =
    {
        exeDir + QStringLiteral("/") + MEMORY_CONFIG_FILE_NAME,
        QDir::currentPath() + QStringLiteral("/") + MEMORY_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../") + MEMORY_CONFIG_FILE_NAME,
        exeDir + QStringLiteral("/../../") + MEMORY_CONFIG_FILE_NAME
    };

    for (const QString &candidatePath : candidatePaths)
    {
        if (QFileInfo::exists(candidatePath))
        {
            return QFileInfo(candidatePath).absoluteFilePath();
        }
    }

    return QString();
}

} // namespace vpet
