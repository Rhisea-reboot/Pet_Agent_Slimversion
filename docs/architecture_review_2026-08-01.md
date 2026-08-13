# Agent 架构评审与设计决策记录

日期：2026-08-01
背景：对 VPet 项目整体架构的一次评审对话，后续可作为计划参考。

---

## 1. 项目整体评判

**总体：高于平均水准的个人项目，工程素养明显。** 已完成从"桌宠动画"到"DAG 编排 Agent"的实质性演进，配套测试、文档与安全实践完整。

### 1.1 优点

- 架构演进真实落地：`AgentRuntime` + GraphExecutor + NodeRegistry + AsyncBridge + 队列策略（FIFO/latest-wins）构成完整异步调度体系
- 配置驱动：DAG 用 JSON 定义、语义 key 协议（`semantic.*`）解耦节点
- 有测试且接入 CTest：4 个测试目标（DAG 图、调度器、集成、web client）
- 安全习惯好：真实 API key 不入库、模板配置、.gitignore 正确
- 文档完备：README、开发报告、key 协议、框架文档相互补充
- 构建可复现：多套 Qt 6.9.2 MinGW 构建目录均有成功产物

### 1.2 问题清单

1. **上帝类风险**：`PetController`（约 800 行）职责重；`AgentRuntime` 本身膨胀到约 1580 行，同时承担配置查找、调度、pending 管理、历史维护
2. **LLM 客户端功能缺**：无流式（SSE）、无请求取消、无并发上限与退避重试
3. **去重是内容 hash 而非感知级**：画面微小变化会漏检或误判
4. **情感是"标签"而非"行为"**：emotion 标签未驱动动画状态，核心卖点之一没有闭环
5. **主动打扰控制简陋**：只有冷却+去重，无用户忙碌/播放中检测
6. **仓库膨胀**：vendored GPT-SoVITS 源码进库；`vendor/`、`scripts/`、`web/` 为未跟踪新模块
7. **端到端验证缺**：真实 API key 的完整链路、长时间稳定性、TTS 降级体验均未验证

### 1.3 下一阶段建议聚焦

- LLM 客户端工程化（流式+重试）
- 情感→动画联动
- 被动打扰控制
- 拆分 `AgentRuntime` 和 `PetController`

---

## 2. 与现役 Agent 框架的对比

**核心结论：骨架不过时，能力层过时。** 结构是 2024+ 的，Agent 能力是 2022 年的。

- DAG + 共享 Context + 异步挂起/恢复 + 队列策略 = 与 LangGraph / OpenAI Agents SDK 同范式
- 缺的四层能力：
  1. **工具调用循环**（最大差距）：LLM 无法自主决定"下一步做什么"
  2. **流式输出**（SSE）：当前全量请求
  3. **记忆体系**：只有滚动 `conversation.history`，无分层记忆（working/semantic/episodic）
  4. **可观测性**：无 trace span，调试靠 qDebug
- 跟得上的部分：触发源裁剪、latest-wins/FIFO 队列 ≈ interrupt + 优先级调度；`semantic.*` key ≈ typed state schema；异步 pending 恢复 ≈ async node continuation；JSON 配置驱动

### 2.1 建议演进方向（按性价比排序）

1. 加 tool-calling 节点：`llm.chat` 支持 tools 参数，新增 `tool.executor` 节点（web_search 是现成第一个工具）
2. 结构化输出：`output.format` 用 JSON schema 约束
3. 持久化会话：session context 落盘（checkpoint），重启可恢复
4. 流式（SSE）视 UI 需求再定，桌面气泡场景优先级不高

---

## 3. 产品定位：FL Studio 式客制化（重要决策）

**DAG 不是内部实现细节，而是面向用户的客制化界面。**

类比映射：

- `semantic.*` key 协议 = 信号总线（FL 的 mixer channel），节点间交换数据的"线缆"
- `user.input` / `vision.input` = MIDI 事件源（触发时机）
- 队列策略 = 音频缓冲与事件合并
- `proactive.topic` = 带参数的模块

方向不过时：LangGraph Studio、ComfyUI 式 agent builder 全是"Agent 即用户可编辑的图"。

### 3.1 客制化必须解决的三个问题

1. **用户不会读运行时日志** → 配置加载时 dry-run 校验 + 人话错误信息，最好做右键菜单"检查 DAG"
2. **热加载是客制化的生命线** → `AgentRuntime::Reload()`（原子替换 DAG 定义 + 清空队列）优先级提高
3. **预设即产品** → 提供可复制的模板 DAG（纯聊天 / 视觉主动 / 带情感 / 带 web 搜索），存为 `presets/` 目录，从"改别人的配置"开始 onboarding

远期加分项：Qt 6 做节点可视化编辑器（拖线连节点、自动生成 JSON），届时"桌面版 ComfyUI"定位才真正成立。**key 协议是插件 SDK，应像对外 API 一样维护和文档化。**

---

## 4. 工具调用循环的实现决策（核心设计结论）

### 4.1 参考：LangGraph 中的循环

LangGraph 的循环是**图里可见的一条环边**（model → tools → model），依赖条件边 + 回边 + superstep + recursion_limit。但它也可以封装为子图节点（`create_react_agent` 就是把整个循环封装成一个单元）。

### 4.2 本框架的决策：循环 = 子图节点内部的控制流，不引入环

**最终结论：不需要允许图环。** 图结构只表达数据流，控制流（循环）属于节点实现内部。

- 实现：子图节点（如 `agent.think`）内部跑 `for round < max_rounds` 的循环，每轮顺序执行 `llm.chat → tool.executor`，图层面永远无环
- 终止条件内聚：最大轮数（等价 recursion_limit）、工具报错降级（回退成纯文本回复）、无 tool_call 即出
- 对外只暴露语义 key：`semantic.text.tool_calls` / `semantic.text.tool_results` 作观察仪表盘
- 多轮 async：AsyncBridge 按 requestId 挂起/恢复，节点内多轮完全可行，每轮一个 requestId，超时/失败按轮处理
- 若将来需要"每轮 UI 介入确认"：节点内挂起、UI 确认后恢复同一轮即可，依然不需要环

### 4.3 与 LangGraph 的等价性

**无环 DAG + 节点内循环 + async 让位** 在表达能力上等价于 LangGraph 的显式环，但实现和维护更简单。

### 4.4 与 FL Studio 定位的权衡

循环藏在节点里 = 用户改不了内部结构（相当于打包成 patcher 模块）。解法：留两档——

- 默认：`agent.think`（封装循环，普通用户用）
- 高级用户：`inline_loop: true` 配置项展开成显式节点链（但仍是子图节点，内部循环实现，不引入图环）

---

## 5. 关于"聊天 Agent 定位"的讨论

当前无工具调用循环是因为定位是聊天 Agent，该取舍合理（复杂度在调用协议、结果回灌、循环终止、错误处理）。

两点提醒：

1. **已半只脚在工具路上了**：`vision.llm` 本质是"感知工具"。未来转 tool-calling 时，最自然形态是让 LLM 自主决定"要不要看屏幕"，而非每轮固定执行。`proactive.topic` 已做决策前置，迁移成本低。
2. **现在只留"接缝"，不要提前实现**：
   - `llm.chat` 请求体支持可选 `tools` 数组（空数组时行为不变，0 成本）
   - 输出协议约定一种结构化格式（如 `semantic.text.tool_call`），暂不解析
   - `AgentNodeRegistry` 预留 `tool.executor` 类型名
3. 聊天 Agent 边界模糊：屏幕感知已接入聊天上下文，"查时间/天气"类轻量工具与它是同一类东西。接缝留好后定位升级只是加配置的事。

---

## 6. 决策摘要（速查）

| 主题 | 决策 |
|------|------|
| 图结构 | 保持无环 DAG，不引入图环 |
| 工具循环 | 子图节点内部循环实现（`agent.think`），等价于 LangGraph 显式环但更简单 |
| 高级定制 | `inline_loop: true` 展开配置项（仍为子图节点） |
| 预留接缝 | `tools` 参数、`semantic.text.tool_call` key、`tool.executor` 类型名 |
| 客制化 UX | dry-run 校验 + 人话错误、热加载 Reload()、presets 模板 |
| 下一阶段 | LLM 工程化、情感→动画、打扰控制、拆分上帝类 |
| 远期 | 节点可视化编辑器、分层记忆、可观测性 |
