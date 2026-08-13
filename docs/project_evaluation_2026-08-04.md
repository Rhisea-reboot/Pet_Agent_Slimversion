# VPet 项目评判报告

日期：2026-08-04
评判方式：源码走查 + 构建验证 + 测试验证 + 安全/卫生检查（非人工运行 GUI 交互验证）
参考：`docs/architecture_review_2026-08-01.md`（上一轮架构评审）

---

## 1. 总评

**结论：B+（高于平均水平的个人项目，工程质量良好，仍有关键短板未闭环）。**

与 2026-08-01 评审相比，项目显著进步：web 联网研究从"计划"变为"默认启用的完整组件"，llm.chat 配置参数透传问题已修复，Agent 运行时完成拆分。当前处于"功能齐备、工程完善、产品化不足"的阶段。

| 维度 | 评级 | 说明 |
|------|------|------|
| 架构设计 | A- | DAG + 语义 key + 异步挂起/恢复，范式与 LangGraph 同代 |
| 工程质量 | B+ | 构建/测试/文档/安全卫生均达标，个别历史遗留 |
| 测试 | B+ | 6 目标 / 68 测试函数全部通过，含真实 daemon 集成测试 |
| 安全卫生 | A | 无密钥入库历史，真实配置全部 gitignore |
| 文档 | A- | README、key 协议、评审记录、开发日志形成闭环 |
| 产品完成度 | C+ | 情感→动画、打扰控制、记忆、工具调用循环未闭环 |

---

## 2. 评判方法与验证事实

### 2.1 构建验证

- Qt 6.9.2 MinGW 64-bit，CMake + Ninja；增量构建 100% 成功，无警告待办
- 工作树为**未提交的 WIP 状态**：`agent_runtime_async.cpp` / `agent_runtime_nodes.cpp` / `agent_runtime_scheduler.cpp` 等 5 个新文件未跟踪，`CMakeLists.txt`、`agent_runtime.h/cpp` 等已修改未提交（构建与测试均已纳入这些新文件，验证通过）

### 2.2 测试验证

- CTest 6/6 全部通过（agent_dag_graph / agent_runtime_scheduler / application_integration / web_search_client / web_research_engine / web_search_daemon_integration）
- 约 68 个测试函数；daemon 集成测试在服务不可达时 QSKIP 优雅跳过（实测 8.2s 完整通过路径）
- **已知问题**：Windows 下直接运行 `ctest` 因 Qt DLL 不在 PATH 全部以 0xc0000135 失败（6 项全挂），需在 Qt Creator 环境或手工加 PATH 后运行。无 `ctest` 前置环境文档说明

### 2.3 规模事实

- 41 个 .cpp + 37 个 .h，约 16,590 行
- 854 个跟踪文件（其中 539 个动画资产约 77 MB）；仓库体积 65.85 MiB
- 35 个提交，2026-07-13 → 2026-08-04（约 3 周），master + ClaudeCode 双分支，远端 github.com/Rhisea-reboot/Pet_Agent

### 2.4 代码卫生扫描

- TODO/FIXME/HACK/XXX：**0 处**
- 裸 new/delete：41 处，全部为 Qt 父子所有权或成员 RAII，无泄漏模式
- 锁/原子：0 处 —— 单线程事件循环设计成立，但与"异步"语义需在文档中澄清线程模型
- 日志：149 处 qDebug/qWarning/qCritical，可观测性良好
- assert/Q_ASSERT：0 处（防御依赖错误路径返回，风格一致）

---

## 3. 优点

1. **DAG 运行时架构落地扎实**：GraphExecutor + NodeRegistry + AsyncBridge + FIFO/latest-wins 队列策略 + 触发源子图裁剪，与 `semantic.*` key 协议配合，是 2024+ 范式（同 LangGraph / OpenAI Agents SDK）
2. **配置驱动 + 语义协议解耦**：节点间只读写公共语义 key，私有 key 依赖被文档明确禁止（README 节点插入原则）
3. **测试覆盖真实路径**：web_search_daemon 集成测试对真实 Bing 服务做健康/状态/协议矩阵断言，且对环境不可达有降级；llm.chat 参数越界有显式报错路径
4. **安全卫生极好**：`llm_config.json` / `vision_llm_config.json` / `web_search_config.json` 全部 gitignore；git 历史中 api_key 恒为 `YOUR_API_KEY` 占位，无泄露痕迹
5. **文档-代码同步好**：README 的模块表格与实现一致（含参数范围、报错行为），开发日志记录决策与验证过程
6. **错误处理有体系**：节点失败→`failure_policy` 降级（continue）、主动抑制原因写入 `semantic.proactive.reason`、运行时加载拓扑校验（环检测）
7. **上一轮评审问题有跟进**：God 类 AgentRuntime 已按文件拆分（runtime/nodes/scheduler/async 四文件）；llm.chat 配置透传静默忽略 bug 已修复并回归

---

## 4. 问题与风险（按优先级）

### P0（建议尽快）

1. **工作树 WIP 未提交**：运行时拆分 5 个新文件未跟踪，一旦误清或换机即丢失；且 CTest 验证过的是"未提交状态"，提交后需再回归
2. **Windows 上 ctest 默认全挂**：0xc0000135 现象无文档、无脚本兜底，新环境接入成本高（建议 `tools/` 提供带 PATH 的 run-tests 脚本）

### P1（功能未闭环，影响产品定位）

3. **情感→动画未闭环 —— 已决定推迟（2026-08-04 用户决策）**：`emotion.rewrite` 输出的是 LLM 自由生成的英文标签（`pet_emotion`，词表无界），而动画资产仅覆盖 3 种情绪（Happy ~78 帧、ill ~40 帧、PoorCondition ~44 帧，含 A/B/C 状态变体），无 Sad/Angry/Shy 等资产。闭环需要先补齐资产或建立"标签→mood 映射 + 未知标签回退"策略，当前收益不匹配投入，暂时跳过
4. **主动打扰控制简陋**：仅固定冷却 + 摘要指纹去重；无用户忙碌/播放中检测、无专注模式（README 当前限制已列）
5. **LLM 客户端能力缺**：无流式（SSE）、无重试/退避、无并发上限与取消 —— 2026-08-01 评审遗留
6. **去重是内容 hash 而非感知级**：画面微小变化漏检/误判问题依旧

### P2（工程细节）

7. **仓库体积膨胀**：539 个动画资源 ~77MB 直接入库（.gitattributes 是否 LFS 待查，当前仓库 65.85MiB）；GPT-SoVITS 源码 vendored
8. **tts_config.json 被跟踪且含个人营销文本**（`prompt_text` 含"关注我的公众号…"），该文件会推送到公开仓库 —— 属个人信息而非密钥，建议模板化
9. **无 LICENSE 文件**：README 写"以仓库内实际声明为准"，但仓库内无声明
10. **无 CI 配置**（.github 不存在）：跨机器回归完全依赖本地，与 6 个测试目标的投入不匹配
11. **0 assert**：约定俗成的防御性检查缺失，异常路径只能靠日志事后排查

---

## 5. 与 2026-08-01 评审的对照

| 上轮问题 | 状态 |
|---------|------|
| AgentRuntime God 类（~1580 行） | ✅ 部分解决：同一类拆 4 文件，逻辑内聚改善（仍为单类，类头 551 行） |
| web.research 完整性 | ✅ 已闭环：mode=auto 默认启用，daemon 验收矩阵 + 集成测试 |
| llm.chat 配置静默忽略 | ✅ 已修复（2026-08-03 提交，含参数范围校验与回归） |
| 无流式/重试/取消 | ⚠️ 未动 |
| 情感→动画 | ⏸ 推迟（2026-08-04 决策：资产仅覆盖 Happy/ill/PoorCondition，闭环收益不匹配投入） |
| 打扰控制 | ⚠️ 未动（仅新增摘要指纹去重） |
| 仓库膨胀 | ⚠️ 未动 |
| 端到端真实链路验证 | ⚠️ 部分：web 搜索已真实验证；TTS + 语音 + 视觉全链路未验证 |

---

## 6. 下一步建议（按性价比）

1. **立即提交当前 WIP**，然后跑一遍干净环境 CTest（补上 PATH 问题文档或脚本）
2. `tools/run-tests.ps1` 兜底 ctest 环境 + 加 GitHub Actions（Windows + Qt 6.9.2）—— 一次投入长期收益
3. 接入预留接缝：`llm.chat` 的 tools 参数 + `tool.executor` 类型名（上轮已设计好 `agent.think` 子图方案）
4. tts_config.json 模板化 + 补 LICENSE + 评估动画资源走 Git LFS
5. ~~情感→动画联动~~ —— **已推迟**：动画资产仅 3 种情绪可用，待资产侧补充或明确"标签→mood 映射 + 回退"方案后再做

---

## 7. 评判结论

- **值得继续投入**：架构骨架与工程习惯是可持续的，测试与文档基础让后续演进风险可控
- **优先级排序**：先提交 WIP 并补 CI（防回归）→ 工具调用循环（能力升级）；情感→动画按用户决策推迟
- 当前不构成阻塞性缺陷，P0 只有"提交 WIP"和"测试环境兜底"两项

## 8. 后续状态校正

本报告生成后，`ae173a9 Harden agent runtime scheduling and test setup` 已提交到 `master`，因此第 2.1 节和第 4 节中关于“工作树 WIP 未提交”的描述仅代表本报告评审时点，不再代表当前仓库状态。

同一提交已落地 `scripts/Run-Tests.ps1`，用于从 CMake 缓存推导 Qt/MinGW 运行时路径并运行 CTest；因此 Windows 命令行的 DLL 环境兜底已具备。当前文档现状以 `PROJECT_DEVELOPMENT_REPORT.md`、`FRAMEWORK.md` 和 `Structure.md` 为准，历史评审中的其他 P1/P2 风险仍然有效。
