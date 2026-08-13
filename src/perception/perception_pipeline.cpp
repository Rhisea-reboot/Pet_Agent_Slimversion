#include "vpet/perception/perception_pipeline.h"

#include <QDateTime>
#include <QtConcurrent>
#include <QSize>
#include <QtGlobal>

namespace vpet
{

namespace
{

constexpr std::size_t DEFAULT_BUFFER_CAPACITY = 5;
constexpr int DEFAULT_CHANGE_THRESHOLD = 24;
constexpr int DEFAULT_CHANGED_PIXEL_PERCENT = 3;
constexpr int DEFAULT_MIN_DISPATCH_INTERVAL_MS = 60000;

} // anonymous namespace

PerceptionPipeline::PerceptionPipeline(const _tagConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(NormalizeConfig(config))
    , m_sensor(new ScreenshotSensor(m_config.sensorConfig, this))
    , m_frameBuffer(m_config.bufferCapacity)
    , m_processors()
    , m_encoderWatcher(new QFutureWatcher<_tagEncodedFrame>(this))
    , m_latestEncodedData()
    , m_latestFrameSize()
    , m_latestFrameId(-1)
    , m_lastChangeThumbnail()
    , m_pendingImage()
    , m_pendingFrameId(-1)
    , m_pendingFrameSize()
    , m_lastDispatchAt()
    , m_isRunning(false)
{
    connect(m_sensor,
            &ScreenshotSensor::FrameCaptured,
            this,
            &PerceptionPipeline::OnFrameCaptured);
    connect(m_sensor,
            &ScreenshotSensor::ErrorOccurred,
             this,
             &PerceptionPipeline::OnSensorErrorOccurred);
    connect(m_encoderWatcher,
            &QFutureWatcher<_tagEncodedFrame>::finished,
            this,
            &PerceptionPipeline::OnEncodingFinished);
}

PerceptionPipeline::~PerceptionPipeline()
{
    Stop();

    if (m_encoderWatcher != nullptr)
    {
        m_encoderWatcher->waitForFinished();
    }
}

bool PerceptionPipeline::Start()
{
    if (m_isRunning)
    {
        return true;
    }

    if (m_sensor == nullptr)
    {
        emit ErrorOccurred(QStringLiteral("Perception sensor is not available."));
        return false;
    }

    // The sensor emits its initial frame synchronously from Start().
    m_isRunning = true;
    const bool started = m_sensor->Start();

    if (!started)
    {
        m_isRunning = false;
        emit ErrorOccurred(QStringLiteral("Perception sensor failed to start."));
        return false;
    }

    return true;
}

void PerceptionPipeline::Stop()
{
    if (m_sensor != nullptr)
    {
        m_sensor->Stop();
    }

    m_pendingImage = QImage();
    m_pendingFrameId = -1;
    m_pendingFrameSize = QSize();
    m_isRunning = false;
}

bool PerceptionPipeline::IsRunning() const
{
    return m_isRunning;
}

bool PerceptionPipeline::CaptureOnce()
{
    if (m_sensor == nullptr)
    {
        emit ErrorOccurred(QStringLiteral("Perception sensor is not available."));
        return false;
    }

    return m_sensor->CaptureOnce();
}

QByteArray PerceptionPipeline::GetLatestEncodedData() const
{
    return m_latestEncodedData;
}

QSize PerceptionPipeline::GetLatestFrameSize() const
{
    return m_latestFrameSize;
}

QVector<QByteArray> PerceptionPipeline::GetRecentEncodedData(std::size_t count) const
{
    QVector<QByteArray> encodedBatch;

    if ((count == 0) || !m_config.enableBuffer)
    {
        return encodedBatch;
    }

    const QVector<_tagFrame> recentFrames = m_frameBuffer.GetRecent(count);
    encodedBatch.reserve(recentFrames.size());

    for (const _tagFrame &frame : recentFrames)
    {
        if (frame.pixmap.isNull())
        {
            continue;
        }

        const QByteArray encodedData = EncodePixmap(frame.pixmap);

        if (!encodedData.isEmpty())
        {
            encodedBatch.append(encodedData);
        }
    }

    return encodedBatch;
}

std::size_t PerceptionPipeline::GetBufferedFrameCount() const
{
    if (!m_config.enableBuffer)
    {
        return 0;
    }

    return m_frameBuffer.GetSize();
}

bool PerceptionPipeline::AddProcessor(const Processor &processor)
{
    if (!processor)
    {
        return false;
    }

    m_processors.append(processor);

    return true;
}

void PerceptionPipeline::ClearProcessors()
{
    m_processors.clear();
}

void PerceptionPipeline::OnFrameCaptured(const QPixmap &pixmap,
                                          int frameCount,
                                          const QSize &frameSize)
{
    if (pixmap.isNull())
    {
        emit ErrorOccurred(QStringLiteral("Perception sensor returned an empty frame."));
        return;
    }

    if ((frameCount <= 0) || !frameSize.isValid())
    {
        emit ErrorOccurred(QStringLiteral("Perception sensor returned invalid frame metadata."));
        return;
    }

    QString errorMessage;

    if (!ProcessSensorFrame(pixmap, frameCount, errorMessage))
    {
        emit ErrorOccurred(errorMessage);
    }
}

void PerceptionPipeline::OnEncodingFinished()
{
    if (m_encoderWatcher == nullptr)
    {
        return;
    }

    const _tagEncodedFrame frame = m_encoderWatcher->result();

    if (!m_isRunning)
    {
        return;
    }

    if (frame.data.isEmpty() || (frame.frameId <= 0) || !frame.frameSize.isValid())
    {
        emit ErrorOccurred(QStringLiteral("Perception failed to encode the selected screenshot."));
    }
    else
    {
        m_latestEncodedData = frame.data;
        m_latestFrameSize = frame.frameSize;
        m_latestFrameId = frame.frameId;
        emit DataReady(m_latestEncodedData, m_latestFrameId);
    }

    if (!m_pendingImage.isNull())
    {
        const QImage pendingImage = m_pendingImage;
        const int pendingFrameId = m_pendingFrameId;
        const QSize pendingFrameSize = m_pendingFrameSize;
        m_pendingImage = QImage();
        m_pendingFrameId = -1;
        m_pendingFrameSize = QSize();
        StartEncoding(pendingImage, pendingFrameId, pendingFrameSize);
    }
}

void PerceptionPipeline::OnSensorErrorOccurred(const QString &message)
{
    const QString normalizedMessage = message.trimmed();

    if (normalizedMessage.isEmpty())
    {
        emit ErrorOccurred(QStringLiteral("Perception sensor reported an empty error."));
        return;
    }

    emit ErrorOccurred(normalizedMessage);
}

PerceptionPipeline::_tagConfig PerceptionPipeline::NormalizeConfig(const _tagConfig &config)
{
    _tagConfig normalizedConfig = config;

    if (normalizedConfig.bufferCapacity == 0)
    {
        normalizedConfig.bufferCapacity = DEFAULT_BUFFER_CAPACITY;
    }

    normalizedConfig.sensorConfig.autoStart = false;

    if (!normalizedConfig.changeDetectionSize.isValid())
    {
        normalizedConfig.changeDetectionSize = QSize(320, 180);
    }

    if ((normalizedConfig.pixelDifferenceThreshold < 1)
        || (normalizedConfig.pixelDifferenceThreshold > 255))
    {
        normalizedConfig.pixelDifferenceThreshold = DEFAULT_CHANGE_THRESHOLD;
    }

    if ((normalizedConfig.changedPixelPercent < 1)
        || (normalizedConfig.changedPixelPercent > 100))
    {
        normalizedConfig.changedPixelPercent = DEFAULT_CHANGED_PIXEL_PERCENT;
    }

    if (normalizedConfig.minDispatchIntervalMs < 0)
    {
        normalizedConfig.minDispatchIntervalMs = DEFAULT_MIN_DISPATCH_INTERVAL_MS;
    }

    return normalizedConfig;
}

QPixmap PerceptionPipeline::ApplyProcessors(const QPixmap &pixmap) const
{
    if (pixmap.isNull())
    {
        return QPixmap();
    }

    QPixmap processedPixmap = pixmap;

    for (const Processor &processor : m_processors)
    {
        if (!processor)
        {
            return QPixmap();
        }

        processedPixmap = processor(processedPixmap);

        if (processedPixmap.isNull())
        {
            return QPixmap();
        }
    }

    return processedPixmap;
}

bool PerceptionPipeline::ProcessSensorFrame(const QPixmap &pixmap,
                                            int frameCount,
                                            QString &errorMessage)
{
    if (frameCount <= 0)
    {
        errorMessage = QStringLiteral("Perception frame count is invalid.");
        return false;
    }

    if (pixmap.isNull())
    {
        errorMessage = QStringLiteral("Perception sensor frame is empty.");
        return false;
    }

    const QPixmap processedPixmap = ApplyProcessors(pixmap);

    if (processedPixmap.isNull())
    {
        errorMessage = QStringLiteral("Perception processor returned an empty frame.");
        return false;
    }

    if (m_config.enableBuffer)
    {
        _tagFrame frame;
        frame.pixmap = processedPixmap;
        frame.timestamp = QDateTime::currentDateTimeUtc();
        frame.sequenceId = frameCount;
        frame.filePath = m_sensor->GetLatestFilePath();

        if (!m_frameBuffer.Push(frame))
        {
            errorMessage = QStringLiteral("Perception failed to buffer processed frame.");
            return false;
        }
    }

    const QImage thumbnail = MakeChangeThumbnail(processedPixmap);

    if (thumbnail.isNull())
    {
        errorMessage = QStringLiteral("Perception failed to prepare the change detection thumbnail.");
        return false;
    }

    const bool hasSignificantChange = HasSignificantChange(thumbnail);
    m_lastChangeThumbnail = thumbnail;

    if (m_config.enableChangeDetection && !hasSignificantChange)
    {
        return true;
    }

    if (!CanDispatchNow())
    {
        return true;
    }

    // Convert the full frame only after the inexpensive local gate admits it.
    const QImage image = processedPixmap.toImage();

    if (image.isNull())
    {
        errorMessage = QStringLiteral("Perception failed to create an image from the selected frame.");
        return false;
    }

    if ((m_encoderWatcher != nullptr) && m_encoderWatcher->isRunning())
    {
        m_pendingImage = image;
        m_pendingFrameId = frameCount;
        m_pendingFrameSize = processedPixmap.size();
        return true;
    }

    StartEncoding(image, frameCount, processedPixmap.size());

    return true;
}

QImage PerceptionPipeline::MakeChangeThumbnail(const QPixmap &pixmap) const
{
    if (pixmap.isNull())
    {
        return QImage();
    }

    const QPixmap scaledPixmap = pixmap.scaled(m_config.changeDetectionSize,
                                               Qt::KeepAspectRatio,
                                               Qt::FastTransformation);
    return scaledPixmap.toImage().convertToFormat(QImage::Format_Grayscale8);
}

bool PerceptionPipeline::HasSignificantChange(const QImage &thumbnail) const
{
    if (thumbnail.isNull())
    {
        return false;
    }

    if (m_lastChangeThumbnail.isNull()
        || (m_lastChangeThumbnail.size() != thumbnail.size())
        || !m_config.enableChangeDetection)
    {
        return true;
    }

    const int width = thumbnail.width();
    const int height = thumbnail.height();
    const int requiredChangedPixels = (width * height * m_config.changedPixelPercent + 99) / 100;
    int changedPixels = 0;

    for (int y = 0; y < height; ++y)
    {
        const uchar *current = thumbnail.constScanLine(y);
        const uchar *previous = m_lastChangeThumbnail.constScanLine(y);

        for (int x = 0; x < width; ++x)
        {
            if (qAbs(static_cast<int>(current[x]) - static_cast<int>(previous[x]))
                >= m_config.pixelDifferenceThreshold)
            {
                ++changedPixels;

                if (changedPixels >= requiredChangedPixels)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool PerceptionPipeline::CanDispatchNow() const
{
    if (!m_lastDispatchAt.isValid())
    {
        return true;
    }

    return m_lastDispatchAt.msecsTo(QDateTime::currentDateTimeUtc())
           >= m_config.minDispatchIntervalMs;
}

void PerceptionPipeline::StartEncoding(const QImage &image,
                                       int frameId,
                                       const QSize &frameSize)
{
    if ((m_encoderWatcher == nullptr) || image.isNull() || (frameId <= 0) || !frameSize.isValid())
    {
        return;
    }

    const VisionEncoder::VISION_ENCODE_FORMAT encodeFormat = m_config.encodeFormat;
    const VisionEncoder::_tagEncodeOptions encodeOptions = m_config.encodeOptions;
    m_lastDispatchAt = QDateTime::currentDateTimeUtc();
    m_encoderWatcher->setFuture(QtConcurrent::run([image, frameId, frameSize, encodeFormat, encodeOptions]()
    {
        _tagEncodedFrame frame;
        frame.data = VisionEncoder::Encode(image, encodeFormat, encodeOptions);
        frame.frameId = frameId;
        frame.frameSize = frameSize;
        return frame;
    }));
}

QByteArray PerceptionPipeline::EncodePixmap(const QPixmap &pixmap) const
{
    if (pixmap.isNull())
    {
        return QByteArray();
    }

    return VisionEncoder::Encode(pixmap, m_config.encodeFormat, m_config.encodeOptions);
}

} // namespace vpet
