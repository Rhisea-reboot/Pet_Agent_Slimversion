#include "main_window.h"
#include "vpet/agent/agent_runtime.h"
#include "vpet/chat_bubble_window.h"
#include "vpet/llm/vision_llm_client.h"
#include "vpet/memory/memory_manager_dialog.h"
#include "vpet/perception/perception_pipeline.h"
#include "vpet/perception/vision_encoder.h"
#include "vpet/speech/voice_input_manager.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QPixmapCache>
#include <QScreen>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace vpet
{

namespace
{

constexpr int BUBBLE_OFFSET_Y = 10;     ///< 气泡与窗口顶部的偏移
constexpr int TARGET_DISPLAY_WIDTH = 300; ///< 宠物显示宽度，按原图比例缩放
constexpr int SCREENSHOT_INTERVAL_MS = 3000; ///< 自动截图间隔
constexpr int SAY_BUBBLE_DURATION_MS = 5000; ///< Say 气泡固定显示时长
constexpr int VOICE_HOTKEY_ID = 0x56504554;

} // anonymous namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
    , m_controller(nullptr)
    , m_imageLabel(nullptr)
    , m_bubbleLabel(nullptr)
    , m_perceptionIndicatorLabel(nullptr)
    , m_chatBubbleWindow(nullptr)
    , m_perceptionPipeline(nullptr)
    , m_voiceInputManager(nullptr)
    , m_trayIcon(nullptr)
    , m_trayMenu(nullptr)
    , m_agentRuntime(nullptr)
    , m_memoryManagerDialog(nullptr)
    , m_currentImageSize()
    , m_lastFramePath()
    , m_isVoiceHotkeyRegistered(false)
    , m_isScreenPerceptionEnabled(false)
    , m_isExiting(false)
{
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFocusPolicy(Qt::StrongFocus);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setScaledContents(false);

    m_bubbleLabel = new QLabel(this);
    m_bubbleLabel->setVisible(false);
    m_bubbleLabel->setStyleSheet(
        QStringLiteral("QLabel { background-color: white; color: black; "
                       "border: 1px solid gray; border-radius: 8px; "
                       "padding: 6px 10px; font-size: 12px; }"));

    m_perceptionIndicatorLabel = new QLabel(this);
    m_perceptionIndicatorLabel->setFixedSize(10, 10);
    m_perceptionIndicatorLabel->setStyleSheet(
        QStringLiteral("QLabel { background-color: #E53935; border-radius: 5px; "
                       "border: 1px solid #B71C1C; }"));
    m_perceptionIndicatorLabel->setToolTip(
        QStringLiteral("屏幕感知已开启：正在周期性截图并发送到外部 API"));
    m_perceptionIndicatorLabel->setVisible(false);
}

MainWindow::~MainWindow()
{
    if (m_trayIcon != nullptr)
    {
        m_trayIcon->hide();
        m_trayIcon->setContextMenu(nullptr);
        delete m_trayIcon;
        m_trayIcon = nullptr;
    }

    if (m_trayMenu != nullptr)
    {
        delete m_trayMenu;
        m_trayMenu = nullptr;
    }

    UnregisterVoiceHotkey();
    delete m_chatBubbleWindow;
    delete m_memoryManagerDialog;
    delete m_voiceInputManager;
    delete m_perceptionPipeline;
    delete m_controller;
}

bool MainWindow::Initialize(const QString &animationBasePath)
{
    m_controller = new PetController(animationBasePath, this);

    if (!m_controller->Initialize())
    {
        return false;
    }

    const QScreen *screen = QGuiApplication::primaryScreen();

    if (screen != nullptr)
    {
        m_controller->SetScreenBounds(screen->availableGeometry());
    }

    connect(m_controller, &PetController::FrameChanged,
            this, &MainWindow::OnFrameChanged);
    connect(m_controller, &PetController::BubbleChanged,
            this, &MainWindow::OnBubbleChanged);
    connect(m_controller, &PetController::PositionChanged,
            this, &MainWindow::OnPositionChanged);
    connect(m_controller, &PetController::StateChanged,
            this, &MainWindow::OnStateChanged);
    connect(m_controller, &PetController::SayStarted,
            this, &MainWindow::OnSayStarted);
    connect(m_controller, &PetController::SayTextReady,
            this, &MainWindow::OnSayTextReady);

    // 创建聊天气泡窗口（独立顶层窗口，用于 IPC 式通信）
    m_chatBubbleWindow = new ChatBubbleWindow(nullptr);

    if (m_chatBubbleWindow != nullptr)
    {
        m_chatBubbleWindow->hide();
    }

    m_voiceInputManager = new VoiceInputManager(this);

    connect(m_voiceInputManager, &VoiceInputManager::RecordingStarted, this, []()
    {
        qDebug() << "[VoiceInput] Recording started. Press Ctrl+Alt+V again to submit.";
    });
    connect(m_voiceInputManager, &VoiceInputManager::RecordingStopped, this, [](const QString &audioPath)
    {
        qDebug() << "[VoiceInput] Recording stopped, path available:"
                 << !audioPath.isEmpty();
    });
    connect(m_voiceInputManager, &VoiceInputManager::TranscriptionCompleted,
            this, &MainWindow::OnVoiceTranscriptionCompleted);
    connect(m_voiceInputManager, &VoiceInputManager::TranscriptionFailed,
            this, &MainWindow::OnVoiceTranscriptionFailed);

    if (!RegisterVoiceHotkey())
    {
        qWarning() << "[VoiceInput] Failed to register global hotkey Ctrl+Alt+V.";
    }

    PerceptionPipeline::_tagConfig perceptionConfig;
    perceptionConfig.sensorConfig.intervalMs = SCREENSHOT_INTERVAL_MS;
    perceptionConfig.sensorConfig.captureAllScreens = false;
    perceptionConfig.sensorConfig.saveToDisk = false;
    perceptionConfig.bufferCapacity = 1;
    perceptionConfig.encodeFormat = VisionEncoder::VISION_ENCODE_FORMAT::BASE64_JPEG;
    perceptionConfig.encodeOptions.maxWidth = 1280;
    perceptionConfig.encodeOptions.maxHeight = 1280;
    perceptionConfig.encodeOptions.quality = 70;
    perceptionConfig.enableBuffer = false;
    perceptionConfig.enableChangeDetection = true;
    perceptionConfig.minDispatchIntervalMs = 60000;

    m_perceptionPipeline = new PerceptionPipeline(perceptionConfig, this);

    connect(m_perceptionPipeline, &PerceptionPipeline::DataReady,
            this, &MainWindow::OnPerceptionDataReady);
    connect(m_perceptionPipeline, &PerceptionPipeline::ErrorOccurred,
            this, &MainWindow::OnPerceptionError);

    // 默认不启动截图管道：需用户通过右键菜单主动开启（隐私 Opt-in）。
    m_isScreenPerceptionEnabled = false;
    UpdatePerceptionIndicator();

    CenterOnScreen();

    const QString framePath = m_controller->GetCurrentFramePath();

    if (!framePath.isEmpty())
    {
        OnFrameChanged(framePath);
    }

    InitializeSystemTrayIcon();

    return true;
}

void MainWindow::SetAgentRuntime(AgentRuntime *agentRuntime)
{
    m_agentRuntime = agentRuntime;

    if (m_agentRuntime == nullptr)
    {
        return;
    }

    connect(m_agentRuntime, &AgentRuntime::LogMessage,
            this, &MainWindow::OnAgentLogMessage);
    connect(m_agentRuntime, &AgentRuntime::LlmResponseReceived,
            this, &MainWindow::OnAgentLlmResponseReceived);
    connect(m_agentRuntime, &AgentRuntime::AgentOutputReady,
            this, &MainWindow::OnAgentOutputReady);
    connect(m_agentRuntime, &AgentRuntime::StreamSentenceReady,
            this, &MainWindow::OnStreamSentenceReady);
    connect(m_agentRuntime, &AgentRuntime::StreamResponseFinished,
            this, &MainWindow::OnStreamResponseFinished);
    connect(m_agentRuntime, &AgentRuntime::LlmRequestFailed,
            this, &MainWindow::OnAgentLlmRequestFailed);
    connect(m_agentRuntime, &AgentRuntime::AgentRequestFailed,
            this, &MainWindow::OnAgentRequestFailed);
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton)
    {
        ShowPetContextMenu(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    if (m_agentRuntime != nullptr)
    {
        m_agentRuntime->CancelActiveStreaming();
    }

    m_streamingRequests.clear();

    if (m_controller != nullptr)
    {
        m_controller->OnMousePress(event->pos());
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_controller != nullptr)
    {
        m_controller->OnMouseMove(event->globalPosition().toPoint());
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_controller != nullptr)
    {
        m_controller->OnMouseRelease(event->pos());
    }
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event == nullptr)
    {
        return;
    }

    if (event->isAutoRepeat())
    {
        event->accept();
        return;
    }

    if ((event->key() == Qt::Key_V)
        && event->modifiers().testFlag(Qt::ControlModifier)
        && event->modifiers().testFlag(Qt::AltModifier))
    {
        if ((m_voiceInputManager != nullptr) && !m_voiceInputManager->IsRecording())
        {
            m_voiceInputManager->StartRecording();
        }

        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (event == nullptr)
    {
        return;
    }

    if (event->isAutoRepeat())
    {
        event->accept();
        return;
    }

    if ((event->key() == Qt::Key_V)
        && event->modifiers().testFlag(Qt::ControlModifier)
        && event->modifiers().testFlag(Qt::AltModifier))
    {
        if ((m_voiceInputManager != nullptr) && m_voiceInputManager->IsRecording())
        {
            m_voiceInputManager->StopRecording();
        }

        event->accept();
        return;
    }

    QWidget::keyReleaseEvent(event);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (message == nullptr)
    {
        return QWidget::nativeEvent(eventType, message, result);
    }

#ifdef Q_OS_WIN
    MSG *windowsMessage = static_cast<MSG *>(message);

    if ((windowsMessage->message == WM_HOTKEY)
        && (windowsMessage->wParam == static_cast<WPARAM>(VOICE_HOTKEY_ID)))
    {
        ToggleVoiceRecording();

        if (result != nullptr)
        {
            *result = 0;
        }

        return true;
    }
#else
    (void)eventType;
    (void)result;
#endif

    return QWidget::nativeEvent(eventType, message, result);
}

void MainWindow::OnFrameChanged(const QString &framePath)
{
    if (framePath.isEmpty())
    {
        return;
    }

    if ((framePath == m_lastFramePath) && m_currentImageSize.isValid())
    {
        return;
    }

    const QString cacheKey = QStringLiteral("vpet_frame_%1_%2")
                                 .arg(TARGET_DISPLAY_WIDTH)
                                 .arg(framePath);

    QPixmap displayPixmap;

    if (!QPixmapCache::find(cacheKey, &displayPixmap))
    {
        const QPixmap originalPixmap(framePath);

        if (originalPixmap.isNull())
        {
            return;
        }

        displayPixmap = originalPixmap.scaledToWidth(
            TARGET_DISPLAY_WIDTH, Qt::SmoothTransformation);
        QPixmapCache::insert(cacheKey, displayPixmap);
    }

    const bool isFirstFrame = !m_currentImageSize.isValid();

    m_lastFramePath = framePath;
    m_currentImageSize = displayPixmap.size();
    m_imageLabel->setPixmap(displayPixmap);
    m_imageLabel->resize(m_currentImageSize);

    if (m_controller != nullptr)
    {
        m_controller->SetFrameSize(m_currentImageSize);
    }

    UpdateHitRegions(m_currentImageSize);

    if (m_perceptionIndicatorLabel != nullptr)
    {
        m_perceptionIndicatorLabel->move(m_currentImageSize.width() - 14, 2);
    }

    resize(m_currentImageSize);
    update();

    if (isFirstFrame)
    {
        CenterOnScreen();
    }
}

void MainWindow::OnBubbleChanged(bool visible, const QString &text)
{
    m_bubbleLabel->setText(text);
    m_bubbleLabel->adjustSize();

    if (visible)
    {
        const int x = (width() - m_bubbleLabel->width()) / 2;
        const int y = -m_bubbleLabel->height() - BUBBLE_OFFSET_Y;
        m_bubbleLabel->move(x, y);
    }

    m_bubbleLabel->setVisible(visible);
}

void MainWindow::OnPositionChanged(const QPoint &position)
{
    move(position);

    // 气泡跟随宠物移动
    if ((m_chatBubbleWindow != nullptr) && m_chatBubbleWindow->IsVisible())
    {
        m_chatBubbleWindow->FollowTarget(position, m_currentImageSize);
    }
}

void MainWindow::OnStateChanged(PET_STATE newState)
{
    (void)newState;
}

void MainWindow::UpdateHitRegions(const QSize &imageSize)
{
    if (!imageSize.isValid())
    {
        return;
    }

    const int headBottom = imageSize.height() / 3;
    const int bodyTop = headBottom;

    const QRect headRegion(0, 0, imageSize.width(), headBottom);
    const QRect bodyRegion(0, bodyTop, imageSize.width(), imageSize.height() - bodyTop);
    const QRect dragRegion; // 空：拖拽由移动距离判断，不依赖固定区域

    m_controller->SetHitRegions(headRegion, bodyRegion, dragRegion);
}

void MainWindow::CenterOnScreen()
{
    const QScreen *screen = QGuiApplication::primaryScreen();

    if (screen == nullptr)
    {
        return;
    }

    const QRect screenGeometry = screen->availableGeometry();
    const QPoint center = screenGeometry.center();

    const QSize frameSize = m_currentImageSize.isValid()
                            ? m_currentImageSize
                            : QSize(TARGET_DISPLAY_WIDTH, TARGET_DISPLAY_WIDTH);

    const QPoint position(center.x() - (frameSize.width() / 2),
                          center.y() - (frameSize.height() / 2));

    m_controller->SetPosition(position);
}

void MainWindow::OnSayStarted(const QString &groupName)
{
    (void)groupName;
    // TTS 合成与台词选择已在 PetController 中处理，
    // 此处仅记录分组名，气泡显示通过 SayTextReady 信号触发。
}

void MainWindow::OnSayTextReady(const QString &text)
{
    if (text.isEmpty())
    {
        return;
    }

    if (m_chatBubbleWindow == nullptr)
    {
        return;
    }

    // Say 气泡不再绑定 SAYING 状态，避免音频失败或快速退出时立即消失。
    m_chatBubbleWindow->Show(text, SAY_BUBBLE_DURATION_MS);
    m_chatBubbleWindow->FollowTarget(m_controller->GetPosition(), m_currentImageSize);
}

void MainWindow::OnPerceptionDataReady(const QByteArray &encodedData, int frameId)
{
    if (encodedData.isEmpty())
    {
        qWarning() << "[Vision] Empty perception payload ignored.";
        return;
    }

    if (frameId <= 0)
    {
        qWarning() << "[Vision] Invalid perception frame ID:" << frameId;
        return;
    }

    if (m_perceptionPipeline == nullptr)
    {
        qWarning() << "[Vision] Perception pipeline is not available.";
        return;
    }

    const QSize frameSize = m_perceptionPipeline->GetLatestFrameSize();

    if (!frameSize.isValid())
    {
        qWarning() << "[Vision] Invalid perception frame size:" << frameSize;
        return;
    }

    const QString modality = QStringLiteral("image/jpeg");

    if (m_agentRuntime != nullptr)
    {
        QString errorMessage;

        if (!m_agentRuntime->UpdatePerceptionFrame(encodedData,
                                                   frameId,
                                                   frameSize,
                                                   modality,
                                                   errorMessage))
        {
            qWarning() << "[Agent] Failed to update perception context:" << errorMessage;
        }
        else if (!m_agentRuntime->Execute(errorMessage))
        {
            qWarning() << "[Agent] Perception DAG execution failed:" << errorMessage;
        }
    }

    emit PerceptionReceived(encodedData, modality);
}

void MainWindow::OnPerceptionError(const QString &message)
{
    if (message.isEmpty())
    {
        qWarning() << "[Vision] Perception pipeline reported an empty error message.";
        return;
    }

    qWarning() << "[Vision]" << message;
}

void MainWindow::OnVoiceTranscriptionCompleted(const QString &text)
{
    if (text.trimmed().isEmpty())
    {
        qWarning() << "[VoiceInput] Empty transcription ignored.";
        return;
    }

    qDebug() << "[VoiceInput] Transcription completed, characters:" << text.size();
    SubmitTextToAgent(text);
}

void MainWindow::OnVoiceTranscriptionFailed(const QString &message)
{
    if (message.isEmpty())
    {
        qWarning() << "[VoiceInput] Unknown voice input failure.";
        return;
    }

    qWarning() << "[VoiceInput]" << message;
}

void MainWindow::OnAgentLogMessage(const QString &message)
{
    if (message.isEmpty())
    {
        return;
    }

    qDebug() << "[Agent]" << message;
}

void MainWindow::OnAgentLlmResponseReceived(int requestId, const QString &content)
{
    if (requestId <= 0)
    {
        qWarning() << "[AgentLLM] Invalid completed request ID:" << requestId;
        return;
    }

    if (content.isEmpty())
    {
        qWarning() << "[AgentLLM] Empty response for request:" << requestId;
        return;
    }

    qDebug() << "[AgentLLM] Voice response, request:" << requestId
             << "characters:" << content.size();
}

void MainWindow::OnStreamSentenceReady(const SentenceChunk &chunk)
{
    if (m_controller == nullptr)
    {
        return;
    }

    m_streamingRequests.insert(chunk.requestId);
    m_controller->EnqueueStreamSentence(chunk);
}

void MainWindow::OnStreamResponseFinished(int requestId)
{
    if (m_controller != nullptr)
    {
        m_controller->FinishStreamDialogue(requestId);
    }
}

void MainWindow::OnAgentOutputReady(int requestId, const QString &content, const QString &source)
{
    if (requestId <= 0)
    {
        qWarning() << "[AgentOutput] Invalid request ID:" << requestId;
        return;
    }

    const QString normalizedContent = content.trimmed();

    if (normalizedContent.isEmpty())
    {
        qWarning() << "[AgentOutput] Empty final output for request:" << requestId;
        return;
    }

    if (m_streamingRequests.contains(requestId))
    {
        m_streamingRequests.remove(requestId);
        return;
    }

    const QString normalizedSource = source.trimmed();
    const SaySource saySource =
        (normalizedSource == QStringLiteral("vision_proactive"))
        ? SaySource::VisionProactive
        : SaySource::UserResponse;

    qDebug() << "[AgentOutput] Final output ready, request:" << requestId
             << "source:" << normalizedSource
             << "characters:" << normalizedContent.size();

    if (m_controller != nullptr)
    {
        if (!m_controller->RequestSay(normalizedContent, saySource))
        {
            qWarning() << "[AgentOutput] RequestSay rejected, request:" << requestId
                       << "source:" << normalizedSource;
        }
    }
}

void MainWindow::OnAgentLlmRequestFailed(int requestId, const QString &message, int statusCode)
{
    if (message.isEmpty())
    {
        qWarning() << "[AgentLLM] Empty error message, request:" << requestId
                   << "status:" << statusCode;
        return;
    }

    qWarning() << "[AgentLLM] Request failed:" << requestId
               << "status:" << statusCode
                << "message:" << message;
}

void MainWindow::OnAgentRequestFailed(int requestId,
                                      const QString &message,
                                      int statusCode,
                                      const QString &source)
{
    (void)requestId;
    (void)message;
    (void)statusCode;

    if (source == QStringLiteral("user_response"))
    {
        ShowAgentFailureBubble();
    }
}

void MainWindow::ShowPetContextMenu(const QPoint &globalPosition)
{
    QMenu menu(this);
    QAction *voiceInputAction = menu.addAction(QStringLiteral("Ctrl+Alt+V 语音输入"));

    if (voiceInputAction != nullptr)
    {
        voiceInputAction->setEnabled(m_voiceInputManager != nullptr);
        connect(voiceInputAction, &QAction::triggered, this, &MainWindow::ToggleVoiceRecording);
    }

    menu.addSeparator();

    QAction *screenPerceptionAction = menu.addAction(QStringLiteral("屏幕感知（截图）"));

    if (screenPerceptionAction != nullptr)
    {
        screenPerceptionAction->setCheckable(true);
        screenPerceptionAction->setChecked(m_isScreenPerceptionEnabled);
        connect(screenPerceptionAction, &QAction::toggled, this, [this](bool checked)
        {
            SetScreenPerceptionEnabled(checked);
        });
    }

    if ((m_agentRuntime != nullptr) && m_agentRuntime->IsVisionLlmConfigured())
    {
        menu.addSeparator();

        QMenu *visionModelMenu = menu.addMenu(QStringLiteral("图像识别模型设置"));

        if (visionModelMenu != nullptr)
        {
            // QActionGroup 必须 heap 分配并挂到 menu，否则 menu.exec 前栈对象已析构，
            // 互斥勾选会失效。
            QActionGroup *modelActionGroup = new QActionGroup(&menu);
            modelActionGroup->setExclusive(true);

            QAction *mimoAction = visionModelMenu->addAction(QStringLiteral("mimo-v2.5"));
            QAction *gptAction = visionModelMenu->addAction(QStringLiteral("gpt"));

            if ((mimoAction != nullptr) && (gptAction != nullptr))
            {
                mimoAction->setCheckable(true);
                gptAction->setCheckable(true);
                modelActionGroup->addAction(mimoAction);
                modelActionGroup->addAction(gptAction);

                const VISION_LLM_MODEL_PROFILE activeProfile =
                    m_agentRuntime->GetActiveVisionLlmProfile();
                mimoAction->setChecked(activeProfile == VISION_LLM_MODEL_PROFILE::MIMO_V2_5);
                gptAction->setChecked(activeProfile == VISION_LLM_MODEL_PROFILE::GPT);

                connect(mimoAction, &QAction::triggered, this, [this]()
                {
                    if ((m_agentRuntime == nullptr)
                        || !m_agentRuntime->SetActiveVisionLlmProfile(
                               VISION_LLM_MODEL_PROFILE::MIMO_V2_5))
                    {
                        qWarning() << "[VisionLLM] Failed to switch to mimo-v2.5 profile.";
                    }
                });

                connect(gptAction, &QAction::triggered, this, [this]()
                {
                    if ((m_agentRuntime == nullptr)
                        || !m_agentRuntime->SetActiveVisionLlmProfile(
                               VISION_LLM_MODEL_PROFILE::GPT))
                    {
                        qWarning() << "[VisionLLM] Failed to switch to gpt profile.";
                    }
                });
            }
        }
    }

    menu.addSeparator();

    QAction *memoryManagerAction = menu.addAction(QStringLiteral("长期记忆"));

    if (memoryManagerAction != nullptr)
    {
        memoryManagerAction->setEnabled((m_agentRuntime != nullptr)
                                        && m_agentRuntime->IsMemoryEnabled());
        connect(memoryManagerAction, &QAction::triggered,
                this, &MainWindow::ShowMemoryManager);
    }

    const QStringList surfacedMemoryIds = (m_agentRuntime != nullptr)
                                               ? m_agentRuntime->GetLatestSurfacedMemoryIds()
                                               : QStringList();

    QAction *helpfulMemoryAction = menu.addAction(QStringLiteral("这次记忆有帮助"));
    QAction *unhelpfulMemoryAction = menu.addAction(QStringLiteral("这次记忆无帮助"));

    if ((helpfulMemoryAction != nullptr) && (unhelpfulMemoryAction != nullptr))
    {
        const bool feedbackEnabled = !surfacedMemoryIds.isEmpty();
        helpfulMemoryAction->setEnabled(feedbackEnabled);
        unhelpfulMemoryAction->setEnabled(feedbackEnabled);

        connect(helpfulMemoryAction, &QAction::triggered, this,
                [this, surfacedMemoryIds]()
        {
            quint64 requestId = 0;

            if ((m_agentRuntime != nullptr)
                && !m_agentRuntime->SubmitMemoryFeedback(surfacedMemoryIds, true, requestId))
            {
                qWarning() << "[Memory] Helpful feedback was rejected.";
            }
        });

        connect(unhelpfulMemoryAction, &QAction::triggered, this,
                [this, surfacedMemoryIds]()
        {
            quint64 requestId = 0;

            if ((m_agentRuntime != nullptr)
                && !m_agentRuntime->SubmitMemoryFeedback(surfacedMemoryIds, false, requestId))
            {
                qWarning() << "[Memory] Unhelpful feedback was rejected.";
            }
        });
    }

    menu.addSeparator();

    QAction *exitAction = menu.addAction(QStringLiteral("退出程序"));

    if (exitAction != nullptr)
    {
        connect(exitAction, &QAction::triggered,
                this, &MainWindow::RequestApplicationExit);
    }

    menu.exec(globalPosition);
}

void MainWindow::ShowMemoryManager()
{
    if ((m_agentRuntime == nullptr) || !m_agentRuntime->IsMemoryEnabled())
    {
        return;
    }

    if (m_memoryManagerDialog == nullptr)
    {
        m_memoryManagerDialog = new MemoryManagerDialog(m_agentRuntime, this);
        m_memoryManagerDialog->setAttribute(Qt::WA_DeleteOnClose, false);
    }

    m_memoryManagerDialog->ShowAndRefresh();
}

void MainWindow::RequestApplicationExit()
{
    if (m_isExiting)
    {
        return;
    }

    m_isExiting = true;

    hide();

    if (m_chatBubbleWindow != nullptr)
    {
        m_chatBubbleWindow->hide();
    }

    if (m_trayIcon != nullptr)
    {
        m_trayIcon->hide();
    }

    UnregisterVoiceHotkey();

    if ((m_voiceInputManager != nullptr) && m_voiceInputManager->IsRecording())
    {
        m_voiceInputManager->StopRecording();
    }

    if (m_perceptionPipeline != nullptr)
    {
        m_perceptionPipeline->Stop();
    }

    m_isScreenPerceptionEnabled = false;
    UpdatePerceptionIndicator();
    QCoreApplication::quit();
}

void MainWindow::InitializeSystemTrayIcon()
{
    if (m_trayIcon != nullptr)
    {
        return;
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        qWarning() << "[Tray] System tray is not available.";
        return;
    }

    m_trayMenu = new QMenu(this);
    QAction *showAction = m_trayMenu->addAction(QStringLiteral("显示桌宠"));

    if (showAction != nullptr)
    {
        connect(showAction, &QAction::triggered, this, [this]()
        {
            show();
            raise();
            activateWindow();
        });
    }

    m_trayMenu->addSeparator();

    QAction *exitAction = m_trayMenu->addAction(QStringLiteral("退出程序"));

    if (exitAction != nullptr)
    {
        connect(exitAction, &QAction::triggered,
                this, &MainWindow::RequestApplicationExit);
    }

    QIcon trayIcon;

    if (!m_lastFramePath.isEmpty())
    {
        trayIcon = QIcon(m_lastFramePath);
    }

    if (trayIcon.isNull())
    {
        trayIcon = QApplication::windowIcon();
    }

    if (trayIcon.isNull())
    {
        trayIcon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }

    m_trayIcon = new QSystemTrayIcon(trayIcon, this);
    m_trayIcon->setToolTip(QStringLiteral("VPet"));
    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason)
    {
        if (reason != QSystemTrayIcon::DoubleClick)
        {
            return;
        }

        show();
        raise();
        activateWindow();
    });

    m_trayIcon->show();
}

void MainWindow::SetScreenPerceptionEnabled(bool enabled)
{
    if (m_perceptionPipeline == nullptr)
    {
        qWarning() << "[Vision] Perception pipeline is not available.";
        m_isScreenPerceptionEnabled = false;
        UpdatePerceptionIndicator();
        return;
    }

    if (enabled == m_isScreenPerceptionEnabled)
    {
        UpdatePerceptionIndicator();
        return;
    }

    if (enabled)
    {
        if (!m_perceptionPipeline->Start())
        {
            qWarning() << "[Vision] Perception pipeline failed to start.";
            m_isScreenPerceptionEnabled = false;
            UpdatePerceptionIndicator();
            return;
        }

        m_isScreenPerceptionEnabled = true;
        qDebug() << "[Vision] Screen perception enabled by user.";
    }
    else
    {
        m_perceptionPipeline->Stop();
        m_isScreenPerceptionEnabled = false;
        qDebug() << "[Vision] Screen perception disabled by user.";
    }

    UpdatePerceptionIndicator();
}

void MainWindow::UpdatePerceptionIndicator()
{
    if (m_perceptionIndicatorLabel == nullptr)
    {
        return;
    }

    m_perceptionIndicatorLabel->setVisible(m_isScreenPerceptionEnabled);

    if (m_isScreenPerceptionEnabled && m_currentImageSize.isValid())
    {
        m_perceptionIndicatorLabel->move(m_currentImageSize.width() - 14, 2);
        m_perceptionIndicatorLabel->raise();
    }
}

void MainWindow::ToggleVoiceRecording()
{
    if (m_voiceInputManager == nullptr)
    {
        qWarning() << "[VoiceInput] Voice input manager is not initialized.";
        return;
    }

    if (m_voiceInputManager->IsRecording())
    {
        m_voiceInputManager->StopRecording();
        return;
    }

    m_voiceInputManager->StartRecording();
}

bool MainWindow::RegisterVoiceHotkey()
{
    if (m_isVoiceHotkeyRegistered)
    {
        return true;
    }

#ifdef Q_OS_WIN
    const HWND windowHandle = reinterpret_cast<HWND>(winId());

    if (windowHandle == nullptr)
    {
        return false;
    }

    const BOOL isRegistered = RegisterHotKey(windowHandle,
                                             VOICE_HOTKEY_ID,
                                             MOD_CONTROL | MOD_ALT,
                                             'V');

    if (isRegistered == FALSE)
    {
        return false;
    }

    m_isVoiceHotkeyRegistered = true;
    qDebug() << "[VoiceInput] Global hotkey registered: Ctrl+Alt+V";

    return true;
#else
    qWarning() << "[VoiceInput] Global hotkey is only implemented on Windows.";
    return false;
#endif
}

void MainWindow::UnregisterVoiceHotkey()
{
    if (!m_isVoiceHotkeyRegistered)
    {
        return;
    }

#ifdef Q_OS_WIN
    const HWND windowHandle = reinterpret_cast<HWND>(winId());

    if (windowHandle != nullptr)
    {
        UnregisterHotKey(windowHandle, VOICE_HOTKEY_ID);
    }
#endif

    m_isVoiceHotkeyRegistered = false;
}

void MainWindow::SubmitTextToAgent(const QString &text)
{
    const QString normalizedText = text.trimmed();

    if (normalizedText.isEmpty())
    {
        qWarning() << "[Agent] Voice text is empty.";
        return;
    }

    if (m_agentRuntime != nullptr)
    {
        m_agentRuntime->CancelActiveStreaming();
    }

    m_streamingRequests.clear();

    if (m_controller != nullptr)
    {
        m_controller->InterruptStreamDialogue();
    }

    if (m_agentRuntime == nullptr)
    {
        qWarning() << "[Agent] Runtime is not connected. Voice text kept in log only.";
    }
    else
    {
        QString errorMessage;

        if (!m_agentRuntime->ExecuteWithUserInput(normalizedText, errorMessage))
        {
            qWarning() << "[Agent] Voice input execution failed:" << errorMessage;
            ShowAgentFailureBubble();
        }
        else
        {
            qDebug() << "[Agent] Voice input entered runtime context.";
        }
    }
}

void MainWindow::ShowAgentFailureBubble()
{
    if ((m_chatBubbleWindow == nullptr) || (m_controller == nullptr))
    {
        return;
    }

    m_chatBubbleWindow->Show(QStringLiteral("这次没有成功响应，请稍后再试。"),
                             SAY_BUBBLE_DURATION_MS);
    m_chatBubbleWindow->FollowTarget(m_controller->GetPosition(), m_currentImageSize);
}

} // namespace vpet
