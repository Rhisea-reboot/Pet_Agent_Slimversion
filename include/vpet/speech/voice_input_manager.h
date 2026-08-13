#ifndef VPET_SPEECH_VOICE_INPUT_MANAGER_H
#define VPET_SPEECH_VOICE_INPUT_MANAGER_H

#include <QMediaRecorder>
#include <QObject>
#include <QString>

class QAudioInput;
class QMediaCaptureSession;
class QProcess;
class QTimer;

namespace vpet
{

/**
 * @brief 按键语音输入管理器
 *
 * 负责录制用户语音、调用 SenseVoice 转写脚本，并输出识别文本。
 */
class VoiceInputManager : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父对象
     */
    explicit VoiceInputManager(QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~VoiceInputManager() override;

    /**
     * @brief 开始录音
     * @return 启动成功返回 true
     */
    bool StartRecording();

    /**
     * @brief 停止录音并启动 ASR
     * @return 停止成功返回 true
     */
    bool StopRecording();

    /**
     * @brief 判断是否正在录音
     * @return 正在录音返回 true
     */
    bool IsRecording() const;

signals:
    /**
     * @brief 录音开始信号
     */
    void RecordingStarted();

    /**
     * @brief 录音停止信号
     * @param[in] audioPath 录音文件路径
     */
    void RecordingStopped(const QString &audioPath);

    /**
     * @brief 语音识别完成信号
     * @param[in] text 识别文本
     */
    void TranscriptionCompleted(const QString &text);

    /**
     * @brief 语音识别失败信号
     * @param[in] message 错误描述
     */
    void TranscriptionFailed(const QString &message);

private slots:
    /**
     * @brief 处理 ASR 进程结束
     * @param[in] exitCode 进程退出码
     * @param[in] exitStatus 进程退出状态
     */
    void OnAsrProcessFinished(int exitCode, int exitStatus);

    /**
     * @brief 处理录音错误
     */
    void OnRecorderError();

    /**
     * @brief 处理录音器状态变化
     * @param[in] state 录音器状态
     */
    void OnRecorderStateChanged(QMediaRecorder::RecorderState state);

    /** @brief 处理录音器停止超时。 */
    void OnRecorderStopTimeout();

    /** @brief 处理 ASR 子进程超时。 */
    void OnAsrProcessTimeout();

private:
    /**
     * @brief 准备本次录音目录
     * @param[out] errorMessage 错误描述
     * @return 准备成功返回 true
     */
    bool PrepareRecordDirectory(QString &errorMessage);

    /**
     * @brief 启动 SenseVoice ASR 进程
     * @param[out] errorMessage 错误描述
     * @return 启动成功返回 true
     */
    bool StartAsrProcess(QString &errorMessage);

    /**
     * @brief 查找项目根目录
     * @return 项目根目录；未找到返回空字符串
     */
    QString FindProjectRootPath() const;

    /**
     * @brief 查找 Python 可执行程序
     * @return Python 可执行程序路径或命令名
     */
    QString FindPythonExecutable() const;

    /**
     * @brief 从 ASR 输出文件读取识别文本
     * @param[out] text 识别文本
     * @param[out] errorMessage 错误描述
     * @return 读取成功返回 true
     */
    bool ReadTranscriptionText(QString &text, QString &errorMessage) const;

    /**
     * @brief 删除当前语音会话产生的临时目录并清空路径状态
     * @return 目录不存在或删除成功返回 true
     */
    bool CleanupRecordDirectory();

    QMediaCaptureSession *m_captureSession; ///< Qt 多媒体采集会话
    QAudioInput *m_audioInput;              ///< 麦克风输入
    QMediaRecorder *m_mediaRecorder;        ///< 音频录制器
    QProcess *m_asrProcess;                 ///< SenseVoice ASR 进程
    QTimer *m_recorderStopTimer;            ///< 等待录音器停止的看门狗
    QTimer *m_asrTimeoutTimer;              ///< ASR 子进程执行看门狗
    QString m_recordSessionDirectory;       ///< 当前语音会话临时目录
    QString m_recordInputDirectory;         ///< 当前录音输入目录
    QString m_recordOutputDirectory;        ///< 当前 ASR 输出目录
    QString m_recordAudioPath;              ///< 当前录音文件路径
    QString m_asrOutputFilePath;            ///< 当前 ASR 输出 list 文件路径
    bool m_isRecording;                     ///< 是否正在录音
    bool m_awaitingRecorderStop;            ///< 是否等待录音器真正停止后再启动 ASR
    bool m_asrTimedOut;                     ///< 是否已报告本轮 ASR 超时
};

} // namespace vpet

#endif // VPET_SPEECH_VOICE_INPUT_MANAGER_H
