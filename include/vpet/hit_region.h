#ifndef VPET_HIT_REGION_H
#define VPET_HIT_REGION_H

#include <QPoint>
#include <QRect>

namespace vpet
{

/**
 * @brief 命中区域类型
 */
enum class HIT_TYPE
{
    NONE,
    HEAD,
    BODY,
    DRAG
};

/**
 * @brief 头部、身体、拖拽命中区域配置
 *
 * 所有区域均使用宠物本地坐标系（与当前显示帧同尺寸）。
 */
class HitRegion
{
public:
    /**
     * @brief 构造函数
     */
    HitRegion();

    /**
     * @brief 设置头部命中区域
     * @param[in] region 头部区域
     */
    void SetHeadRegion(const QRect &region);

    /**
     * @brief 设置身体命中区域
     * @param[in] region 身体区域
     */
    void SetBodyRegion(const QRect &region);

    /**
     * @brief 设置拖拽命中区域
     * @param[in] region 拖拽区域；通常设置为整个窗口
     */
    void SetDragRegion(const QRect &region);

    /**
     * @brief 获取坐标对应的命中类型
     *
     * 优先级：DRAG > HEAD > BODY，未命中返回 NONE。
     *
     * @param[in] point 本地坐标点
     * @return 命中类型
     */
    HIT_TYPE GetHitType(const QPoint &point) const;

private:
    QRect m_headRegion; ///< 头部区域
    QRect m_bodyRegion; ///< 身体区域
    QRect m_dragRegion; ///< 拖拽区域
};

} // namespace vpet

#endif // VPET_HIT_REGION_H
