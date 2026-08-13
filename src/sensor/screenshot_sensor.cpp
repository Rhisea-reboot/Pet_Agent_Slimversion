#include "vpet/sensor/screenshot_sensor.h"
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QTimer>

namespace vpet
{

namespace
{

constexpr int MIN_CAPTURE_INTERVAL_MS = 500;
constexpr int DEFAULT_CAPTURE_INTERVAL_MS = 3000;

} // anonymous namespace

ScreenshotSensor::ScreenshotSensor(const _tagConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(NormalizeConfig(config))
    , m_timer(new QTimer(this))
    , m_latestFrame()
    , m_latestFilePath()
    , m_frameCount(0)
    , m_isRunning(false)
{
    m_timer->setInterval(m_config.intervalMs);

    connect(m_timer, &QTimer::timeout, this, &ScreenshotSensor::OnTimeout);

    if (m_config.autoStart)
    {
        Start();
    }
}

ScreenshotSensor::~ScreenshotSensor()
{
    Stop();
}

bool ScreenshotSensor::Start()
{
    if (m_isRunning)
    {
        return true;
    }

    if (m_timer == nullptr)
    {
        emit ErrorOccurred(QStringLiteral("Screenshot timer is not available."));
        return false;
    }

    m_isRunning = true;

    const bool captured = CaptureOnce();

    if (!captured)
    {
        m_isRunning = false;
        return false;
    }

    if ((m_config.maxFrames < 0) || (m_frameCount < m_config.maxFrames))
    {
        m_timer->start(m_config.intervalMs);
    }

    return true;
}

void ScreenshotSensor::Stop()
{
    if (m_timer != nullptr)
    {
        m_timer->stop();
    }

    m_isRunning = false;
}

bool ScreenshotSensor::IsRunning() const
{
    return m_isRunning;
}

bool ScreenshotSensor::CaptureOnce()
{
    if ((m_config.maxFrames >= 0) && (m_frameCount >= m_config.maxFrames))
    {
        Stop();
        return false;
    }

    const QPixmap pixmap = m_config.captureAllScreens
                           ? CaptureAllScreens()
                           : CapturePrimaryScreen();

    if (pixmap.isNull())
    {
        emit ErrorOccurred(QStringLiteral("Screen capture returned an empty frame."));
        return false;
    }

    m_latestFrame = pixmap;
    m_frameCount += 1;
    m_latestFilePath = SaveFrameToDisk(pixmap);

    emit FrameCaptured(m_latestFrame, m_frameCount, m_latestFrame.size());

    return true;
}

QPixmap ScreenshotSensor::GetLatestFrame() const
{
    return m_latestFrame;
}

QSize ScreenshotSensor::GetFrameSize() const
{
    return m_latestFrame.size();
}

int ScreenshotSensor::GetFrameCount() const
{
    return m_frameCount;
}

QString ScreenshotSensor::GetLatestFilePath() const
{
    return m_latestFilePath;
}

bool ScreenshotSensor::SetInterval(int intervalMs)
{
    if (intervalMs < MIN_CAPTURE_INTERVAL_MS)
    {
        return false;
    }

    m_config.intervalMs = intervalMs;

    if (m_timer != nullptr)
    {
        m_timer->setInterval(m_config.intervalMs);

        if (m_isRunning)
        {
            m_timer->start(m_config.intervalMs);
        }
    }

    return true;
}

void ScreenshotSensor::OnTimeout()
{
    const bool captured = CaptureOnce();

    if (!captured)
    {
        return;
    }

    if ((m_config.maxFrames >= 0) && (m_frameCount >= m_config.maxFrames))
    {
        Stop();
    }
}

ScreenshotSensor::_tagConfig ScreenshotSensor::NormalizeConfig(const _tagConfig &config)
{
    _tagConfig normalizedConfig = config;

    if (normalizedConfig.intervalMs < MIN_CAPTURE_INTERVAL_MS)
    {
        normalizedConfig.intervalMs = DEFAULT_CAPTURE_INTERVAL_MS;
    }

    const QString imageFormat = normalizedConfig.imageFormat.trimmed().toUpper();

    if ((imageFormat != QStringLiteral("PNG"))
        && (imageFormat != QStringLiteral("JPG"))
        && (imageFormat != QStringLiteral("JPEG")))
    {
        normalizedConfig.imageFormat = QStringLiteral("PNG");
    }
    else
    {
        normalizedConfig.imageFormat = imageFormat;
    }

    if ((normalizedConfig.quality < -1) || (normalizedConfig.quality > 100))
    {
        normalizedConfig.quality = -1;
    }

    return normalizedConfig;
}

QPixmap ScreenshotSensor::CapturePrimaryScreen() const
{
    QScreen *screen = QGuiApplication::primaryScreen();

    if (screen == nullptr)
    {
        return QPixmap();
    }

    return screen->grabWindow(0);
}

QPixmap ScreenshotSensor::CaptureAllScreens() const
{
    const QList<QScreen *> screens = QGuiApplication::screens();

    if (screens.isEmpty())
    {
        return QPixmap();
    }

    QRect virtualGeometry;

    for (QScreen *screen : screens)
    {
        if (screen != nullptr)
        {
            virtualGeometry = virtualGeometry.united(screen->geometry());
        }
    }

    if (!virtualGeometry.isValid())
    {
        return QPixmap();
    }

    QPixmap stitchedPixmap(virtualGeometry.size());
    stitchedPixmap.fill(Qt::transparent);

    QPainter painter(&stitchedPixmap);

    for (QScreen *screen : screens)
    {
        if (screen == nullptr)
        {
            continue;
        }

        const QPixmap screenPixmap = screen->grabWindow(0);

        if (screenPixmap.isNull())
        {
            continue;
        }

        const QPoint targetPoint = screen->geometry().topLeft() - virtualGeometry.topLeft();
        painter.drawPixmap(targetPoint, screenPixmap);
    }

    painter.end();

    return stitchedPixmap;
}

QString ScreenshotSensor::SaveFrameToDisk(const QPixmap &pixmap) const
{
    if (!m_config.saveToDisk)
    {
        return QString();
    }

    if (pixmap.isNull())
    {
        return QString();
    }

    if (m_config.saveDir.trimmed().isEmpty())
    {
        return QString();
    }

    QDir directory(m_config.saveDir);

    if (!directory.exists())
    {
        const bool created = directory.mkpath(QStringLiteral("."));

        if (!created)
        {
            return QString();
        }
    }

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(
                                  QStringLiteral("yyyyMMdd_hhmmss_zzz"));
    const QString extension = m_config.imageFormat.toLower() == QStringLiteral("jpeg")
                              ? QStringLiteral("jpg")
                              : m_config.imageFormat.toLower();
    const QString fileName = QStringLiteral("screenshot_%1_%2.%3")
                             .arg(m_frameCount)
                             .arg(timestamp, extension);
    const QString filePath = directory.filePath(fileName);
    const bool saved = pixmap.save(filePath,
                                   m_config.imageFormat.toLatin1().constData(),
                                   m_config.quality);

    if (!saved)
    {
        return QString();
    }

    return filePath;
}

} // namespace vpet
