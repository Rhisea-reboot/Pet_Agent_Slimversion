#include "vpet/animation_segment.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <algorithm>

namespace vpet
{

namespace
{

const QString FRAME_FILE_EXTENSION = QStringLiteral(".png");
const int INVALID_FRAME_INDEX = -1;

} // anonymous namespace

AnimationSegment::AnimationSegment(ANIMATION_TYPE type)
    : m_type(type)
    , m_frames()
{
}

bool AnimationSegment::LoadFromDirectory(const QString &directoryPath)
{
    m_frames.clear();

    QDir dir(directoryPath);
    if (!dir.exists())
    {
        return false;
    }

    const QStringList filters = QStringList() << QStringLiteral("*.png");
    const QFileInfoList fileInfoList = dir.entryInfoList(filters, QDir::Files, QDir::Name);

    struct _tagParsedFrame
    {
        int frameIndex;
        AnimationFrame frame;
    };

    QList<_tagParsedFrame> parsedFrames;

    for (const QFileInfo &fileInfo : fileInfoList)
    {
        int frameIndex = INVALID_FRAME_INDEX;
        int durationMs = 0;

        if (!ParseFrameFileName(fileInfo.fileName(), frameIndex, durationMs))
        {
            continue;
        }

        const AnimationFrame frame(fileInfo.absoluteFilePath(), durationMs);
        parsedFrames.append({frameIndex, frame});
    }

    std::sort(parsedFrames.begin(), parsedFrames.end(),
              [](const _tagParsedFrame &left, const _tagParsedFrame &right)
    {
        return left.frameIndex < right.frameIndex;
    });

    for (const _tagParsedFrame &parsedFrame : parsedFrames)
    {
        m_frames.append(parsedFrame.frame);
    }

    return (!m_frames.isEmpty());
}

ANIMATION_TYPE AnimationSegment::GetType() const
{
    return m_type;
}

int AnimationSegment::GetFrameCount() const
{
    return m_frames.size();
}

const AnimationFrame &AnimationSegment::GetFrame(int index) const
{
    static const AnimationFrame INVALID_FRAME;

    if ((index < 0) || (index >= m_frames.size()))
    {
        return INVALID_FRAME;
    }

    return m_frames.at(index);
}

bool AnimationSegment::IsEmpty() const
{
    return m_frames.isEmpty();
}

int AnimationSegment::GetTotalDurationMs() const
{
    int totalDurationMs = 0;

    for (const AnimationFrame &frame : m_frames)
    {
        totalDurationMs += frame.GetDurationMs();
    }

    return totalDurationMs;
}

bool AnimationSegment::ParseFrameFileName(const QString &fileName, int &frameIndex, int &durationMs)
{
    frameIndex = INVALID_FRAME_INDEX;
    durationMs = 0;

    if (!fileName.endsWith(FRAME_FILE_EXTENSION, Qt::CaseInsensitive))
    {
        return false;
    }

    const QString baseName = fileName.left(fileName.length() - FRAME_FILE_EXTENSION.length());
    const QStringList parts = baseName.split('_', Qt::SkipEmptyParts);

    if (parts.size() < 2)
    {
        return false;
    }

    bool frameOk = false;
    bool durationOk = false;

    const int parsedFrameIndex = parts.at(parts.size() - 2).toInt(&frameOk);
    const int parsedDurationMs = parts.at(parts.size() - 1).toInt(&durationOk);

    if ((!frameOk) || (!durationOk))
    {
        return false;
    }

    if ((parsedFrameIndex < 0) || (parsedDurationMs <= 0))
    {
        return false;
    }

    frameIndex = parsedFrameIndex;
    durationMs = parsedDurationMs;

    return true;
}

} // namespace vpet
