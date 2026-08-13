#ifndef VPET_ANIMATION_FRAME_H
#define VPET_ANIMATION_FRAME_H

#include <QString>

namespace vpet
{

/**
 * @brief 单帧动画数据
 *
 * 保存一帧动画对应的图片文件路径与显示时长。
 */
class AnimationFrame
{
public:
    /**
     * @brief 默认构造函数
     */
    AnimationFrame();

    /**
     * @brief 构造函数
     * @param[in] imagePath 帧图片的绝对路径
     * @param[in] durationMs 帧显示时长，单位毫秒，必须大于 0
     */
    AnimationFrame(const QString &imagePath, int durationMs);

    /**
     * @brief 获取帧图片路径
     * @return 图片文件路径
     */
    QString GetImagePath() const;

    /**
     * @brief 获取帧显示时长
     * @return 显示时长，单位毫秒
     */
    int GetDurationMs() const;

    /**
     * @brief 判断当前帧数据是否有效
     * @return 路径非空且时长大于 0 时返回 true
     */
    bool IsValid() const;

private:
    QString m_imagePath; ///< 帧图片路径
    int m_durationMs;    ///< 帧显示时长（毫秒）
};

} // namespace vpet

#endif // VPET_ANIMATION_FRAME_H
