#include "vpet/perception/frame_buffer.h"

#include <QPainter>
#include <QSize>
#include <QtGlobal>

namespace vpet
{

namespace
{

constexpr std::size_t MIN_CAPACITY = 1;
constexpr std::size_t MAX_CAPACITY = 4096;

} // anonymous namespace

FrameBuffer::FrameBuffer(std::size_t capacity)
    : m_capacity(NormalizeCapacity(capacity))
    , m_frames()
{
    m_frames.reserve(static_cast<qsizetype>(m_capacity));
}

bool FrameBuffer::Push(const _tagFrame &frame)
{
    if (frame.pixmap.isNull())
    {
        return false;
    }

    if (m_capacity < MIN_CAPACITY)
    {
        return false;
    }

    if (static_cast<std::size_t>(m_frames.size()) >= m_capacity)
    {
        m_frames.removeFirst();
    }

    _tagFrame normalizedFrame = frame;

    if (!normalizedFrame.timestamp.isValid())
    {
        normalizedFrame.timestamp = QDateTime::currentDateTimeUtc();
    }

    m_frames.append(normalizedFrame);

    return true;
}

_tagFrame FrameBuffer::GetLatest() const
{
    return GetAt(0);
}

_tagFrame FrameBuffer::GetAt(std::size_t index) const
{
    if (m_frames.isEmpty())
    {
        return MakeInvalidFrame();
    }

    if (index >= static_cast<std::size_t>(m_frames.size()))
    {
        return MakeInvalidFrame();
    }

    const qsizetype frameIndex = m_frames.size() - 1 - static_cast<qsizetype>(index);

    if ((frameIndex < 0) || (frameIndex >= m_frames.size()))
    {
        return MakeInvalidFrame();
    }

    return m_frames.at(frameIndex);
}

QVector<_tagFrame> FrameBuffer::GetRecent(std::size_t count) const
{
    QVector<_tagFrame> recentFrames;

    if ((count == 0) || m_frames.isEmpty())
    {
        return recentFrames;
    }

    const std::size_t frameCount = qMin(count, static_cast<std::size_t>(m_frames.size()));
    recentFrames.reserve(static_cast<qsizetype>(frameCount));

    for (std::size_t index = 0; index < frameCount; index += 1)
    {
        const _tagFrame frame = GetAt(index);

        if (!frame.pixmap.isNull())
        {
            recentFrames.append(frame);
        }
    }

    return recentFrames;
}

std::size_t FrameBuffer::GetSize() const
{
    return static_cast<std::size_t>(m_frames.size());
}

std::size_t FrameBuffer::GetCapacity() const
{
    return m_capacity;
}

bool FrameBuffer::IsEmpty() const
{
    return m_frames.isEmpty();
}

void FrameBuffer::Clear()
{
    m_frames.clear();
}

QPixmap FrameBuffer::StitchRecent(std::size_t count, Qt::Orientation orientation) const
{
    if ((orientation != Qt::Horizontal) && (orientation != Qt::Vertical))
    {
        return QPixmap();
    }

    const QVector<_tagFrame> recentFrames = GetRecent(count);

    if (recentFrames.isEmpty())
    {
        return QPixmap();
    }

    int targetWidth = 0;
    int targetHeight = 0;

    for (const _tagFrame &frame : recentFrames)
    {
        if (frame.pixmap.isNull())
        {
            continue;
        }

        if (orientation == Qt::Horizontal)
        {
            targetWidth += frame.pixmap.width();
            targetHeight = qMax(targetHeight, frame.pixmap.height());
        }
        else
        {
            targetWidth = qMax(targetWidth, frame.pixmap.width());
            targetHeight += frame.pixmap.height();
        }
    }

    if ((targetWidth <= 0) || (targetHeight <= 0))
    {
        return QPixmap();
    }

    QPixmap stitchedPixmap(targetWidth, targetHeight);
    stitchedPixmap.fill(Qt::transparent);

    QPainter painter(&stitchedPixmap);
    int offset = 0;

    for (const _tagFrame &frame : recentFrames)
    {
        if (frame.pixmap.isNull())
        {
            continue;
        }

        if (orientation == Qt::Horizontal)
        {
            painter.drawPixmap(offset, 0, frame.pixmap);
            offset += frame.pixmap.width();
        }
        else
        {
            painter.drawPixmap(0, offset, frame.pixmap);
            offset += frame.pixmap.height();
        }
    }

    painter.end();

    return stitchedPixmap;
}

std::size_t FrameBuffer::NormalizeCapacity(std::size_t capacity)
{
    if (capacity < MIN_CAPACITY)
    {
        return MIN_CAPACITY;
    }

    if (capacity > MAX_CAPACITY)
    {
        return MAX_CAPACITY;
    }

    return capacity;
}

_tagFrame FrameBuffer::MakeInvalidFrame()
{
    _tagFrame frame;
    frame.sequenceId = -1;

    return frame;
}

} // namespace vpet
