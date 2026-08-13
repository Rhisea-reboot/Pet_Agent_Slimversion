#include "vpet/animation_clip.h"

namespace vpet
{

AnimationClip::AnimationClip()
    : m_name()
    , m_mood(PET_MOOD::NORMAL)
    , m_segments()
{
}

AnimationClip::AnimationClip(const QString &name, PET_MOOD mood)
    : m_name(name)
    , m_mood(mood)
    , m_segments()
{
}

bool AnimationClip::AddSegment(const AnimationSegment &segment)
{
    if (m_segments.contains(segment.GetType()))
    {
        return false;
    }

    m_segments.insert(segment.GetType(), segment);
    return true;
}

const AnimationSegment *AnimationClip::GetSegment(ANIMATION_TYPE type) const
{
    const auto it = m_segments.find(type);

    if (it == m_segments.end())
    {
        return nullptr;
    }

    return &(it.value());
}

bool AnimationClip::HasSegment(ANIMATION_TYPE type) const
{
    return m_segments.contains(type);
}

QString AnimationClip::GetName() const
{
    return m_name;
}

PET_MOOD AnimationClip::GetMood() const
{
    return m_mood;
}

bool AnimationClip::IsValid() const
{
    if (m_segments.contains(ANIMATION_TYPE::SINGLE))
    {
        return true;
    }

    return m_segments.contains(ANIMATION_TYPE::A_START)
           && m_segments.contains(ANIMATION_TYPE::B_LOOP);
}

} // namespace vpet
