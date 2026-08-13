#ifndef VPET_SENSOR_SCREENSHOT_SENSOR_H
#define VPET_SENSOR_SCREENSHOT_SENSOR_H

#include <QByteArray>
#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>

class QTimer;

namespace vpet
{

/**
 * @brief 自动截图传感器
 *
 * 使用 QTimer 定时抓取屏幕。图像编码由感知管道异步完成，避免阻塞 GUI 事件循环。
 */
class ScreenshotSensor : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 截图传感器配置
     */
    struct _tagConfig
    {
        int intervalMs = 3000;                  ///< 截图间隔，单位毫秒
        int maxFrames = -1;                     ///< 最大截图帧数，-1 表示无限
        bool captureAllScreens = false;         ///< 是否拼接所有屏幕
        bool saveToDisk = false;                ///< 是否保存截图到磁盘
        QString saveDir;                        ///< 截图保存目录
        QString imageFormat = QStringLiteral("PNG"); ///< 图像格式
        int quality = -1;                       ///< 图像质量，-1 表示使用 Qt 默认值
        bool autoStart = false;                 ///< 构造后是否自动启动
    };

    /**
     * @brief 构造函数
     * @param[in] config 截图配置
     * @param[in] parent 父对象
     */
    explicit ScreenshotSensor(const _tagConfig &config, QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~ScreenshotSensor() override;

    /**
     * @brief 启动定时截图
     * @return 启动成功返回 true
     */
    bool Start();

    /**
     * @brief 停止定时截图
     */
    void Stop();

    /**
     * @brief 判断传感器是否运行中
     * @return 运行中返回 true
     */
    bool IsRunning() const;

    /**
     * @brief 立即截取一帧
     * @return 截图成功返回 true
     */
    bool CaptureOnce();

    /**
     * @brief 获取最新截图
     * @return 最新帧图像；无有效帧时返回空图像
     */
    QPixmap GetLatestFrame() const;

    /**
     * @brief 获取最新帧尺寸
     * @return 最新帧尺寸；无有效帧时返回无效尺寸
     */
    QSize GetFrameSize() const;

    /**
     * @brief 获取已截图帧数
     * @return 已截图帧数
     */
    int GetFrameCount() const;

    /**
     * @brief 获取最新截图保存路径
     * @return 最新保存路径；未保存时返回空字符串
     */
    QString GetLatestFilePath() const;

    /**
     * @brief 修改截图间隔
     * @param[in] intervalMs 新间隔，单位毫秒
     * @return 修改成功返回 true
     */
    bool SetInterval(int intervalMs);

signals:
    /**
     * @brief 截图完成信号
     * @param[in] pixmap 原始截图，仅可在 GUI 线程使用
     * @param[in] frameCount 截图序号
     * @param[in] frameSize 截图尺寸
     */
    void FrameCaptured(const QPixmap &pixmap, int frameCount, const QSize &frameSize);

    /**
     * @brief 错误信号
     * @param[in] message 错误描述
     */
    void ErrorOccurred(const QString &message);

private slots:
    /**
     * @brief 定时器触发截图槽
     */
    void OnTimeout();

private:
    /**
     * @brief 校验并修正配置
     * @param[in] config 输入配置
     * @return 修正后的配置
     */
    static _tagConfig NormalizeConfig(const _tagConfig &config);

    /**
     * @brief 抓取主屏幕图像
     * @return 屏幕图像；失败时返回空图像
     */
    QPixmap CapturePrimaryScreen() const;

    /**
     * @brief 抓取并拼接所有屏幕图像
     * @return 拼接后的屏幕图像；失败时返回空图像
     */
    QPixmap CaptureAllScreens() const;

    /**
     * @brief 将截图保存到磁盘
     * @param[in] pixmap 输入图像
     * @return 保存路径；无需保存或失败时返回空字符串
     */
    QString SaveFrameToDisk(const QPixmap &pixmap) const;

private:
    _tagConfig m_config;             ///< 截图配置
    QTimer *m_timer;                 ///< 定时器
    QPixmap m_latestFrame;           ///< 最新截图
    QString m_latestFilePath;        ///< 最新截图保存路径
    int m_frameCount;                ///< 已截图帧数
    bool m_isRunning;                ///< 是否运行中
};

} // namespace vpet

#endif // VPET_SENSOR_SCREENSHOT_SENSOR_H
