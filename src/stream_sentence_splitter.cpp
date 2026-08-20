#include "vpet/stream_sentence_splitter.h"

#include <QtGlobal>

namespace vpet
{

namespace
{

constexpr int MIN_STRONG_SENTENCE_LENGTH = 4;
constexpr int FIRST_SOFT_SPLIT_LENGTH = 6;
constexpr int LATER_SOFT_SPLIT_LENGTH = 15;
constexpr int MAX_BUFFER_LENGTH = 35;

} // anonymous namespace

StreamSentenceSplitter::StreamSentenceSplitter(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<SentenceChunk>("vpet::SentenceChunk");
}

void StreamSentenceSplitter::AppendDelta(int requestId, const QString &delta)
{
    if ((requestId <= 0) || delta.isEmpty())
    {
        return;
    }

    m_buffers[requestId].append(delta);
    ProcessBuffer(requestId, false);
}

void StreamSentenceSplitter::FinalizeStream(int requestId)
{
    if (requestId <= 0)
    {
        return;
    }

    ProcessBuffer(requestId, true);
    m_buffers.remove(requestId);
    m_sentenceCounters.remove(requestId);
}

void StreamSentenceSplitter::Reset(int requestId)
{
    m_buffers.remove(requestId);
    m_sentenceCounters.remove(requestId);
}

void StreamSentenceSplitter::ProcessBuffer(int requestId, bool forceFlush)
{
    auto iterator = m_buffers.find(requestId);

    if (iterator == m_buffers.end())
    {
        return;
    }

    while (!iterator.value().isEmpty())
    {
        const QString &buffer = iterator.value();
        const bool isFirst = (m_sentenceCounters.value(requestId, 0) == 0);
        int splitLength = -1;

        for (int index = 0; index < buffer.size(); ++index)
        {
            const QChar character = buffer.at(index);
            const int candidateLength = index + 1;

            if (IsStrongDelimiter(character)
                && ((candidateLength >= MIN_STRONG_SENTENCE_LENGTH) || forceFlush))
            {
                splitLength = candidateLength;
                break;
            }

            const int softThreshold = isFirst
                                      ? FIRST_SOFT_SPLIT_LENGTH
                                      : LATER_SOFT_SPLIT_LENGTH;

            if (IsSoftDelimiter(character) && (candidateLength >= softThreshold))
            {
                splitLength = candidateLength;
                break;
            }
        }

        if ((splitLength < 0) && (buffer.size() > MAX_BUFFER_LENGTH))
        {
            splitLength = FindForceSplitPosition(buffer);
        }

        if (splitLength > 0)
        {
            const bool isLast = forceFlush
                                && iterator.value().mid(splitLength).trimmed().isEmpty();
            EmitSentence(requestId, splitLength, isLast);
            iterator = m_buffers.find(requestId);
            continue;
        }

        if (forceFlush)
        {
            EmitSentence(requestId, buffer.size(), true);
        }

        break;
    }
}

void StreamSentenceSplitter::EmitSentence(int requestId, int length, bool isLast)
{
    QString &buffer = m_buffers[requestId];
    const QString sentence = buffer.left(length).trimmed();
    buffer.remove(0, length);

    if (sentence.isEmpty())
    {
        return;
    }

    SentenceChunk chunk;
    chunk.requestId = requestId;
    chunk.index = m_sentenceCounters.value(requestId, 0);
    chunk.text = sentence;
    chunk.isFirst = (chunk.index == 0);
    chunk.isLast = isLast && buffer.trimmed().isEmpty();
    m_sentenceCounters[requestId] = chunk.index + 1;
    emit SentenceReady(chunk);
}

bool StreamSentenceSplitter::IsStrongDelimiter(QChar character)
{
    return (character == QChar(0x3002))
           || (character == QChar(0xFF01))
           || (character == QChar(0xFF1F))
           || (character == QLatin1Char('!'))
           || (character == QLatin1Char('?'))
           || (character == QLatin1Char('\n'))
           || (character == QChar(0xFF1B))
           || (character == QLatin1Char(';'));
}

bool StreamSentenceSplitter::IsSoftDelimiter(QChar character)
{
    return (character == QChar(0xFF0C))
           || (character == QLatin1Char(','))
           || (character == QChar(0x3001))
           || (character == QChar(0xFF1A))
           || (character == QLatin1Char(':'))
           || (character == QChar(0x2014));
}

int StreamSentenceSplitter::FindForceSplitPosition(const QString &buffer)
{
    const int searchLimit = qMin(buffer.size(), MAX_BUFFER_LENGTH);

    for (int index = searchLimit - 1; index >= 0; --index)
    {
        if (IsStrongDelimiter(buffer.at(index)) || IsSoftDelimiter(buffer.at(index)))
        {
            const int candidateLength = index + 1;

            if (candidateLength >= MIN_STRONG_SENTENCE_LENGTH)
            {
                return candidateLength;
            }
        }
    }

    return -1;
}

} // namespace vpet
