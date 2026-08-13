#ifndef VPET_PET_CONTROLLER_H
#define VPET_PET_CONTROLLER_H

#include "vpet/bubble_message.h"
#include "vpet/common_types.h"
#include "vpet/hit_region.h"
#include "vpet/pet_state_machine.h"
#include "vpet/animation_resource_manager.h"

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QTemporaryDir>
#include <QTimer>

namespace vpet
{

class TtsClient;
class TtsAudioPlayer;

/**
 * @brief 说话请求来源
 */
enum class SaySource
{
    IdleRandom,
    VisionProactive,
    UserResponse
};

/**
 * @brief 桌宠控制器
 *
 * 连接 Qt UI 事件与核心状态机，管理位置移动、气泡显示与命中区域。
 */
class PetController : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] animationBasePath 动画资源根目录
     * @param[in] parent 父对象
     */
    explicit PetController(const QString &animationBasePath, QObject *parent = nullptr);

    /**
     * @brief 初始化资源、状态机与定时器
     * @return 资源加载成功返回 true
     */
    bool Initialize();

    /**
     * @brief 处理鼠标按下事件
     * @param[in] localPos 窗口本地坐标
     */
    void OnMousePress(const QPoint &localPos);

    /**
     * @brief 处理鼠标移动事件
     * @param[in] globalPos 全局鼠标坐标
     */
    void OnMouseMove(const QPoint &globalPos);

    /**
     * @brief 处理鼠标释放事件
     * @param[in] localPos 窗口本地坐标
     */
    void OnMouseRelease(const QPoint &localPos);

    /**
     * @brief 获取当前帧图片路径
     * @return 当前帧路径；无有效帧返回空字符串
     */
    QString GetCurrentFramePath() const;

    /**
     * @brief 获取宠物当前位置
     * @return 窗口左上角全局坐标
     */
    QPoint GetPosition() const;

    /**
     * @brief 设置宠物位置
     * @param[in] position 窗口左上角全局坐标
     */
    void SetPosition(const QPoint &position);

    /**
     * @brief 判断当前是否有气泡
     * @return 有气泡返回 true
     */
    bool HasBubble() const;

    /**
     * @brief 获取气泡文本
     * @return 气泡文本
     */
    QString GetBubbleText() const;

    /**
     * @brief 判断当前是否处于拖拽状态
     * @return 拖拽中返回 true
     */
    bool IsDragging() const;

    /**
     * @brief 获取当前状态
     * @return 当前状态
     */
    PET_STATE GetCurrentState() const;

    /**
     * @brief 设置宠物情绪
     * @param[in] mood 情绪
     */
    void SetMood(PET_MOOD mood);

    /**
     * @brief 设置屏幕边界
     * @param[in] bounds 屏幕可用区域
     */
    void SetScreenBounds(const QRect &bounds);

    /**
     * @brief 设置命中区域
     * @param[in] head 头部区域
     * @param[in] body 身体区域
     * @param[in] drag 拖拽区域
     */
    void SetHitRegions(const QRect &head, const QRect &body, const QRect &drag);

    /**
     * @brief 设置当前帧尺寸
     *
     * 用于边界限制与命中区域计算。
     *
     * @param[in] size 帧尺寸
     */
    void SetFrameSize(const QSize &size);

    /**
     * @brief 获取当前帧尺寸
     * @return 帧尺寸
     */
    QSize GetFrameSize() const;

    /**
     * @brief 请求桌宠说一句话
     *
     * 所有主动/被动输出统一走该入口，再由控制器按来源处理优先级和排队。
     *
     * @param[in] text 说话文本
     * @param[in] source 说话来源
     * @return 请求被接受返回 true
     */
    bool RequestSay(const QString &text, SaySource source);

signals:
    /**
     * @brief 当前帧变化信号
     * @param[in] framePath 新帧路径
     */
    void FrameChanged(const QString &framePath);

    /**
     * @brief 气泡变化信号
     * @param[in] visible 是否可见
     * @param[in] text 气泡文本
     */
    void BubbleChanged(bool visible, const QString &text);

    /**
     * @brief 位置变化信号
     * @param[in] position 新位置
     */
    void PositionChanged(const QPoint &position);

    /**
     * @brief 状态变化信号
     * @param[in] newState 新状态
     */
    void StateChanged(PET_STATE newState);

    /**
     * @brief 说话动画开始信号
     *
     * 当待机触发了 Say 动作时发射，参数为具体的分组名（如 "say_self"）。
     * 后续可连接此信号到 XAML 聊天气泡绘制逻辑。
     *
     * @param[in] groupName Say 分组名
     */
    void SayStarted(const QString &groupName);

    /**
     * @brief Say 台词文本已就绪信号
     *
     * 当 Say 触发后选中台词文本时发射，用于通知聊天气泡窗口显示。
     *
     * @param[in] text 台词文本
     */
    void SayTextReady(const QString &text);

private slots:
    /**
     * @brief 定时更新槽
     */
    void OnUpdate();

    /**
     * @brief TTS 合成完成后的处理槽
     * @param[in] filePath 合成后的音频文件路径
     */
    void OnTtsSynthesisFinished(const QString &filePath);

    /**
     * @brief 音频播放完成后的处理槽
     */
    void OnAudioPlaybackFinished();

    /**
     * @brief 说话状态超时处理槽
     */
    void OnSayingTimeout();

    /**
     * @brief 删除临时音频文件，失败时延迟重试几次
     * @param[in] filePath 要删除的音频文件路径
     * @param[in] attempt 已重试次数
     */
    void DeleteAudioFileWithRetry(const QString &filePath, int attempt = 0);

private:
    /**
     * @brief 根据当前状态更新步行位置
     * @param[in] deltaTimeMs 时间间隔
     */
    void UpdateWalkPosition(int deltaTimeMs);

    /**
     * @brief 显示交互对应的气泡文本
     * @param[in] state 触发气泡的状态
     */
    void ShowInteractionBubble(PET_STATE state);

    /**
     * @brief 获取随机摸头气泡文本
     * @return 气泡文本
     */
    static QString GetRandomTouchHeadText();

    /**
     * @brief 获取随机摸身体气泡文本
     * @return 气泡文本
     */
    static QString GetRandomTouchBodyText();

    /**
     * @brief 立即启动说话流程
     * @param[in] text 说话文本
     * @param[in] source 说话来源
     * @return 成功启动返回 true
     */
    bool StartSayText(const QString &text, SaySource source, const QString &preferredAction);

    /**
     * @brief 将说话请求放入单条队列
     * @param[in] text 说话文本
     * @param[in] source 说话来源
     * @return 请求被排队返回 true
     */
    bool QueueSayText(const QString &text, SaySource source);

    /**
     * @brief 在状态允许时启动队列中的说话请求
     */
    void TryStartQueuedSay();

    /**
     * @brief 无 Say 动画资源时，仅显示气泡并清空队列
     * @param[in] text 说话文本
     * @param[in] source 说话来源
     * @return 文本非空时返回 true
     */
    bool ShowSayTextWithoutAction(const QString &text, SaySource source);

    /**
     * @brief 将窗口位置限制在屏幕边界内
     * @param[in,out] position 待限制的位置
     */
    void ClampPositionToScreen(QPoint &position) const;

private:
    AnimationResourceManager m_resourceManager; ///< 动画资源管理器
    PetStateMachine m_stateMachine;             ///< 状态机
    BubbleMessage m_bubbleMessage;              ///< 气泡消息
    HitRegion m_hitRegion;                      ///< 命中区域
    QTimer *m_updateTimer;                      ///< 更新定时器
    QPoint m_position;                          ///< 当前窗口位置
    QPoint m_dragStartGlobalPos;                ///< 拖拽开始时鼠标全局位置
    QPoint m_windowStartPos;                    ///< 拖拽开始时窗口位置
    QRect m_screenBounds;                       ///< 屏幕边界
    int m_walkSpeedPixelsPerSecond;             ///< 步行速度
    bool m_isDragging;                          ///< 是否正在拖拽
    bool m_dragMoved;                           ///< 本次按下后是否移动过
    HIT_TYPE m_pendingHitType;                  ///< 按下时记录的命中类型，用于区分点击
    PET_STATE m_lastState;                      ///< 上一帧状态，用于检测状态变化
    QSize m_frameSize;                          ///< 当前帧尺寸
    TtsClient *m_ttsClient;                     ///< TTS 客户端
    TtsAudioPlayer *m_ttsAudioPlayer;           ///< TTS 音频播放器
    QTimer *m_sayingTimeoutTimer;               ///< SAYING 状态超时兜底定时器
    QTemporaryDir m_tempDir;                    ///< 临时目录，存放合成的音频文件
    QString m_currentSayText;                   ///< 当前 Say 台词文本
    SaySource m_currentSaySource;               ///< 当前 Say 来源
    int m_synthesisCounter;                     ///< TTS 合成序号，用于生成唯一文件名
    int m_sayCooldownRemainingMs;               ///< Say 触发冷却剩余时间
    QString m_pendingAudioPath;                 ///< 已合成待播放的音频文件路径
    QString m_pendingSayAction;                 ///< 待进入的 Say 动作名
    SaySource m_pendingSaySource;               ///< 正在合成的 Say 来源
    bool m_discardPendingSynthesis;             ///< 是否丢弃下一次 TTS 合成结果
    QString m_queuedSayText;                    ///< 下一条待播放 Say 文本
    QString m_queuedSayAction;                  ///< 下一条待播放 Say 动作名
    SaySource m_queuedSaySource;                ///< 下一条待播放 Say 来源
    QString m_lastEmittedFramePath;             ///< 最近一次已发射的帧路径
    bool m_lastEmittedBubbleVisible;            ///< 最近一次已发射的气泡可见性
    QString m_lastEmittedBubbleText;            ///< 最近一次已发射的气泡文本
};

} // namespace vpet

#endif // VPET_PET_CONTROLLER_H
