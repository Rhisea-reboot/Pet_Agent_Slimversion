# 工具调度层（tool.loop）构建方案

日期：2026-09-04
状态：阶段 1–4 已代码实现与本地自动验证完成（阶段 5 另议不实现）；默认 DAG 保持不变，example DAG 提供变体；真实 API 与外发手工验收未做。
参考基线：[pi-agent-core@0.73.1](https://github.com/earendil-works/pi)（MIT，`@mariozechner/pi-agent-core`，2026-09-04 经 npm registry 核实）与现有架构文档 [AGENT_CONTEXT_KEY_PROTOCOL.md](../AGENT_CONTEXT_KEY_PROTOCOL.md)、[Structure.md](../Structure.md)、[docs/long_term_memory_implementation_plan.md](long_term_memory_implementation_plan.md)。

---

## 1. 背景与目标

桌宠将升级为「轻量工作助手」。当前 Agent 的联网与否靠提示词规则猜测（`web.research` 的 `mode=auto` 检索决策），能力边界由固定 DAG 决定，无法由 LLM 自主组合工具。

目标：在**不替换用户自定义 DAG** 的前提下，新增一层「LLM 动态决定工具调用」的微编排。分层原则：

- **宏编排（保持不变）**：DAG 决定一轮的骨架，用户通过 DAG 编辑器完全掌控；
- **微编排（本次新增）**：`tool.loop` 节点内部，LLM 通过 function calling 决定调哪个工具、传什么参数、如何解读结果；
- **纪律层（本次新增）**：`ToolScheduler` 统一执行预算、超时、取消、权限门控与审计。

控制权划分：**人定边界（DAG 的 `allowed_tools`/预算）、模型选路径（LLM 工具调用）、调度器管纪律（ToolScheduler）**。

技术路线：纯 C++ 实现，不引入 Node sidecar（理由见 [docs/tool_scheduling_decision.md] 讨论纪要，核心是工具引力在 C++ 侧、双 LLM 栈与分发倒退）；pi 仅作**设计参考**，其 loop 状态机、工具契约与钩子机制直接借鉴。

## 2. 参考基线：pi-agent-core 设计要点与映射

pi-agent-core 提供有状态的 Agent 循环：`AgentTool[]` + 消息数组 + 事件流，核心机制与本方案的对应关系：

| pi 机制（版本 0.73.1） | 本方案对应 | 处理 |
|---|---|---|
| `AgentTool`：`{name, label, description, parameters(JSON Schema), execute(id, params, signal, onUpdate) → {content, details, terminate}}` | `ITool` + `_tagToolSpec` + `_tagToolExecutionResult` | 采纳，C++ 化 |
| 工具**错误即结果**：throw → 循环捕获为 `isError` 工具结果回喂 LLM | 错误以结构化结果返回，`ok=false` 同样回喂 | 采纳（保证 LLM 能解释失败） |
| `beforeToolCall → {block, reason}`：参数校验后、执行前门控 | `ToolScheduler::Gate()` 权限门控，拒绝同样回喂错误结果 | 采纳，权限分级见 §4 |
| `afterToolCall → {content?, details?, isError?, terminate?}`：执行后审计钩子 | 审计 trace（`semantic.tool.calls`） | 采纳语义，合并进 scheduler |
| `shouldStopAfterTurn`：每轮结束的提前收束检查点 | 预算检查点（`max_rounds` / `time_budget_ms`） | 采纳，本方案注入预算 |
| `terminate: true` 提示：工具结果请求提前收束 | `_tagToolExecutionResult.terminateHint` | 采纳 |
| `getSteeringMessages` / `getFollowUpMessages`：轮间注入 | `SteeringProvider` 钩子位 | v1 空实现（沿用现有 invocation 队列），v2 实现 mid-loop steering |
| `transformContext → convertToLlm`：LLM 调用边界前的消息转换 | 每轮 LLM 调用前的消息组装（system + history + 注入 + 本轮转写） | 采纳为「消息组装边界」 |
| AgentMessage / CustomAgentMessages 声明合并 | 不做自定义消息类型；v1 仅 LLM 三种 role + 转写结构 | 简化 |
| `AgentEvent` 事件流（`turn_start` / `tool_execution_*` 等） | v1 只写 trace 不冒泡；v2 可选桥接气泡 | 简化 |
| `toolExecution: parallel/sequential` | v1 一律 `Sequential` 单飞（对齐 `PerceptionPipeline` 单飞模式） | 简化 |
| pi-ai 多 provider 抽象、thinkingLevel、streamFn/proxy | 不采纳（本方案只服务 OpenAI 兼容端点，`llm_client` 已有） | 排除 |

pi 源码复现（供实现期对照）：`curl -sSL --ssl-no-revoke https://registry.npmjs.org/@mariozechner/pi-agent-core/-/pi-agent-core-0.73.1.tgz`，重点读 `dist/types.d.ts`（工具契约/钩子签名）与 `dist/agent-loop.js`（循环状态机）。

## 3. 总体架构

```text
DAG（宏编排，用户可编辑）
  user.input → tool_loop → emotion.rewrite → output.format → memory.store
                    │
                    ▼
ToolLoopExecutor（微编排，节点内部）
  组装消息（system + history + memory/research 注入）
  loop:
    1. LLM 调用（带 tools 声明）            ← llm_client function calling
    2. 无 tool_calls → 最终回复 → semantic.text.response，退出
    3. 预算耗尽 → 强制收束（最后一次总结调用或降级）
    4. 顺序执行本轮 tool_calls：
       Gate 权限 → ValidateArguments → ITool::Execute → 结果回喂
    5. trace 记录 → 回到 1
                    │
                    ▼
ToolScheduler（纪律层）
  预算检查 · 超时 watchdog · 取消传播 · 权限门控 · 审计
                    │
                    ▼
ToolRegistry（工具表）→ ITool 实现：
  web.search（包 WebSearchClient/WebSearchTool）
  memory.search（包 MemoryService 检索）
  screen.describe（包 ScreenshotSensor + VisionLlmClient，返回文本摘要）
```

## 4. 组件设计

### 4.1 `ITool`（新增 `include/vpet/agent/tools/itool.h`）

```cpp
enum class ToolTrustTier { ReadOnly, SideEffect };   // v1 权限分级
enum class ToolExecutionMode { Sequential, Parallel }; // v1 只用 Sequential

struct _tagToolParameterSchema {
    QString name;
    QString type;          // "string"|"integer"|"boolean"|"array"（v1 子集）
    QString description;   // 喂给 LLM
    bool required = false;
    QStringList enumValues;
};

struct _tagToolSpec {
    QString name;                    // 全局唯一，如 "web.search"
    QString label;                   // UI 展示名（DAG 编辑器）
    QString description;             // 能力描述，喂给 LLM
    ToolTrustTier trustTier = ToolTrustTier::ReadOnly;
    QVector<_tagToolParameterSchema> parameters;
};

struct _tagToolExecutionResult {
    bool ok = false;
    QString textOutput;              // 回喂 LLM 的文本（成功内容或脱敏错误）
    QJsonObject details;             // 审计信息，不进 LLM（对齐 pi 的 details）
    bool terminateHint = false;      // 对齐 pi 的 terminate 提示
};

class ITool : public QObject {
    Q_OBJECT
public:
    explicit ITool(QObject *parent = nullptr);
    virtual _tagToolSpec Spec() const = 0;
    virtual bool ValidateArguments(const QJsonObject &arguments,
                                   QString &errorMessage) const = 0;
    virtual void Execute(const _tagToolCall &call) = 0;  // 异步启动
    virtual void Cancel() = 0;                           // 对齐 pi 的 AbortSignal
    virtual bool IsBusy() const = 0;
signals:
    void Completed(const _tagToolExecutionResult &result);  // 对齐 WebSearchTool 模式
};
```

要点：完全对齐现有 `WebSearchTool` 的工具层形态（结构化输入输出、忙态、可取消、不碰提示词与上下文）；错误**不抛异常**，以 `ok=false` + 脱敏消息返回，由循环转成 `isError` 工具结果回喂 LLM（pi 的错误即结果契约的 C++ 化）。

### 4.2 `ToolRegistry`（`include/vpet/agent/tools/tool_registry.h`）

- `Register(std::shared_ptr<ITool>)` / `Find(name)` / `Specs()`（汇总 JSON Schema 数组喂 LLM）；
- 重名注册报错；未知工具名在循环内转成 `isError` 结果回喂（pi 行为）。

### 4.3 `ToolScheduler`（`include/vpet/agent/tools/tool_scheduler.h`）——纪律层本体

职责（均为纯逻辑、可单测）：

1. **权限门控**（对应 pi `beforeToolCall`）：`permission` 策略 × `ToolTrustTier`。v1 策略：`auto_readonly`（只读放行、副作用拒绝，拒绝原因回喂 LLM）；`allow_all`（开发档）；`ask`（v2 气泡确认，占位）。
2. **预算检查**（对应 pi `shouldStopAfterTurn`）：`max_rounds` 硬上限、`time_budget_ms` 墙钟预算、`max_tool_calls` 上限；耗尽后由循环按 `on_budget_exhausted`（`answer_with_context` 默认）收束。
3. **超时 watchdog**：单工具调用超时 → `ok=false` 结果回喂并继续（不中断整个循环）。
4. **取消传播**：invocation 被替换（vision latest-wins）或程序退出时，级联 `ITool::Cancel()` 到活动工具。
5. **审计**（对应 pi `afterToolCall`）：每条调用写 `{round, tool, status, duration_ms, args_digest}`，由循环落到 `semantic.tool.calls`。
6. **忙闲查询**：暴露 `IsGateBlocked()`，v1 只按 trustTier；拖拽中/播报中不派副作用工具接入忙闲状态组件（尚未存在时留接口，见 §9 风险 5）。

### 4.4 `ToolLoopExecutor`（`include/vpet/agent/tools/tool_loop_executor.h`）

节点内核，持有本轮消息列表（`QVector<_tagLlmMessage>`），按 §3 循环执行。设计约束：

- 每轮 LLM 调用前执行「消息组装边界」：`context.md` system + `conversation.history`（截尾）+ `semantic.text.prompt` + 记忆/研究注入；本轮转写（assistant 的 tool_calls 与 tool 结果）只存在于 invocation 内。
- 收束条件：LLM 无 tool_calls（含 `finish_reason=="stop"`）→ 最终文本写 `semantic.text.response`；预算耗尽 → 按 `on_budget_exhausted`；全部 `terminateHint` → 提前收束；abort → 失败退出。
- 中间过程不冒泡（不打扰用户）；最终回复经 `output.format` 走既有气泡/TTS 链路。

## 5. `llm_client` 的 function calling 扩展

现状：`include/vpet/llm/llm_client.h` 明确「仅处理文本 messages 请求，不处理…工具调用执行逻辑」。扩展内容：

1. `_tagLlmRequestOptions` 增加 `QJsonArray tools`（OpenAI 格式，由 `ToolRegistry::Specs()` 生成）与 `QString toolChoice`（默认 `"auto"`）；
2. `_tagLlmMessage` 增加 `QVector<_tagLlmToolCall> toolCalls`（assistant role 使用）与 `QString toolCallId`（tool role 使用）；
3. 非流式响应解析 `choices[0].message.tool_calls` 与 `finish_reason == "tool_calls"`；
4. 端点不支持 tools（4xx 且错误体含 `tools` 相关描述）→ 返回明确错误码，`tool.loop` 节点按 `fallback: "plain_chat"` 降级为普通对话并在 trace 记录——这是「模型选路径」的容错底座；
5. 头文件注释同步更新职责边界。

流式 SSE 的 tool_calls delta 分片拼装**不在本阶段做**（风险集中点，独立阶段，见 §7 阶段 5）。

## 6. DAG / 编辑器 / 上下文协议集成

### 6.1 节点类型 `tool.loop`

注册进 `AgentNodeRegistry`（`Register("tool.loop", ...)`，沿用 `web_research_node` 的异步续延模式——AgentAsyncBridge 关联键挂起/恢复，节点处理器签名不变）。示例配置：

```json
{
  "id": "tool_loop",
  "type": "tool.loop",
  "config": {
    "tools": ["web.search", "memory.search", "screen.describe"],
    "max_rounds": 4,
    "time_budget_ms": 20000,
    "permission": "auto_readonly",
    "on_budget_exhausted": "answer_with_context",
    "fallback": "plain_chat"
  }
}
```

- `tools` 引用 `ToolRegistry` 中已注册工具名；空数组等价 `llm.chat`。
- 配置校验进 `agent_dag_validator`（未知工具名、非法枚举值报错），编辑器的 `agent_node_catalog` 增加该节点类型与 schema 属性面板（现有 schema 驱动机制，常规扩展）。
- 图层面依然是无环节点图，DAG 拓扑校验与热重载**无需改动**。

### 6.2 默认 DAG

阶段 3 先在 `agent_dag_structure.example.json` 提供 tool.loop 变体（`user.input → tool_loop → emotion.rewrite → output.format → memory.store`，`web.research` 保留为可选节点）；阶段 4 端到端验收通过后再决定是否切换默认 `agent_dag_structure.json`，保证可回滚。

### 6.3 上下文协议（更新 AGENT_CONTEXT_KEY_PROTOCOL.md）

| Key | 归属 | 说明 |
|---|---|---|
| `semantic.tool.calls` | invocation-local | `QJsonArray`：`{round, tool, status, duration_ms, error?}`，轮末清理（同 `semantic.web.research.*` 待遇） |
| `semantic.tool.trace` | invocation-local | 人类可读审计日志（调试与 DAG 编辑器排障） |
| `semantic.text.prompt` / `semantic.text.response` | 不变 | 循环输入/最终输出，下游节点零感知 |

**`conversation.history` 决策**：v1 保持 `QStringList` 最终文本格式**不变**——`memory.store` 的命令解析、`emotion.rewrite` 的历史注入、主动话题去重都依赖现有格式，工具轮转写不进历史（避免破坏既有节点）。结构化历史（含工具轮）列为阶段 5 开放问题。

## 7. 分阶段实施计划

| 阶段 | 内容 | 交付物 | 验收标准 |
|---|---|---|---|
| 1 | ToolScheduler 骨架（纯逻辑） | `itool.h` / `tool_registry.h/.cpp` / `tool_scheduler.h/.cpp` + `tests/tool_scheduler_tests.cpp` + CMake 目标 | 单测覆盖六条路径：预算耗尽、超时、取消传播、权限拒绝（回喂）、错误即结果、Sequential 单飞；`ctest` 全绿 |
| 2 | `llm_client` 非流式 function calling | `_tagLlmMessage`/`_tagLlmRequestOptions` 扩展 + 解析 + `tests/llm_tool_calling_tests.cpp`（QTcpServer stub） | stub 覆盖：请求含 tools、tool_calls 解析、tool 消息回填、4xx 不支持降级、畸形 JSON |
| 3 | `tool.loop` 节点 + 集成 | `tool_loop_executor` + `tool_loop_node.cpp` + 校验器/catalog 扩展 + example DAG | `tests/tool_loop_tests.cpp`：脚本化 fake LLM 多轮 tool_calls（含预算耗尽、terminate、fallback）；现有 19 个测试零回归 |
| 4 | 原生工具 + 权限 + 文档 | `web.search` / `memory.search` / `screen.describe` 三个 ITool 实现 + 权限策略 v1 + README/协议文档更新 | 真实 API 端到端手工验收清单（含「LLM 自主决定调工具」场景）；决定默认 DAG 是否切换 |
| 5 | 流式与增强（另议） | SSE tool_calls delta 拼装 +（可选）气泡进度、（可选）mid-loop steering、web.research 工具化评估、历史结构化评估 | 独立评审 |

工期估算（单人节奏）：阶段 1-3 约 1.5–2 周；阶段 4 约 3–5 天；阶段 5 另行评估。

## 8. 测试策略

- 框架沿用 QtTest，每个测试独立 `add_executable` + `add_test`（对齐 `CMakeLists.txt:203-222` 的现有模式）；
- LLM 调用用 `QTcpServer` 本地回环 stub，脚本化响应序列——对齐现有测试「受控 fake 派生自真实类」的风格（如 `agent_runtime_scheduler_test.cpp` 的 `ControlledWebResearchEngine`）；
- fake 工具矩阵：立即成功 / 延迟超时 / 可取消挂起 / 报错 / terminate 提示；权限矩阵：只读放行、副作用拒绝；
- 真实网络与真实 API 不进 CI；阶段 4 单独列手工验收清单。

## 9. 风险与开放问题

1. **端点 tools 支持参差**（中转/兼容层常见）：`fallback: "plain_chat"` + 探测降级，trace 记录，用户可感知差异（对话仍可用）；
2. **JSON Schema 校验子集**：v1 仅支持 `type/properties/required/enum/description` 子集，超范围参数忽略并在编辑器提示；后续可换完整校验库（留接口）；
3. **提示词注入**（搜索结果进入工具结果）：v1 只有只读工具、无文件/Shell 类工具、预算硬上限；副作用工具（如写记忆）默认拒绝，属边界控制而非技术问题；
4. **多轮 token 成本与上下文膨胀**：`max_rounds` + `time_budget_ms` 兜底；消息组装边界处预留裁剪钩子（阶段 5）；
5. **忙闲状态组件尚不存在**：v1 门控仅按 trustTier；「拖拽中/播报中不派副作用工具」依赖忙闲状态组件落地后接入（scheduler 留 `IsGateBlocked()` 接口位）；
6. **模型循环不收敛**（反复调工具）：硬上限 + terminateHint + 工具描述措辞审查，trace 留证。

## 10. 明确不做的事

- 不引入 Node/pi sidecar、不替换 DAG 运行时（双核化）、不重建 agent kernel；
- 不做通用插件系统与自定义消息类型（`FRAMEWORK.md` 的 `IModule` 前车之鉴，四个具体工具跑通再谈泛化）；
- v1 不做工具并行执行、mid-loop steering、工具调用的气泡进度展示；
- `conversation.history` 结构化升级不在 v1。

## 11. 阶段 4 真实 API 手工验收清单（目前未做）

本工作区仅包含离线 fake/stub 自动化测试，不调用真实外部网络与 API。在正式生产部署或切换默认 DAG 前，必须人工执行以下真实用例：
1. **真实的 OpenAI 兼容 Function Calling 端点**：
   - 验证实际兼容端点（如 deepseek-chat、gpt-4o、qwen 等）的 `tools` 参数序列化与非流式返回解析。
   - 验证模型能够根据用户提问（如“现在几点了”或“今天天气”）自主生成 `web.search` 或其他原生工具调用。
2. **web.search 真实调用**：
   - 启动本地 `open-websearch` daemon，通过 `tool.loop` 触发实际网页搜索并验证回喂摘要真实可用。
3. **memory.search 真实调用**：
   - 真实写入包含特定偏好的记忆后，发起问题让 LLM 决定调用 `memory.search` 并验证召回结果准确注入回答。
4. **screen.describe 真实调用**：
   - 配置真实 Vision LLM API Key，在有 GUI 环境下触发截图分析，确认截屏不卡死主线程且描述符合当前屏幕。
5. **UI / 气泡与 TTS 协同验证**：
   - 验证最终回复从 `tool.loop` 输出后经 `emotion.rewrite` / `output.format` 正确进入 UI 气泡展示与 Kokoro TTS 语音播报，中间工具转写不污染聊天气泡。
