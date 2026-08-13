#ifndef VPET_COMMON_TYPES_H
#define VPET_COMMON_TYPES_H

#include <QString>

namespace vpet
{

/**
 * @brief 动画段类型：单一动作 / A开始 / B循环 / C结束
 */
enum class ANIMATION_TYPE
{
    SINGLE,
    A_START,
    B_LOOP,
    C_END
};

/**
 * @brief 宠物顶层状态
 */
enum class PET_STATE
{
    IDLE,
    WALKING,
    SAYING,
    TOUCH_HEAD,
    TOUCH_BODY,
    DRAGGING
};

/**
 * @brief 宠物情绪，决定优先使用哪套动画变体
 */
enum class PET_MOOD
{
    NORMAL,
    HAPPY,
    ILL,
    POOR_CONDITION
};

/**
 * @brief 将字符串转换为动画段类型
 * @param[in] segmentName 段标识字符串，如 "A", "B", "C", "SINGLE"
 * @return 对应的 ANIMATION_TYPE；无法识别时返回 SINGLE
 */
ANIMATION_TYPE StringToAnimationType(const QString &segmentName);

/**
 * @brief 将动画段类型转换为字符串
 * @param[in] type 动画段类型
 * @return 对应的字符串描述
 */
QString AnimationTypeToString(ANIMATION_TYPE type);

/**
 * @brief 将字符串转换为情绪枚举
 * @param[in] moodName 情绪名称，如 "Nomal", "Happy", "ill", "PoorCondition"
 * @return 对应的 PET_MOOD；无法识别时返回 NORMAL
 */
PET_MOOD StringToPetMood(const QString &moodName);

/**
 * @brief 获取情绪的字符串键（全小写，用于资源查找）
 * @param[in] mood 情绪枚举
 * @return 小写情绪名称
 */
QString PetMoodToString(PET_MOOD mood);

/**
 * @brief 获取状态优先级数值
 * @param[in] state 宠物状态
 * @return 优先级，数值越大优先级越高
 */
int GetPetStatePriority(PET_STATE state);

/**
 * @brief 将状态转换为对应动作名
 * @param[in] state 宠物状态
 * @return 动作名，如 "normal", "walk_left", "touch_head"
 */
QString PetStateToActionName(PET_STATE state);

} // namespace vpet

#endif // VPET_COMMON_TYPES_H
