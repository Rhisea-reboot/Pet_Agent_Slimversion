# VPet 文字转语音与 XAML 聊天气泡集成计划

## 1. 目标与范围

在现有 Qt 桌宠基础上，新增文字转语音（TTS）模块与 聊天气泡绘制：

TTS采用本地GPT-SoVITS完成，需要先将其集成到Pet Agent项目中

需要找到一种方式，html或者其他绘制方式（xaml），当宠物说话时在宠物头顶绘制聊天气泡，并在气泡中打印宠物说的话

同时在说话时播放对应文字转换成的音频

最终实现效果及其逻辑：
- **触发条件**：宠物进入待机动画 `SAYING`（即 `say_self` / `say_serious` / `say_shining` / `say_shy`）时。
- **行为**：
  1. 根据 Say 分组随机选择一句台词文本。
  2. 调用本地 `GPT-SoVITS-v2pro` 模型将文本合成语音。
  3. 播放合成后的语音。
  4. 同时通过 IPC 通知气泡窗口显示对应文字气泡。
- **非目标**：不替换现有 Qt 宠物窗口；XAML或其他绘制方式 仅负责聊天气泡 UI。

## 2. 现状分析

- 当前项目使用 **Qt 6.9.2 + CMake + MinGW** 构建。
- 宠物主窗口为透明无边框 `QWidget`，渲染、鼠标交互、状态机均在 Qt 侧完成。
- 已预留信号 `PetController::SayStarted(const QString &groupName)`，当前在 `MainWindow::OnSayStarted` 中为空实现，正好作为 TTS 触发点。
- GPT-SoVITS 目录 `E:\GPT-SoVITS-v2pro-20250604-nvidia50` 提供两种调用方式：
  - **HTTP API**：`api_v2.py` 启动 FastAPI 服务，端口默认 9880，端点 `/tts` 返回 WAV 音频流。
  - **CLI**：`GPT_SoVITS/inference_cli.py` 每次调用需加载模型，启动较慢，不推荐高频调用。
- **关键依赖项**：GPT-SoVITS 推理除基底模型外，还需要一段 **参考音频（ref_audio_path）** 及其 **参考文本（prompt_text）** 来克隆音色。当前目录下未找到合适参考音频，需在计划中明确由用户提供。

## 3. GPT-SoVITS 集成方案

### 3.1 调用方式：HTTP API（推荐）

启动一次 `api_v2.py` 作为后台服务，C++ 端通过 HTTP GET/POST 请求 `/tts`：

```
POST http://127.0.0.1:9880/tts
{
    "text": "要合成的文本",
    "text_lang": "zh",
    "ref_audio_path": "E:/.../reference.wav",
    "prompt_text": "参考音频对应的文本",
    "prompt_lang": "zh",
    "text_split_method": "cut5",
    "media_type": "wav",
    "streaming_mode": false
}
```

返回：WAV 音频流（HTTP 200）或错误 JSON（HTTP 400）。

### 3.2 服务启动方式

使用 GPT-SoVITS 自带 Python 3.9 运行时启动：

```powershell
E:\GPT-SoVITS-v2pro-20250604-nvidia50\runtime\python.exe `
  E:\GPT-SoVITS-v2pro-20250604-nvidia50\api_v2.py `
  -a 127.0.0.1 -p 9880 `
  -c GPT_SoVITS/configs/tts_infer.yaml
```

启动工作目录需为 `E:\GPT-SoVITS-v2pro-20250604-nvidia50`。

### 3.3 生命周期管理

- **方案 A（推荐）**：由 VPet 通过 `QProcess` 在启动时拉起 API 服务，退出时关闭。用户无感知，但需处理服务启动失败、端口占用等问题。
- **方案 B**：要求用户手动启动 API 服务，VPet 只做健康检查。实现简单，但使用门槛高。

计划采用 **方案 A**，并在配置中允许关闭自动启动以支持手动管理。

## 4. 模块设计

### 4.1 新增文件

| 文件 | 职责 |
|------|------|
| `include/vpet/tts_config.h` | TTS 配置数据结构 |
| `include/vpet/tts_client.h` | TTS 客户端抽象接口 |
| `include/vpet/tts_http_client.h` | 基于 Qt HTTP 的 GPT-SoVITS 客户端实现 |
| `include/vpet/tts_audio_cache.h` | 合成音频缓存管理 |
| `include/vpet/tts_audio_player.h` | 基于 Qt Multimedia 的音频播放 |
| `include/vpet/speech_text_provider.h` | 根据 Say 分组返回台词文本 |
| `include/vpet/speech_controller.h` | 统筹 TTS + 播放 + XAML 通知 |
| `src/tts_config.cpp` | 配置实现 |
| `src/tts_http_client.cpp` | HTTP 请求实现 |
| `src/tts_audio_cache.cpp` | 缓存实现 |
| `src/tts_audio_player.cpp` | 播放实现 |
| `src/speech_text_provider.cpp` | 台词提供实现 |
| `src/speech_controller.cpp` | 统筹实现 |
| `config/speech_texts.json` | Say 分组与候选台词配置 |

### 4.2 核心类

#### `TtsConfig`
```cpp
class TtsConfig
{
public:
    QString apiHost;          // 例如 "127.0.0.1"
    int apiPort;              // 例如 9880
    QString refAudioPath;     // 参考音频绝对路径
    QString promptText;       // 参考音频对应文本
    QString promptLang;       // 参考音频语言，如 "zh"
    QString textLang;         // 合成文本语言，如 "zh"
    QString cacheDirectory;   // 音频缓存目录
    bool autoStartServer;     // 是否自动启动 GPT-SoVITS 服务
    QString pythonExecutable; // Python 运行时路径
    QString apiScriptPath;    // api_v2.py 路径
    QString configPath;       // tts_infer.yaml 路径
};
```

#### `TtsClient`（接口）
```cpp
class TtsClient
{
public:
    virtual ~TtsClient() = default;
    virtual bool Initialize(const TtsConfig &config) = 0;
    virtual bool IsAvailable() const = 0;
    virtual void Synthesize(const QString &text,
                            std::function<void(bool success, const QString &audioPath)> callback) = 0;
};
```

#### `TtsHttpClient`
- 内部使用 `QNetworkAccessManager` 发送 POST 请求。
- 收到 WAV 流后写入 `cacheDirectory/<hash>.wav`。
- 通过回调通知 `SpeechController`。

#### `TtsAudioCache`
- 根据文本哈希生成缓存文件名。
- 提供 `bool HasCached(const QString &text)` 和 `QString GetCachePath(const QString &text)`。
- 定期清理过期缓存（可选）。

#### `TtsAudioPlayer`
- 基于 `QMediaPlayer` 播放本地 WAV 文件。
- 提供 `Play(const QString &filePath)` 和 `Stop()`。

#### `SpeechTextProvider`
- 读取 `config/speech_texts.json`。
- 结构示例：
  ```json
  {
    "say_self": ["这是我自己", "我在自言自语"],
    "say_serious": ["请认真听我说", "这件事很重要"],
    "say_shining": ["今天天气真好", "闪耀吧"],
    "say_shy": ["哎呀", "有点害羞呢"]
  }
  ```
- 方法 `QString GetText(const QString &groupName)` 随机返回一句。

#### `SpeechController`
- 持有 `TtsClient`、`TtsAudioCache`、`TtsAudioPlayer`、`SpeechTextProvider`。
- 提供槽 `OnSayStarted(const QString &groupName)`，连接 `PetController::SayStarted`。
- 流程：
  1. 通过 `SpeechTextProvider` 获取文本。
  2. 查询缓存；命中则直接播放。
  3. 未命中则调用 `TtsClient::Synthesize`；完成后播放并缓存。
  4. 同时发射信号 `ChatBubbleRequested(const QString &text, const QString &groupName)` 给 XAML 气泡。

### 4.3 Qt 侧新增信号

在 `PetController` 中新增：
```cpp
void ChatBubbleRequested(const QString &text, const QString &groupName);
```

由 `SpeechController` 发射，`MainWindow` 接收后转发给 XAML 气泡进程。

## 5. XAML 聊天气泡集成方案

### 5.1 架构选择：独立 WPF 进程 + IPC（推荐）

原因：
- 当前主程序是 Qt/MinGW，无法直接渲染 XAML。
- 独立 WPF 窗口技术成熟，与 Qt 主窗口通过 IPC 同步位置即可。
- 对现有 Qt 代码侵入最小。

### 5.2 新增项目

创建 `VpetBubble/` 目录，内含 .NET WPF 项目：
- `VpetBubble.sln`
- `MainWindow.xaml`：聊天气泡 UI（圆角、箭头指向宠物、文字换行）。
- `BubblePipeServer.cs`：命名管道服务器，监听 Qt 端命令。

### 5.3 IPC 协议（命名管道）

管道名：`VPetChatBubble`。

消息格式（JSON 行）：
```json
{"command":"show","text":"你好呀","x":1200,"y":600}
{"command":"hide"}
{"command":"move","x":1200,"y":600}
```

### 5.4 Qt 端 IPC 客户端

新增 `include/vpet/bubble_ipc_client.h` 与 `src/bubble_ipc_client.cpp`：
- 使用 `QLocalSocket` 连接 WPF 命名管道。
- 提供 `ShowBubble(const QString &text, const QPoint &position)`、`HideBubble()`、`MoveBubble(const QPoint &position)`。
- 在 `MainWindow::OnSayStarted` 或 `OnChatBubbleRequested` 中调用。

### 5.5 窗口同步

- 当宠物移动时，`MainWindow` 通过 IPC 发送 `move` 命令，WPF 气泡跟随。
- 气泡显示位置基于宠物窗口顶部上方，与现有 `m_bubbleLabel` 逻辑一致。

## 6. 工作流程

```
1. 用户启动 VPet.exe
   └─ Qt 应用初始化
      ├─ 加载动画资源
      ├─ 启动 GPT-SoVITS API 服务（QProcess，可选）
      ├─ 初始化 SpeechController（TTS 客户端、缓存、播放器）
      └─ 启动 WPF 气泡进程（QProcess，可选）

2. 状态机进入 SAYING
   └─ PetController 发射 SayStarted(groupName)
      └─ SpeechController::OnSayStarted(groupName)
         ├─ SpeechTextProvider 获取随机文本
         ├─ 查询缓存
         │   ├─ 命中 → TtsAudioPlayer 播放
         │   └─ 未命中 → TtsHttpClient 请求 GPT-SoVITS
         │         └─ 收到 WAV → 缓存 → 播放
         └─ 发射 ChatBubbleRequested(text, groupName)
            └─ MainWindow 通过 IPC 通知 WPF 显示气泡

3. 语音播放结束 / 动画结束
   └─ TtsAudioPlayer 状态变化 / 状态机回到 IDLE
      └─ MainWindow 通过 IPC 通知 WPF 隐藏气泡
```

## 7. 配置说明

新增配置文件 `config/tts_config.json`：

```json
{
  "api_host": "127.0.0.1",
  "api_port": 9880,
  "ref_audio_path": "E:/GPT-SoVITS-v2pro-20250604-nvidia50/TEMP/gradio/.../audio.wav",
  "prompt_text": "参考音频对应的文本",
  "prompt_lang": "zh",
  "text_lang": "zh",
  "cache_directory": "./cache/tts",
  "auto_start_server": true,
  "python_executable": "E:/GPT-SoVITS-v2pro-20250604-nvidia50/runtime/python.exe",
  "api_script_path": "E:/GPT-SoVITS-v2pro-20250604-nvidia50/api_v2.py",
  "config_path": "E:/GPT-SoVITS-v2pro-20250604-nvidia50/GPT_SoVITS/configs/tts_infer.yaml"
}
```

**必须由用户填写**：`ref_audio_path` 与 `prompt_text`。

## 8. 构建依赖

### 8.1 Qt 模块

在 `CMakeLists.txt` 中新增：
```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia)
```

链接：
```cmake
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Network Qt6::Multimedia)
```

### 8.2 外部依赖

- GPT-SoVITS 目录已存在，无需安装。
- WPF 气泡项目需要 .NET 6/8 SDK（Windows 平台）。

## 9. 风险与决策

| 风险 | 应对 |
|------|------|
| 参考音频缺失导致合成失败 | 计划明确需用户提供；启动时检查并给出错误提示 |
| GPT-SoVITS 服务启动慢（数秒） | 启动画面或服务状态指示；首次 Say 触发前预热 |
| 合成耗时阻塞 UI | 全程异步：HTTP 请求、文件写入、播放均使用 Qt 信号槽 |
| 缓存文件膨胀 | 限制缓存数量/总大小，定期清理 |
| XAML 气泡进程崩溃 | Qt 端 IPC 增加重连机制；崩溃后静默跳过气泡 |
| 多台本语言混合 | 初期仅支持中文；配置中预留 `text_lang` |
| GPU 占用高 | GPT-SoVITS 侧配置 `device: cuda` 或 `cpu`，由用户决定 |

## 10. 验证步骤

1. 确认 `api_v2.py` 可独立启动并通过 `curl` 返回 WAV。
2. 编译 VPet，确认 Qt Network / Multimedia 链接成功。
3. 运行 VPet，进入 SAYING，观察：
   - 缓存目录生成 `<hash>.wav`。
   - 系统音频播放出台词。
   - WPF 气泡窗口显示对应文字。
4. 多次触发同一文本，验证优先读取缓存，不再请求 API。
5. 拖拽/点击宠物，验证语音播放可被更高优先级动作打断。

## 11. 实施顺序建议

1. 搭建 `TtsConfig`、`TtsClient`、`TtsHttpClient`、`TtsAudioCache`。
2. 实现 `TtsAudioPlayer` 播放本地 WAV。
3. 实现 `SpeechTextProvider` 与 `config/speech_texts.json`。
4. 实现 `SpeechController` 并连接 `SayStarted` 信号。
5. 更新 `CMakeLists.txt` 增加 Qt Network / Multimedia。
6. 启动时通过 `QProcess` 拉起 GPT-SoVITS API 服务。
7. 端到端验证 TTS 播放。
8. （后续）创建 WPF 气泡项目与 IPC，连接 Qt 主窗口。
