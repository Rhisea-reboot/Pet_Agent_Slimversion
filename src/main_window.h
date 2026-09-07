#ifndef VPET_MAIN_WINDOW_H
#define VPET_MAIN_WINDOW_H

#include "vpet/pet_config.h"
#include "vpet/pet_controller.h"
#include "vpet/stream_sentence_splitter.h"

#include <QByteArray>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QSet>
#include <QWidget>

class QMenu;
class QSlider;
class QSystemTrayIcon;

namespace vpet
{

class AgentRuntime;
class ChatBubbleWindow;
class DagEditorServer;
class MemoryManagerDialog;
class PerceptionPipeline;
class VoiceInputManager;

/**
 * @brief 桌宠主窗口
 *
 * 透明无边框置顶窗口，负责渲染当前帧、显示气泡、转发鼠标事件，
 * 以及管理聊天气泡窗口的生命周期。
 */
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param[in] parent 父窗口
     */
    explicit MainWindow(QWidget *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~MainWindow() override;

    /**
     * @brief 初始化窗口与控制器
     * @param[in] animationBasePath 动画资源根目录
     * @return 初始化成功返回 true
     */
    bool Initialize(const QString &animationBasePath);

    /**
     * @brief 设置 Agent 运行时
     * @param[in] agentRuntime Agent 运行时对象
     */
    void SetAgentRuntime(AgentRuntime *agentRuntime);

signals:
    /**
     * @brief 视觉截图数据就绪信号
     * @param[in] base64Data Base64 图像数据
     * @param[in] modality 模态名称
     */
    void PerceptionReceived(const QByteArray &base64Data, const QString &modality);

protected:
    /**
     * @brief 鼠标按下事件
     * @param[in] event 鼠标事件
     */
    void mousePressEvent(QMouseEvent *event) override;

    /**
     * @brief 鼠标移动事件
     * @param[in] event 鼠标事件
     */
    void mouseMoveEvent(QMouseEvent *event) override;

    /**
     * @brief 鼠标释放事件
     * @param[in] event 鼠标事件
     */
    void mouseReleaseEvent(QMouseEvent *event) override;

    /**
     * @brief 键盘按下事件
     * @param[in] event 键盘事件
     */
    void keyPressEvent(QKeyEvent *event) override;

    /**
     * @brief 键盘释放事件
     * @param[in] event 键盘事件
     */
    void keyReleaseEvent(QKeyEvent *event) override;

    /**
     * @brief 处理平台原生事件
     * @param[in] eventType 事件类型
     * @param[in] message 原生事件消息
     * @param[out] result 原生事件结果
     * @return 已处理返回 true
     */
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private slots:
    /**
     * @brief 帧变化槽
     * @param[in] framePath 新帧路径
     */
    void OnFrameChanged(const QString &framePath);

    /**
     * @brief 气泡变化槽
     * @param[in] visible 是否可见
     * @param[in] text 气泡文本
     */
    void OnBubbleChanged(bool visible, const QString &text);

    /**
     * @brief 位置变化槽
     * @param[in] position 新位置
     */
    void OnPositionChanged(const QPoint &position);

    /**
     * @brief 状态变化槽
     * @param[in] newState 新状态
     */
    void OnStateChanged(PET_STATE newState);

    /**
     * @brief 说话动画开始槽
     *
     * 预留接口：后续可在此触发 XAML 聊天气泡绘制。
     *
     * @param[in] groupName Say 分组名，如 "say_self"
     */
    void OnSayStarted(const QString &groupName);

    /**
     * @brief Say 台词文本就绪槽
     *
     * 收到台词文本后显示聊天气泡窗口。
     *
     * @param[in] text 台词文本
     */
    void OnSayTextReady(const QString &text);

    /**
     * @brief 感知数据就绪槽
     * @param[in] encodedData 编码后的图像数据
     * @param[in] frameId 帧序号
     */
    void OnPerceptionDataReady(const QByteArray &encodedData, int frameId);

    /**
     * @brief 感知错误槽
     * @param[in] message 错误描述
     */
    void OnPerceptionError(const QString &message);

    /**
     * @brief 语音识别完成槽
     * @param[in] text 识别文本
     */
    void OnVoiceTranscriptionCompleted(const QString &text);

    /**
     * @brief 语音识别失败槽
     * @param[in] message 错误描述
     */
    void OnVoiceTranscriptionFailed(const QString &message);

    /**
     * @brief Agent 日志槽
     * @param[in] message 日志内容
     */
    void OnAgentLogMessage(const QString &message);

    /**
     * @brief Agent LLM 回复完成槽
     * @param[in] requestId 请求 ID
     * @param[in] content 回复文本
     */
    void OnAgentLlmResponseReceived(int requestId, const QString &content);

    /**
     * @brief Agent 最终输出就绪槽
     * @param[in] requestId 请求 ID
     * @param[in] content 最终输出文本
     * @param[in] source 输出来源，允许值为 user_response 或 vision_proactive
     */
    void OnAgentOutputReady(int requestId, const QString &content, const QString &source);

    void OnStreamSentenceReady(const vpet::SentenceChunk &chunk);
    void OnStreamResponseFinished(int requestId);

    /**
     * @brief Agent LLM 请求失败槽
     * @param[in] requestId 请求 ID
     * @param[in] message 错误描述
     * @param[in] statusCode HTTP 状态码
     */
    void OnAgentLlmRequestFailed(int requestId, const QString &message, int statusCode);

    /** @brief 处理包含触发来源的 Agent 失败，用户请求显示气泡。 */
    void OnAgentRequestFailed(int requestId,
                              const QString &message,
                              int statusCode,
                              const QString &source);

private:
    /**
     * @brief 显示桌宠右键菜单
     * @param[in] globalPosition 菜单弹出全局坐标
     */
    void ShowPetContextMenu(const QPoint &globalPosition);

    /**
     * @brief 显示长期记忆管理窗口
     */
    void ShowMemoryManager();

    /**
     * @brief 显示设置窗口（LLM 与长期记忆配置，保存后重启生效）
     */
    void ShowSettingsDialog();

    /**
     * @brief 启动或打开 DAG 可视化编辑器（本地 HTTP 服务 + 浏览器）
     */
    void ShowDagEditor();

    /**
     * @brief 请求应用程序退出
     *
     * 停止窗口层管理的异步资源后，请求 Qt 事件循环正常返回。
     */
    void RequestApplicationExit();

    /**
     * @brief 初始化系统托盘图标和菜单
     */
    void InitializeSystemTrayIcon();

    /**
     * @brief 切换屏幕感知（截图）开关
     * @param[in] enabled 是否启用
     */
    void SetScreenPerceptionEnabled(bool enabled);

    /**
     * @brief 更新屏幕感知状态指示器
     */
    void UpdatePerceptionIndicator();

    /**
     * @brief 切换语音录制状态
     */
    void ToggleVoiceRecording();

    /**
     * @brief 注册系统全局语音热键
     * @return 注册成功返回 true
     */
    bool RegisterVoiceHotkey();

    /**
     * @brief 注销系统全局语音热键
     */
    void UnregisterVoiceHotkey();

    /**
     * @brief 将文本输入提交到 Agent/LLM 链路
     * @param[in] text 用户文本
     */
    void SubmitTextToAgent(const QString &text);

    /** @brief 显示一次简短的用户请求失败提示。 */
    void ShowAgentFailureBubble();

    /**
     * @brief 根据图片尺寸更新命中区域
     * @param[in] imageSize 图片尺寸
     */
    void UpdateHitRegions(const QSize &imageSize);

    /**
     * @brief 获取当前显示宽度（基准宽度 × 显示缩放）
     * @return 实际显示宽度（像素）
     */
    int EffectiveDisplayWidth() const;

    /**
     * @brief 应用显示缩放并保持脚底中点锚点
     *
     * 按新缩放重取当前帧、更新帧尺寸与命中区域，随后调整窗口位置使宠物
     * 脚底中点不动，并经屏幕钳制后通知气泡跟随。
     *
     * @param[in] scale 缩放系数，越界值会被钳制；与当前值相同则忽略
     */
    void ApplyDisplayScale(float scale);

    /**
     * @brief 加载 pet_config.json 并应用音量与显示缩放
     *
     * 文件缺失或非法时使用默认值；须在首帧渲染前调用。
     */
    void LoadPetSettings();

    /**
     * @brief 将当前音量与显示缩放写回 pet_config.json
     *
     * 与最近一次读/写内容一致时不写盘。
     */
    void SavePetSettings();

    /**
     * @brief 将窗口居中到主屏幕
     */
    void CenterOnScreen();

private:
    PetController *m_controller;          ///< 宠物控制器
    QSet<int> m_streamingRequests;         ///< 当前前台流式请求 ID
    QLabel *m_imageLabel;                 ///< 帧显示标签
    QLabel *m_bubbleLabel;                ///< 气泡标签
    QLabel *m_perceptionIndicatorLabel;   ///< 屏幕感知开启时的红色指示点
    ChatBubbleWindow *m_chatBubbleWindow; ///< 聊天气泡窗口（独立顶层窗口）
    PerceptionPipeline *m_perceptionPipeline; ///< 视觉感知管道
    VoiceInputManager *m_voiceInputManager; ///< 按键语音输入管理器
    QSystemTrayIcon *m_trayIcon;        ///< 系统托盘图标
    QMenu *m_trayMenu;                  ///< 系统托盘菜单
    AgentRuntime *m_agentRuntime;          ///< Agent 运行时对象，不持有所有权
    MemoryManagerDialog *m_memoryManagerDialog; ///< 长期记忆管理窗口
    DagEditorServer *m_dagEditorServer;   ///< DAG 可视化编辑器服务（懒创建）
    QSize m_currentImageSize;             ///< 当前图片尺寸
    QString m_lastFramePath;              ///< 最近一次已加载的帧路径（避免重复磁盘 I/O）
    float m_displayScale;                 ///< 桌宠显示缩放（0.5 ~ 2.0，1.0 为基准）
    PetConfig m_lastSavedPetConfig;       ///< 最近一次读取/写入的桌宠配置（写盘脏检查）
    QString m_petConfigPath;              ///< pet_config.json 路径；为空时保存到可执行文件目录
    bool m_isVoiceHotkeyRegistered;        ///< 系统全局语音热键是否已注册
    bool m_isScreenPerceptionEnabled;      ///< 屏幕感知（截图上传）是否开启，默认关闭
    bool m_isExiting;                      ///< 是否已经请求应用程序退出
};

} // namespace vpet

#endif // VPET_MAIN_WINDOW_H
