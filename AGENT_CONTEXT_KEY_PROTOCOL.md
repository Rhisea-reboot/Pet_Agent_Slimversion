# Agent Context Key Protocol

本文档定义 Agent DAG 节点通过 `AgentContext` 交换数据时必须遵守的 key 命名协议。新增节点应优先使用本文档中的语义层 key，避免直接依赖其他节点的私有实现 key。

## 命名分层

Agent 上下文 key 分为五层：

1. `semantic.*`：跨节点语义数据层，用于节点互插和自动兼容。
2. `node.input.*` / `node.output.*`：当前节点标准端口层，用于运行时桥接输入输出。
3. `<module>.*`：节点或模块私有状态层，例如 `llm.*`、`emotion.*`、`vision.*`、`output.*`。
4. `runtime.*`：AgentRuntime 内部状态层，不应被业务节点当作业务输入使用。
5. `user.*` / `input.*`：运行时输入状态层，用于记录本轮用户输入和输入可用性。

## P2 Context Lifecycle

Runtime 将上下文分为两个生命周期层：

1. `m_sessionContext` 是跨 invocation 的只读基座。当前仅 `conversation.history` 在成功完成 invocation 后提交到该基座。
2. `InvocationState::_tagBranchState::local` 是单条本轮执行线的可写增量。它只保存相对 session base 的新增或修改键；删除键单独记录。
3. 节点执行前，Runtime 构造 `view = session base + branch.local - removedKeys`。节点仍接收 `AgentContext&`，无需感知 branch。
4. 节点完成后，Runtime 将执行 view 提取为 branch local delta，并记录到 `NodeResult`。单父后继继承父 `NodeResult` 的独立副本。
5. 入度大于 1 的节点由 Runtime 合并所有父 `NodeResult`。单一来源键直接复制，相同值保留，不同值或删除/写值冲突默认失败。
6. join 节点可通过 `config.merge` 按键声明 `prefer_user`、`prefer_vision` 或 `concat`。前两者依据源节点 `config.trigger` 选择；`concat` 按父节点声明顺序以换行连接字符串。
7. `runtime.executed_nodes` 在 join 处按父声明顺序去重合并，`runtime.trigger.type` 从 InvocationState 恢复；其他 `runtime.*` 调度内部键不跨 join 传播。

示例：

```json
{
  "id": "join",
  "type": "context.merge",
  "config": {
    "merge": {
      "semantic.text.user": "prefer_user",
      "semantic.text.response": "concat"
    }
  }
}
```
8. invocation 失败时不得提交本轮 branch 产出到 session base；成功时仅提交显式允许持久化的数据。

## P4 Async Lifecycle

1. 异步节点发出请求后，Runtime 按 `clientType:requestId` 复合键将 `invocationId`、客户端类型、节点、分支、节点类型和独立 context 保存到 `pendingByRequestId`。文本与视觉客户端可安全使用相同的整数 request ID。
2. 一个 invocation 可以同时保存多个 pending。单个节点挂起不会阻塞其他已就绪分支。
3. 回调只能按自身客户端类型和 request ID 读取并移除对应 pending，写回该分支后再完成节点和推进后继。
4. 未知、重复和 invocation 结束后的晚到回调只记录日志，不得修改当前上下文。
5. 任一已知异步请求失败时终止所属 invocation，清空该轮全部 pending，且不得提交 session context。
6. `runtime.async.*` 仅用于节点声明本次执行已挂起，不再承担全局单槽控制或恢复索引语义。
7. 异步节点可通过 `config.async_timeout_ms` 设置超时，范围为 1 到 600000 毫秒，默认 120000 毫秒；超时回调必须同时匹配 request ID 和 invocation ID。

## P5 Invocation Queue And Trigger Pruning

1. Runtime 同一时刻只执行一个 invocation。活动轮次未结束时到达的 user 或 vision 触发按 FIFO 保存输入增量，不得覆盖活动分支。
2. 前一轮成功、失败或超时结束后，Runtime 在下一次事件循环中启动队首轮次，避免上一轮输出信号与下一轮状态交错。
3. 源节点可通过 `config.trigger` 声明 `user` 或 `vision`。只要 DAG 存在 trigger 声明，Runtime 就从与本轮 trigger 匹配的源节点开始，仅执行其可达子图。
4. 裁剪子图中的运行时入度只统计仍处于子图内的前驱。共享 join 只有一个活动父节点时按单父分支继承处理。
5. 旧 DAG 的所有源节点均未声明 trigger 时继续执行全图，以保持默认单链配置兼容。
6. 成功 invocation 只提交允许持久化的 `conversation.history`；失败 invocation 不提交 branch 结果。排队轮次启动时以最新 session base 叠加入队输入增量。

7. `AgentRuntime::Start()` 在仅加载 DAG 的启动阶段不创建空 trigger invocation；user 和 vision invocation 必须分别由输入入口创建。
8. `UpdatePerceptionFrame()` 在活动 invocation 期间直接将 vision 输入加入 FIFO；调用方随后可以调用 `Execute()` 完成兼容入口，但 Runtime 不得重复加入同一帧。

## 跨节点语义层

新增节点的跨节点输入输出应优先使用以下 key：

| Key | 含义 | 推荐用途 |
| --- | --- | --- |
| `semantic.text.prompt` | 用户或上游组装后的提示词 | LLM、改写、路由节点的输入 |
| `semantic.text.response` | 中间回复文本 | LLM 输出、过滤、改写、情感模块输出 |
| `semantic.text.final` | 最终回复文本 | UI、TTS、日志和最终输出 |
| `semantic.output.source` | 最终输出来源，允许值为 `user_response` 或 `vision_proactive` | UI、TTS 队列优先级和日志；由 `output.format` 根据 `runtime.trigger.type` 写入 |
| `semantic.image.base64` | 当前图像的 base64 编码数据 | 视觉模型、图像处理节点输入 |
| `semantic.image.media_type` | 当前图像媒体类型，如 `image/png` | 视觉模型请求参数 |
| `semantic.image.width` | 当前图像宽度 | 图像预处理、尺寸判断 |
| `semantic.image.height` | 当前图像高度 | 图像预处理、尺寸判断 |
| `semantic.proactive.should_speak` | 本轮是否应继续生成主动发话 | 主动决策、后续节点条件判断 |
| `semantic.proactive.topic` | 本轮主动话题候选 | 文本生成、去重和日志 |
| `semantic.proactive.summary_hash` | 当前视觉摘要指纹 | 主动摘要去重 |
| `semantic.proactive.reason` | 主动发话决策原因 | 调试、日志和策略解释 |
| `semantic.vision.frame_id` | 当前视觉帧 ID | 帧去重、时序处理和结果关联 |
| `semantic.vision.state` | 当前视觉处理状态 | 视觉节点调度和状态展示 |
| `semantic.vision.summary` | 当前视觉帧的自然语言摘要 | 视觉 LLM 输出、主动话题生成输入 |
| `semantic.vision.frame_hash` | 当前视觉帧内容指纹 | 感知帧去重和请求抑制 |
| `semantic.web.research.need_search` | 本轮是否执行联网研究 | `web.research` 检索决策和下游诊断 |
| `semantic.web.research.plan` | 待核实关键事实列表 | 研究计划审计 |
| `semantic.web.research.queries` | 本轮实际搜索 query 列表 | 研究过程审计 |
| `semantic.web.research.evidence` | 结构化证据条目 | 下游事实核验和回答组装 |
| `semantic.web.research.unsupported_claims` | 未获充分支持的关键事实 | 下游不确定性约束 |
| `semantic.web.research.conflicts` | 独立来源之间的证据冲突 | 下游冲突披露 |
| `semantic.web.research.status` | 研究终止状态 | 调度、降级和日志 |
| `semantic.web.research.citations` | 去重后的来源引用 | 回答引用 |
| `semantic.web.research.round_count` | 已完成研究轮数 | 预算审计 |

`semantic.vision.state` 当前允许值：

| 值 | 含义 |
| --- | --- |
| `available` | 已写入视觉帧，尚未经过视觉输入节点检查 |
| `ready` | 视觉帧完整，可以提交视觉模型 |
| `waiting` | 视觉数据不完整，等待新的感知帧 |
| `processing` | 视觉 LLM 正在处理当前帧 |
| `analyzed` | 当前帧已生成 `semantic.vision.summary` |
| `error` | 当前视觉 LLM 请求失败 |

后续新增多模态或记忆节点时，按同一格式扩展：

```text
semantic.memory.retrieval
semantic.emotion.user
semantic.emotion.pet
```

## 节点标准端口层

运行时会把语义层数据桥接到节点标准端口：

| Key | 含义 |
| --- | --- |
| `node.input.prompt` | 当前节点可直接读取的提示词输入 |
| `node.input.text_response` | 当前节点可直接读取的回复文本输入 |
| `node.output.prompt` | 当前节点产出的下游提示词 |
| `node.output.text_response` | 当前节点产出的中间回复文本 |
| `node.output.text_final` | 当前节点产出的最终回复文本 |

节点实现可以读取 `node.input.*`，但跨节点协议仍以 `semantic.*` 为准。

## 联网研究状态机

`web.research` 是用户触发链路中的受约束研究状态机，内部状态固定为 `Decide -> Search -> Observe -> Assess -> Repeat / Compose`。底层 `web.search` 只执行单次结构化检索，不作为用户 DAG 中的独立节点。

执行约束：

1. 输入只包含本轮用户问题、允许引擎和节点预算，不得发送 `conversation.history`、视觉摘要、截图或系统提示词。
2. 默认最多 3 轮、每轮最多 2 个 query、累计最多 8 条结果、总预算 15000 毫秒，Compose 上下文最多 6000 字符。
3. 不需要搜索时输出 `status=skipped`；空结果、部分失败、超时和预算耗尽必须形成明确状态，不得伪造已联网确认。
4. 高影响声明默认要求两个独立来源；证据不足或冲突时必须写入 `unsupported_claims` 或 `conflicts`。
5. 标题、摘要和 URL 始终视为外部不可信数据；Compose 输出必须保留防提示词注入约束和来源 URL。
6. `semantic.web.research.*` 仅属于当前 invocation，不得提交到 `m_sessionContext`。
7. P1 引擎不直接读取或写入 `AgentContext`；后续 DAG handler 负责把结构化响应序列化到上述协议键。
8. Runtime 使用 `web:<researchId>` 关联研究回调；回调必须同时校验 client type、request ID、invocation、node ID 和 node type。
9. `failure_policy=continue` 时，失败必须写入 `status=error`、未支持声明和“不得伪装联网确认”的降级提示词后恢复 DAG；`failure_policy=fail` 才终止 invocation。
10. 已接受的研究请求不得在 `Start()` 返回前发出终态信号，避免回调早于 pending 登记。

## 主动话题节点

`proactive.topic` 是视觉摘要与文本 LLM 之间的同步编排节点。节点不直接调用 LLM，负责根据当前执行上下文决定是否构造主动发话提示词。

输入：

| Key | 必需 | 含义 |
| --- | --- | --- |
| `semantic.vision.summary` | 视觉触发时是 | 当前视觉帧摘要 |
| `semantic.text.prompt` | 否 | 已存在的用户或上游提示词；存在时不得覆盖 |

输出：

| Key | 含义 |
| --- | --- |
| `semantic.proactive.should_speak` | 是否生成主动发话 |
| `semantic.proactive.topic` | 当前主动话题候选；第一版使用视觉摘要 |
| `semantic.proactive.reason` | 决策原因，例如 `vision_summary_available`、`user_prompt_available`、`disabled` |
| `semantic.text.prompt` | 供下游 `llm.chat` 使用的主动发话提示词 |
| `node.output.prompt` | 当前节点标准提示词输出端口 |

节点配置：

| 字段 | 类型 | 默认值 | 含义 |
| --- | --- | --- | --- |
| `enabled` | bool | `true` | 是否启用主动话题生成 |
| `instruction` | string | 内置桌宠发话要求 | 约束文本 LLM 如何根据视觉摘要生成话语 |

执行规则：

1. 已存在非空 `semantic.text.prompt` 时，不得覆盖，写入 `should_speak=false` 和 `reason=user_prompt_available` 后正常结束。
2. 节点禁用、视觉摘要缺失或为空时，写入 `should_speak=false` 和对应原因，不生成提示词。
3. 视觉摘要有效时，写入主动话题、决策结果、`semantic.text.prompt` 和 `node.output.prompt`。
4. `should_speak=false` 且没有其他文本输出时，`output.format` 必须正常静默结束，不得设置 `output.pending`。
5. `proactive.topic` 使用 `min_interval_ms` 和 `dedup_window_ms` 实施基础冷却与摘要去重；用户忙碌、语音播放和专注模式属于后续策略扩展。

## 模块私有状态层

模块私有 key 只用于兼容旧实现、调试或模块内部状态，不应作为新节点互插接口。

当前保留的私有 key：

| Key | 所属模块 | 用途 |
| --- | --- | --- |
| `llm.last_response` | `llm.chat` | LLM 原始回复，兼容旧节点 |
| `llm.last_request_id` | `llm.chat` | 最近一次 LLM 请求 ID |
| `llm.pending` | `llm.chat` | LLM 请求等待状态 |
| `emotion.last_request_id` | `emotion.rewrite` | 最近一次情感改写 LLM 请求 ID |
| `emotion.output_text` | `emotion.rewrite` | 情感节点改写结果 |
| `emotion.prompt_text` | `emotion.rewrite` | 情感改写节点组装后的内部提示词 |
| `emotion.raw_response` | `emotion.rewrite` | 情感改写 LLM 原始 JSON 或文本回复 |
| `emotion.source_text` | `emotion.rewrite` | 情感改写节点接收到的原始回复文本 |
| `emotion.user` | `emotion.rewrite` | 用户情绪标签 |
| `emotion.pet` | `emotion.rewrite` | 桌宠情绪标签 |
| `vision.analysis` | `vision.llm` | 视觉 LLM 原始分析文本，兼容旧节点 |
| `vision.available` | `vision.input` | 是否已有可用视觉帧 |
| `vision.input_ready` | `vision.input` | 当前视觉输入节点是否可执行 |
| `vision.latest_base64` | `vision.input` | 最近视觉帧的 base64 数据，兼容 `semantic.image.base64` |
| `vision.latest_frame_id` | `vision.input` | 最近视觉帧 ID |
| `vision.latest_width` | `vision.input` | 最近视觉帧宽度 |
| `vision.latest_height` | `vision.input` | 最近视觉帧高度 |
| `vision.latest_modality` | `vision.input` | 最近视觉帧模态，运行时转换为 `semantic.image.media_type` |
| `vision.updated_at` | `vision.input` | 最近视觉帧更新时间 |
| `vision.llm.last_request_id` | `vision.llm` | 最近一次视觉 LLM 请求 ID |
| `vision.llm.pending` | `vision.llm` | 视觉 LLM 请求等待状态 |
| `output.text` | `output.format` | 最终输出文本，兼容 UI 层 |
| `output.pending` | `output.format` | 输出节点等待上游文本时的临时状态 |
| `conversation.history` | conversation | 最近对话历史 |
| `proactive.last_spoken_at` | `proactive.topic` | 最近一次视觉主动输出的 UTC 毫秒时间戳 |
| `proactive.last_summary_hash` | `proactive.topic` | 最近一次视觉主动输出对应的摘要指纹 |
| `web.research.last_request_id` | `web.research` | 当前 invocation 的研究 ID |
| `web.research.failure_policy` | `web.research` | 当前节点的 `continue` / `fail` 策略 |

## 输入状态层

`user.*` 与 `input.*` key 由运行时准备本轮执行输入时维护。业务节点可以读取 `semantic.text.prompt` 或 `node.input.prompt`，不应优先依赖这些状态 key。

| Key | 用途 |
| --- | --- |
| `user.input` | 本轮用户原始输入文本 |
| `input.available` | 本轮是否存在可用文本输入 |
| `prompt.text` | 旧版提示词输入，兼容旧节点；应桥接到 `semantic.text.prompt` |

## 运行时状态层

`runtime.*` key 由 `AgentRuntime` 维护，业务节点不得依赖这些 key 做业务判断。

| Key | 用途 |
| --- | --- |
| `runtime.executed_nodes` | 已执行节点列表 |
| `runtime.last_node_type` | 最近执行节点类型 |
| `runtime.async.pending` | 是否存在异步节点等待回调 |
| `runtime.async.pending_node_id` | 等待回调的节点 ID |
| `runtime.async.pending_node_type` | 等待回调的节点类型 |
| `runtime.async.pending_request_id` | 等待回调的请求 ID |
| `runtime.trigger.type` | 当前一轮 DAG 的显式触发来源，允许值为 `user` 或 `vision` |
| `runtime.pass_through.<node_id>` | 尚未实现或透传节点的执行记录，仅用于调试和运行时追踪 |

`runtime.trigger.type` 只描述当前一轮执行，不得作为持久会话状态。用户输入入口必须设置为 `user`，视觉帧入口必须设置为 `vision`；本轮执行完成或失败后，运行时必须清除该 key。

## 新增节点规则

新增节点必须遵守以下规则：

1. 跨节点输入优先读取 `semantic.*` 或 `node.input.*`。
2. 跨节点输出必须写入对应的 `semantic.*` 或由运行时同步到 `semantic.*`。
3. 不要让节点直接依赖某个特定上游节点的私有 key，例如只读 `llm.last_response`。
4. 私有 key 使用 `<module>.<field>` 命名，例如 `memory.retrieved_text`。
5. 异步状态统一使用 `runtime.async.*`，不得新增并行的异步状态协议。
6. 新增 key 必须先加入 `include/vpet/agent/agent_context_keys.h`，不得在 `.cpp` 中散落硬编码字符串。
7. 新增节点类型必须优先产出 `semantic.*` 或 `node.output.*`，模块私有 key 只能作为兼容、调试或模块内部状态。
8. 视觉、多模态、记忆等新增语义数据应优先扩展 `semantic.*`，只有输入采集、请求状态或旧实现兼容才使用模块私有 key。
9. 本轮是否存在用户输入必须由 `runtime.trigger.type` 与本轮 `user.input` 共同确定，不得仅根据长期上下文中是否残留 `user.input` 推断。
10. 一轮执行完成或失败后必须清除 `user.input`、`input.available`、提示词端口和 `runtime.trigger.type`；`conversation.history` 作为持久状态保留。
11. 用户触发排队保持 FIFO；视觉触发排队采用 latest-wins，不应积压已经过期的视觉帧。

## 当前桥接关系

运行时当前维护以下桥接：

```text
prompt.text -> semantic.text.prompt
semantic.text.prompt -> prompt.text
semantic.text.prompt -> node.input.prompt
node.input.prompt -> prompt.text
llm.last_response -> semantic.text.response
llm.last_response -> node.output.text_response
semantic.text.response -> llm.last_response
emotion.output_text -> semantic.text.response
emotion.output_text -> node.output.text_response
semantic.text.response -> node.input.text_response
output.text -> semantic.text.final
semantic.text.final -> node.output.text_final
output.text -> node.output.text_final
runtime.trigger.type -> semantic.output.source
semantic.output.source -> AgentOutputReady.source
vision.latest_base64 -> semantic.image.base64
vision.latest_modality -> semantic.image.media_type
vision.latest_width -> semantic.image.width
vision.latest_height -> semantic.image.height
vision.latest_frame_id -> semantic.vision.frame_id
vision.available / vision.input_ready / vision.llm.pending -> semantic.vision.state
vision.analysis -> semantic.vision.summary
semantic.vision.summary -> proactive.topic
proactive.topic -> semantic.proactive.should_speak
proactive.topic -> semantic.proactive.topic
proactive.topic -> semantic.proactive.reason
proactive.topic -> semantic.text.prompt
proactive.topic -> node.output.prompt
node.output.prompt -> semantic.text.prompt
web.research response -> semantic.web.research.*
web.research summary -> semantic.text.prompt / node.output.prompt
```

这意味着逻辑兼容的文本节点可以通过 `semantic.text.response` 互插。

异步 LLM 类节点必须在回调完成后补齐上述桥接，因为节点发起请求时尚无可用输出，不能只依赖节点执行后的同步逻辑。

视觉节点必须优先读取 `semantic.image.*` 和 `semantic.vision.*`。`vision.*` 仅作为感知模块内部状态和旧实现兼容回退，不应成为新增视觉节点的跨节点接口。
