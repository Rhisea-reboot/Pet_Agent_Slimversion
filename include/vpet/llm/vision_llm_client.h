#ifndef VPET_LLM_VISION_LLM_CLIENT_H
#define VPET_LLM_VISION_LLM_CLIENT_H

#include <QByteArray>
#include <QObject>
#include <QJsonObject>
#include <QSize>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace vpet
{

/**
 * @brief 多模态 LLM 图片细节级别
 */
enum class VISION_LLM_DETAIL_LEVEL
{
    LOW,
    HIGH,
    AUTO
};

/**
 * @brief 多模态模型配置档位
 */
enum class VISION_LLM_MODEL_PROFILE
{
    GPT,
    MIMO_V2_5
};

/**
 * @brief 多模态 LLM 客户端配置
 */
struct _tagVisionLlmConfig
{
    QString baseUrl;   ///< OpenAI 兼容 API 根地址
    QString apiKey;    ///< API Key
    QString model;     ///< 支持视觉输入的模型 ID
    VISION_LLM_MODEL_PROFILE profile = VISION_LLM_MODEL_PROFILE::GPT; ///< 当前模型档位
    QString defaultPrompt; ///< 默认截图识别提示词
    QString mediaType; ///< 默认图片媒体类型，如 image/png
    int timeoutMs = 30000; ///< HTTP 超时时间，单位毫秒
};

/**
 * @brief 多模态截图识别请求参数
 */
struct _tagVisionLlmRequestOptions
{
    double temperature = 0.2; ///< 采样温度，范围 0 到 2
    int maxTokens = 1024;     ///< 最大输出 token 数
    VISION_LLM_DETAIL_LEVEL detailLevel = VISION_LLM_DETAIL_LEVEL::AUTO; ///< 图片细节级别
};

/**
 * @brief OpenAI 兼容多模态 LLM 截图识别客户端
 *
 * 接收截图模块输出的 Base64 图像，构造 image_url 多模态请求，返回模型识别文本。
 */
class VisionLlmClient : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父对象
     */
    explicit VisionLlmClient(QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~VisionLlmClient() override;

    /**
     * @brief 从 JSON 配置文件加载多模态 LLM 配置
     * @param[in] configPath 配置文件路径
     * @return 加载成功返回 true
     */
    bool LoadConfig(const QString &configPath);

    /**
     * @brief 直接设置多模态 LLM 配置
     * @param[in] config 配置信息
     * @return 配置有效返回 true
     */
    bool SetConfig(const _tagVisionLlmConfig &config);

    /**
     * @brief 判断客户端是否已配置
     * @return 已配置返回 true
     */
    bool IsConfigured() const;

    /**
     * @brief 设置当前视觉模型档位
     * @param[in] profile 目标模型档位
     * @return 切换成功返回 true
     */
    bool SetActiveProfile(VISION_LLM_MODEL_PROFILE profile);

    /**
     * @brief 获取当前视觉模型档位
     * @return 当前模型档位
     */
    VISION_LLM_MODEL_PROFILE GetActiveProfile() const;

    /**
     * @brief 识别一张 Base64 截图
     * @param[in] prompt 识别提示词
     * @param[in] base64Image 截图 Base64 数据，不包含 data URL 前缀
     * @param[in] mediaType 图片媒体类型，如 image/png
     * @param[in] options 请求参数
     * @return 请求 ID；发送失败返回 -1
     */
    int AnalyzeScreenshot(const QString &prompt,
                          const QByteArray &base64Image,
                          const QString &mediaType,
                          const _tagVisionLlmRequestOptions &options);

    /**
     * @brief 使用默认参数识别一张 Base64 截图
     * @param[in] prompt 识别提示词
     * @param[in] base64Image 截图 Base64 数据，不包含 data URL 前缀
     * @param[in] mediaType 图片媒体类型，如 image/png
     * @return 请求 ID；发送失败返回 -1
     */
    int AnalyzeScreenshot(const QString &prompt,
                          const QByteArray &base64Image,
                          const QString &mediaType);

    /**
     * @brief 取消指定的在途视觉 HTTP 请求。
     * @param[in] requestId 要取消的请求 ID。
     * @return 找到并请求取消返回 true。
     */
    bool CancelRequest(int requestId);

public slots:
    /**
     * @brief 识别 ScreenshotSensor 输出的截图帧
     * @param[in] base64Data 截图 Base64 数据
     * @param[in] frameCount 截图序号
     * @param[in] frameSize 截图尺寸
     */
    void AnalyzeCapturedFrame(const QByteArray &base64Data,
                              int frameCount,
                              const QSize &frameSize);

signals:
    /**
     * @brief 截图识别完成信号
     * @param[in] requestId 请求 ID
     * @param[in] content 模型识别结果
     */
    void AnalysisCompleted(int requestId, const QString &content);

    /**
     * @brief 截图识别失败信号
     * @param[in] requestId 请求 ID；请求未发出时为 -1
     * @param[in] message 错误描述
     * @param[in] statusCode HTTP 状态码；非 HTTP 错误时为 0
     */
    void AnalysisFailed(int requestId, const QString &message, int statusCode);

private slots:
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
    static bool NormalizeConfig(const _tagVisionLlmConfig &config,
                                _tagVisionLlmConfig &normalizedConfig,
                                QString &errorMessage);

    /**
     * @brief 校验请求参数并补齐范围
     * @param[in] options 输入请求参数
     * @return 修正后的请求参数
     */
    static _tagVisionLlmRequestOptions NormalizeOptions(
        const _tagVisionLlmRequestOptions &options);

    /**
     * @brief 将图片细节级别转换为 API 字符串
     * @param[in] detailLevel 图片细节级别
     * @return API 细节级别字符串
     */
    static QString DetailLevelToString(VISION_LLM_DETAIL_LEVEL detailLevel);

    /**
     * @brief 将配置字符串转换为图片细节级别
     * @param[in] detailLevelName 图片细节级别字符串
     * @return 图片细节级别
     */
    static VISION_LLM_DETAIL_LEVEL StringToDetailLevel(const QString &detailLevelName);

    /**
     * @brief 从 API 响应 JSON 中提取第一条回复文本
     * @param[in] responseData 响应 JSON 字节
     * @param[out] content 回复文本
     * @param[out] errorMessage 错误描述
     * @return 提取成功返回 true
     */
    static bool ExtractAssistantContent(const QByteArray &responseData,
                                        VISION_LLM_MODEL_PROFILE profile,
                                        QString &content,
                                        QString &errorMessage);

    /**
     * @brief 从多模态消息对象中提取文本
     * @param[in] message 消息对象
     * @param[in] profile 当前模型档位
     * @param[out] errorMessage 错误描述
     * @return 提取到的文本；失败时返回空字符串
     */
    static QString ExtractMessageText(const QJsonObject &message,
                                      VISION_LLM_MODEL_PROFILE profile,
                                      QString &errorMessage);

    /**
     * @brief 构造图片 data URL
     * @param[in] base64Image 图片 Base64 数据
     * @param[in] mediaType 图片媒体类型
     * @param[out] dataUrl 输出 data URL
     * @param[out] errorMessage 错误描述
     * @return 构造成功返回 true
     */
    static bool BuildImageDataUrl(const QByteArray &base64Image,
                                  const QString &mediaType,
                                  QString &dataUrl,
                                  QString &errorMessage);

private:
    /**
     * @brief 根据当前档位选择活动配置
     * @param[in] profile 模型档位
     * @return 对应配置可用返回 true
     */
    bool ActivateProfileConfig(VISION_LLM_MODEL_PROFILE profile);

    /**
     * @brief 从模型标识推断档位
     * @param[in] modelName 模型标识
     * @return 推断出的模型档位
     */
    static VISION_LLM_MODEL_PROFILE InferProfileFromModelName(const QString &modelName);

    QNetworkAccessManager *m_networkManager; ///< HTTP 网络管理器
    _tagVisionLlmConfig m_gptConfig;         ///< GPT 配置
    _tagVisionLlmConfig m_mimoConfig;        ///< MiMo 配置
    _tagVisionLlmConfig m_config;            ///< 多模态 LLM 配置信息
    _tagVisionLlmRequestOptions m_defaultOptions; ///< 默认请求参数
    bool m_isConfigured;                     ///< 是否已配置
    bool m_hasGptConfig;                     ///< GPT 配置是否可用
    bool m_hasMimoConfig;                    ///< MiMo 配置是否可用
    VISION_LLM_MODEL_PROFILE m_activeProfile; ///< 当前激活档位
    int m_nextRequestId;                     ///< 下一个请求 ID
};

} // namespace vpet

#endif // VPET_LLM_VISION_LLM_CLIENT_H
