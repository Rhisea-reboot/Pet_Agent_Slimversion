# VPet / Pet Agent

基于 Qt 6 的 Windows 桌面宠物，支持动画交互、语音输入、屏幕视觉感知、DAG 编排的 LLM Agent，以及 TTS 语音播报。

目标：从“会动的桌宠”升级为“能看屏幕、能听你说话、能主动搭话”的陪伴式 Agent。

---

## 功能概览

| 能力 | 说明 | 状态 |
|------|------|------|
| 桌宠动画 | PNG 序列帧、ABC 三段式动作、待机/点击/拖拽等 | 可用 |
| 气泡与说话 | 聊天气泡窗口、说话队列与优先级 | 可用 |
| 语音输入 | 全局热键录音转写，提交到 Agent | 可用 |
| 屏幕感知 | 定时截图 → 编码 → Agent 上下文 | 可用 |
| 视觉 LLM | 截图理解，生成画面摘要 | 可用 |
| 主动话题 | `proactive.topic` 根据视觉摘要和策略决定是否发话 | 可用，含冷却与摘要去重 |
| 长期记忆 | 对话记忆自动沉淀与检索、记忆管理对话框、导入/导出 | 可用 |
| 文本 LLM | 用户回复 / 主动话语生成 | 可用（需配置） |
| 情感改写 | 对话上下文下的情感标签与改写 | 部分可用 |
| TTS | GPT-SoVITS HTTP 合成与播放 | 可用（需本地服务） |

尚未完善：情绪驱动动画、生产级打扰控制、真实服务端端到端测试。

---

## 架构

```text
截图 / 语音
    │
    ▼
PerceptionPipeline / VoiceInputManager
    │
    ▼
AgentRuntime（DAG）
    user.input → web.research → memory.retrieve ────────┐
                                                        ▼
    vision.input → vision.llm → proactive.topic → llm.chat
                                                        → emotion.rewrite → output.format → memory.store
    │
    ▼
MainWindow → PetController::RequestSay(source)
    │
    ▼
TTS 合成 → 气泡 + 音频 + 说话动画
```

Agent 节点通过 `AgentContext` 交换数据，key 协议见 [AGENT_CONTEXT_KEY_PROTOCOL.md](AGENT_CONTEXT_KEY_PROTOCOL.md)。

当前默认 DAG（[agent_dag_structure.json](agent_dag_structure.json)）：

```text
user.input → web.research → memory.retrieve → llm.chat → emotion.rewrite → output.format → memory.store
vision.input → vision.llm → proactive.topic → llm.chat → emotion.rewrite → output.format → memory.store
```

触发来源：

- `runtime.trigger.type = user`：语音/文本输入
- `runtime.trigger.type = vision`：截图主动感知

最终输出携带 `semantic.output.source`：

- `user_response` → `SaySource::UserResponse`
- `vision_proactive` → `SaySource::VisionProactive`

---

## 目录结构

```text
Pet Agent/
├── Animation/                 # 桌宠动画资源
├── include/vpet/              # 公共头文件
│   ├── agent/                 # DAG 运行时、节点、上下文 key
│   ├── llm/                   # 文本/视觉 LLM 客户端
│   ├── memory/                # 长期记忆（图存储、SQLite 向量库、维护）
│   ├── perception/            # 帧缓冲、编码、感知管道
│   ├── sensor/                # 截图传感器
│   └── speech/                # 语音输入
├── src/                       # 实现与 MainWindow
│   ├── memory/                # 记忆服务实现（图、仓库、巩固、维护）
│   └── agent/                 # DAG 运行时与节点实现
├── GPT-SoVITS/                # 本地 TTS 服务（可选）
├── docs/                      # 设计与集成文档
├── agent_dag_structure.json   # Agent DAG 配置
├── llm_config.example.json    # 文本 LLM 配置模板
├── vision_llm_config.example.json
├── memory_config.example.json # 长期记忆配置模板
├── tts_config.json            # TTS 服务配置
└── CMakeLists.txt
```

---

## 环境要求

- Windows 10/11
- C++17 编译器（建议 MinGW 或 MSVC，与 Qt 套件一致）
- CMake ≥ 3.16
- Qt 6（`Core` / `Gui` / `Widgets` / `Network` / `Multimedia`）
- 可选：Python 环境 + GPT-SoVITS（TTS）
- 可选：OpenAI 兼容的文本/视觉 LLM API

---

## 构建

```bash
# 在项目根目录
cmake -S . -B build -DCMAKE_PREFIX_PATH="E:/Qt/6.x.x/mingw_64"
cmake --build build --config Debug
```

用 Qt Creator 打开 `CMakeLists.txt` 亦可。可执行目标名：`VPet`。

## 发行打包

使用 `packaging/Build-Release.ps1` 创建精简发行目录，并在已安装 Inno Setup 时生成安装程序。`QtPrefix` 是 Qt 套件目录，例如 `E:\Qt\6.9.2\mingw_64`；脚本按“显式参数 → PATH → 同一 Qt 根目录下的 `Tools/`”顺序查找 CMake、Ninja 与 MinGW。

```powershell
.\packaging\Build-Release.ps1 -QtPrefix E:\Qt\6.9.2\mingw_64
```

非标准安装或 CI 环境可显式传入工具路径，避免依赖 Qt Online Installer 的目录结构：

```powershell
.\packaging\Build-Release.ps1 `
  -QtPrefix D:\SDK\Qt\6.9.2\mingw_64 `
  -CMakePath C:\Tools\CMake\bin\cmake.exe `
  -NinjaPath C:\Tools\Ninja\ninja.exe `
  -MinGWBin C:\Tools\mingw64\bin
```

`build/`、安装包和发行目录均为生成物，已由 `.gitignore` 忽略；`packaging/` 下的脚本与 Inno Setup 配置应提交版本控制。

## 测试

Windows 下直接运行 `ctest` 时，若 Qt 的 `bin/` 目录不在 `PATH`，测试进程会以
`0xc0000135` 启动失败。使用项目脚本可从 `CMakeCache.txt` 推导 Qt 与匹配的 MinGW
运行时目录，并只为测试子进程补齐 `PATH`：

```powershell
.\scripts\Run-Tests.ps1
```

首次配置或使用其他构建目录时，显式指定 Qt 安装前缀：

```powershell
.\scripts\Run-Tests.ps1 -BuildDirectory build-clean -QtPrefix E:\Qt\6.9.2\mingw_64
```

脚本会构建 `Debug` 目标后执行全部 CTest。传入 `-SkipBuild` 可只运行已构建的测试。

运行时需能找到：

1. `Animation/` 动画目录（可执行文件旁，或项目根目录）
2. `agent_dag_structure.json`
3. `llm_config.json`（文本对话/主动发话）
4. `vision_llm_config.json`（截图理解）
5. `memory_config.json`（长期记忆，可选）
6. `tts_config.json`（TTS，可选）

配置查找路径通常包括：可执行文件目录、当前工作目录、上级目录。

---

## 配置

### 1. 文本 LLM

```bash
copy llm_config.example.json llm_config.json
```

编辑 `llm_config.json`：

```json
{
  "base_url": "https://api.openai.com/v1",
  "api_key": "YOUR_API_KEY",
  "model": "gpt-4o-mini",
  "timeout_ms": 30000
}
```

支持 OpenAI 兼容接口（改 `base_url` / `model` 即可）。

### 2. 视觉 LLM

```bash
copy vision_llm_config.example.json vision_llm_config.json
```

编辑 API Key、模型与 `default_prompt`。右键桌宠可切换图像识别模型档位（`mimo-v2.5` / `gpt`），切换作用于 `AgentRuntime` 内部视觉客户端。

### 3. Agent DAG

默认使用根目录 `agent_dag_structure.json`。可参考 `agent_dag_structure.example.json` 调整节点与边。

默认 DAG 使用两个 trigger source：`user.input` 声明 `trigger=user`，`vision.input` 声明 `trigger=vision`。Runtime 会按触发来源裁剪可达子图；活动 invocation 期间用户输入进入 FIFO，视觉输入采用 latest-wins 替换等待队列中的旧帧。

#### DAG 修改方式

编辑根目录下的 `agent_dag_structure.json`，修改后重启程序生效。建议先复制并参考 `agent_dag_structure.example.json`。DAG 配置由 `nodes` 和 `edges` 两部分组成：

```json
{
  "nodes": [
    {
      "id": "user_input",
      "type": "user.input",
      "config": { "trigger": "user" }
    },
    {
      "id": "web_research",
      "type": "web.research",
      "config": { "mode": "auto", "failure_policy": "continue" }
    },
    {
      "id": "call_llm",
      "type": "llm.chat",
      "config": {}
    },
    {
      "id": "format_output",
      "type": "output.format",
      "config": {}
    }
  ],
  "edges": [
    { "from": "user_input", "to": "web_research" },
    { "from": "web_research", "to": "call_llm" },
    { "from": "call_llm", "to": "format_output" }
  ]
}
```

修改规则：

- `id` 在整个配置中必须唯一，边中的 `from` 和 `to` 必须引用已定义的节点 ID。
- `type` 必须是下方“可用模块”中的类型，否则 Runtime 会在执行时报告未注册处理器。
- 边表示执行依赖，不表示数据字段映射；节点通过 `semantic.*` 上下文 key 交换数据。
- 图必须是 DAG，不能包含环；Runtime 会在加载时执行拓扑校验。
- 没有 `config.trigger` 的源节点会参与所有触发类型；声明 `trigger=user` 或 `trigger=vision` 后，只在对应入口触发。
- 用户输入入口应连接到需要文本输入的下游节点；视觉输入入口应连接到 `vision.llm`。
- 多个父节点汇入同一个节点时会触发 Join 合并；存在相同 key 的不同值时必须在节点配置中提供合并策略，否则本轮执行失败。
- 修改配置后必须重启程序；当前不会动态热加载 DAG。

常见组合：

```text
仅文本对话：user.input → llm.chat → output.format
显式联网：user.input → web.research → llm.chat → output.format
视觉主动发话：vision.input → vision.llm → proactive.topic → llm.chat → output.format
带情感改写：user.input → llm.chat → emotion.rewrite → output.format
带长期记忆：user.input → web.research → memory.retrieve → llm.chat → emotion.rewrite → output.format → memory.store
完整默认链路：user.input → web.research → memory.retrieve → llm.chat → emotion.rewrite → output.format → memory.store（用户触发）；vision.input → vision.llm → proactive.topic → llm.chat → emotion.rewrite → output.format → memory.store（视觉触发）
```

#### 可用模块

| 模块类型 | 作用 | 主要输入 | 主要输出与配置 |
|---|---|---|---|
| `user.input` | 用户输入触发源 | `user.input` | 设置 `trigger: "user"`；通常作为文本链路源节点 |
| `vision.input` | 视觉输入触发源 | 最新截图、帧尺寸、帧 ID | 设置 `trigger: "vision"`；写入 `semantic.image.*` 和 `semantic.vision.*` |
| `vision.llm` | 调用视觉 LLM 生成屏幕摘要 | `semantic.image.base64`、媒体类型和尺寸 | 输出 `semantic.vision.summary`；可配置 `prompt` |
| `proactive.topic` | 判断是否允许主动发话并组装提示词 | `semantic.vision.summary` | 输出 `semantic.proactive.*`、`semantic.text.prompt`；可配置 `enabled`、`instruction`、`min_interval_ms`、`dedup_window_ms` |
| `web.research` | 受预算限制的联网研究 | `semantic.text.prompt` | 输出 invocation-local 的 `semantic.web.research.*` 并重组 `semantic.text.prompt`；默认 `mode=auto`（按检索决策规则判断，`/search` 等显式触发词始终强制检索），失败策略为 `continue` |
| `memory.retrieve` | 检索相关记忆并注入提示词 | `semantic.text.prompt` | 输出 `semantic.memory.retrieval`；异步非阻塞（结果下一轮可用），embedding 未启用时退化为关键词检索 |
| `memory.store` | 解析记忆命令并沉淀本轮对话 | `semantic.text.final`、`conversation.history` | 识别“记住/忘记/以后不要/更正”等命令写入记忆；无命令时由巩固逻辑决定是否提取 |
| `llm.chat` | 调用文本 LLM | `semantic.text.prompt` | 输出 `semantic.text.response`；可配置 `temperature`（0-2）、`top_p`（0-1）、`frequency_penalty`/`presence_penalty`（-2 到 2）、`max_tokens`（1-32768），缺省时使用客户端默认值，越界值会明确报错 |
| `emotion.rewrite` | 根据对话上下文总结情绪并改写回复 | `semantic.text.response`、`conversation.history` | 输出情绪标签和改写后的 `semantic.text.response`；无历史时可透传 |
| `output.format` | 生成最终输出并维护对话历史 | `semantic.text.response` | 输出 `semantic.text.final`、`semantic.output.source`；无输出且主动策略拒绝时静默结束 |

主动话题节点示例：

```json
{
  "id": "proactive_topic",
  "type": "proactive.topic",
  "config": {
    "enabled": true,
    "min_interval_ms": 30000,
    "dedup_window_ms": 300000,
    "instruction": "请根据画面中最值得关注的新内容，以桌宠口吻自然地说一句简短中文。"
  }
}
```

其中：

- `enabled=false`：关闭主动发话，但节点仍会正常结束。
- `min_interval_ms`：两次视觉主动输出之间的最短间隔，单位为毫秒。
- `dedup_window_ms`：相同视觉摘要指纹的抑制窗口，单位为毫秒。
- 抑制原因会写入 `semantic.proactive.reason`，常见值为 `cooldown`、`duplicate_summary`、`disabled` 和 `vision_summary_missing`。

#### 节点插入原则

新增模块时，应优先读写以下公共语义 key：

```text
semantic.text.prompt       文本提示词
semantic.text.response     中间回复
semantic.text.final        最终回复
semantic.vision.summary    视觉摘要
semantic.proactive.topic   主动话题
semantic.memory.retrieval  本轮检索到的相关记忆
conversation.history      对话历史
```

不要让新节点直接依赖某个上游节点的私有 key，例如只读取 `llm.last_response`。完整 key 约定见 [AGENT_CONTEXT_KEY_PROTOCOL.md](AGENT_CONTEXT_KEY_PROTOCOL.md)。

### 4. TTS

编辑 `tts_config.json`（服务地址、参考音频、语种等）。启动时会尝试拉起本地 GPT-SoVITS；失败时桌宠仍可运行，说话可能退化为仅文字气泡。

### 5. 长期记忆

```bash
copy memory_config.example.json memory_config.json
```

编辑 `memory_config.json` 可控制记忆系统：

| 配置项 | 默认值 | 含义 |
|---|---|---|
| `enabled` | `true` | 是否启用长期记忆（关闭时 DAG 记忆节点空转，不影响对话） |
| `max_results` | `6` | 每轮最多注入 LLM 的相关记忆条数 |
| `prompt_budget_chars` | `1200` | 注入记忆的提示词预算上限 |
| `default_scope` | `pet` | 默认记忆作用域（`pet` / `global`） |
| `automatic_extraction` | `false` | 是否自动从对话中提取记忆（开启时每轮调用 `llm.chat` 巩固） |
| `consolidation_max_candidates` | `4` | 单轮巩固最多候选记忆数 |
| `maintenance` | 见模板 | 衰减间隔、检索触发维护、深度维护（重复合并/弱记忆剪枝/簇重组）等调度参数 |
| `embedding.enabled` | `false` | 是否启用本地向量检索；未启用时 `memory.retrieve` 退化为关键词检索 |
| `embedding.backend` | `local_onnx` | 仅支持本地 ONNX（`BAAI/bge-small-zh-v1.5`，模型文件放 `models/embedding/`，向量存 SQLite） |

记忆完全本地存储（记忆图 `graph.json` 与 SQLite 向量库位于应用数据目录），不依赖外部服务。

### 6. 提示词修改指南

提示词按存放位置分两类：**改配置文件即可**（重启生效）和**改 C++ 源码**（需重新构建）。

#### 改配置文件即可

| 提示词 | 位置 | 说明 |
|---|---|---|
| 视觉识别提示词 | `agent_dag_structure.json` → `vision_llm` 节点 `config.prompt` | 非空时优先于 `vision_llm_config.json` 的 `default_prompt`；两者都为空时不发提示词 |
| 视觉识别默认提示词 | `vision_llm_config.json` → `default_prompt` | 仅当 DAG 节点未配置 `prompt` 时生效 |
| 主动话题提示词 | `agent_dag_structure.json` → `proactive_topic` 节点 `config.instruction` | 约束文本 LLM 如何根据画面摘要生成主动话语 |
| 文本 LLM 系统提示词（桌宠人设） | `context.md` | 与 `llm_config.json` 同目录，或可执行文件目录 / 当前工作目录（按此顺序查找）；文件不存在或为空时，LLM 请求不带 system 消息 |

示例：修改桌宠人设只需在项目根目录创建/编辑 `context.md`，内容即整体系统提示词，改完重启程序。

#### 需改 C++ 源码（硬编码）

| 提示词 | 位置 |
|---|---|
| 情感改写提示词 | `src/agent/emotion_rewrite_node.cpp` 的 `BuildPrompt`（约 218-232 行） |
| 主动话题包装模板（"当前画面摘要…只输出最终要说的话"） | `src/agent/proactive_topic_node.cpp` 的 `BuildPrompt`（约 187-193 行） |
| 联网研究结果组装 / 失败降级提示词 | `src/agent/web_research_node.cpp` 的 `WriteResearchPrompt` |
| 视觉 LLM 的 system 消息（MiMo 档位） | `src/llm/vision_llm_client.cpp`（约 253-257 行） |

注意：

- 情感改写的提示词强制要求输出严格 JSON（字段 `user_emotion`、`pet_emotion`、`rewrite`），修改时不要破坏该格式，否则下游解析失败。
- 联网研究的降级提示词包含"不得伪装已联网验证"的安全约束，修改时建议保留。
- 修改 C++ 源码后需重新构建；修改 JSON / `context.md` 后重启程序即可。

---

## 使用

### 1. 首次使用前准备

按需准备以下文件（详见[配置](#配置)章节）：

| 文件 | 用途 | 必需 |
|---|---|---|
| `llm_config.json` | 文本 LLM（对话/主动发话） | 是（无则对话不可用） |
| `vision_llm_config.json` | 视觉 LLM（截图理解） | 屏幕感知需要 |
| `web_search_config.json` | 联网研究（配合本地 daemon） | 联网研究需要 |
| `memory_config.json` | 长期记忆（含维护/embedding 配置） | 可选，缺失时记忆节点空转 |
| `tts_config.json` + GPT-SoVITS 环境 | 语音播报 | 可选，缺失时退化为文字气泡 |
| `context.md` | 桌宠人设（系统提示词） | 可选 |

可选能力：

- **联网研究**：先启动本地 `open-webSearch` daemon（默认 `127.0.0.1:3210`，详见 `vendor/open-webSearch/README.md`），再将 `web_search_config.example.json` 复制为 `web_search_config.json`。daemon 未启动时 Agent 会自动降级为普通对话。
- **TTS 播报**：应用启动时自动尝试拉起本地 GPT-SoVITS 服务（`127.0.0.1:9880`）并健康检查；也可先手动运行 `start_tts_server.bat`。

### 2. 启动

构建（见[构建](#构建)）后运行 `VPet`：

1. 出现启动画面，等待 Agent/TTS 初始化（TTS 健康检查最多约 36 秒，失败或超时会自动跳过）
2. 桌宠出现在屏幕上，待机动画随机走动或发呆

### 3. 基础交互

| 操作 | 效果 |
|---|---|
| 左键按下后拖动（超过 5 像素） | 提起/拖拽桌宠，松手后落回；拖动期间播放 Raise 动画并显示气泡 |
| 点击头部 | 摸头动画（A→B→C 完整播放） |
| 点击身体 | 摸身体动画（A→B→C 完整播放） |
| 右键 | 打开菜单（见下） |

动画播放遵循优先级：`拖拽 > 触摸 > 行走 > 待机`，高优先级操作会打断低优先级动画。

右键菜单包含：

- **Ctrl+Alt+V 语音输入**：等同于快捷键，见[语音输入](#4-语音输入)
- **屏幕感知（截图）**：复选框，开关屏幕感知（默认关闭）
- **图像识别模型设置**：`mimo-v2.5` / `gpt` 档位切换（仅当视觉 LLM 已配置时出现）
- **退出程序**：停止录音和屏幕感知管道后正常退出应用

程序启动后会在 Windows 系统托盘显示 VPet 图标。双击托盘图标或选择托盘菜单中的
**显示桌宠**可以重新显示并激活桌宠窗口；托盘菜单中的 **退出程序** 与右键菜单使用相同的退出流程。

### 4. 语音输入

1. 按 `Ctrl+Alt+V`（或右键菜单"语音输入"）开始录音，再次按下结束
2. 录音自动经 GPT-SoVITS 环境中的 ASR 脚本转写为文字（需要本地 `GPT-SoVITS/` 目录）
3. 转写结果作为用户输入提交给 Agent DAG，回复通过气泡/TTS 输出

### 5. 屏幕感知与主动发话

1. 右键菜单勾选 **屏幕感知（截图）** 开启（隐私 opt-in，默认关闭；截图仅存内存，不落盘）
2. 约每 3 秒截取一屏，经 `vision.llm` 生成画面摘要
3. 摘要进入 `proactive.topic` 节点，满足以下条件才主动说话：
   - 距上次主动发话超过 `min_interval_ms`（默认 30 秒）
   - 画面摘要与过去 `dedup_window_ms`（默认 5 分钟）内的摘要指纹不同
4. 满足时以桌宠口吻说一句话，来源标记为 `vision_proactive`

### 6. 联网研究

- 在对话中提及需要实时信息的提问，或直接以 `/search` 开头，会触发 `web.research` 节点联网检索
- 默认最多 3 轮检索、每轮 2 个 query、总预算 15 秒；结果附来源
- daemon 不可用或检索失败时按 `failure_policy=continue` 降级为普通对话（回答中会说明未联网）

### 7. 长期记忆

1. 记忆随对话自动沉淀：每轮输出经 `memory.store` 异步写入，`memory.retrieve` 在下一轮注入相关记忆（LLM 不等待，结果延迟一轮可用）
2. 也可直接用对话命令管理记忆：

| 命令示例 | 效果 |
|---|---|
| “记住我不喜欢辣” / “帮我记住…” | 写入事实/偏好记忆（带“不要/不喜欢”等措辞时归为负面记忆） |
| “以后不要再提这件事” | 写入负面记忆，之后相关话题自动避开 |
| “更正一下：我喜欢的是猫” | 写入更正记忆 |
| “记住流程：点右键；选择退出程序” | 写入流程记忆（分号分隔步骤） |
| “忘记…” / “删除记忆…” | 按关键词删除记忆 |

3. 右键桌宠 → **长期记忆**：打开管理对话框，可浏览/编辑/删除记忆、导入/导出记忆图，并可对“这次记忆有帮助/无帮助”反馈以调整记忆权重
4. 记忆数据位置：应用数据目录（记忆图 `graph.json`、SQLite 向量库），随应用退出 flush 落盘

### 8. 常见调整（常用入口速查）

| 想调整什么 | 改哪里 |
|---|---|
| 桌宠性格/人设 | 根目录 `context.md` |
| 主动说话的频率/内容 | `agent_dag_structure.json` 的 `proactive_topic` 节点（`enabled` / `min_interval_ms` / `dedup_window_ms` / `instruction`） |
| 视觉识别描述方式 | 同一文件 `vision_llm` 节点的 `config.prompt` |
| 对话温度/长度 | 同一文件 `call_llm` 节点（`temperature` / `max_tokens` 等） |
| 联网检索强度 | 同一文件 `web_research` 节点（轮数/query 数/预算） |
| 记忆检索量/注入预算 | `memory_config.json`（`max_results` / `prompt_budget_chars`） |
| 本地向量检索开关 | `memory_config.json` → `embedding.enabled`（需 `models/embedding/` 模型） |
| 文本/视觉模型与 API Key | `llm_config.json` / `vision_llm_config.json` |
| TTS 音色与参考音频 | `tts_config.json` |

所有 JSON 修改重启程序生效。另见[提示词修改指南](#6-提示词修改指南)。

### 9. 退出

可以通过桌宠右键菜单或 Windows 系统托盘菜单中的 **退出程序** 正常退出。退出流程会先停止
正在进行的语音录制和屏幕感知管道，然后请求 Qt 事件循环结束；`main.cpp` 的收尾逻辑会继续停止
随进程拉起的 TTS 服务。托盘图标会在窗口销毁前隐藏，避免通知区域残留图标。

---

## Agent 数据约定（摘要）

跨节点优先使用语义层 key：

| Key | 含义 |
|-----|------|
| `semantic.vision.summary` | 画面摘要 |
| `semantic.proactive.should_speak` | 是否主动发话 |
| `semantic.text.prompt` | 文本 LLM 提示词 |
| `semantic.text.response` | 中间回复 |
| `semantic.text.final` | 最终回复 |
| `semantic.memory.retrieval` | 本轮注入的相关记忆（memory.retrieve 输出） |
| `semantic.output.source` | `user_response` / `vision_proactive` |
| `semantic.web.research.*` | 当前 invocation 的研究计划、证据、冲突、引用和状态；不跨轮持久化 |
| `runtime.trigger.type` | `user` / `vision` |

一轮结束后会清理本轮 `user.input`、提示词端口和触发类型；`conversation.history` 持久保留。主动发话历史可只记录 `assistant:`，不伪造用户输入。

完整协议：[AGENT_CONTEXT_KEY_PROTOCOL.md](AGENT_CONTEXT_KEY_PROTOCOL.md)

---

## 当前限制

- 长期记忆的向量检索默认关闭（`embedding.enabled=false`，需本地 ONNX 模型 `BAAI/bge-small-zh-v1.5`），未配置时检索退化为关键词匹配
- 主动策略目前提供固定冷却和摘要指纹去重，尚未接入用户忙碌、语音播放和专注模式
- 视觉帧按编码内容 hash 去重，但尚未实现感知级相似度检测
- 情感标签尚未驱动桌宠动画状态
- 文本 LLM 使用本地 `llm_config.json`；示例模板为 `llm_config.example.json`，真实配置不会提交到 Git
- `FRAMEWORK.md` 中的完整 `IModule`/`Agent` 模块中心仍在演进，当前主路径是 `MainWindow` + `AgentRuntime`

---

## 相关文档

| 文档 | 内容 |
|------|------|
| [AGENT_CONTEXT_KEY_PROTOCOL.md](AGENT_CONTEXT_KEY_PROTOCOL.md) | Agent 上下文 key 协议 |
| [FRAMEWORK.md](FRAMEWORK.md) | 视觉感知框架设计 |
| [DEVELOPMENT_LOG.md](DEVELOPMENT_LOG.md) | 开发日志 |
| [vpet.md](vpet.md) | 动画与交互设计 |
| [Structure.md](Structure.md) | Agent DAG 设计理念 |
| [PROJECT_DEVELOPMENT_REPORT.md](PROJECT_DEVELOPMENT_REPORT.md) | 项目开发报告 |
| [docs/memory_architecture.md](docs/memory_architecture.md) | 长期记忆架构设计 |
| [docs/long_term_memory_implementation_plan.md](docs/long_term_memory_implementation_plan.md) | 长期记忆实施计划与进度 |
| [docs/tts_integration_plan.md](docs/tts_integration_plan.md) | TTS 集成说明 |
| [docs/open-websearch-local-deployment.md](docs/open-websearch-local-deployment.md) | 联网搜索本地部署 |
| [docs/release_slimming_plan.md](docs/release_slimming_plan.md) | 精简发行计划 |

---

## 许可证

以仓库内实际声明为准。第三方组件（如 GPT-SoVITS、Qt）遵循各自许可证。
