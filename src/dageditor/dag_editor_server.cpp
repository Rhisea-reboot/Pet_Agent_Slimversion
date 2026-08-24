#include "vpet/dageditor/dag_editor_server.h"

#include "vpet/agent/agent_runtime.h"
#include "vpet/dageditor/agent_node_catalog.h"
#include "vpet/dageditor/dag_document_store.h"
#include "vpet/dageditor/dag_http_request.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include <QUrlQuery>

namespace vpet
{

namespace
{

constexpr int EXTERNAL_CHANGE_DEBOUNCE_MS = 300;

/**
 * @brief 静态资源路径到 qrc 资源与 MIME 的映射
 */
struct _tagStaticAsset
{
    const char *urlPath;
    const char *resourcePath;
    const char *contentType;
};

const _tagStaticAsset kStaticAssets[] = {
    {"/", ":/dag_editor/index.html", "text/html; charset=utf-8"},
    {"/index.html", ":/dag_editor/index.html", "text/html; charset=utf-8"},
    {"/api.js", ":/dag_editor/api.js", "text/javascript; charset=utf-8"},
    {"/app.js", ":/dag_editor/app.js", "text/javascript; charset=utf-8"},
    {"/style.css", ":/dag_editor/style.css", "text/css; charset=utf-8"},
    {"/graph_adapter.js", ":/dag_editor/graph_adapter.js", "text/javascript; charset=utf-8"},
    {"/widgets.js", ":/dag_editor/widgets.js", "text/javascript; charset=utf-8"},
    {"/panels/library.js", ":/dag_editor/panels/library.js", "text/javascript; charset=utf-8"},
    {"/panels/property.js", ":/dag_editor/panels/property.js", "text/javascript; charset=utf-8"},
    {"/vendor/litegraph.min.js", ":/dag_editor/vendor/litegraph.min.js",
     "text/javascript; charset=utf-8"},
    {"/vendor/litegraph.css", ":/dag_editor/vendor/litegraph.css", "text/css; charset=utf-8"}};

/**
 * @brief 构造 issues 数组的 JSON 表示
 */
QJsonArray SerializeIssues(const QVector<DagValidationIssue> &issues)
{
    QJsonArray issueArray;

    for (const DagValidationIssue &issue : issues)
    {
        QJsonObject issueObject;
        issueObject.insert(QStringLiteral("severity"),
                           issue.severity == DagValidationSeverity::Error
                               ? QStringLiteral("error")
                               : QStringLiteral("warning"));
        issueObject.insert(QStringLiteral("nodeId"), issue.nodeId);
        issueObject.insert(QStringLiteral("code"), issue.code);
        issueObject.insert(QStringLiteral("message"), issue.message);
        issueArray.append(issueObject);
    }

    return issueArray;
}

} // anonymous namespace

DagEditorServer::DagEditorServer(QObject *parent)
    : QObject(parent)
    , m_httpServer(new DagHttpServer(this))
    , m_documentStore(nullptr)
    , m_agentRuntime(nullptr)
    , m_token()
    , m_configWatcher(nullptr)
    , m_externalDebounceTimer(nullptr)
{
    m_httpServer->SetRequestHandler([this](const _tagDagHttpRequest &request)
    {
        return HandleRequest(request);
    });
}

DagEditorServer::~DagEditorServer()
{
    Stop();
}

bool DagEditorServer::Start(const QString &configPath,
                            AgentRuntime *agentRuntime,
                            const AgentNodeCatalog *catalog,
                            QString &errorMessage)
{
    if (IsRunning())
    {
        errorMessage.clear();
        return true;
    }

    Stop();

    m_documentStore = new DagDocumentStore(this);

    if (!m_documentStore->Initialize(configPath, catalog, errorMessage))
    {
        Stop();
        return false;
    }

    connect(m_documentStore, &DagDocumentStore::DocumentSaved, this,
            [this](quint64 revision)
    {
        QJsonObject payload;
        payload.insert(QStringLiteral("revision"), static_cast<qint64>(revision));
        Broadcast(QStringLiteral("graph.saved"), payload);
    });

    connect(m_documentStore, &DagDocumentStore::ExternalChanged, this,
            [this](quint64 revision)
    {
        QJsonObject payload;
        payload.insert(QStringLiteral("revision"), static_cast<qint64>(revision));
        Broadcast(QStringLiteral("graph.external_changed"), payload);
    });

    m_agentRuntime = agentRuntime;

    if (m_agentRuntime != nullptr)
    {
        // 热重载结果转发到 SSE（无论重载由保存触发还是外部修改触发）。
        connect(m_agentRuntime, &AgentRuntime::DagGraphReloaded, this,
                [this](const QString &path, bool applied)
        {
            QJsonObject payload;
            payload.insert(QStringLiteral("config_path"), path);
            payload.insert(QStringLiteral("applied"), applied);

            if (!applied)
            {
                payload.insert(QStringLiteral("reason"), QStringLiteral("deferred"));
            }

            Broadcast(QStringLiteral("graph.reloaded"), payload);
        });

        connect(m_agentRuntime, &AgentRuntime::DagGraphReloadFailed, this,
                [this](const QString &path, const QString &error)
        {
            QJsonObject payload;
            payload.insert(QStringLiteral("config_path"), path);
            payload.insert(QStringLiteral("applied"), false);
            payload.insert(QStringLiteral("reason"), error);
            Broadcast(QStringLiteral("graph.reload_failed"), payload);
        });
    }

    // 外部修改监视：编辑器独立于运行时 Watcher，负责把磁盘变化采纳进文档基线。
    m_configWatcher = new QFileSystemWatcher(this);
    const QString absolutePath = QDir::cleanPath(QFileInfo(configPath).absoluteFilePath());

    if (QFileInfo::exists(absolutePath))
    {
        m_configWatcher->addPath(absolutePath);
    }

    m_externalDebounceTimer = new QTimer(this);
    m_externalDebounceTimer->setSingleShot(true);
    m_externalDebounceTimer->setInterval(EXTERNAL_CHANGE_DEBOUNCE_MS);
    connect(m_externalDebounceTimer, &QTimer::timeout,
            this, &DagEditorServer::OnConfigFileChanged);
    connect(m_configWatcher, &QFileSystemWatcher::fileChanged, this,
            [this]() { m_externalDebounceTimer->start(); });
    connect(m_configWatcher, &QFileSystemWatcher::directoryChanged, this,
            [this, absolutePath]()
    {
        // “替换式保存”会让文件路径监听失效，重新挂载后再走防抖采纳。
        if (!m_configWatcher->files().contains(absolutePath)
            && QFileInfo::exists(absolutePath))
        {
            m_configWatcher->addPath(absolutePath);
        }

        if (!m_configWatcher->files().contains(absolutePath))
        {
            return; // 文件被删除：等它重新出现
        }

        m_externalDebounceTimer->start();
    });

    m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);

    if (!m_httpServer->Start(0, errorMessage)) // 端口 0：内核分配临时端口
    {
        Stop();
        return false;
    }

    qDebug() << "[DagEditor] Serving at" << EditorUrl().toString();
    return true;
}

void DagEditorServer::Stop()
{
    if (m_httpServer != nullptr)
    {
        m_httpServer->Stop();
    }

    if (m_documentStore != nullptr)
    {
        m_documentStore->deleteLater();
        m_documentStore = nullptr;
    }

    if (m_configWatcher != nullptr)
    {
        m_configWatcher->deleteLater();
        m_configWatcher = nullptr;
    }

    if (m_externalDebounceTimer != nullptr)
    {
        m_externalDebounceTimer->stop();
    }

    m_agentRuntime = nullptr;
    m_token.clear();
}

bool DagEditorServer::IsRunning() const
{
    return (m_httpServer != nullptr) && (m_httpServer->Port() != 0)
           && !m_token.isEmpty();
}

QUrl DagEditorServer::EditorUrl() const
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(QStringLiteral("127.0.0.1"));

    if (m_httpServer != nullptr)
    {
        url.setPort(m_httpServer->Port());
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"), m_token);
    url.setQuery(query);
    return url;
}

_tagDagHttpResponse DagEditorServer::HandleRequest(const _tagDagHttpRequest &request)
{
    // ---- Host 白名单（DNS rebinding 防御）------------------------------------
    const QString hostHeader = request.HeaderValue(QStringLiteral("host"));
    const QString expectedHost =
        QStringLiteral("127.0.0.1:%1").arg(m_httpServer->Port());
    const QString altHost =
        QStringLiteral("localhost:%1").arg(m_httpServer->Port());

    if (hostHeader.compare(expectedHost, Qt::CaseInsensitive) != 0
        && hostHeader.compare(altHost, Qt::CaseInsensitive) != 0)
    {
        return JsonResponse(401, QJsonObject{{QStringLiteral("error"),
                                              QStringLiteral("invalid host header")}});
    }

    // ---- 路由分发 -------------------------------------------------------------
    const QString path = request.path;

    if (path.startsWith(QStringLiteral("/api/")))
    {
        // 全部 API 要求 token；静态资源豁免（不含敏感数据）。
        const QString providedToken =
            request.HeaderValue(QStringLiteral("x-dag-token")).trimmed();

        if ((providedToken.isEmpty()
             || providedToken != m_token))
        {
            const QString queryToken = request.QueryValue(QStringLiteral("token")).trimmed();

            if (queryToken.isEmpty() || queryToken != m_token)
            {
                return JsonResponse(401, QJsonObject{{QStringLiteral("error"),
                                                     QStringLiteral("missing or invalid token")}});
            }
        }

        if (path == QStringLiteral("/api/meta")
            && request.method == QStringLiteral("GET"))
        {
            return HandleMeta();
        }

        if (path == QStringLiteral("/api/object_info")
            && request.method == QStringLiteral("GET"))
        {
            return HandleObjectInfo();
        }

        if (path == QStringLiteral("/api/graph"))
        {
            if (request.method == QStringLiteral("GET"))
            {
                return HandleGetGraph();
            }

            if (request.method == QStringLiteral("PUT"))
            {
                return HandlePutGraph(request);
            }
        }

        if (path == QStringLiteral("/api/graph/validate")
            && request.method == QStringLiteral("POST"))
        {
            return HandleValidate(request);
        }

        if (path == QStringLiteral("/api/graph/reload")
            && request.method == QStringLiteral("POST"))
        {
            return HandleReload();
        }

        if (path == QStringLiteral("/api/runtime/status")
            && request.method == QStringLiteral("GET"))
        {
            return HandleRuntimeStatus();
        }

        if (path == QStringLiteral("/api/events")
            && request.method == QStringLiteral("GET"))
        {
            _tagDagHttpResponse streamResponse;
            streamResponse.isEventStream = true;
            streamResponse.eventChannel = QStringLiteral("main");
            return streamResponse;
        }

        return JsonResponse(404, QJsonObject{{QStringLiteral("error"),
                                             QStringLiteral("unknown api route")}});
    }

    return HandleStaticResource(request);
}

_tagDagHttpResponse DagEditorServer::HandleStaticResource(const _tagDagHttpRequest &request)
{
    for (const _tagStaticAsset &asset : kStaticAssets)
    {
        if (request.path != QLatin1String(asset.urlPath))
        {
            continue;
        }

        QFile resourceFile(QLatin1String(asset.resourcePath));

        if (!resourceFile.open(QIODevice::ReadOnly))
        {
            break;
        }

        _tagDagHttpResponse response;
        response.contentType = asset.contentType;
        response.body = resourceFile.readAll();
        // 前端脚本迭代频繁：强制浏览器每次回源校验，避免旧缓存脚本。
        response.extraHeaders.append(
            qMakePair(QByteArrayLiteral("Cache-Control"),
                      QByteArrayLiteral("no-cache")));
        return response;
    }

    return JsonResponse(404, QJsonObject{{QStringLiteral("error"),
                                         QStringLiteral("not found")}});
}

_tagDagHttpResponse DagEditorServer::HandleMeta()
{
    QJsonObject meta;
    meta.insert(QStringLiteral("app_version"),
                QCoreApplication::applicationVersion().isEmpty()
                    ? QStringLiteral("dev")
                    : QCoreApplication::applicationVersion());
    meta.insert(QStringLiteral("config_path"),
                m_documentStore != nullptr ? m_documentStore->GetConfigPath() : QString());
    meta.insert(QStringLiteral("revision"),
                m_documentStore != nullptr
                    ? static_cast<qint64>(m_documentStore->GetRevision())
                    : 0);
    meta.insert(QStringLiteral("readonly"), false);
    return JsonResponse(200, meta);
}

_tagDagHttpResponse DagEditorServer::HandleObjectInfo()
{
    QJsonObject objectInfo;

    for (const DagNodeSpec &spec : AgentNodeCatalog::Instance().All())
    {
        objectInfo.insert(spec.type, SerializeSpec(spec));
    }

    return JsonResponse(200, objectInfo);
}

QJsonObject DagEditorServer::SerializeSpec(const DagNodeSpec &spec)
{
    QJsonObject specObject;
    specObject.insert(QStringLiteral("display_name"), spec.displayName);
    specObject.insert(QStringLiteral("category"), spec.category);
    specObject.insert(QStringLiteral("color"), spec.accentColor.name());
    specObject.insert(QStringLiteral("description"), spec.description);
    specObject.insert(QStringLiteral("is_source"), spec.isSource);
    specObject.insert(QStringLiteral("trigger_source"), spec.triggerSource);
    specObject.insert(QStringLiteral("reads"), QJsonArray::fromStringList(spec.reads));
    specObject.insert(QStringLiteral("writes"), QJsonArray::fromStringList(spec.writes));

    auto serializeInput = [](const DagInputSpec &input)
    {
        QJsonObject inputObject;
        inputObject.insert(QStringLiteral("type"), static_cast<int>(input.widget));

        switch (input.widget)
        {
        case DagWidgetType::Int: inputObject[QStringLiteral("type")] = QStringLiteral("int"); break;
        case DagWidgetType::Float: inputObject[QStringLiteral("type")] = QStringLiteral("float"); break;
        case DagWidgetType::Bool: inputObject[QStringLiteral("type")] = QStringLiteral("bool"); break;
        case DagWidgetType::Enum: inputObject[QStringLiteral("type")] = QStringLiteral("enum"); break;
        case DagWidgetType::MultilineText:
            inputObject[QStringLiteral("type")] = QStringLiteral("multiline");
            break;
        case DagWidgetType::StringList:
            inputObject[QStringLiteral("type")] = QStringLiteral("string_list");
            break;
        case DagWidgetType::Text: inputObject[QStringLiteral("type")] = QStringLiteral("text"); break;
        }

        if (input.defaultValue.isValid())
        {
            inputObject.insert(QStringLiteral("default"),
                               QJsonValue::fromVariant(input.defaultValue));
        }

        if (input.minValue.isValid())
        {
            inputObject.insert(QStringLiteral("min"), input.minValue.toDouble());
        }

        if (input.maxValue.isValid())
        {
            inputObject.insert(QStringLiteral("max"), input.maxValue.toDouble());
        }

        if (input.step > 0.0)
        {
            inputObject.insert(QStringLiteral("step"), input.step);
        }

        if (!input.enumValues.isEmpty())
        {
            inputObject.insert(QStringLiteral("choices"),
                               QJsonArray::fromStringList(input.enumValues));
        }

        inputObject.insert(QStringLiteral("tooltip"), input.tooltip);
        inputObject.insert(QStringLiteral("required"), input.required);
        return inputObject;
    };

    QJsonObject requiredInputs;
    QJsonObject optionalInputs;

    for (const DagInputSpec &input : spec.inputs)
    {
        if (input.required)
        {
            requiredInputs.insert(input.key, serializeInput(input));
        }
        else
        {
            optionalInputs.insert(input.key, serializeInput(input));
        }
    }

    QJsonObject inputs;
    inputs.insert(QStringLiteral("required"), requiredInputs);
    inputs.insert(QStringLiteral("optional"), optionalInputs);
    specObject.insert(QStringLiteral("input"), inputs);
    return specObject;
}

_tagDagHttpResponse DagEditorServer::HandleGetGraph()
{
    if (m_documentStore == nullptr)
    {
        return JsonResponse(500, QJsonObject{{QStringLiteral("error"),
                                             QStringLiteral("store not initialized")}});
    }

    QJsonObject response;
    response.insert(QStringLiteral("revision"),
                    static_cast<qint64>(m_documentStore->GetRevision()));
    response.insert(QStringLiteral("graph"),
                    m_documentStore->GetDocument().ToJsonObject());
    return JsonResponse(200, response);
}

bool DagEditorServer::ParseDocumentBody(const _tagDagHttpRequest &request, DagDocument &document)
{
    QString parseError;
    return DagDocument::FromJsonData(request.body, document, parseError);
}

_tagDagHttpResponse DagEditorServer::HandlePutGraph(const _tagDagHttpRequest &request)
{
    if (m_documentStore == nullptr)
    {
        return JsonResponse(500, QJsonObject{{QStringLiteral("error"),
                                             QStringLiteral("store not initialized")}});
    }

    bool revisionOk = false;
    const quint64 baseRevision =
        request.QueryValue(QStringLiteral("baseRevision")).toULongLong(&revisionOk);

    if (!revisionOk)
    {
        return JsonResponse(400, QJsonObject{{QStringLiteral("error"),
                                             QStringLiteral("baseRevision query parameter is required")}});
    }

    DagDocument document;

    if (!ParseDocumentBody(request, document))
    {
        return JsonResponse(400, QJsonObject{{QStringLiteral("error"),
                                             QStringLiteral("request body is not a valid DAG document")}});
    }

    QVector<DagValidationIssue> issues;
    QString saveError;
    const DagDocumentStore::SaveStatus status =
        m_documentStore->Save(document, baseRevision, issues, saveError);

    switch (status)
    {
    case DagDocumentStore::SaveStatus::Invalid:
    {
        _tagDagHttpResponse response = IssuesResponse(
            400, QStringLiteral("issues"), issues);
        return response;
    }
    case DagDocumentStore::SaveStatus::Conflict:
    {
        QJsonObject conflict;
        conflict.insert(QStringLiteral("error"), QStringLiteral("revision conflict"));
        conflict.insert(QStringLiteral("revision"),
                        static_cast<qint64>(m_documentStore->GetRevision()));
        conflict.insert(QStringLiteral("graph"),
                        m_documentStore->GetDocument().ToJsonObject());
        return JsonResponse(409, conflict);
    }
    case DagDocumentStore::SaveStatus::Saved:
        break;
    }

    if (!saveError.isEmpty())
    {
        return JsonResponse(500, QJsonObject{{QStringLiteral("error"), saveError}});
    }

    QJsonObject response;
    response.insert(QStringLiteral("ok"), true);
    response.insert(QStringLiteral("revision"),
                    static_cast<qint64>(m_documentStore->GetRevision()));
    response.insert(QStringLiteral("issues"), SerializeIssues(issues));
    response.insert(QStringLiteral("reload"), TriggerHotReload());
    return JsonResponse(200, response);
}

_tagDagHttpResponse DagEditorServer::HandleValidate(const _tagDagHttpRequest &request)
{
    DagDocument document;

    if (!ParseDocumentBody(request, document))
    {
        return JsonResponse(400, QJsonObject{{QStringLiteral("error"),
                                             QStringLiteral("request body is not a valid DAG document")}});
    }

    if (m_documentStore == nullptr)
    {
        return JsonResponse(500, QJsonObject{{QStringLiteral("error"),
                                             QStringLiteral("store not initialized")}});
    }

    return IssuesResponse(200, QStringLiteral("issues"),
                          m_documentStore->Validate(document));
}

_tagDagHttpResponse DagEditorServer::HandleReload()
{
    return JsonResponse(200, TriggerHotReload());
}

_tagDagHttpResponse DagEditorServer::HandleRuntimeStatus()
{
    QJsonObject status;

    if (m_agentRuntime == nullptr)
    {
        status.insert(QStringLiteral("busy"), false);
        status.insert(QStringLiteral("active_invocation"), false);
        status.insert(QStringLiteral("pending_async"), false);
        status.insert(QStringLiteral("queued_invocations"), false);
        status.insert(QStringLiteral("runtime_available"), false);
        return JsonResponse(200, status);
    }

    const AgentRuntime::_tagRuntimeStatus runtimeStatus = m_agentRuntime->GetRuntimeStatus();
    const bool busy = runtimeStatus.invocationActive || runtimeStatus.pendingAsync;
    status.insert(QStringLiteral("busy"), busy);
    status.insert(QStringLiteral("active_invocation"), runtimeStatus.invocationActive);
    status.insert(QStringLiteral("pending_async"), runtimeStatus.pendingAsync);
    status.insert(QStringLiteral("queued_invocations"), runtimeStatus.queuedInvocations);
    status.insert(QStringLiteral("runtime_available"), true);
    return JsonResponse(200, status);
}

QJsonObject DagEditorServer::TriggerHotReload()
{
    QJsonObject reloadResult;

    if (m_agentRuntime == nullptr)
    {
        reloadResult.insert(QStringLiteral("applied"), false);
        reloadResult.insert(QStringLiteral("reason"),
                            QStringLiteral("runtime_unavailable"));
        return reloadResult;
    }

    AgentRuntime::DagReloadOutcome outcome = AgentRuntime::DagReloadOutcome::Failed;
    QString reloadError;

    if (m_agentRuntime->RequestDagReload(m_documentStore->GetConfigPath(),
                                          outcome,
                                          reloadError))
    {
        reloadResult.insert(QStringLiteral("applied"),
                            outcome == AgentRuntime::DagReloadOutcome::Applied);

        if (outcome == AgentRuntime::DagReloadOutcome::Deferred)
        {
            reloadResult.insert(QStringLiteral("reason"), QStringLiteral("deferred"));
        }
    }
    else
    {
        reloadResult.insert(QStringLiteral("applied"), false);
        reloadResult.insert(QStringLiteral("reason"), reloadError);
    }

    return reloadResult;
}

void DagEditorServer::Broadcast(const QString &eventName, const QJsonObject &payloadObject)
{
    m_httpServer->BroadcastEvent(QStringLiteral("main"),
                                 eventName,
                                 QString::fromUtf8(
                                     QJsonDocument(payloadObject)
                                         .toJson(QJsonDocument::Compact)));
}

void DagEditorServer::OnConfigFileChanged()
{
    if (m_documentStore == nullptr)
    {
        return;
    }

    QString adoptError;

    if (!m_documentStore->AdoptExternalFile(adoptError) && !adoptError.isEmpty())
    {
        qWarning() << "[DagEditor] Failed to adopt external config change:"
                   << adoptError;

        QJsonObject payload;
        payload.insert(QStringLiteral("error"), adoptError);
        Broadcast(QStringLiteral("graph.external_change_failed"), payload);
    }
}

_tagDagHttpResponse DagEditorServer::JsonResponse(int statusCode, const QJsonObject &object)
{
    _tagDagHttpResponse response;
    response.statusCode = statusCode;
    response.body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return response;
}

_tagDagHttpResponse DagEditorServer::IssuesResponse(int statusCode,
                                                    const QString &prefix,
                                                    const QVector<DagValidationIssue> &issues)
{
    QJsonObject body;

    if (!prefix.isEmpty())
    {
        body.insert(prefix, SerializeIssues(issues));
    }
    else
    {
        body.insert(QStringLiteral("issues"), SerializeIssues(issues));
    }

    body.insert(QStringLiteral("has_errors"), AgentDagValidator::HasErrors(issues));
    return JsonResponse(statusCode, body);
}

} // namespace vpet
