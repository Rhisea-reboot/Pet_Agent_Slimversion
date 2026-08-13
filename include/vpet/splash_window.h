#ifndef VPET_SPLASH_WINDOW_H
#define VPET_SPLASH_WINDOW_H

#include <QWidget>
#include <QString>
#include <QTimer>
#include <QPoint>

namespace vpet
{

/**
 * @brief 启动画面窗口
 *
 * 在 TTS 服务初始化期间显示加载动画和状态文本，
 * 服务就绪后关闭并显示主宠物窗口。
 * 使用 QPainter 绘制简单的圆形进度指示器。
 */
class SplashWindow : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父窗口
     */
    explicit SplashWindow(QWidget *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~SplashWindow() override;

    /**
     * @brief 更新状态文本
     * @param[in] text 状态文本
     */
    void SetStatusText(const QString &text);

protected:
    /**
     * @brief 绘制事件：绘制背景、动画圆环、状态文本
     * @param[in] event 绘制事件
     */
    void paintEvent(QPaintEvent *event) override;

private slots:
    /**
     * @brief 动画帧更新
     */
    void OnAnimationTick();

private:
    QString m_statusText;             ///< 当前状态文本
    QTimer *m_animationTimer;         ///< 动画定时器
    int m_rotationAngle;              ///< 当前旋转角度
    int m_dotCount;                   ///< 加载点数量

    static constexpr int WINDOW_WIDTH = 320;
    static constexpr int WINDOW_HEIGHT = 200;
    static constexpr int ARC_SIZE = 40;
    static constexpr int ARC_WIDTH = 4;
    static constexpr int ANIMATION_INTERVAL_MS = 50;
};

} // namespace vpet

#endif // VPET_SPLASH_WINDOW_H
