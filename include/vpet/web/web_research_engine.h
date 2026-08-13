#ifndef VPET_WEB_WEB_RESEARCH_ENGINE_H
#define VPET_WEB_WEB_RESEARCH_ENGINE_H

#include "vpet/web/web_search_tool.h"

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

class QTimer;

namespace vpet
{

/**
 * @brief web.research 研究引擎的预算与模式配置。
 *
 * V1 受约束 ReAct 不是开放式通用 Agent，所有循环和耗时都必须受该配置约束。
 */
struct _tagWebResearchConfig
{
    QString mode = QStringLiteral("auto");           ///< 检索模式：auto 或 explicit
    int maxSearchRounds = 3;                         ///< 最大研究轮数
    int maxQueriesPerRound = 2;                      ///< 每轮最大 query 数
    int maxTotalResults = 8;                         ///< 全部轮次累计最大结果数
    int maxContextChars = 6000;                      ///< Compose 上下文最大字符数
    int totalDeadlineMs = 15000;                     ///< 研究总预算（毫秒）
    bool requireCitationsForRealtimeClaims = true;   ///< 实时声明是否必须获得来源支持
    bool requireIndependentSourcesForHighImpactClaims = true; ///< 高影响声明是否要求独立来源
    QString failurePolicy = QStringLiteral("continue"); ///< 搜索失败策略：continue 或 fail
};

/**
 * @brief web.research 研究请求。
 *
 * 只携带本次研究所需的问题、引擎和预算，不读取对话历史、视觉内容或系统提示词。
 */
struct _tagWebResearchRequest
{
    QString question;                                ///< 去除显式触发词后的原始问题
    QStringList engines;                             ///< 允许使用的搜索引擎列表
    _tagWebResearchConfig config;                    ///< 研究预算与模式配置
};

/**
 * @brief 单条研究证据。
 *
 * 证据字段全部来自外部搜索结果，必须按不可信数据处理。
 */
struct _tagWebResearchEvidence
{
    QString claim;                                   ///< 该证据试图验证的关键事实
    QString sourceTitle;                             ///< 页面标题
    QString url;                                     ///< 规范化 URL
    QString publisher;                               ///< 来源站点主机名
    QString publishedAt;                             ///< 可选发布日期描述
    QString snippet;                                 ///< 搜索摘要
    bool supports = true;                            ///< 是否作为支持性证据
    QString sourceTier;                              ///< 来源层级：official/primary/reputable/unknown
    QString freshness;                               ///< 时效性：current/dated/unknown
    QString confidence;                              ///< 置信度：high/medium/low
    QString engine;                                  ///< 提供该结果的搜索引擎
};

/**
 * @brief 研究过程中发现的证据冲突记录。
 */
struct _tagWebResearchConflict
{
    QString claim;                                   ///< 存在冲突的关键事实
    QStringList sourceUrls;                          ///< 冲突涉及的来源 URL
    QString reason;                                  ///< 冲突原因描述
};

/**
 * @brief web.research 研究完成后的结构化输出。
 */
struct _tagWebResearchResponse
{
    int researchId = -1;                             ///< 本次研究标识
    QString question;                                ///< 原始问题
    bool needSearch = false;                         ///< 本轮是否执行了联网检索
    QString status;                                  ///< 最终状态：skipped/completed/empty/partial/error/throttled/cancelled
    QString reason;                                  ///< 终止原因诊断
    QString summary;                                 ///< Compose 阶段组装的研究上下文文本
    QStringList plan;                                ///< 按优先级排序的关键事实列表
    QStringList queries;                             ///< 实际发起过的 query 列表
    QVector<_tagWebResearchEvidence> evidence;       ///< 全部证据条目
    QVector<_tagWebResearchConflict> conflicts;      ///< 冲突记录
    QStringList unsupportedClaims;                   ///< 未获得证据支持的关键事实
    QStringList citations;                           ///< 引用列表，格式为“标题 - URL”
    QStringList partialFailures;                     ///< 部分引擎失败诊断信息
    int roundCount = 0;                              ///< 已完成的检索轮数
    int searchCount = 0;                             ///< 实际发起的搜索次数
};

/**
 * @brief 受约束的 web.research 研究状态机。
 *
 * 状态转换：Decide -> Search -> Observe -> Assess -> Repeat / Compose。
 * 该引擎只负责研究决策、预算执行、证据收集与摘要组装，不读取 AgentContext，
 * 不构造最终 LLM 提示词，也不向跨轮状态写入任何数据。
 */
class WebResearchEngine : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造研究引擎。
     * @param[in] tool 单次搜索工具；为空时由引擎创建并持有。
     * @param[in] parent QObject 父对象。
     */
    explicit WebResearchEngine(WebSearchTool *tool = nullptr,
                               QObject *parent = nullptr);

    /**
     * @brief 销毁研究引擎并取消活动研究。
     */
    ~WebResearchEngine() override;

    /**
     * @brief 设置底层搜索客户端配置。
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
     * @brief 判断引擎是否正在执行研究。
     * @return 有活动研究返回 true。
     */
    bool IsBusy() const;

    /**
     * @brief 启动一次受约束的联网研究。
     * @param[in] request 研究请求。
     * @return 研究 ID；输入无效或引擎忙时返回 -1。
     */
    virtual int Start(const _tagWebResearchRequest &request);

    /**
     * @brief 取消当前研究。
     * @return 成功取消返回 true。
     */
    bool Cancel();

    /**
     * @brief 剥离显式搜索触发词。
     *
     * 支持前缀：/search、联网搜索、联网搜、搜索、搜一下、查一下、查查、帮我查。
     * 前缀后必须紧跟冒号或空白才剥离，避免误伤普通词汇。
     * @param[in] question 用户原始问题。
     * @return 剥离后的查询文本；无触发词时返回原问题。
     */
    static QString StripExplicitPrefix(const QString &question);

    /**
     * @brief 按明确规则判断问题是否需要联网检索。
     *
     * 规则顺序：用户明确拒绝联网 -> 显式检索请求 -> 时效性/高影响关键词 -> 默认不检索。
     * @param[in] question 待判断的问题。
     * @return 需要联网检索返回 true。
     */
    static bool ShouldSearch(const QString &question);

    /**
     * @brief 将问题分解为按优先级排序的关键事实列表。
     *
     * 优先级：引号内的明确事实 -> 完整问题 -> 按分隔符拆分出的子问题。
     * @param[in] question 待分解的问题。
     * @return 关键事实列表；输入为空时返回空列表。
     */
    static QStringList DecomposeClaims(const QString &question);

signals:
    /**
     * @brief 研究完成（包括未触发检索、预算耗尽和降级完成）。
     * @param[in] response 结构化研究结果。
     */
    void Completed(const _tagWebResearchResponse &response);

    /**
     * @brief 研究失败或取消。
     * @param[in] researchId 研究 ID；输入校验失败时为 -1。
     * @param[in] message 脱敏错误描述。
     * @param[in] statusCode HTTP 状态码；非 HTTP 错误时为 0。
     */
    void Failed(int researchId, const QString &message, int statusCode);

private slots:
    /**
     * @brief 处理单次搜索成功。
     * @param[in] response 单次搜索的结构化输出。
     */
    void OnToolCompleted(const _tagWebSearchToolResponse &response);

    /**
     * @brief 处理单次搜索失败。
     * @param[in] requestId 客户端请求 ID。
     * @param[in] query 失败时的查询文本。
     * @param[in] message 脱敏错误描述。
     * @param[in] statusCode HTTP 状态码。
     */
    void OnToolFailed(int requestId,
                      const QString &query,
                      const QString &message,
                      int statusCode);

    /**
     * @brief 处理研究总预算超时。
     */
    void OnDeadlineTimeout();

private:
    enum WEB_RESEARCH_STATE
    {
        WEB_RESEARCH_STATE_IDLE = 0,
        WEB_RESEARCH_STATE_SEARCHING
    };

    /**
     * @brief 校验并归一化研究请求。
     * @param[in] request 输入请求。
     * @param[out] normalizedRequest 归一化请求。
     * @param[out] errorMessage 错误描述。
     * @return 请求有效返回 true。
     */
    static bool NormalizeRequest(const _tagWebResearchRequest &request,
                                 _tagWebResearchRequest &normalizedRequest,
                                 QString &errorMessage);

    /**
     * @brief 执行 Decide 阶段并按第一轮计划开始检索。
     * @param[in] request 归一化请求。
     */
    void BeginResearch(const _tagWebResearchRequest &request);

    /**
     * @brief 发起当前轮次的下一跳查询。
     */
    void IssueNextQuery();

    /**
     * @brief 收集当前查询返回的结果为证据。
     * @param[in] results 单次搜索结果。
     */
    void ObserveResults(const QVector<_tagWebSearchResult> &results);

    /**
     * @brief 评估当前轮次证据并决定继续或终止。
     */
    void AssessRound();

    /**
     * @brief 按独立来源数量为证据分配置信度。
     */
    void ComputeConfidence();

    /**
     * @brief 按数值事实差异检测来源冲突。
     */
    void DetectConflicts();

    /**
     * @brief 以指定终止原因结束研究并组装输出。
     * @param[in] reason 终止原因诊断。
     */
    void FinishResearch(const QString &reason);

    /**
     * @brief 发射失败信号并复位引擎。
     * @param[in] message 脱敏错误描述。
     * @param[in] statusCode HTTP 状态码。
     */
    void EmitFailureAndReset(const QString &message, int statusCode);

    /**
     * @brief 复位全部研究状态。
     */
    void ResetState();

    /**
     * @brief 根据终止原因和证据推导最终状态。
     * @return 最终状态字符串。
     */
    QString DeriveStatus() const;

    /**
     * @brief 组装研究上下文摘要文本。
     * @return Compose 输出文本。
     */
    QString ComposeSummary() const;

    /**
     * @brief 规范化来源主机名。
     * @param[in] url 结果 URL。
     * @return 规范化主机名。
     */
    static QString NormalizeSourceHost(const QString &url);

    /**
     * @brief 按主机名分类来源层级。
     * @param[in] url 结果 URL。
     * @return official/reputable/unknown。
     */
    static QString ClassifySourceTier(const QString &url);

    /**
     * @brief 从摘要文本提取时效性。
     * @param[in] snippet 搜索摘要。
     * @return current/dated/unknown。
     */
    static QString ClassifyFreshness(const QString &snippet);

    /**
     * @brief 从摘要文本提取可选发布日期。
     * @param[in] snippet 搜索摘要。
     * @return 日期描述；未发现时返回空字符串。
     */
    static QString ExtractPublishedDate(const QString &snippet);

    /**
     * @brief 提取文本中的数值事实 token。
     * @param[in] text 待提取文本。
     * @return 数值 token 列表。
     */
    static QStringList ExtractNumericTokens(const QString &text);

    /**
     * @brief 清理外部文本中的控制字符。
     * @param[in] text 外部文本。
     * @return 清理后的文本。
     */
    static QString SanitizeExternalText(const QString &text);

    /**
     * @brief 根据失败消息分类失败类型。
     * @param[in] message 失败消息。
     * @return throttled/timeout/cancelled/search_failed。
     */
    static QString ClassifyFailure(const QString &message);

    /**
     * @brief 判断声明是否属于需要独立来源支持的高影响领域。
     * @param[in] claim 待判断声明。
     * @return 属于高影响领域返回 true。
     */
    static bool IsHighImpactClaim(const QString &claim);

    /**
     * @brief 判断声明是否已获得足够证据。
     * @param[in] claim 待判断声明。
     * @return 满足当前配置的证据要求返回 true。
     */
    bool HasSufficientEvidence(const QString &claim) const;

    WebSearchTool *m_tool;                 ///< 单次搜索工具
    bool m_ownsTool;                       ///< 是否持有底层工具
    QTimer *m_deadlineTimer;               ///< 研究总预算计时器

    int m_nextResearchId;                  ///< 下一个研究 ID
    int m_activeResearchId;                ///< 当前研究 ID
    WEB_RESEARCH_STATE m_state;            ///< 当前研究状态
    QString m_mode;                        ///< 归一化检索模式
    QString m_failurePolicy;               ///< 归一化失败策略
    QStringList m_engines;                 ///< 归一化引擎列表
    int m_maxSearchRounds;                 ///< 归一化最大轮数
    int m_maxQueriesPerRound;              ///< 归一化每轮最大 query 数
    int m_maxTotalResults;                 ///< 归一化最大结果数
    int m_maxContextChars;                 ///< Compose 上下文最大字符数
    bool m_requireCitationsForRealtimeClaims; ///< 实时声明是否要求引用
    bool m_requireIndependentSourcesForHighImpactClaims; ///< 高影响声明是否要求独立来源
    qint64 m_deadlineAtMs;                 ///< 研究预算截止时间戳

    QString m_question;                    ///< 归一化问题
    QStringList m_claimsOrdered;           ///< 按优先级排序的关键事实
    QStringList m_roundQueries;            ///< 当前轮次待发起的 query
    int m_roundQueryIndex;                 ///< 当前轮次下一 query 下标
    int m_roundCount;                      ///< 已完成的检索轮数
    int m_searchCount;                     ///< 已发起的搜索次数
    int m_activeRequestId;                 ///< 当前在途搜索请求 ID
    QString m_activeClaim;                 ///< 当前在途搜索对应的关键事实
    QString m_failureType;                 ///< 最近一次失败类型
    bool m_abortRequested;                 ///< 是否已请求终止
    QVector<_tagWebResearchEvidence> m_evidence; ///< 已收集证据
    QVector<_tagWebResearchConflict> m_conflicts; ///< 已检测冲突
    QStringList m_partialFailures;         ///< 累积的部分失败诊断
    QStringList m_queriesIssued;           ///< 已发起的 query 文本列表
    QStringList m_unsupportedClaims;       ///< 未获得证据支持的关键事实
    QString m_reason;                      ///< 终止原因
    QSet<QString> m_seenUrls;              ///< 已收录 URL
    QSet<QString> m_seenTitleKeys;         ///< 已收录的“主机名|标题”键
};

} // namespace vpet

Q_DECLARE_METATYPE(vpet::_tagWebResearchRequest)
Q_DECLARE_METATYPE(vpet::_tagWebResearchResponse)

#endif // VPET_WEB_WEB_RESEARCH_ENGINE_H
