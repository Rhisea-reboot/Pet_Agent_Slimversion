#ifndef VPET_CHAT_BUBBLE_WINDOW_H
#define VPET_CHAT_BUBBLE_WINDOW_H

#include <QWidget>
#include <QString>
#include <QTimer>
#include <QPoint>
#include <QSize>

namespace vpet
{

/**
 * @brief 聊天气泡覆盖窗口
 *
 * 独立的无边框透明窗口，使用 QPainter 绘制带三角指示器的圆角聊天气泡。
 * 位于宠物窗口上方，跟随宠物位置移动。
 */
class ChatBubbleWindow : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父窗口
     */
    explicit ChatBubbleWindow(QWidget *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~ChatBubbleWindow() override;

    /**
     * @brief 显示聊天气泡
     * @param[in] text 显示文本，不得为空
     * @param[in] durationMs 显示时长（毫秒），0 表示手动隐藏
     */
    void Show(const QString &text, int durationMs);

    /**
     * @brief 隐藏气泡
     */
    void Hide();

    /**
     * @brief 随目标窗口更新气泡位置
     * @param[in] targetPosition 目标窗口（宠物窗口）的全局左上角坐标
     * @param[in] targetSize 目标窗口的尺寸
     */
    void FollowTarget(const QPoint &targetPosition, const QSize &targetSize);

    /**
     * @brief 判断气泡是否可见
     * @return 可见返回 true
     */
    bool IsVisible() const;

protected:
    /**
     * @brief 绘制事件：使用 QPainter 绘制气泡形状与文字
     * @param[in] event 绘制事件
     */
    void paintEvent(QPaintEvent *event) override;

private:
    /**
     * @brief 计算气泡的推荐尺寸
     * @return 基于文本内容计算的气泡尺寸
     */
    QSize CalculateBubbleSize() const;

private:
    QString m_text;            ///< 气泡文本
    QTimer *m_hideTimer;       ///< 自动隐藏定时器
    QPoint m_targetPosition;   ///< 跟踪目标的位置
    QSize m_targetSize;        ///< 跟踪目标的尺寸

    static constexpr int TRIANGLE_HEIGHT = 8;    ///< 三角指针高度
    static constexpr int BUBBLE_RADIUS = 10;     ///< 气泡圆角半径
    static constexpr int HORIZONTAL_PADDING = 14;///< 水平内边距
    static constexpr int VERTICAL_PADDING = 8;   ///< 垂直内边距
    static constexpr int BUBBLE_GAP = 6;         ///< 气泡底部与宠物顶部的间距
    static constexpr int MAX_BUBBLE_WIDTH = 250; ///< 气泡最大宽度
    static constexpr int MIN_BUBBLE_WIDTH = 50;  ///< 气泡最小宽度
};

} // namespace vpet

#endif // VPET_CHAT_BUBBLE_WINDOW_H
