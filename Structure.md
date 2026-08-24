# Pet Agent 当前架构

## 核心目标

Pet Agent 是一个基于 Qt 6 / C++17 的 Windows 桌面宠物应用。动画、鼠标交互、语音输入、屏幕感知、联网研究、文本/视觉 LLM 和 TTS 由应用内模块协作完成，不依赖一个单独的聊天服务进程。

Agent 的核心职责是把用户输入和屏幕感知转换为可追踪的 DAG invocation，并将最终文本通过 `AgentOutputReady` 返回给 UI。TTS 和桌宠动画仍由 UI/控制器层消费输出信号。

## DAG 模型

配置文件使用 JSON，节点是带 `id`、`type`、`config` 的对象，边只表达依赖关系：

```json
{
  "nodes": [
    { "id": "call_llm", "type": "llm.chat", "config": { "temperature": 0.7 } }
  ],
  "edges": [
    { "from": "user_input", "to": "call_llm" }
  ]
}
```

`AgentDagGraph` 负责 JSON 解析、节点 ID 校验、边引用校验、DAG 环检测和拓扑序计算。运行时通过快照热重载支持不中断的配置更新（见下文「DAG 热重载」）；`AgentDagValidator` 提供更细粒度的错误/警告诊断，供编辑器与保存流程使用。

节点的执行逻辑通过 `AgentNodeRegistry` 按 `type` 注册和分发。节点之间不应依赖另一个节点的私有状态，而应使用 `semantic.*` 或 `node.input.*`/`node.output.*` 协议，完整约定见 [AGENT_CONTEXT_KEY_PROTOCOL.md](AGENT_CONTEXT_KEY_PROTOCOL.md)。

## 当前运行时

```text
MainWindow
    |
    +-- user text / ASR --> AgentRuntime
    |
    +-- PerceptionPipeline --> AgentRuntime

AgentRuntime
    +-- AgentGraphExecutor       DAG、触发源裁剪、分支和 Join
    +-- AgentNodeRegistry         节点类型处理器
    +-- AgentAsyncBridge          text / vision / web request 关联
    +-- InvocationQueuePolicy     user FIFO、vision latest-wins
    +-- session AgentContext      只提交允许持久化的会话历史
    +-- LlmClient                 文本 LLM
    +-- VisionLlmClient           视觉 LLM
    +-- WebResearchEngine         受预算限制的联网研究
```

运行时同一时刻只执行一个 invocation。节点同步完成后继续推进 ready queue；异步节点把独立 context、invocation ID、分支、节点类型和 request ID 登记到 `AgentAsyncBridge`，回调验证复合关联后恢复原分支。

每个 invocation 以 session context 为基座。分支只保存本轮增量，Join 默认拒绝相同 key 的冲突写入；可通过 `config.merge` 为指定 key 使用 `prefer_user`、`prefer_vision` 或 `concat`。失败 invocation 不提交本轮结果，成功 invocation 当前只持久化允许的 `conversation.history`。

## DAG 热重载

运行时持有 `std::shared_ptr<const AgentDagGraph>` 快照。`AgentRuntime::RequestDagReload` 从磁盘构建新快照并调用 `AgentGraphExecutor::ReplaceGraphIfIdle`：

- 空闲（无进行中的 invocation、无挂起异步回调、队列非空时也不打断）：立即应用；
- 忙碌：延迟到当前 invocation 的完成点（同步完成、异步恢复、超时或队列切换）再原子替换；
- 新快照无效：保留旧图并发出 `DagGraphReloadFailed`。

`Start()` 会用 `QFileSystemWatcher` 监视配置文件（250 ms 防抖 + SHA-256 指纹去重）。进行中的一轮永远用旧图跑完，新一轮用新图。

## DAG Web 编辑器

桌宠右键菜单的「DAG 编辑器」启动 `DagEditorServer`（懒创建）：

- **传输层**：`DagHttpServer` 基于 `QTcpServer` 实现最小 HTTP/1.1 + SSE。只绑定回环地址，端口 0 由内核分配；请求体上限 1 MB；普通请求 `Connection: close`，SSE 长连接每 15 秒发心跳。
- **安全**：随机一次性 token（启动时经 URL 打开浏览器），API 请求必须带 `X-DAG-Token` 头或 `?token=`；Host 头白名单（`127.0.0.1:port` / `localhost:port`）防御 DNS rebinding；不发 CORS 头。
- **文档层**：`DagDocumentStore` 管理格式 v1.1 文档（`version/nodes/edges/editor`），保存走 QSaveFile 原子提交 + `.bak` 备份；revision 乐观锁（PUT 带 `?baseRevision=N`，冲突返回 409 与最新文档）；外部修改通过指纹检测后 `AdoptExternalFile` 采纳为新基线。
- **REST API**：`GET /api/meta`、`GET /api/object_info`（节点 schema 目录）、`GET/PUT /api/graph`、`POST /api/graph/validate`、`POST /api/graph/reload`、`GET /api/runtime/status`、`GET /api/events`（SSE 单通道 `main`）。
- **前端**：零构建 ES5 脚本从 qrc 资源提供（`resources/dag_editor/`），litegraph.js 0.7.18（MIT，见 `THIRD_PARTY_NOTICES.md`）负责画布渲染；左侧节点库、右侧按 schema 渲染的属性面板、底部校验问题栏。左键只选中/框选/拖拽/连线，只有右键打开节点、连线或画布菜单；保存成功即触发运行时热重载并在工具栏显示结果。

## 默认 DAG

```text
user.input -> web.research -> llm.chat -> emotion.rewrite -> output.format

vision.input -> vision.llm -> proactive.topic -> llm.chat
```

用户触发源声明 `trigger=user`，视觉触发源声明 `trigger=vision`。Runtime 根据触发类型裁剪可达子图：视觉触发不会进入联网研究；用户触发不会执行视觉 LLM。多个触发链共享 `llm.chat` 时，必须为 Join 冲突配置明确合并策略，否则执行失败。

### 当前节点

| 类型 | 作用 |
|---|---|
| `user.input` | 用户触发源；由 Runtime 准备本轮文本输入 |
| `vision.input` | 检查并规范化当前感知帧 |
| `vision.llm` | 调用视觉 LLM 生成 `semantic.vision.summary` |
| `proactive.topic` | 按冷却和摘要指纹决定是否主动发话 |
| `web.research` | 在 `auto` 或显式模式下执行受预算限制的联网研究 |
| `llm.chat` | 调用文本 LLM，支持节点级采样参数校验和透传 |
| `emotion.rewrite` | 根据对话历史进行情绪分析和回复改写 |
| `output.format` | 设置最终输出、来源并维护对话历史 |

未知节点类型不会执行隐式业务逻辑，而是记录 `runtime.pass_through.<node_id>` 并继续；需要真实行为的节点应显式注册处理器。

## 联网研究边界

`web.research` 内部使用 `Decide -> Search -> Observe -> Assess -> Repeat / Compose` 状态机。默认最多 3 轮、每轮 2 个 query、累计 8 条结果、15 秒研究预算和 6000 字符 Compose 上下文。研究数据只属于当前 invocation，不写入 session context；外部标题、摘要和 URL 按不可信数据处理。

底层 `WebSearchClient` 通过本地 `open-webSearch` REST daemon 的 `POST /search` 工作。当前默认配置是回环地址 `127.0.0.1:3210` 和 Bing `request` 模式；daemon 不可用时，默认 `failure_policy=continue` 会将研究状态和限制说明写入提示词后继续普通对话。

## 配置文件

- `agent_dag_structure.json`：默认 DAG，运行时读取。
- `agent_dag_structure.example.json`：可复制的 DAG 示例。
- `llm_config.example.json`：文本 LLM 模板；真实 `llm_config.json` 被 Git 忽略。
- `vision_llm_config.example.json`：视觉 LLM 模板；真实 `vision_llm_config.json` 被 Git 忽略。
- `web_search_config.example.json`：本地搜索 daemon 客户端模板；真实 `web_search_config.json` 被 Git 忽略。
- `tts_config.json`：本地 GPT-SoVITS 服务配置，部署时应检查其中的路径和参考音频。

应用启动时会自动查找上述真实配置文件。缺少 LLM、视觉 LLM 或搜索配置只记录 warning，对应能力不可用或降级，不阻塞桌宠主窗口启动。

## 测试与验证

CTest 覆盖：DAG 图、节点目录、文档格式、校验器、热重载、文档存储、HTTP/编辑器服务（含 SSE 与鉴权）、Runtime 调度、应用集成、搜索客户端、研究引擎和 daemon 集成测试。Windows 命令行环境应使用：

```powershell
.\scripts\Run-Tests.ps1
```

编辑器手工冒烟验证可用 `scripts\smoke_dag_editor.ps1`（校验构建产物、前端资源，并对运行中的编辑器 URL 逐项探测 API）。

该脚本会从 `CMakeCache.txt` 推导 Qt 和匹配 MinGW 运行时目录，补齐测试子进程的 `PATH`，然后构建 Debug 目标并运行 CTest。daemon 集成测试在 daemon 不可达时使用 `QSKIP`，不把本地服务作为所有开发者的硬前置条件。
