#ifndef VPET_WEB_WEB_SEARCH_TOOL_H
#define VPET_WEB_WEB_SEARCH_TOOL_H

#include "vpet/web/web_search_client.h"

#include <QObject>
#include <QStringList>

namespace vpet
{

/**
 * @brief 单次 web.search 工具的输入。
 *
 * 工具只接受本次检索所需的 query 和引擎列表，不读取对话历史、视觉内容或系统提示词。
 */
struct _tagWebSearchToolRequest
{
    QString query;
    QStringList engines;
};

/**
 * @brief 单次 web.search 工具的结构化输出。
 */
struct _tagWebSearchToolResponse
{
    int requestId = -1;
    QString query;
    QVector<_tagWebSearchResult> results;
    QStringList partialFailures;
};

/**
 * @brief 可测试的单次 web.search 工具层。
 *
 * 该类只把 query 转换为结构化搜索结果，不构造 LLM prompt，不写入 AgentContext，
 * 也不负责研究循环、证据评估或引用摘要。
 */
class WebSearchTool : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造单次搜索工具。
     * @param[in] client HTTP 搜索客户端；为空时由工具创建并持有。
     * @param[in] parent QObject 父对象。
     */
    explicit WebSearchTool(WebSearchClient *client = nullptr,
                           QObject *parent = nullptr);

    /**
     * @brief 销毁搜索工具并取消活动调用。
     */
    ~WebSearchTool() override;

    /**
     * @brief 设置 HTTP 搜索客户端配置。
     * @param[in] config 搜索服务配置。
     * @return 配置有效返回 true。
     */
    bool SetClientConfig(const _tagWebSearchConfig &config);

    /**
     * @brief 从 JSON 文件加载底层搜索客户端配置。
     * @param[in] configPath 配置文件路径。
     * @param[out] errorMessage 错误描述。
     * @return 加载成功返回 true。
     */
    bool LoadClientConfig(const QString &configPath, QString &errorMessage);

    /**
     * @brief 判断工具是否有活动调用。
     * @return 有活动调用返回 true。
     */
    bool IsBusy() const;

    /**
     * @brief 执行一次结构化网页搜索。
     * @param[in] request query 和允许使用的引擎。
     * @return 请求 ID；输入无效或工具忙时返回 -1。
     */
    int Execute(const _tagWebSearchToolRequest &request);

    /**
     * @brief 取消当前工具调用。
     * @return 成功取消返回 true。
     */
    bool Cancel();

signals:
    /**
     * @brief 单次搜索成功完成。
     * @param[in] response 结构化搜索结果。
     */
    void Completed(const _tagWebSearchToolResponse &response);

    /**
     * @brief 单次搜索失败。
     * @param[in] requestId 请求 ID；输入校验失败时为 -1。
     * @param[in] query 原始归一化 query；输入不可用时为空。
     * @param[in] message 脱敏错误描述。
     * @param[in] statusCode HTTP 状态码；非 HTTP 错误时为 0。
     */
    void Failed(int requestId,
                const QString &query,
                const QString &message,
                int statusCode);

private slots:
    /**
     * @brief 转发底层客户端的成功结果。
     * @param[in] requestId 请求 ID。
     * @param[in] query 搜索 query。
     * @param[in] results 结构化结果。
     * @param[in] partialFailures 部分引擎失败信息。
     */
    void OnClientCompleted(int requestId,
                           const QString &query,
                           const QVector<_tagWebSearchResult> &results,
                           const QStringList &partialFailures);

    /**
     * @brief 转发底层客户端的失败结果。
     * @param[in] requestId 请求 ID。
     * @param[in] message 错误描述。
     * @param[in] statusCode HTTP 状态码。
     */
    void OnClientFailed(int requestId, const QString &message, int statusCode);

private:
    /**
     * @brief 校验工具输入并归一化 query 和引擎列表。
     * @param[in] request 输入请求。
     * @param[out] normalizedRequest 归一化请求。
     * @param[out] errorMessage 错误描述。
     * @return 输入有效返回 true。
     */
    static bool NormalizeRequest(const _tagWebSearchToolRequest &request,
                                  _tagWebSearchToolRequest &normalizedRequest,
                                  QString &errorMessage);

    WebSearchClient *m_client;
    bool m_ownsClient;
    QString m_activeQuery;
};

} // namespace vpet

Q_DECLARE_METATYPE(vpet::_tagWebSearchToolRequest)
Q_DECLARE_METATYPE(vpet::_tagWebSearchToolResponse)

#endif // VPET_WEB_WEB_SEARCH_TOOL_H
