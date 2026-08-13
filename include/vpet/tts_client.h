#ifndef VPET_TTS_CLIENT_H
#define VPET_TTS_CLIENT_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace vpet
{

/**
 * @brief TTS 配置信息
 */
struct _tagTtsConfig
{
    QString serverUrl;      ///< TTS 服务器地址
    QString voice;          ///< Kokoro 音色（如 zf_xiaobei / zm_yunjian）
    double speed;           ///< 语速（0.5 ~ 2.0）
    QString lang;           ///< 合成语言（如 z=中文 / a=美式英语)
};

/**
 * @brief Kokoro TTS HTTP 客户端
 *
 * 通过 HTTP POST 请求将文本发送到 Kokoro API 服务器，
 * 并将返回的 WAV 音频流保存到本地文件。
 * 所有请求均为异步，通过信号通知完成。
 */
class TtsClient : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父对象
     */
    explicit TtsClient(QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~TtsClient() override;

    /**
     * @brief 从 JSON 文件加载 TTS 配置
     * @param[in] configPath 配置文件路径
     * @return 加载成功返回 true
     */
    bool LoadConfig(const QString &configPath);

    /**
     * @brief 判断是否已加载有效配置
     * @return 已配置返回 true
     */
    bool IsConfigured() const;

    /**
     * @brief 异步合成语音
     *
     * 向 TTS 服务器发送文本并异步等待音频响应。
     * 完成后发送 SynthesisFinished 信号。
     *
     * @param[in] text 要合成的文本，不得为空
     * @param[in] outputPath 输出 WAV 文件路径，不得为空
     */
    void Synthesize(const QString &text, const QString &outputPath);

signals:
    /**
     * @brief 语音合成完成信号
     * @param[in] filePath 合成后的音频文件路径；失败时为空字符串
     */
    void SynthesisFinished(const QString &filePath);

private slots:
    /**
     * @brief 处理 HTTP 响应
     * @param[in] reply 网络响应对象
     */
    void OnReplyFinished(QNetworkReply *reply);

private:
    QNetworkAccessManager *m_networkManager; ///< HTTP 网络管理器
    _tagTtsConfig m_config;                  ///< TTS 配置信息
    bool m_isConfigured;                     ///< 是否已加载配置
};

} // namespace vpet

#endif // VPET_TTS_CLIENT_H
