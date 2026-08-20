#include "vpet/pet_controller.h"
#include "vpet/say_dialog.h"
#include "vpet/stream_dialogue_coordinator.h"
#include "vpet/tts_client.h"
#include "vpet/tts_audio_player.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QScreen>

namespace vpet
{

namespace
{

constexpr int UPDATE_INTERVAL_MS = 16;                  ///< 主循环更新间隔，约 60 FPS
constexpr int BUBBLE_DURATION_MS = 2000;                ///< 气泡默认显示时长
constexpr int SAY_BUBBLE_DURATION_MS = 5000;            ///< Say 气泡显示时长
constexpr int IDLE_TRIGGER_INTERVAL_MS = 3000;          ///< Idle 随机触发间隔
constexpr int WALK_SPEED_PIXELS_PER_SECOND = 80;        ///< 默认步行速度
constexpr int DRAG_MOVE_THRESHOLD_PIXELS = 5;           ///< 拖拽与点击的位移阈值
constexpr int SAYING_TIMEOUT_MS = 20000;                ///< SAYING 状态最长持续时间
constexpr int SAY_TRIGGER_COOLDOWN_MS = 8000;           ///< Say 连续触发冷却时间
constexpr int AUDIO_DELETE_RETRY_DELAY_MS = 300;        ///< 删除临时音频失败后的重试间隔
constexpr int AUDIO_DELETE_MAX_ATTEMPTS = 8;            ///< 删除临时音频的最大重试次数

int GetSaySourcePriority(SaySource source)
{
    switch (source)
    {
    case SaySource::UserResponse:
        return 3;

    case SaySource::VisionProactive:
        return 2;

    case SaySource::IdleRandom:
    default:
        return 1;
    }
}

QString SaySourceToString(SaySource source)
{
    switch (source)
    {
    case SaySource::UserResponse:
        return QStringLiteral("user_response");

    case SaySource::VisionProactive:
        return QStringLiteral("vision_proactive");

    case SaySource::IdleRandom:
    default:
        return QStringLiteral("idle_random");
    }
}

} // anonymous namespace

PetController::PetController(const QString &animationBasePath, QObject *parent)
    : QObject(parent)
    , m_resourceManager(animationBasePath)
    , m_stateMachine(m_resourceManager)
    , m_bubbleMessage()
    , m_hitRegion()
    , m_updateTimer(nullptr)
    , m_position(100, 100)
    , m_dragStartGlobalPos()
    , m_windowStartPos()
    , m_screenBounds()
    , m_walkSpeedPixelsPerSecond(WALK_SPEED_PIXELS_PER_SECOND)
    , m_isDragging(false)
    , m_dragMoved(false)
    , m_pendingHitType(HIT_TYPE::NONE)
    , m_lastState(PET_STATE::IDLE)
    , m_frameSize(100, 100)
    , m_ttsClient(nullptr)
    , m_ttsAudioPlayer(nullptr)
    , m_streamCoordinator(nullptr)
    , m_sayingTimeoutTimer(nullptr)
    , m_tempDir()
    , m_currentSayText()
    , m_currentSaySource(SaySource::IdleRandom)
    , m_synthesisCounter(0)
    , m_pendingTtsRequestId(-1)
    , m_sayCooldownRemainingMs(0)
    , m_pendingSaySource(SaySource::IdleRandom)
    , m_discardPendingSynthesis(false)
    , m_queuedSayText()
    , m_queuedSayAction()
    , m_queuedSaySource(SaySource::IdleRandom)
    , m_lastEmittedFramePath()
    , m_lastEmittedBubbleVisible(false)
    , m_lastEmittedBubbleText()
{
}

bool PetController::Initialize()
{
    if (!m_resourceManager.LoadAll())
    {
        return false;
    }

    m_stateMachine.Initialize();
    m_lastState = m_stateMachine.GetCurrentState();

    // 初始化 TTS 客户端
    m_ttsClient = new TtsClient(this);

    // 多级路径查找配置文件（与 TtsServerManager::FindConfigFile 逻辑一致）
    const QString exeDir = QCoreApplication::applicationDirPath();
    qDebug() << "[TTS] PetController init - exeDir:" << exeDir;
    qDebug() << "[TTS] PetController init - cwd:" << QDir::currentPath();

    const QStringList candidatePaths =
    {
        exeDir + QStringLiteral("/tts_config.json"),
        QDir::currentPath() + QStringLiteral("/tts_config.json"),
        exeDir + QStringLiteral("/../tts_config.json"),
        exeDir + QStringLiteral("/../../tts_config.json"),
    };

    QString ttsConfigPath;

    for (const QString &candidate : candidatePaths)
    {
        const bool exists = QFile::exists(candidate);
        qDebug() << "[TTS]   checking:" << candidate << "->" << (exists ? "FOUND" : "not found");

        if (exists)
        {
            ttsConfigPath = QFileInfo(candidate).absoluteFilePath();
            qDebug() << "[TTS]   resolved:" << ttsConfigPath;
            break;
        }
    }

    if (!ttsConfigPath.isEmpty())
    {
        m_ttsClient->LoadConfig(ttsConfigPath);
    }
    else
    {
        qDebug() << "[TTS]   WARNING: tts_config.json not found in any search path!";
    }

    connect(m_ttsClient, &TtsClient::SynthesisFinished,
            this, &PetController::OnTtsSynthesisFinished);

    // 初始化 TTS 音频播放器
    m_ttsAudioPlayer = new TtsAudioPlayer(this);

    connect(m_ttsAudioPlayer, &TtsAudioPlayer::PlaybackFinished,
            this, &PetController::OnAudioPlaybackFinished);

    m_streamCoordinator = new StreamDialogueCoordinator(m_ttsClient,
                                                         m_ttsAudioPlayer,
                                                         m_tempDir.path(),
                                                         this);
    connect(m_streamCoordinator, &StreamDialogueCoordinator::SentencePlaybackStarted,
            this, &PetController::OnStreamSentencePlaybackStarted);
    connect(m_streamCoordinator, &StreamDialogueCoordinator::DialogueFinished,
            this, &PetController::OnStreamDialogueFinished);

    m_sayingTimeoutTimer = new QTimer(this);
    m_sayingTimeoutTimer->setSingleShot(true);
    m_sayingTimeoutTimer->setInterval(SAYING_TIMEOUT_MS);

    connect(m_sayingTimeoutTimer, &QTimer::timeout,
            this, &PetController::OnSayingTimeout);

    // 初始化更新定时器
    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(UPDATE_INTERVAL_MS);

    connect(m_updateTimer, &QTimer::timeout, this, &PetController::OnUpdate);
    m_updateTimer->start();

    return true;
}

void PetController::OnMousePress(const QPoint &localPos)
{
    InterruptStreamDialogue();
    m_pendingHitType = m_hitRegion.GetHitType(localPos);
    m_dragStartGlobalPos = QCursor::pos();
    m_windowStartPos = m_position;
    m_dragMoved = false;
    m_isDragging = false;
}

void PetController::OnMouseMove(const QPoint &globalPos)
{
    const QPoint delta = globalPos - m_dragStartGlobalPos;

    if (!m_isDragging)
    {
        if (delta.manhattanLength() <= DRAG_MOVE_THRESHOLD_PIXELS)
        {
            return;
        }

        m_isDragging = true;
        m_dragMoved = true;
        m_stateMachine.DragStart();
        ShowInteractionBubble(PET_STATE::DRAGGING);
        emit StateChanged(m_stateMachine.GetCurrentState());
    }

    QPoint newPosition = m_windowStartPos + delta;
    ClampPositionToScreen(newPosition);

    m_position = newPosition;
    emit PositionChanged(m_position);
}

void PetController::OnMouseRelease(const QPoint &localPos)
{
    (void)localPos;

    if (m_isDragging)
    {
        m_isDragging = false;
        m_stateMachine.DragEnd();
        emit StateChanged(m_stateMachine.GetCurrentState());
    }
    else if (!m_dragMoved)
    {
        if (m_pendingHitType == HIT_TYPE::HEAD)
        {
            if (m_stateMachine.ClickHead())
            {
                ShowInteractionBubble(PET_STATE::TOUCH_HEAD);
                emit StateChanged(m_stateMachine.GetCurrentState());
            }
        }
        else if (m_pendingHitType == HIT_TYPE::BODY)
        {
            if (m_stateMachine.ClickBody())
            {
                ShowInteractionBubble(PET_STATE::TOUCH_BODY);
                emit StateChanged(m_stateMachine.GetCurrentState());
            }
        }
    }

    m_dragMoved = false;
    m_pendingHitType = HIT_TYPE::NONE;
}

QString PetController::GetCurrentFramePath() const
{
    return m_stateMachine.GetCurrentFrame().GetImagePath();
}

QPoint PetController::GetPosition() const
{
    return m_position;
}

void PetController::SetPosition(const QPoint &position)
{
    m_position = position;
    ClampPositionToScreen(m_position);
    emit PositionChanged(m_position);
}

bool PetController::HasBubble() const
{
    return m_bubbleMessage.IsVisible();
}

QString PetController::GetBubbleText() const
{
    return m_bubbleMessage.GetText();
}

bool PetController::IsDragging() const
{
    return m_isDragging;
}

PET_STATE PetController::GetCurrentState() const
{
    return m_stateMachine.GetCurrentState();
}

void PetController::SetMood(PET_MOOD mood)
{
    m_stateMachine.SetMood(mood);
}

void PetController::SetScreenBounds(const QRect &bounds)
{
    m_screenBounds = bounds;
}

void PetController::SetHitRegions(const QRect &head, const QRect &body, const QRect &drag)
{
    m_hitRegion.SetHeadRegion(head);
    m_hitRegion.SetBodyRegion(body);
    m_hitRegion.SetDragRegion(drag);
}

void PetController::SetFrameSize(const QSize &size)
{
    if (size.isValid())
    {
        m_frameSize = size;
    }
}

QSize PetController::GetFrameSize() const
{
    return m_frameSize;
}

bool PetController::RequestSay(const QString &text, SaySource source)
{
    const QString normalizedText = text.trimmed();

    if (normalizedText.isEmpty())
    {
        return false;
    }

    const PET_STATE currentState = m_stateMachine.GetCurrentState();
    const bool canStartNow = ((currentState == PET_STATE::IDLE)
                              || (currentState == PET_STATE::WALKING))
                             && m_pendingSayAction.isEmpty()
                             && !m_discardPendingSynthesis;

    if (canStartNow)
    {
        return StartSayText(normalizedText, source, QString());
    }

    return QueueSayText(normalizedText, source);
}

void PetController::EnqueueStreamSentence(const SentenceChunk &chunk)
{
    if (m_streamCoordinator == nullptr)
    {
        return;
    }

    m_streamCoordinator->EnqueueSentence(chunk);
}

void PetController::FinishStreamDialogue(int requestId)
{
    if (m_streamCoordinator != nullptr)
    {
        m_streamCoordinator->FinishStream(requestId);
    }
}

void PetController::InterruptStreamDialogue()
{
    if ((m_streamCoordinator == nullptr) || !m_streamCoordinator->IsActive())
    {
        return;
    }

    m_streamCoordinator->Cancel();

    if (m_sayingTimeoutTimer != nullptr)
    {
        m_sayingTimeoutTimer->stop();
    }

    if (m_stateMachine.GetCurrentState() == PET_STATE::SAYING)
    {
        m_stateMachine.RequestExitLoop();
    }

    m_bubbleMessage.Clear();
    m_lastEmittedBubbleVisible = false;
    m_lastEmittedBubbleText.clear();
    emit BubbleChanged(false, QString());
}

bool PetController::IsStreamingRequest(int requestId) const
{
    return (m_streamCoordinator != nullptr)
           && (m_streamCoordinator->ActiveRequestId() == requestId);
}

void PetController::OnUpdate()
{
    m_stateMachine.Update(UPDATE_INTERVAL_MS);
    m_bubbleMessage.Update(UPDATE_INTERVAL_MS);

    if (m_sayCooldownRemainingMs > 0)
    {
        m_sayCooldownRemainingMs -= UPDATE_INTERVAL_MS;

        if (m_sayCooldownRemainingMs < 0)
        {
            m_sayCooldownRemainingMs = 0;
        }
    }

    const PET_STATE currentState = m_stateMachine.GetCurrentState();
    const bool isInIdleGroup = (currentState == PET_STATE::IDLE)
                               || (currentState == PET_STATE::WALKING);

    if (isInIdleGroup && m_queuedSayText.isEmpty() && m_pendingSayAction.isEmpty())
    {
        const int value = QRandomGenerator::global()->bounded(IDLE_TRIGGER_INTERVAL_MS);

        if (value < UPDATE_INTERVAL_MS)
        {
            m_stateMachine.IdleTrigger();
        }
    }

    if (currentState == PET_STATE::WALKING)
    {
        UpdateWalkPosition(UPDATE_INTERVAL_MS);
    }

    if (m_lastState != currentState)
    {
        const PET_STATE previousState = m_lastState;
        m_lastState = currentState;
        emit StateChanged(currentState);

        if (currentState == PET_STATE::SAYING)
        {
            // SAYING 状态由 OnTtsSynthesisFinished 中调用 EnterSayState 进入，
            // 此时动画、气泡、音频同步开始
            const QString clipName = m_stateMachine.GetCurrentClipName();
            qDebug() << "[TTS] === Entering SAYING state ===";
            qDebug() << "[TTS]   clipName:" << clipName;
            emit SayStarted(clipName);

            // 显示气泡
            if (!m_currentSayText.isEmpty())
            {
                qDebug() << "[TTS]   bubble shown, characters:" << m_currentSayText.size();
                m_bubbleMessage.Show(m_currentSayText, SAY_BUBBLE_DURATION_MS);
                emit BubbleChanged(true, m_currentSayText);
                emit SayTextReady(m_currentSayText);
            }

            // 播放已合成的音频
            if (!m_pendingAudioPath.isEmpty()
                && (m_ttsAudioPlayer != nullptr))
            {
                qDebug() << "[TTS]   playing pre-synthesized audio";

                const bool playStarted = m_ttsAudioPlayer->Play(m_pendingAudioPath);

                if (playStarted)
                {
                    m_sayingTimeoutTimer->start();
                }
                else
                {
                    qDebug() << "[TTS]   playback failed, requesting SAYING exit";
                    m_stateMachine.RequestExitLoop();
                    m_pendingAudioPath.clear();
                }
            }
            else
            {
                qDebug() << "[TTS]   no pending audio, requesting SAYING exit";
                m_stateMachine.RequestExitLoop();
            }
        }
        else
        {
            if (previousState == PET_STATE::SAYING)
            {
                m_sayCooldownRemainingMs = SAY_TRIGGER_COOLDOWN_MS;
            }

            // 离开 SAYING 状态时清理
            if (m_sayingTimeoutTimer != nullptr)
            {
                m_sayingTimeoutTimer->stop();
            }

            m_currentSayText.clear();
            m_currentSaySource = SaySource::IdleRandom;
            m_pendingAudioPath.clear();
        }
    }

    TryStartQueuedSay();

    // 处理 Say 动作：IdleTrigger 概率命中后，先合成音频再进入状态
    if (m_stateMachine.ConsumeSayPending())
    {
        if (m_sayCooldownRemainingMs > 0)
        {
            qDebug() << "[TTS] Say skipped by cooldown, remaining ms:"
                     << m_sayCooldownRemainingMs;
        }
        else if (!m_pendingSayAction.isEmpty())
        {
            qDebug() << "[TTS] Say skipped because synthesis is already in flight:"
                     << m_pendingSayAction;
        }
        else
        {
            const QString sayAction = m_stateMachine.SelectRandomSayAction();

            qDebug() << "[TTS] Say pending, action:" << sayAction;

            if (sayAction.isEmpty())
            {
                qDebug() << "[TTS]   no say actions available, falling back to Idle";
                m_stateMachine.IdleTrigger();
            }
            else
            {
                const QString sayText = SayDialog::GetRandomText(sayAction);

                if (!StartSayText(sayText, SaySource::IdleRandom, sayAction))
                {
                    qDebug() << "[TTS]   idle random say request rejected.";
                }
            }
        }
    }

    const QString framePath = GetCurrentFramePath();

    if (!framePath.isEmpty() && (framePath != m_lastEmittedFramePath))
    {
        m_lastEmittedFramePath = framePath;
        emit FrameChanged(framePath);
    }

    const bool bubbleVisible = m_bubbleMessage.IsVisible();
    const QString bubbleText = m_bubbleMessage.GetText();

    if ((bubbleVisible != m_lastEmittedBubbleVisible)
        || (bubbleText != m_lastEmittedBubbleText))
    {
        m_lastEmittedBubbleVisible = bubbleVisible;
        m_lastEmittedBubbleText = bubbleText;
        emit BubbleChanged(bubbleVisible, bubbleText);
    }
}

void PetController::UpdateWalkPosition(int deltaTimeMs)
{
    if (deltaTimeMs <= 0)
    {
        return;
    }

    const int movePixels = (m_walkSpeedPixelsPerSecond * deltaTimeMs) / 1000;
    QPoint newPosition = m_position;
    const bool isWalkingLeft = m_stateMachine.GetCurrentFrame().GetImagePath().contains(
                                   QStringLiteral("walk.left"));

    if (isWalkingLeft)
    {
        newPosition.setX(newPosition.x() - movePixels);
    }
    else
    {
        newPosition.setX(newPosition.x() + movePixels);
    }

    const bool hitLeft = newPosition.x() <= m_screenBounds.left();
    const bool hitRight = (newPosition.x() + m_frameSize.width()) >= m_screenBounds.right();

    if (hitLeft || hitRight)
    {
        // 到达边界后调头向反方向行走，而不是停住
        const QString newAction = isWalkingLeft
                                  ? QStringLiteral("walk_right")
                                  : QStringLiteral("walk_left");
        m_stateMachine.StartWalking(newAction);
        return;
    }

    m_position = newPosition;
    ClampPositionToScreen(m_position);
    emit PositionChanged(m_position);
}

void PetController::ShowInteractionBubble(PET_STATE state)
{
    QString text;

    switch (state)
    {
    case PET_STATE::TOUCH_HEAD:
        text = GetRandomTouchHeadText();
        break;

    case PET_STATE::TOUCH_BODY:
        text = GetRandomTouchBodyText();
        break;

    case PET_STATE::DRAGGING:
        text = QStringLiteral("干嘛……");
        break;

    default:
        return;
    }

    m_bubbleMessage.Show(text, BUBBLE_DURATION_MS);
    emit BubbleChanged(true, text);
}

QString PetController::GetRandomTouchHeadText()
{
    const QStringList texts =
    {
        QStringLiteral("好舒服~"),
        QStringLiteral("再摸摸头嘛"),
        QStringLiteral("嗯？"),
        QStringLiteral("嘿嘿")
    };

    const int index = QRandomGenerator::global()->bounded(texts.size());
    return texts.at(index);
}

QString PetController::GetRandomTouchBodyText()
{
    const QStringList texts =
    {
        QStringLiteral("痒……"),
        QStringLiteral("别闹啦"),
        QStringLiteral("好害羞"),
        QStringLiteral("哼哼")
    };

    const int index = QRandomGenerator::global()->bounded(texts.size());
    return texts.at(index);
}

bool PetController::StartSayText(const QString &text,
                                 SaySource source,
                                 const QString &preferredAction)
{
    const QString normalizedText = text.trimmed();

    if (normalizedText.isEmpty())
    {
        return false;
    }

    if ((m_streamCoordinator != nullptr) && m_streamCoordinator->IsActive())
    {
        return QueueSayText(normalizedText, source);
    }

    QString sayAction = preferredAction.trimmed();

    if (sayAction.isEmpty())
    {
        sayAction = m_stateMachine.SelectRandomSayAction();
    }

    if (sayAction.isEmpty())
    {
        // 无 Animation/Say 资源时：直接出气泡，清空队列，避免 16ms 重试刷屏。
        return ShowSayTextWithoutAction(normalizedText, source);
    }

    const PET_STATE currentState = m_stateMachine.GetCurrentState();
    const bool canEnterSay = (currentState == PET_STATE::IDLE)
                             || (currentState == PET_STATE::WALKING);

    if (!canEnterSay || !m_pendingSayAction.isEmpty() || m_discardPendingSynthesis)
    {
        return QueueSayText(normalizedText, source);
    }

    m_currentSayText = normalizedText;
    m_currentSaySource = source;
    m_pendingSaySource = source;
    m_pendingAudioPath.clear();

    qDebug() << "[TTS] Say request accepted, source:" << SaySourceToString(source)
             << "action:" << sayAction
             << "characters:" << m_currentSayText.size();

    if ((m_ttsClient != nullptr) && m_ttsClient->IsConfigured())
    {
        if ((m_ttsAudioPlayer != nullptr) && m_ttsAudioPlayer->IsPlaying())
        {
            m_ttsAudioPlayer->Stop();
        }

        const QString uniqueName = QStringLiteral("say_%1.wav").arg(m_synthesisCounter);

        m_pendingAudioPath = m_tempDir.filePath(uniqueName);
        m_pendingSayAction = sayAction;
        m_synthesisCounter += 1;

        qDebug() << "[TTS]   synthesizing audio first";
        m_pendingTtsRequestId = m_ttsClient->Synthesize(m_currentSayText, m_pendingAudioPath);
        return m_pendingTtsRequestId > 0;
    }

    qDebug() << "[TTS]   TTS not configured, entering SAYING without audio";

    if (!m_stateMachine.EnterSayState(sayAction))
    {
        m_currentSayText.clear();
        return QueueSayText(normalizedText, source);
    }

    return true;
}

bool PetController::ShowSayTextWithoutAction(const QString &text, SaySource source)
{
    const QString normalizedText = text.trimmed();

    if (normalizedText.isEmpty())
    {
        return false;
    }

    m_queuedSayText.clear();
    m_queuedSayAction.clear();
    m_queuedSaySource = SaySource::IdleRandom;
    m_pendingSayAction.clear();
    m_pendingAudioPath.clear();
    m_discardPendingSynthesis = false;
    m_currentSayText = normalizedText;
    m_currentSaySource = source;
    m_pendingSaySource = source;
    m_sayCooldownRemainingMs = SAY_TRIGGER_COOLDOWN_MS;

    qDebug() << "[TTS] Say fallback without assets, source:" << SaySourceToString(source);
    m_bubbleMessage.Show(normalizedText, SAY_BUBBLE_DURATION_MS);
    m_lastEmittedBubbleVisible = true;
    m_lastEmittedBubbleText = normalizedText;
    emit BubbleChanged(true, normalizedText);
    emit SayTextReady(normalizedText);

    return true;
}

bool PetController::QueueSayText(const QString &text, SaySource source)
{
    const QString normalizedText = text.trimmed();

    if (normalizedText.isEmpty())
    {
        return false;
    }

    const int newPriority = GetSaySourcePriority(source);
    const bool hasQueuedSay = !m_queuedSayText.trimmed().isEmpty();

    if (hasQueuedSay && (newPriority < GetSaySourcePriority(m_queuedSaySource)))
    {
        qDebug() << "[TTS] Say request dropped by priority, source:" << SaySourceToString(source);
        return false;
    }

    m_queuedSayText = normalizedText;
    m_queuedSayAction.clear();
    m_queuedSaySource = source;

    if (!m_pendingSayAction.isEmpty()
        && (newPriority > GetSaySourcePriority(m_pendingSaySource)))
    {
        qDebug() << "[TTS] Higher priority say queued; discarding pending synthesis when it returns.";
        m_discardPendingSynthesis = true;
        m_pendingSayAction.clear();
        m_pendingAudioPath.clear();
        m_currentSayText.clear();
    }

    qDebug() << "[TTS] Say request queued, source:" << SaySourceToString(source);
    return true;
}

void PetController::TryStartQueuedSay()
{
    if (m_queuedSayText.trimmed().isEmpty())
    {
        return;
    }

    if (!m_pendingSayAction.isEmpty() || m_discardPendingSynthesis)
    {
        return;
    }

    const PET_STATE currentState = m_stateMachine.GetCurrentState();

    if ((currentState != PET_STATE::IDLE) && (currentState != PET_STATE::WALKING))
    {
        return;
    }

    const QString text = m_queuedSayText;
    const QString action = m_queuedSayAction;
    const SaySource source = m_queuedSaySource;

    m_queuedSayText.clear();
    m_queuedSayAction.clear();
    m_queuedSaySource = SaySource::IdleRandom;

    if (!StartSayText(text, source, action))
    {
        QueueSayText(text, source);
    }
}

void PetController::ClampPositionToScreen(QPoint &position) const
{
    if (!m_screenBounds.isValid())
    {
        return;
    }

    if (position.x() < m_screenBounds.left())
    {
        position.setX(m_screenBounds.left());
    }

    if (position.y() < m_screenBounds.top())
    {
        position.setY(m_screenBounds.top());
    }

    if (position.x() > (m_screenBounds.right() - m_frameSize.width()))
    {
        position.setX(m_screenBounds.right() - m_frameSize.width());
    }

    if (position.y() > (m_screenBounds.bottom() - m_frameSize.height()))
    {
        position.setY(m_screenBounds.bottom() - m_frameSize.height());
    }
}

void PetController::OnTtsSynthesisFinished(int requestId, const QString &filePath)
{
    if (requestId != m_pendingTtsRequestId)
    {
        return;
    }

    m_pendingTtsRequestId = -1;
    qDebug() << "[TTS] OnTtsSynthesisFinished, file available:" << !filePath.isEmpty();

    if (m_discardPendingSynthesis
        || ((m_streamCoordinator != nullptr) && m_streamCoordinator->IsActive()))
    {
        qDebug() << "[TTS]   discarding obsolete synthesized audio.";

        if (!filePath.isEmpty() && QFileInfo::exists(filePath))
        {
            QFile::remove(filePath);
        }

        m_discardPendingSynthesis = false;
        m_pendingAudioPath.clear();
        m_pendingSayAction.clear();
        m_currentSayText.clear();
        TryStartQueuedSay();
        return;
    }

    // 检查参数有效性
    if (filePath.isEmpty())
    {
        qDebug() << "[TTS]   FAILED - empty filePath (synthesis error or server error)";
        m_pendingAudioPath.clear();

        if (!m_pendingSayAction.isEmpty() && !m_currentSayText.isEmpty())
        {
            const QString actionName = m_pendingSayAction;
            m_pendingSayAction.clear();

            if (!m_stateMachine.EnterSayState(actionName))
            {
                m_currentSayText.clear();
            }
        }
        return;
    }

    // 检查音频文件是否存在
    if (!QFileInfo::exists(filePath))
    {
        qDebug() << "[TTS]   FAILED - audio file not found";
        m_pendingAudioPath.clear();

        if (!m_pendingSayAction.isEmpty() && !m_currentSayText.isEmpty())
        {
            const QString actionName = m_pendingSayAction;
            m_pendingSayAction.clear();

            if (!m_stateMachine.EnterSayState(actionName))
            {
                m_currentSayText.clear();
            }
        }
        return;
    }

    if (QFileInfo(filePath).size() <= 0)
    {
        qDebug() << "[TTS]   FAILED - audio file is empty";
        QFile::remove(filePath);
        m_pendingAudioPath.clear();

        if (!m_pendingSayAction.isEmpty() && !m_currentSayText.isEmpty())
        {
            const QString actionName = m_pendingSayAction;
            m_pendingSayAction.clear();

            if (!m_stateMachine.EnterSayState(actionName))
            {
                m_currentSayText.clear();
            }
        }
        return;
    }

    // 如果有待处理的 Say 动作，先进入 SAYING 状态再播放
    if (!m_pendingSayAction.isEmpty())
    {
        const QString actionName = m_pendingSayAction;
        m_pendingSayAction.clear();

        qDebug() << "[TTS]   entering SAYING state with action:" << actionName;

        if (m_stateMachine.EnterSayState(actionName))
        {
            // EnterSayState 成功后，OnUpdate 下一帧检测到 SAYING，
            // 会播放 m_pendingAudioPath 中的音频
            return;
        }

        // 进入 SAYING 失败（用户在此期间点击/拖拽了宠物），
        // 丢弃本次合成的音频，不做任何播放
        qDebug() << "[TTS]   EnterSayState failed - pet was interrupted, discarding audio";

        if (!m_pendingAudioPath.isEmpty())
        {
            QFile::remove(m_pendingAudioPath);
            m_pendingAudioPath.clear();
        }

        return;
    }

    // 直接播放（宠物已在 SAYING 状态，例如由其他路径触发）
    if (m_ttsAudioPlayer != nullptr)
    {
        qDebug() << "[TTS]   playing audio directly...";

        if (!m_ttsAudioPlayer->Play(filePath))
        {
            OnAudioPlaybackFinished();
        }
    }
}

void PetController::OnStreamSentencePlaybackStarted(const SentenceChunk &chunk)
{
    QString actionName;

    if (m_stateMachine.GetCurrentState() != PET_STATE::SAYING)
    {
        actionName = m_stateMachine.SelectRandomSayAction();

        if (!actionName.isEmpty())
        {
            m_stateMachine.EnterSayState(actionName);
            emit SayStarted(actionName);
        }
    }

    m_currentSayText = chunk.text;
    m_currentSaySource = SaySource::UserResponse;
    m_bubbleMessage.Show(chunk.text, SAY_BUBBLE_DURATION_MS);
    m_lastEmittedBubbleVisible = true;
    m_lastEmittedBubbleText = chunk.text;
    emit BubbleChanged(true, chunk.text);
    emit SayTextReady(chunk.text);

    if (m_sayingTimeoutTimer != nullptr)
    {
        m_sayingTimeoutTimer->start();
    }
}

void PetController::OnStreamDialogueFinished(int requestId)
{
    (void)requestId;

    if (m_sayingTimeoutTimer != nullptr)
    {
        m_sayingTimeoutTimer->stop();
    }

    if (m_stateMachine.GetCurrentState() == PET_STATE::SAYING)
    {
        m_stateMachine.RequestExitLoop();
    }

    m_sayCooldownRemainingMs = SAY_TRIGGER_COOLDOWN_MS;
    m_currentSayText.clear();
    m_currentSaySource = SaySource::IdleRandom;
    TryStartQueuedSay();
}

void PetController::OnAudioPlaybackFinished()
{
    if ((m_streamCoordinator != nullptr) && m_streamCoordinator->IsActive())
    {
        return;
    }

    qDebug() << "[TTS] OnAudioPlaybackFinished - audio playback done";

    if (m_sayingTimeoutTimer != nullptr)
    {
        m_sayingTimeoutTimer->stop();
    }

    // 告诉状态机退出 SAYING 的 B 循环，进入 C→IDLE
    if (m_stateMachine.GetCurrentState() == PET_STATE::SAYING)
    {
        qDebug() << "[TTS]   requesting exit from SAYING loop";
        m_stateMachine.RequestExitLoop();
    }

    m_sayCooldownRemainingMs = SAY_TRIGGER_COOLDOWN_MS;

// 播放完毕后删除音频文件以节约磁盘空间
    if (!m_pendingAudioPath.isEmpty())
    {
        const QString audioPathToDelete = m_pendingAudioPath;
        m_pendingAudioPath.clear();
        DeleteAudioFileWithRetry(audioPathToDelete);
    }

    // 清理 SAYING 相关状态
    m_currentSayText.clear();
    m_currentSaySource = SaySource::IdleRandom;
    TryStartQueuedSay();
}

void PetController::DeleteAudioFileWithRetry(const QString &filePath, int attempt)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath))
    {
        // 已经删除/不属于本轮会话，无需处理
        return;
    }

    if (QFile::remove(filePath))
    {
        qDebug() << "[TTS]   deleted audio file, success: true";
        return;
    }

    // 播放结束后媒体后端可能仍持有文件句柄（Windows 独占锁定），延迟重试。
    if (attempt >= AUDIO_DELETE_MAX_ATTEMPTS)
    {
        qWarning() << "[TTS]   WARNING: failed to delete audio file after"
                   << AUDIO_DELETE_MAX_ATTEMPTS << "retries:" << filePath;
        return;
    }

    QTimer::singleShot(AUDIO_DELETE_RETRY_DELAY_MS, this,
                       [this, filePath, attempt]()
    {
        DeleteAudioFileWithRetry(filePath, attempt + 1);
    });
}

void PetController::OnSayingTimeout()
{
    qDebug() << "[TTS] SAYING timeout reached, forcing exit";

    if (m_ttsAudioPlayer != nullptr)
    {
        m_ttsAudioPlayer->Stop();
    }

    if (m_stateMachine.GetCurrentState() == PET_STATE::SAYING)
    {
        m_stateMachine.RequestExitLoop();
    }

    m_sayCooldownRemainingMs = SAY_TRIGGER_COOLDOWN_MS;

    m_pendingAudioPath.clear();
    m_pendingSayAction.clear();
    m_discardPendingSynthesis = false;
    m_currentSayText.clear();
    m_currentSaySource = SaySource::IdleRandom;
    TryStartQueuedSay();
}

} // namespace vpet
