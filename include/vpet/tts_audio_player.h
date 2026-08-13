#ifndef VPET_TTS_AUDIO_PLAYER_H
#define VPET_TTS_AUDIO_PLAYER_H

#include <QObject>
#include <QString>

class QAudioOutput;
class QMediaPlayer;

namespace vpet
{

/**
 * @brief TTS 音频播放器
 *
 * 使用 QMediaPlayer 播放 TTS 音频文件，支持播放完成信号通知。
 */
class TtsAudioPlayer : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父对象
     */
    explicit TtsAudioPlayer(QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~TtsAudioPlayer() override;

    /**
     * @brief 播放音频文件
     * @param[in] filePath 音频文件路径，不得为空
     * @return 播放请求成功发出返回 true
     */
    bool Play(const QString &filePath);

    /**
     * @brief 停止当前播放
     */
    void Stop();

    /**
     * @brief 判断是否正在播放
     * @return 正在播放返回 true
     */
    bool IsPlaying() const;

signals:
    /**
     * @brief 播放完成信号
     */
    void PlaybackFinished();

private:
    QMediaPlayer *m_mediaPlayer; ///< 媒体播放器
    QAudioOutput *m_audioOutput; ///< 音频输出设备
};

} // namespace vpet

#endif // VPET_TTS_AUDIO_PLAYER_H
