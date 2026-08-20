#include "vpet/stream_dialogue_coordinator.h"

#include "vpet/tts_audio_player.h"
#include "vpet/tts_client.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

namespace vpet
{

namespace
{

constexpr int AUDIO_DELETE_RETRY_DELAY_MS = 300;
constexpr int AUDIO_DELETE_MAX_ATTEMPTS = 8;
constexpr int MAX_CONCURRENT_SYNTHESIS = 3;

} // anonymous namespace

StreamDialogueCoordinator::StreamDialogueCoordinator(TtsClient *ttsClient,
                                                       TtsAudioPlayer *audioPlayer,
                                                       const QString &temporaryDirectory,
                                                       QObject *parent)
    : QObject(parent)
    , m_ttsClient(ttsClient)
    , m_audioPlayer(audioPlayer)
    , m_temporaryDirectory(temporaryDirectory)
    , m_tasks()
    , m_taskByTtsRequest()
    , m_synthesisWaitQueue()
    , m_activeRequestId(-1)
    , m_inFlightSynthesis(0)
    , m_maxConcurrentSynthesis(MAX_CONCURRENT_SYNTHESIS)
    , m_nextPlaybackIndex(0)
    , m_playingIndex(-1)
    , m_streamFinished(false)
{
    if (m_ttsClient != nullptr)
    {
        connect(m_ttsClient, &TtsClient::SynthesisFinished,
                this, &StreamDialogueCoordinator::OnSynthesisFinished);
    }

    if (m_audioPlayer != nullptr)
    {
        connect(m_audioPlayer, &TtsAudioPlayer::PlaybackFinished,
                this, &StreamDialogueCoordinator::OnPlaybackFinished);
    }
}

void StreamDialogueCoordinator::EnqueueSentence(const SentenceChunk &chunk)
{
    if ((chunk.requestId <= 0) || chunk.text.trimmed().isEmpty()
        || (m_ttsClient == nullptr) || !m_ttsClient->IsConfigured())
    {
        return;
    }

    if ((m_activeRequestId > 0) && (m_activeRequestId != chunk.requestId))
    {
        Cancel();
    }

    if (m_activeRequestId <= 0)
    {
        m_activeRequestId = chunk.requestId;
        m_streamFinished = false;
    }

    SentenceTask task;
    task.chunk = chunk;
    task.audioFilePath = QDir(m_temporaryDirectory).filePath(
        QStringLiteral("stream_req%1_seq%2.wav").arg(chunk.requestId).arg(chunk.index));

    const int taskIndex = m_tasks.size();
    m_tasks.append(task);

    if (m_inFlightSynthesis < m_maxConcurrentSynthesis)
    {
        SubmitSynthesis(taskIndex);
    }
    else
    {
        m_synthesisWaitQueue.append(taskIndex);
    }
}

void StreamDialogueCoordinator::FinishStream(int requestId)
{
    if (requestId != m_activeRequestId)
    {
        return;
    }

    m_streamFinished = true;
    FinishIfComplete();
}

void StreamDialogueCoordinator::Cancel(int requestId)
{
    if ((requestId > 0) && (requestId != m_activeRequestId))
    {
        return;
    }

    if ((m_audioPlayer != nullptr) && m_audioPlayer->IsPlaying())
    {
        m_audioPlayer->Stop();
    }

    for (const SentenceTask &task : m_tasks)
    {
        if ((task.ttsRequestId > 0) && (m_ttsClient != nullptr))
        {
            m_ttsClient->Cancel(task.ttsRequestId);
        }

        RemoveAudioFile(task.audioFilePath);
    }

    m_tasks.clear();
    m_taskByTtsRequest.clear();
    m_synthesisWaitQueue.clear();
    m_inFlightSynthesis = 0;
    m_activeRequestId = -1;
    m_nextPlaybackIndex = 0;
    m_playingIndex = -1;
    m_streamFinished = false;
}

bool StreamDialogueCoordinator::IsActive() const
{
    return m_activeRequestId > 0;
}

int StreamDialogueCoordinator::ActiveRequestId() const
{
    return m_activeRequestId;
}

void StreamDialogueCoordinator::OnSynthesisFinished(int requestId, const QString &filePath)
{
    const auto iterator = m_taskByTtsRequest.constFind(requestId);

    if (iterator == m_taskByTtsRequest.constEnd())
    {
        return;
    }

    const int taskIndex = iterator.value();
    m_taskByTtsRequest.remove(requestId);

    if (m_inFlightSynthesis > 0)
    {
        --m_inFlightSynthesis;
    }

    if ((taskIndex < 0) || (taskIndex >= m_tasks.size()))
    {
        SubmitNextQueuedSynthesis();
        return;
    }

    SentenceTask &task = m_tasks[taskIndex];

    if (!filePath.isEmpty() && QFileInfo::exists(filePath) && (QFileInfo(filePath).size() > 0))
    {
        task.audioFilePath = filePath;
        task.state = SentenceState::SynthesisFinished;
    }
    else
    {
        task.state = SentenceState::Failed;
    }

    SubmitNextQueuedSynthesis();
    TryPlayNextSentence();
}

void StreamDialogueCoordinator::OnPlaybackFinished()
{
    if ((m_playingIndex < 0) || (m_playingIndex >= m_tasks.size()))
    {
        return;
    }

    SentenceTask &task = m_tasks[m_playingIndex];
    task.state = SentenceState::Played;
    RemoveAudioFile(task.audioFilePath);
    m_playingIndex = -1;
    ++m_nextPlaybackIndex;
    TryPlayNextSentence();
}

void StreamDialogueCoordinator::TryPlayNextSentence()
{
    if (m_playingIndex >= 0)
    {
        return;
    }

    while (m_nextPlaybackIndex < m_tasks.size())
    {
        SentenceTask &task = m_tasks[m_nextPlaybackIndex];

        if (task.state == SentenceState::PendingSynthesis)
        {
            return;
        }

        if (task.state == SentenceState::Failed)
        {
            ++m_nextPlaybackIndex;
            continue;
        }

        if (task.state != SentenceState::SynthesisFinished)
        {
            ++m_nextPlaybackIndex;
            continue;
        }

        task.state = SentenceState::Playing;
        m_playingIndex = m_nextPlaybackIndex;
        emit SentencePlaybackStarted(task.chunk);

        if ((m_audioPlayer == nullptr) || !m_audioPlayer->Play(task.audioFilePath))
        {
            OnPlaybackFinished();
        }

        return;
    }

    FinishIfComplete();
}

void StreamDialogueCoordinator::SubmitSynthesis(int taskIndex)
{
    if ((taskIndex < 0) || (taskIndex >= m_tasks.size()) || (m_ttsClient == nullptr))
    {
        return;
    }

    ++m_inFlightSynthesis;
    SentenceTask &task = m_tasks[taskIndex];
    const int ttsRequestId = m_ttsClient->Synthesize(task.chunk.text, task.audioFilePath);
    task.ttsRequestId = ttsRequestId;

    if (ttsRequestId > 0)
    {
        m_taskByTtsRequest.insert(ttsRequestId, taskIndex);
        return;
    }

    --m_inFlightSynthesis;
    task.state = SentenceState::Failed;
    TryPlayNextSentence();
    SubmitNextQueuedSynthesis();
}

void StreamDialogueCoordinator::SubmitNextQueuedSynthesis()
{
    while (!m_synthesisWaitQueue.isEmpty()
           && (m_inFlightSynthesis < m_maxConcurrentSynthesis))
    {
        const int taskIndex = m_synthesisWaitQueue.takeFirst();

        if ((taskIndex < 0) || (taskIndex >= m_tasks.size()))
        {
            continue;
        }

        if (m_tasks[taskIndex].state == SentenceState::PendingSynthesis)
        {
            SubmitSynthesis(taskIndex);
        }
    }
}

void StreamDialogueCoordinator::FinishIfComplete()
{
    if (!m_streamFinished || (m_playingIndex >= 0)
        || (m_nextPlaybackIndex < m_tasks.size()))
    {
        return;
    }

    const int completedRequestId = m_activeRequestId;
    m_tasks.clear();
    m_taskByTtsRequest.clear();
    m_synthesisWaitQueue.clear();
    m_inFlightSynthesis = 0;
    m_activeRequestId = -1;
    m_nextPlaybackIndex = 0;
    m_streamFinished = false;
    emit DialogueFinished(completedRequestId);
}

void StreamDialogueCoordinator::RemoveAudioFile(const QString &filePath, int attempt)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath) || QFile::remove(filePath))
    {
        return;
    }

    if (attempt >= AUDIO_DELETE_MAX_ATTEMPTS)
    {
        return;
    }

    QTimer::singleShot(AUDIO_DELETE_RETRY_DELAY_MS, this, [this, filePath, attempt]()
    {
        RemoveAudioFile(filePath, attempt + 1);
    });
}

} // namespace vpet
