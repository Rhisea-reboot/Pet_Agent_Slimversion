#ifndef VPET_PERCEPTION_PERCEPTION_PIPELINE_H
#define VPET_PERCEPTION_PERCEPTION_PIPELINE_H

#include "vpet/perception/frame_buffer.h"
#include "vpet/perception/vision_encoder.h"
#include "vpet/sensor/screenshot_sensor.h"

#include <QByteArray>
#include <QDateTime>
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstddef>
#include <functional>

namespace vpet
{

/**
 * @brief 视觉感知管道
 *
 * 负责协调截图传感器、图像处理链、帧缓冲和视觉编码器。
 */
class PerceptionPipeline : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 图像处理函数类型
     */
    using Processor = std::function<QPixmap(const QPixmap &)>;

    /**
     * @brief 感知管道配置
     */
    struct _tagConfig
    {
        ScreenshotSensor::_tagConfig sensorConfig;                           ///< 截图传感器配置
        std::size_t bufferCapacity = 5;                                       ///< 帧缓冲容量
        VisionEncoder::VISION_ENCODE_FORMAT encodeFormat =
            VisionEncoder::VISION_ENCODE_FORMAT::BASE64_JPEG;                ///< 输出编码格式
        VisionEncoder::_tagEncodeOptions encodeOptions = {1280, 1280, 70, true}; ///< 输出编码选项
        bool enableBuffer = false;                                            ///< 是否启用全分辨率帧缓冲
        bool enableChangeDetection = true;                                    ///< 是否仅派发显著变化的画面
        QSize changeDetectionSize = QSize(320, 180);                          ///< 本地差分缩略图最大尺寸
        int pixelDifferenceThreshold = 24;                                    ///< 单像素灰度差阈值
        int changedPixelPercent = 3;                                          ///< 触发编码的最小变化像素比例
        int minDispatchIntervalMs = 60000;                                    ///< 两次视觉派发的最小间隔
    };

    /**
     * @brief 构造函数
     * @param[in] config 管道配置
     * @param[in] parent 父对象
     */
    explicit PerceptionPipeline(const _tagConfig &config, QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~PerceptionPipeline() override;

    /**
     * @brief 启动感知管道
     * @return 启动成功返回 true
     */
    bool Start();

    /**
     * @brief 停止感知管道
     */
    void Stop();

    /**
     * @brief 判断感知管道是否运行中
     * @return 运行中返回 true
     */
    bool IsRunning() const;

    /**
     * @brief 立即捕获并处理一帧
     * @return 捕获成功返回 true
     */
    bool CaptureOnce();

    /**
     * @brief 获取最新编码数据
     * @return 最新编码数据；无有效数据时返回空数组
     */
    QByteArray GetLatestEncodedData() const;

    /**
     * @brief 获取最新帧尺寸
     * @return 最新帧尺寸；无有效帧时返回无效尺寸
     */
    QSize GetLatestFrameSize() const;

    /**
     * @brief 获取最近 N 帧的编码数据
     * @param[in] count 请求帧数
     * @return 编码数据列表，按最新到最旧排序
     */
    QVector<QByteArray> GetRecentEncodedData(std::size_t count) const;

    /**
     * @brief 获取当前缓冲帧数
     * @return 当前缓冲帧数
     */
    std::size_t GetBufferedFrameCount() const;

    /**
     * @brief 添加图像处理器
     * @param[in] processor 图像处理函数
     * @return 添加成功返回 true
     */
    bool AddProcessor(const Processor &processor);

    /**
     * @brief 清空图像处理器
     */
    void ClearProcessors();

signals:
    /**
     * @brief 编码数据就绪信号
     * @param[in] encodedData 编码后的图像数据
     * @param[in] frameId 帧序号
     */
    void DataReady(const QByteArray &encodedData, int frameId);

    /**
     * @brief 批量编码数据就绪信号
     * @param[in] batch 编码后的批量图像数据
     * @param[in] latestFrameId 最新帧序号
     */
    void BatchReady(const QVector<QByteArray> &batch, int latestFrameId);

    /**
     * @brief 错误信号
     * @param[in] message 错误描述
     */
    void ErrorOccurred(const QString &message);

private slots:
    /**
     * @brief 处理截图完成信号
     * @param[in] pixmap 截图传感器输出的原始截图
     * @param[in] frameCount 截图帧序号
     * @param[in] frameSize 截图尺寸
     */
    void OnFrameCaptured(const QPixmap &pixmap, int frameCount, const QSize &frameSize);

    /** @brief 处理后台图像编码完成事件。 */
    void OnEncodingFinished();

    /**
     * @brief 转发截图错误信号
     * @param[in] message 错误描述
     */
    void OnSensorErrorOccurred(const QString &message);

private:
    /**
     * @brief 校验并修正管道配置
     * @param[in] config 输入配置
     * @return 修正后的配置
     */
    static _tagConfig NormalizeConfig(const _tagConfig &config);

    /**
     * @brief 应用图像处理链
     * @param[in] pixmap 输入图像
     * @return 处理后的图像；处理失败时返回空图像
     */
    QPixmap ApplyProcessors(const QPixmap &pixmap) const;

    /**
     * @brief 处理传感器最新帧并在满足门控条件时提交后台编码
     * @param[in] pixmap 原始截图
     * @param[in] frameCount 帧序号
     * @param[out] errorMessage 错误描述
     * @return 处理成功返回 true
     */
    bool ProcessSensorFrame(const QPixmap &pixmap, int frameCount, QString &errorMessage);

    /** @brief 将帧缩放为灰度差分缩略图。 */
    QImage MakeChangeThumbnail(const QPixmap &pixmap) const;

    /** @brief 判断当前缩略图是否相较前帧出现显著变化。 */
    bool HasSignificantChange(const QImage &thumbnail) const;

    /** @brief 判断当前帧是否满足派发到视觉链路的本地预算。 */
    bool CanDispatchNow() const;

    /** @brief 启动后台 JPEG/Base64 编码，调用方必须位于 GUI 线程。 */
    void StartEncoding(const QImage &image, int frameId, const QSize &frameSize);

    /**
     * @brief 编码图像
     * @param[in] pixmap 输入图像
     * @return 编码数据；失败时返回空数组
     */
    QByteArray EncodePixmap(const QPixmap &pixmap) const;

    struct _tagEncodedFrame
    {
        QByteArray data;
        int frameId = -1;
        QSize frameSize;
    };

private:
    _tagConfig m_config;                  ///< 管道配置
    ScreenshotSensor *m_sensor;           ///< 截图传感器
    FrameBuffer m_frameBuffer;            ///< 帧缓冲
    QVector<Processor> m_processors;      ///< 图像处理链
    QFutureWatcher<_tagEncodedFrame> *m_encoderWatcher; ///< 单飞后台编码任务
    QByteArray m_latestEncodedData;       ///< 最新编码数据
    QSize m_latestFrameSize;              ///< 最新帧尺寸
    int m_latestFrameId;                  ///< 最新帧序号
    QImage m_lastChangeThumbnail;         ///< 上一帧用于本地差分的缩略图
    QImage m_pendingImage;                ///< 编码期间保留的最新候选帧
    int m_pendingFrameId;                 ///< 待编码候选帧序号
    QSize m_pendingFrameSize;             ///< 待编码候选帧尺寸
    QDateTime m_lastDispatchAt;           ///< 最近一次开始编码的时间
    bool m_isRunning;                     ///< 是否运行中
};

} // namespace vpet

#endif // VPET_PERCEPTION_PERCEPTION_PIPELINE_H
