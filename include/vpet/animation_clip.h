#ifndef VPET_ANIMATION_CLIP_H
#define VPET_ANIMATION_CLIP_H

#include "vpet/animation_segment.h"
#include "vpet/common_types.h"

#include <QMap>
#include <QString>

namespace vpet
{

/**
 * @brief 完整动画剪辑，包含某动作某情绪变体下的所有段
 */
class AnimationClip
{
public:
    /**
     * @brief 默认构造函数
     *
     * 生成一个无效的空剪辑。
     */
    AnimationClip();

    /**
     * @brief 构造函数
     * @param[in] name 动作名称
     * @param[in] mood 情绪变体
     */
    AnimationClip(const QString &name, PET_MOOD mood);

    /**
     * @brief 添加动画段
     * @param[in] segment 要添加的段
     * @return 添加成功返回 true；同类型段已存在时返回 false
     */
    bool AddSegment(const AnimationSegment &segment);

    /**
     * @brief 获取指定类型的段
     * @param[in] type 段类型
     * @return 对应段的常量指针；不存在时返回 nullptr
     */
    const AnimationSegment *GetSegment(ANIMATION_TYPE type) const;

    /**
     * @brief 判断是否存在指定类型的段
     * @param[in] type 段类型
     * @return 存在返回 true
     */
    bool HasSegment(ANIMATION_TYPE type) const;

    /**
     * @brief 获取动作名
     * @return 动作名
     */
    QString GetName() const;

    /**
     * @brief 获取情绪变体
     * @return 情绪变体
     */
    PET_MOOD GetMood() const;

    /**
     * @brief 判断剪辑是否有效
     *
     * 有效条件：包含 SINGLE 段，或同时包含 A_START 与 B_LOOP 段。
     *
     * @return 有效返回 true
     */
    bool IsValid() const;

private:
    QString m_name;                          ///< 动作名称
    PET_MOOD m_mood;                         ///< 情绪变体
    QMap<ANIMATION_TYPE, AnimationSegment> m_segments; ///< 段映射表
};

} // namespace vpet

#endif // VPET_ANIMATION_CLIP_H
