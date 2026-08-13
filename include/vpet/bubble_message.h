#ifndef VPET_BUBBLE_MESSAGE_H
#define VPET_BUBBLE_MESSAGE_H

#include <QString>

namespace vpet
{

/**
 * @brief 气泡消息模型
 *
 * 维护一段待显示或正在显示的文字及其剩余显示时间。
 */
class BubbleMessage
{
public:
    /**
     * @brief 构造函数
     */
    BubbleMessage();

    /**
     * @brief 显示一条消息
     * @param[in] text 消息文本
     * @param[in] durationMs 显示时长，单位毫秒，必须大于 0
     */
    void Show(const QString &text, int durationMs);

    /**
     * @brief 清除当前消息
     */
    void Clear();

    /**
     * @brief 判断当前是否有可见消息
     * @return 有可见消息返回 true
     */
    bool IsVisible() const;

    /**
     * @brief 获取当前消息文本
     * @return 消息文本；无消息时返回空字符串
     */
    QString GetText() const;

    /**
     * @brief 推进时间并自动隐藏过期消息
     * @param[in] deltaTimeMs 时间间隔，单位毫秒，必须 >= 0
     */
    void Update(int deltaTimeMs);

private:
    QString m_text;    ///< 当前消息文本
    int m_remainingMs; ///< 剩余显示时间（毫秒）
    bool m_visible;    ///< 是否可见
};

} // namespace vpet

#endif // VPET_BUBBLE_MESSAGE_H
