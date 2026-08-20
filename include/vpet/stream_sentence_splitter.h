#ifndef VPET_STREAM_SENTENCE_SPLITTER_H
#define VPET_STREAM_SENTENCE_SPLITTER_H

#include <QHash>
#include <QObject>
#include <QString>

namespace vpet
{

struct SentenceChunk
{
    int requestId = -1;
    int index = 0;
    QString text;
    bool isFirst = false;
    bool isLast = false;
};

class StreamSentenceSplitter : public QObject
{
    Q_OBJECT

public:
    explicit StreamSentenceSplitter(QObject *parent = nullptr);

    void AppendDelta(int requestId, const QString &delta);
    void FinalizeStream(int requestId);
    void Reset(int requestId);

signals:
    void SentenceReady(const vpet::SentenceChunk &chunk);

private:
    void ProcessBuffer(int requestId, bool forceFlush);
    void EmitSentence(int requestId, int length, bool isLast);
    static bool IsStrongDelimiter(QChar character);
    static bool IsSoftDelimiter(QChar character);
    static int FindForceSplitPosition(const QString &buffer);

    QHash<int, QString> m_buffers;
    QHash<int, int> m_sentenceCounters;
};

} // namespace vpet

Q_DECLARE_METATYPE(vpet::SentenceChunk)

#endif // VPET_STREAM_SENTENCE_SPLITTER_H
