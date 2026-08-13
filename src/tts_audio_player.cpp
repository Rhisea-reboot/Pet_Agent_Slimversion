#include "vpet/tts_audio_player.h"

#include <QAudioOutput>
#include <QDebug>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QUrl>

namespace vpet
{

TtsAudioPlayer::TtsAudioPlayer(QObject *parent)
    : QObject(parent)
    , m_mediaPlayer(nullptr)
    , m_audioOutput(nullptr)
{
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(1.0f);

    m_mediaPlayer = new QMediaPlayer(this);
    m_mediaPlayer->setAudioOutput(m_audioOutput);

    connect(m_mediaPlayer, &QMediaPlayer::mediaStatusChanged,
            this, [this](QMediaPlayer::MediaStatus status)
    {
        if (status == QMediaPlayer::EndOfMedia)
        {
            // 播放结束后媒体后端可能仍持有源文件句柄（Windows 独占锁定），
            // 显式停止并解除媒体源以释放句柄，否则 PetController 删除临时 WAV 会失败。
            if (m_mediaPlayer->playbackState() != QMediaPlayer::StoppedState)
            {
                m_mediaPlayer->stop();
            }

            m_mediaPlayer->setSource(QUrl());
            emit PlaybackFinished();
        }
    });

    connect(m_mediaPlayer, &QMediaPlayer::errorOccurred,
            this, [this](QMediaPlayer::Error error, const QString &errorString)
    {
        if (error == QMediaPlayer::NoError)
        {
            return;
        }

        qDebug() << "[TTS]   media player error:" << error << errorString;

        if (m_mediaPlayer->playbackState() != QMediaPlayer::StoppedState)
        {
            m_mediaPlayer->stop();
        }

        m_mediaPlayer->setSource(QUrl());
        emit PlaybackFinished();
    });
}

TtsAudioPlayer::~TtsAudioPlayer()
{
    Stop();
}

bool TtsAudioPlayer::Play(const QString &filePath)
{
    qDebug() << "[TTS] TtsAudioPlayer::Play";

    // 检查参数有效性
    if (filePath.isEmpty())
    {
        qDebug() << "[TTS]   FAILED - empty filePath";
        return false;
    }

    if ((m_mediaPlayer == nullptr) || (m_audioOutput == nullptr))
    {
        qDebug() << "[TTS]   FAILED - media player is null";
        return false;
    }

    QFileInfo fileInfo(filePath);

    if (!fileInfo.exists() || !fileInfo.isFile())
    {
        qDebug() << "[TTS]   FAILED - file not found or not a file. exists:" << fileInfo.exists();
        return false;
    }

    if (fileInfo.size() <= 0)
    {
        qDebug() << "[TTS]   FAILED - empty audio file";
        return false;
    }

    qDebug() << "[TTS]   file size:" << fileInfo.size() << "bytes";

    Stop();

    m_mediaPlayer->setSource(QUrl::fromLocalFile(fileInfo.absoluteFilePath()));
    m_audioOutput->setVolume(1.0f);
    m_mediaPlayer->play();

    qDebug() << "[TTS]   playback started";
    return true;
}

void TtsAudioPlayer::Stop()
{
    if (m_mediaPlayer == nullptr)
    {
        return;
    }

    if (m_mediaPlayer->playbackState() != QMediaPlayer::StoppedState)
    {
        m_mediaPlayer->stop();
    }
}

bool TtsAudioPlayer::IsPlaying() const
{
    if (m_mediaPlayer == nullptr)
    {
        return false;
    }

    return m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState;
}

} // namespace vpet
