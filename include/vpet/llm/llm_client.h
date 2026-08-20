#ifndef VPET_LLM_LLM_CLIENT_H
#define VPET_LLM_LLM_CLIENT_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

namespace vpet
{

/**
 * @brief LLM 消息角色
 */
enum class LLM_MESSAGE_ROLE
{
    SYSTEM,
    USER,
    ASSISTANT,
    TOOL
};

/**
 * @brief LLM 单条聊天消息
 */
struct _tagLlmMessage
{
    LLM_MESSAGE_ROLE role = LLM_MESSAGE_ROLE::USER; ///< 消息角色
    QString content;                                ///< 消息正文
};

/**
 * @brief LLM 客户端配置
 */
struct _tagLlmConfig
{
    QString baseUrl; ///< OpenAI 兼容 API 根地址
    QString apiKey;  ///< API Key
    QString model;   ///< 模型 ID
    int timeoutMs = 30000; ///< HTTP 超时时间，单位毫秒
};

/**
 * @brief LLM 请求参数
 */
struct _tagLlmRequestOptions
{
    double temperature = 0.7;      ///< 采样温度，范围 0 到 2
    double topP = 1.0;             ///< 核采样参数，范围 0 到 1
    double frequencyPenalty = 0.0; ///< 频率惩罚，范围 -2 到 2
    double presencePenalty = 0.0;  ///< 存在惩罚，范围 -2 到 2
    int maxTokens = 2048;          ///< 最大输出 token 数
    bool stream = false;           ///< 是否使用 SSE 流式响应
};

/**
 * @brief OpenAI 兼容纯文本 LLM HTTP 客户端
 *
 * 仅处理文本 messages 请求，不处理图片、音频或工具调用执行逻辑。
 * 所有请求异步发送，结果通过信号返回。
 */
class LlmClient : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父对象
     */
    explicit LlmClient(QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~LlmClient() override;

    /**
     * @brief 从 JSON 配置文件加载 LLM 配置
     * @param[in] configPath 配置文件路径
     * @return 加载成功返回 true
     */
    bool LoadConfig(const QString &configPath);

    /**
     * @brief 直接设置 LLM 配置
     * @param[in] config 配置信息
     * @return 配置有效返回 true
     */
    bool SetConfig(const _tagLlmConfig &config);

    /**
     * @brief 判断客户端是否已配置
     * @return 已配置返回 true
     */
    bool IsConfigured() const;

    /**
     * @brief 从指定文件加载系统提示词
     * @param[in] contextPath 系统提示词文件路径
     * @return 加载成功返回 true
     */
    bool LoadSystemPrompt(const QString &contextPath);

    /**
     * @brief 发送多轮聊天消息
     * @param[in] messages 聊天消息列表，至少包含一条非空消息
     * @param[in] options 请求参数
     * @return 请求 ID；发送失败返回 -1
     */
    int SendChat(const QVector<_tagLlmMessage> &messages,
                 const _tagLlmRequestOptions &options);

    /**
     * @brief 使用默认参数发送多轮聊天消息
     * @param[in] messages 聊天消息列表，至少包含一条非空消息
     * @return 请求 ID；发送失败返回 -1
     */
    int SendChat(const QVector<_tagLlmMessage> &messages);

    /**
     * @brief 发送单轮用户文本
     * @param[in] prompt 用户输入文本
     * @param[in] options 请求参数
     * @return 请求 ID；发送失败返回 -1
     */
    int SendPrompt(const QString &prompt, const _tagLlmRequestOptions &options);

    /**
     * @brief 使用默认参数发送单轮用户文本
     * @param[in] prompt 用户输入文本
     * @return 请求 ID；发送失败返回 -1
     */
    int SendPrompt(const QString &prompt);

    /**
     * @brief 取消指定的在途 HTTP 请求。
     * @param[in] requestId 要取消的请求 ID。
     * @return 找到并请求取消返回 true。
     */
    bool CancelRequest(int requestId);

signals:
    /**
     * @brief 流式回复增量信号
     * @param[in] requestId 请求 ID
     * @param[in] deltaContent 本次新增文本
     */
    void ChatDelta(int requestId, const QString &deltaContent);

    /**
     * @brief 流式回复接收完成信号
     * @param[in] requestId 请求 ID
     * @param[in] fullContent 累积的完整回复文本
     */
    void ChatStreamFinished(int requestId, const QString &fullContent);

    /**
     * @brief LLM 回复完成信号
     * @param[in] requestId 请求 ID
     * @param[in] content 回复文本
     */
    void ChatCompleted(int requestId, const QString &content);

    /**
     * @brief LLM 请求失败信号
     * @param[in] requestId 请求 ID；请求未发出时为 -1
     * @param[in] message 错误描述
     * @param[in] statusCode HTTP 状态码；非 HTTP 错误时为 0
     */
    void ChatFailed(int requestId, const QString &message, int statusCode);

private slots:
    /**
     * @brief 处理 SSE 响应的可读数据
     */
    void OnReplyReadyRead();

    /**
     * @brief 处理 HTTP 响应
     * @param[in] reply 网络响应对象
     */
    void OnReplyFinished(QNetworkReply *reply);

private:
    /**
     * @brief 校验配置并补齐默认值
     * @param[in] config 输入配置
     * @param[out] normalizedConfig 输出配置
     * @param[out] errorMessage 错误描述
     * @return 配置有效返回 true
     */
    static bool NormalizeConfig(const _tagLlmConfig &config,
                                _tagLlmConfig &normalizedConfig,
                                QString &errorMessage);

    /**
     * @brief 校验请求参数并补齐范围
     * @param[in] options 输入请求参数
     * @return 修正后的请求参数
     */
    static _tagLlmRequestOptions NormalizeOptions(const _tagLlmRequestOptions &options);

    /**
     * @brief 将消息角色转换为 API 字符串
     * @param[in] role 消息角色
     * @return API 角色字符串
     */
    static QString RoleToString(LLM_MESSAGE_ROLE role);

    /**
     * @brief 从 API 响应 JSON 中提取第一条回复文本
     * @param[in] responseData 响应 JSON 字节
     * @param[out] content 回复文本
     * @param[out] errorMessage 错误描述
     * @return 提取成功返回 true
     */
    static bool ExtractAssistantContent(const QByteArray &responseData,
                                        QString &content,
                                        QString &errorMessage);

    /**
     * @brief 查找默认系统提示词文件
     * @param[in] configPath LLM 配置文件路径
     * @return 系统提示词文件路径；未找到返回空字符串
     */
    static QString FindSystemPromptPath(const QString &configPath);

    /**
     * @brief 判断消息列表是否已包含系统提示词
     * @param[in] messages 聊天消息列表
     * @return 已包含系统提示词返回 true
     */
    static bool HasSystemMessage(const QVector<_tagLlmMessage> &messages);

    /**
     * @brief 解析当前缓冲区中的完整 SSE 行
     * @param[in] requestId 请求 ID
     * @param[in] flushRemainder 是否将末尾无换行内容作为完整行处理
     */
    void ProcessStreamBuffer(int requestId, bool flushRemainder);

    QNetworkAccessManager *m_networkManager; ///< HTTP 网络管理器
    _tagLlmConfig m_config;                  ///< LLM 配置信息
    QString m_systemPrompt;                  ///< 从 context.md 读取的系统提示词
    bool m_isConfigured;                     ///< 是否已配置
    int m_nextRequestId;                     ///< 下一个请求 ID
    QHash<int, QByteArray> m_streamBuffers;  ///< 按请求隔离的 SSE 字节缓冲
    QHash<int, QString> m_accumulatedTexts;  ///< 按请求累积的流式文本
};

} // namespace vpet

#endif // VPET_LLM_LLM_CLIENT_H
