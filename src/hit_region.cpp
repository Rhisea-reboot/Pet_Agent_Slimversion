#include "vpet/hit_region.h"

namespace vpet
{

HitRegion::HitRegion()
    : m_headRegion()
    , m_bodyRegion()
    , m_dragRegion()
{
}

void HitRegion::SetHeadRegion(const QRect &region)
{
    m_headRegion = region;
}

void HitRegion::SetBodyRegion(const QRect &region)
{
    m_bodyRegion = region;
}

void HitRegion::SetDragRegion(const QRect &region)
{
    m_dragRegion = region;
}

HIT_TYPE HitRegion::GetHitType(const QPoint &point) const
{
    if (m_dragRegion.isValid() && m_dragRegion.contains(point))
    {
        return HIT_TYPE::DRAG;
    }

    if (m_headRegion.isValid() && m_headRegion.contains(point))
    {
        return HIT_TYPE::HEAD;
    }

    if (m_bodyRegion.isValid() && m_bodyRegion.contains(point))
    {
        return HIT_TYPE::BODY;
    }

    return HIT_TYPE::NONE;
}

} // namespace vpet
