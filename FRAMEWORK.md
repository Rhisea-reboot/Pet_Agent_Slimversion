# VPet 视觉感知框架

本文档描述当前已经落地的视觉感知链路。早期设计中的 `IModule`、统一 `Agent` 模块中心和独立 CLI 示例尚未作为主路径实现；当前应用使用 `MainWindow` 持有 `PerceptionPipeline`，并将编码帧交给 `AgentRuntime`。

## 设计边界

- 使用 Qt 6 的 `Gui`、`Core` 能力，不依赖额外视觉服务即可完成截图和编码。
- 感知模块由 Qt 事件循环和 `QTimer` 驱动，不创建独立线程。
- 截图默认只保存在内存中，不落盘。
- `PerceptionPipeline` 负责截图、处理、缓冲和编码；`AgentRuntime` 负责视觉 DAG 和视觉 LLM 请求。
- 感知帧在进入 Runtime 前按编码数据计算 SHA-256；相同内容的帧会被丢弃，当前不是感知级相似度去重。

## 当前模块

```text
MainWindow
    |
    | owns
    v
PerceptionPipeline
    | owns                         signals DataReady
    +--> ScreenshotSensor ------------------------+
    |                                             |
    +--> image processor chain                    v
    +--> FrameBuffer                         AgentRuntime
    +--> VisionEncoder                    vision.input
                                                  |
                                                  v
                                            vision.llm
```

### ScreenshotSensor

`ScreenshotSensor` 使用 `QTimer` 定时截取主屏幕，或按配置拼接所有屏幕。默认配置为每 3000 ms 一帧、PNG、内存模式和无限帧数。它提供 `Start()`、`Stop()`、`CaptureOnce()`、最新帧访问和 `FrameCaptured`/`ErrorOccurred` 信号。

关键配置字段：

| 字段 | 默认值 | 说明 |
|---|---:|---|
| `intervalMs` | `3000` | 截图间隔，最小值由配置归一化逻辑保证 |
| `maxFrames` | `-1` | 最大帧数，`-1` 表示不限 |
| `captureAllScreens` | `false` | 是否拼接多屏 |
| `saveToDisk` | `false` | 是否保存截图文件 |
| `imageFormat` | `PNG` | 输出图像格式 |
| `quality` | `-1` | 使用 Qt 默认图像质量 |
| `autoStart` | `false` | 由管道强制关闭，避免信号连接前提前截图 |

### FrameBuffer

`FrameBuffer` 是容量受限的环形缓冲，保存 `_tagFrame`：图像、UTC 时间戳、序列号和可选文件路径。容量会归一化到 `1..4096`，读取接口按“0 为最新帧”约定提供 `GetLatest()`、`GetAt()`、`GetRecent()` 和 `StitchRecent()`。

### VisionEncoder

`VisionEncoder` 将 `QPixmap` 编码为原始 PNG/JPEG、Base64 PNG/JPEG、灰度或 RGB 数据，并支持尺寸限制、保持宽高比和 JPEG 质量选项。应用主路径默认使用 Base64 PNG。

### PerceptionPipeline

管道构造时组合 `ScreenshotSensor` 和 `FrameBuffer`，可注册多个 `Processor`。每次截图完成后的处理顺序固定为：

```text
ScreenshotSensor
    -> latest QPixmap
    -> Processor chain
    -> FrameBuffer (optional)
    -> VisionEncoder
    -> DataReady(encodedData, frameId)
    -> MainWindow::OnPerceptionDataReady
    -> AgentRuntime::UpdatePerceptionFrame
```

启用缓冲时，管道还发出按最新到最旧排列的 `BatchReady`。空帧、无效尺寸、处理器返回空图像、编码失败等情况统一通过 `ErrorOccurred` 上报。

## 与 AgentRuntime 的连接

`MainWindow` 在初始化时创建管道，配置为 3000 ms 截图间隔、PNG 编码和容量为 5 的缓冲。屏幕感知默认关闭，用户在右键菜单主动启用后才会启动管道。管道的 `DataReady` 连接到窗口层，窗口层读取最新尺寸并调用：

```cpp
agentRuntime->UpdatePerceptionFrame(encodedData,
                                    frameId,
                                    frameSize,
                                    QStringLiteral("image/png"),
                                    errorMessage);
```

Runtime 将数据写入 `semantic.image.*` 和 `semantic.vision.*`，在空闲时启动视觉 invocation；有活动 invocation 时，用户输入按 FIFO 排队，视觉帧使用 latest-wins 策略。视觉链路由 `vision.input -> vision.llm -> proactive.topic` 开始，不会进入用户链路的 `web.research` 节点。

## 运行与隐私

- 管道 `Start()` 会启动传感器；`Stop()` 会停止定时器，但不会自动清空缓冲。
- `saveToDisk=false` 时截图不会写入文件；若开启保存，应明确配置目录并注意屏幕内容的隐私。
- 感知数据在进程内通过 Qt signal/slot 传递；项目当前没有为感知管道创建后台线程。
- Runtime 的帧去重只比较编码数据的 SHA-256，窗口或光标的微小变化会被视为新帧。

## 相关实现

- `include/vpet/sensor/screenshot_sensor.h`
- `include/vpet/perception/frame_buffer.h`
- `include/vpet/perception/vision_encoder.h`
- `include/vpet/perception/perception_pipeline.h`
- `src/main_window.cpp`
- `src/agent/agent_runtime.cpp`
- `AGENT_CONTEXT_KEY_PROTOCOL.md`
