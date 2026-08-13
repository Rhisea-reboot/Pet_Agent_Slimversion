#include "vpet/splash_window.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QScreen>
#include <QGuiApplication>
#include <QtMath>

namespace vpet
{

SplashWindow::SplashWindow(QWidget *parent)
    : QWidget(parent)
    , m_statusText()
    , m_animationTimer(nullptr)
    , m_rotationAngle(0)
    , m_dotCount(0)
{
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::Tool
                   | Qt::WindowDoesNotAcceptFocus);

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    setFixedSize(WINDOW_WIDTH, WINDOW_HEIGHT);

    // 居中到主屏幕
    const QScreen *screen = QGuiApplication::primaryScreen();

    if (screen != nullptr)
    {
        const QRect screenGeometry = screen->availableGeometry();
        const QPoint center = screenGeometry.center();
        const int x = center.x() - (WINDOW_WIDTH / 2);
        const int y = center.y() - (WINDOW_HEIGHT / 2);
        move(x, y);
    }

    // 启动动画定时器
    m_animationTimer = new QTimer(this);
    m_animationTimer->setInterval(ANIMATION_INTERVAL_MS);

    connect(m_animationTimer, &QTimer::timeout,
            this, &SplashWindow::OnAnimationTick);

    m_animationTimer->start();
}

SplashWindow::~SplashWindow()
{
    if (m_animationTimer != nullptr)
    {
        m_animationTimer->stop();
    }
}

void SplashWindow::SetStatusText(const QString &text)
{
    m_statusText = text;
    update();
}

void SplashWindow::paintEvent(QPaintEvent *event)
{
    (void)event;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 绘制半透明圆角背景
    const QRectF bgRect(0.0, 0.0,
                        static_cast<qreal>(width()),
                        static_cast<qreal>(height()));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(30, 30, 30, 220));
    painter.drawRoundedRect(bgRect, 16.0, 16.0);

    // 绘制旋转圆环动画
    painter.translate(width() / 2, height() / 2 - 20);

    const int ringCount = 8;

    for (int i = 0; i < ringCount; ++i)
    {
        const qreal angle = (360.0 / ringCount) * i + m_rotationAngle;
        const qreal radian = qDegreesToRadians(angle);
        const int dotRadius = ARC_WIDTH;

        const int cx = static_cast<int>(ARC_SIZE * 0.5 * qCos(radian));
        const int cy = static_cast<int>(ARC_SIZE * 0.5 * qSin(radian));

        // 渐变透明度：当前最前的点最亮
        const int alpha = 60 + (195 * (ringCount - i) / ringCount);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, alpha));
        painter.drawEllipse(QPoint(cx, cy), dotRadius, dotRadius);
    }

    painter.resetTransform();

    // 绘制标题
    QFont titleFont = painter.font();
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(255, 255, 255, 230));
    painter.drawText(QRectF(0, height() / 2 + 15,
                            static_cast<qreal>(width()), 30),
                     Qt::AlignHCenter | Qt::AlignVCenter,
                     QStringLiteral("Pet Agent"));

    // 绘制状态文本
    QFont statusFont = painter.font();
    statusFont.setPointSize(9);
    statusFont.setBold(false);
    painter.setFont(statusFont);
    painter.setPen(QColor(255, 255, 255, 180));
    painter.drawText(QRectF(20, height() - 45,
                            static_cast<qreal>(width()) - 40, 25),
                     Qt::AlignHCenter | Qt::AlignVCenter,
                     m_statusText);
}

void SplashWindow::OnAnimationTick()
{
    m_rotationAngle = (m_rotationAngle + 6) % 360;
    m_dotCount = (m_dotCount + 1) % 4;
    update();
}

} // namespace vpet
