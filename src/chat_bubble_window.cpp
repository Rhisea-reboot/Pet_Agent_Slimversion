#include "vpet/chat_bubble_window.h"

#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QFont>
#include <QtMath>

namespace vpet
{

ChatBubbleWindow::ChatBubbleWindow(QWidget *parent)
    : QWidget(parent)
    , m_text()
    , m_hideTimer(nullptr)
    , m_targetPosition(0, 0)
    , m_targetSize(100, 100)
{
    setWindowFlags(Qt::Tool
                   | Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::WindowDoesNotAcceptFocus);

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);

    connect(m_hideTimer, &QTimer::timeout, this, &ChatBubbleWindow::Hide);

    hide();
}

ChatBubbleWindow::~ChatBubbleWindow()
{
    // m_hideTimer 由 QObject 父子关系自动销毁
}

void ChatBubbleWindow::Show(const QString &text, int durationMs)
{
    // 检查参数有效性
    if (text.isEmpty())
    {
        return;
    }

    m_text = text;

    const QSize bubbleSize = CalculateBubbleSize();
    resize(bubbleSize);

    FollowTarget(m_targetPosition, m_targetSize);

    show();
    raise();

    // 启动自动隐藏定时器
    if (durationMs > 0)
    {
        m_hideTimer->start(durationMs);
    }
    else
    {
        m_hideTimer->stop();
    }
}

void ChatBubbleWindow::Hide()
{
    m_hideTimer->stop();
    m_text.clear();
    hide();
}

void ChatBubbleWindow::FollowTarget(const QPoint &targetPosition, const QSize &targetSize)
{
    // 检查参数有效性
    if (!targetSize.isValid())
    {
        return;
    }

    m_targetPosition = targetPosition;
    m_targetSize = targetSize;

    const int bubbleX = targetPosition.x()
                        + (targetSize.width() - width()) / 2;

    const int bubbleY = targetPosition.y()
                        - height()
                        - BUBBLE_GAP;

    move(bubbleX, bubbleY);
}

bool ChatBubbleWindow::IsVisible() const
{
    return isVisible();
}

void ChatBubbleWindow::paintEvent(QPaintEvent *event)
{
    (void)event;

    if (m_text.isEmpty())
    {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int bodyHeight = height() - TRIANGLE_HEIGHT;
    const int triangleCenterX = w / 2;

    // 构建气泡路径：圆角矩形主体 + 底部三角指针
    QPainterPath bubblePath;

    // 圆角矩形主体
    const QRectF bodyRect(0.0, 0.0,
                          static_cast<qreal>(w),
                          static_cast<qreal>(bodyHeight));
    bubblePath.addRoundedRect(bodyRect, BUBBLE_RADIUS, BUBBLE_RADIUS);

    // 底部三角指针
    const int triangleHalfWidth = TRIANGLE_HEIGHT;
    const QPointF trianglePoints[3] =
    {
        QPointF(triangleCenterX - triangleHalfWidth, bodyHeight),
        QPointF(triangleCenterX + triangleHalfWidth, bodyHeight),
        QPointF(triangleCenterX, bodyHeight + TRIANGLE_HEIGHT)
    };

    QPainterPath trianglePath;
    trianglePath.moveTo(trianglePoints[0]);
    trianglePath.lineTo(trianglePoints[1]);
    trianglePath.lineTo(trianglePoints[2]);
    trianglePath.closeSubpath();

    bubblePath = bubblePath.united(trianglePath);

    // 填充气泡背景（白色，约 92% 不透明度）
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 235));
    painter.drawPath(bubblePath);

    // 绘制边框
    painter.setBrush(Qt::NoBrush);
    QPen borderPen(QColor(200, 200, 200, 200), 1.0);
    painter.setPen(borderPen);
    painter.drawPath(bubblePath);

    // 绘制文字
    painter.setPen(QColor(51, 51, 51));
    QFont font = painter.font();
    font.setPointSize(11);
    painter.setFont(font);

    const QRectF textRect(HORIZONTAL_PADDING,
                          VERTICAL_PADDING,
                          w - (2 * HORIZONTAL_PADDING),
                          bodyHeight - (2 * VERTICAL_PADDING));

    painter.drawText(textRect,
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                     m_text);
}

QSize ChatBubbleWindow::CalculateBubbleSize() const
{
    QFont font;
    font.setPointSize(11);

    const QFontMetrics metrics(font);

    const int availableTextWidth = MAX_BUBBLE_WIDTH
                                   - (2 * HORIZONTAL_PADDING);

    const QRect textBounds = metrics.boundingRect(
        QRect(0, 0, availableTextWidth, 0),
        Qt::AlignLeft | Qt::TextWordWrap,
        m_text);

    const int textWidth = textBounds.width();
    const int textHeight = textBounds.height();

    const int bodyWidth = qMax(textWidth + (2 * HORIZONTAL_PADDING),
                               MIN_BUBBLE_WIDTH);

    const int bodyHeight = textHeight + (2 * VERTICAL_PADDING);

    const int totalWidth = bodyWidth;
    const int totalHeight = bodyHeight + TRIANGLE_HEIGHT;

    return QSize(totalWidth, totalHeight);
}

} // namespace vpet
