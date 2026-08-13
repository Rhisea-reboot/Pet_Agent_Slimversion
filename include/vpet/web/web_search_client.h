#ifndef VPET_WEB_WEB_SEARCH_CLIENT_H
#define VPET_WEB_WEB_SEARCH_CLIENT_H

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace vpet
{

/**
 * @brief 单条网页搜索结果。
 */
struct _tagWebSearchResult
{
    QString title;
    QString url;
    QString description;
    QString source;
    QString engine;
};

/**
 * @brief 网页搜索客户端配置。
 */
struct _tagWebSearchConfig
{
    QUrl baseUrl;
    QString authorizationToken;
    int totalTimeoutMs = 15000;
    int minSearchIntervalMs = 3000;
    int maxThrottleWaitMs = 3000;
    int maxResponseBytes = 1024 * 1024;
    int maxResults = 5;
    int maxDescriptionChars = 500;
};

/**
 * @brief open-webSearch REST 单次搜索客户端。
 *
 * 客户端只负责 REST 协议、请求生命周期和结果解析，不负责构造 LLM 提示词。
 */
class WebSearchClient : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造网页搜索客户端。
     * @param[in] parent QObject 父对象。
     */
    explicit WebSearchClient(QObject *parent = nullptr);

    /**
     * @brief 销毁客户端并取消活动请求。
     */
    ~WebSearchClient() override;

    /**
     * @brief 设置并校验客户端配置。
     * @param[in] config 搜索服务配置。
     * @return 配置有效返回 true。
     */
    bool SetConfig(const _tagWebSearchConfig &config);

    /**
     * @brief 从 JSON 文件加载搜索客户端配置。
     * @param[in] configPath 配置文件路径。
     * @param[out] errorMessage 错误描述。
     * @return 加载成功返回 true。
     */
    bool LoadConfig(const QString &configPath, QString &errorMessage);

    /**
     * @brief 判断客户端是否已配置。
     * @return 已配置返回 true。
     */
    bool IsConfigured() const;

    /**
     * @brief 判断是否有活动请求。
     * @return 有活动请求返回 true。
     */
    bool IsBusy() const;

    /**
     * @brief 发起一次网页搜索。
     * @param[in] query 非空搜索关键词。
     * @param[in] engines 允许使用的搜索引擎名称。
     * @return 请求 ID；无法发起时返回 -1。
     */
    int Search(const QString &query, const QStringList &engines);

    /**
     * @brief 取消当前搜索请求。
     * @return 存在可取消请求并已取消返回 true。
     */
    bool Cancel();

signals:
    /**
     * @brief 搜索成功完成。
     * @param[in] requestId 请求 ID。
     * @param[in] query 原始搜索关键词。
     * @param[in] results 已解析的搜索结果。
     * @param[in] partialFailures 部分引擎失败信息。
     */
    void SearchCompleted(int requestId,
                         const QString &query,
                         const QVector<_tagWebSearchResult> &results,
                         const QStringList &partialFailures);

    /**
     * @brief 搜索失败或被取消。
     * @param[in] requestId 请求 ID；请求未发出时为 -1。
     * @param[in] message 脱敏错误描述。
     * @param[in] statusCode HTTP 状态码；非 HTTP 错误时为 0。
     */
    void SearchFailed(int requestId, const QString &message, int statusCode);

private slots:
    /**
     * @brief 在冷却计时结束后发送请求。
     */
    void OnThrottleTimeout();

    /**
     * @brief 处理网络响应完成事件。
     * @param[in] reply 网络响应对象。
     */
    void OnReplyFinished(QNetworkReply *reply);

    /**
     * @brief 处理响应流式数据到达事件。
     */
    void OnReadyRead();

    /**
     * @brief 校验响应头中的声明长度。
     */
    void OnMetaDataChanged();

    /**
     * @brief 处理总超时事件。
     */
    void OnRequestTimeout();

private:
    /**
     * @brief 校验并归一化配置。
     * @param[in] config 输入配置。
     * @param[out] normalizedConfig 归一化配置。
     * @param[out] errorMessage 错误描述。
     * @return 配置有效返回 true。
     */
    static bool NormalizeConfig(const _tagWebSearchConfig &config,
                                _tagWebSearchConfig &normalizedConfig,
                                QString &errorMessage);

    /**
     * @brief 创建并发送实际 HTTP 请求。
     */
    void StartRequest();

    /**
     * @brief 结束当前请求并清理活动资源。
     * @param[in] emitCancellation 是否发出取消错误。
     */
    void FinishCancellation(bool emitCancellation);

    /**
     * @brief 解析并校验搜索服务 JSON 响应。
     * @param[in] responseData 响应正文。
     * @param[out] results 解析结果。
     * @param[out] partialFailures 部分失败信息。
     * @param[out] errorMessage 协议错误描述。
     * @return 响应有效返回 true。
     */
    bool ParseResponse(const QByteArray &responseData,
                       QVector<_tagWebSearchResult> &results,
                       QStringList &partialFailures,
                       QString &errorMessage) const;

    /**
     * @brief 规范化并校验单个结果 URL。
     * @param[in] value 外部返回的 URL。
     * @param[out] normalizedUrl 规范化 URL。
     * @return URL 可用返回 true。
     */
    static bool NormalizeResultUrl(const QString &value, QString &normalizedUrl);

    QNetworkAccessManager *m_networkManager;
    QNetworkReply *m_activeReply;
    QTimer *m_throttleTimer;
    QTimer *m_timeoutTimer;
    _tagWebSearchConfig m_config;
    QString m_pendingQuery;
    QStringList m_pendingEngines;
    QByteArray m_responseData;
    qint64 m_lastSearchStartedAtMs;
    qint64 m_deadlineAtMs;
    int m_nextRequestId;
    int m_activeRequestId;
    bool m_isConfigured;
};

} // namespace vpet

Q_DECLARE_METATYPE(vpet::_tagWebSearchResult)
Q_DECLARE_METATYPE(QVector<vpet::_tagWebSearchResult>)

#endif // VPET_WEB_WEB_SEARCH_CLIENT_H
