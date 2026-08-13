#include "vpet/web/web_search_tool.h"

#include <QSet>

namespace vpet
{

WebSearchTool::WebSearchTool(WebSearchClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client != nullptr ? client : new WebSearchClient(this))
    , m_ownsClient(client == nullptr)
    , m_activeQuery()
{
    connect(m_client, &WebSearchClient::SearchCompleted,
            this, &WebSearchTool::OnClientCompleted);
    connect(m_client, &WebSearchClient::SearchFailed,
            this, &WebSearchTool::OnClientFailed);
}

WebSearchTool::~WebSearchTool()
{
    Cancel();

    if (!m_ownsClient && (m_client != nullptr))
    {
        disconnect(m_client, nullptr, this, nullptr);
    }
}

bool WebSearchTool::SetClientConfig(const _tagWebSearchConfig &config)
{
    if (m_client == nullptr)
    {
        emit Failed(-1, QString(), QStringLiteral("Web search client is unavailable."), 0);
        return false;
    }

    return m_client->SetConfig(config);
}

bool WebSearchTool::LoadClientConfig(const QString &configPath, QString &errorMessage)
{
    if ((m_client == nullptr) || configPath.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Web search tool config input is invalid.");
        return false;
    }

    return m_client->LoadConfig(configPath, errorMessage);
}

bool WebSearchTool::IsBusy() const
{
    return (m_client != nullptr) && m_client->IsBusy();
}

int WebSearchTool::Execute(const _tagWebSearchToolRequest &request)
{
    _tagWebSearchToolRequest normalizedRequest;
    QString errorMessage;

    if (!NormalizeRequest(request, normalizedRequest, errorMessage))
    {
        emit Failed(-1, QString(), errorMessage, 0);
        return -1;
    }

    if (m_client == nullptr)
    {
        emit Failed(-1, normalizedRequest.query,
                    QStringLiteral("Web search client is unavailable."), 0);
        return -1;
    }

    if (IsBusy())
    {
        emit Failed(-1, normalizedRequest.query,
                    QStringLiteral("Web search tool is busy."), 0);
        return -1;
    }

    m_activeQuery = normalizedRequest.query;
    const int requestId = m_client->Search(normalizedRequest.query,
                                            normalizedRequest.engines);

    if (requestId <= 0)
    {
        m_activeQuery.clear();
    }

    return requestId;
}

bool WebSearchTool::Cancel()
{
    if (m_client == nullptr)
    {
        return false;
    }

    return m_client->Cancel();
}

void WebSearchTool::OnClientCompleted(int requestId,
                                      const QString &query,
                                      const QVector<_tagWebSearchResult> &results,
                                      const QStringList &partialFailures)
{
    if (query != m_activeQuery)
    {
        return;
    }

    _tagWebSearchToolResponse response;
    response.requestId = requestId;
    response.query = query;
    response.results = results;
    response.partialFailures = partialFailures;
    m_activeQuery.clear();

    emit Completed(response);
}

void WebSearchTool::OnClientFailed(int requestId,
                                   const QString &message,
                                   int statusCode)
{
    if ((requestId > 0) && m_activeQuery.isEmpty())
    {
        return;
    }

    const QString query = m_activeQuery;
    m_activeQuery.clear();
    emit Failed(requestId, query, message, statusCode);
}

bool WebSearchTool::NormalizeRequest(const _tagWebSearchToolRequest &request,
                                     _tagWebSearchToolRequest &normalizedRequest,
                                     QString &errorMessage)
{
    normalizedRequest = request;
    normalizedRequest.query = request.query.simplified();

    if (normalizedRequest.query.isEmpty())
    {
        errorMessage = QStringLiteral("Web search tool query is empty.");
        return false;
    }

    QSet<QString> uniqueEngines;
    QStringList normalizedEngines;

    for (const QString &engine : request.engines)
    {
        const QString normalizedEngine = engine.trimmed().toLower();

        if (normalizedEngine.isEmpty() || uniqueEngines.contains(normalizedEngine))
        {
            continue;
        }

        uniqueEngines.insert(normalizedEngine);
        normalizedEngines.append(normalizedEngine);
    }

    normalizedRequest.engines = normalizedEngines;
    return true;
}

} // namespace vpet
