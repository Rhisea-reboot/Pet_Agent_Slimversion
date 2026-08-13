#include "vpet/bubble_message.h"

namespace vpet
{

BubbleMessage::BubbleMessage()
    : m_text()
    , m_remainingMs(0)
    , m_visible(false)
{
}

void BubbleMessage::Show(const QString &text, int durationMs)
{
    if ((text.isEmpty()) || (durationMs <= 0))
    {
        return;
    }

    m_text = text;
    m_remainingMs = durationMs;
    m_visible = true;
}

void BubbleMessage::Clear()
{
    m_text.clear();
    m_remainingMs = 0;
    m_visible = false;
}

bool BubbleMessage::IsVisible() const
{
    return m_visible;
}

QString BubbleMessage::GetText() const
{
    return m_text;
}

void BubbleMessage::Update(int deltaTimeMs)
{
    if (deltaTimeMs < 0)
    {
        return;
    }

    if (!m_visible)
    {
        return;
    }

    m_remainingMs -= deltaTimeMs;

    if (m_remainingMs <= 0)
    {
        Clear();
    }
}

} // namespace vpet
