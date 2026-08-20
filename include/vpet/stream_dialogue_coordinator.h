#ifndef VPET_STREAM_DIALOGUE_COORDINATOR_H
#define VPET_STREAM_DIALOGUE_COORDINATOR_H

#include "vpet/stream_sentence_splitter.h"

#include <QHash>
#include <QObject>
#include <QVector>

namespace vpet
{

class TtsAudioPlayer;
class TtsClient;

enum class SentenceState
{
    PendingSynthesis,
    SynthesisFinished,
    Playing,
    Played,
    Failed
};

struct SentenceTask
{
    SentenceChunk chunk;
    QString audioFilePath;
    SentenceState state = SentenceState::PendingSynthesis;
    int ttsRequestId = -1;
};

class StreamDialogueCoordinator : public QObject
{
    Q_OBJECT

public:
    StreamDialogueCoordinator(TtsClient *ttsClient,
                              TtsAudioPlayer *audioPlayer,
                              const QString &temporaryDirectory,
                              QObject *parent = nullptr);

    void EnqueueSentence(const SentenceChunk &chunk);
    void FinishStream(int requestId);
    void Cancel(int requestId = -1);
    bool IsActive() const;
    int ActiveRequestId() const;

signals:
    void SentencePlaybackStarted(const vpet::SentenceChunk &chunk);
    void DialogueFinished(int requestId);

private slots:
    void OnSynthesisFinished(int requestId, const QString &filePath);
    void OnPlaybackFinished();

private:
    void TryPlayNextSentence();
    void FinishIfComplete();
    void SubmitSynthesis(int taskIndex);
    void SubmitNextQueuedSynthesis();
    void RemoveAudioFile(const QString &filePath, int attempt = 0);

    TtsClient *m_ttsClient;
    TtsAudioPlayer *m_audioPlayer;
    QString m_temporaryDirectory;
    QVector<SentenceTask> m_tasks;
    QHash<int, int> m_taskByTtsRequest;
    QVector<int> m_synthesisWaitQueue;
    int m_activeRequestId;
    int m_inFlightSynthesis;
    int m_maxConcurrentSynthesis;
    int m_nextPlaybackIndex;
    int m_playingIndex;
    bool m_streamFinished;
};

} // namespace vpet

#endif // VPET_STREAM_DIALOGUE_COORDINATOR_H
