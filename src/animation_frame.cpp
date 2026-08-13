#include "vpet/animation_frame.h"

namespace vpet
{

AnimationFrame::AnimationFrame()
    : m_imagePath()
    , m_durationMs(0)
{
}

AnimationFrame::AnimationFrame(const QString &imagePath, int durationMs)
    : m_imagePath(imagePath)
    , m_durationMs(durationMs)
{
}

QString AnimationFrame::GetImagePath() const
{
    return m_imagePath;
}

int AnimationFrame::GetDurationMs() const
{
    return m_durationMs;
}

bool AnimationFrame::IsValid() const
{
    return (!m_imagePath.isEmpty()) && (m_durationMs > 0);
}

} // namespace vpet
