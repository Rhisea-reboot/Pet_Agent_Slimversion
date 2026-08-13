# VPet 长期记忆实施方案

> **状态：** 阶段 1-4 已完成（2026-08-10），阶段 5-6 已完成（2026-08-11）
> **日期：** 2026-08-09
> **关联设计：** [memory_architecture.md](memory_architecture.md)
> **目标：** 以低耦合、非阻塞、可控写入的方式，为 VPet 建立可持续演进的长期记忆能力。

## 1. 实施原则

长期记忆应作为独立的 `memory/` 子模块实现，而不是散落在 `AgentRuntime` 或 DAG 节点中。其原因不是增加文件数量，而是该能力具有独立的：

- 数据模型和索引。
- 持久化与数据损坏恢复责任。
- 后台处理、队列背压和关闭时落盘生命周期。
- 隐私过滤与用户数据管理责任。
- 后续 embedding、图检索、LLM 巩固等演进路径。

模块化边界必须保持简单：主 Agent 只提交任务并消费已完成的结果；后台服务独占记忆图和文件读写。主对话链路绝不等待记忆检索、存储或巩固。

阶段 1 不应过度拆分。建议起始文件结构如下：

```text
include/vpet/memory/
  memory_graph.h          # MemoryEntry、节点/边、标签与关键词索引
  memory_repository.h     # JSON 读写、schema、隐私过滤
  memory_service.h        # 后台 worker、任务队列、结果 mailbox

src/memory/
  memory_graph.cpp
  memory_repository.cpp
  memory_service.cpp
```

后续仅在出现真实复杂度时再新增 `embedding_client`、`consolidator` 或 `clusterer` 等模块。

## 2. 阶段 1 的产品边界

阶段 1 的目标是完成一个真实可用的闭环，而不是提前实现向量检索或自动知识图谱。

### 支持能力

- 显式记忆：用户明确表达“记住”、“以后请”、“不要再”时保存。
- 类型：`Fact`、`Preference`、`Correction`、`Negative`。
- 作用域：`Global`、`Pet`。`Session` 延后，直到运行时具有明确 session 身份和生命周期。
- 标签和中文关键词检索。
- 逻辑删除、标签维护、按标签或作用域列举。
- 人类可读 JSON 持久化、原子写入、损坏文件容错。
- 隐私过滤和超长内容拒绝。
- 异步检索和异步存储，正常回答永不被记忆模块阻塞。

### 暂不支持的能力

- 普通对话自动写入。
- embeddings、余弦相似性、聚类和 BFS 图遍历。
- 伴生 LLM 提取、重复检测、矛盾检测和自动合并。
- 跨设备同步、加密备份和记忆管理 UI。
- `Session` scope、程序性记忆和用户反馈强化。

普通对话在阶段 1 默认不自动写入。直接保存完整对话会迅速积累噪声并污染检索结果；自动提取应在有语义检索和结构化 LLM 输出校验后再开启。

## 3. 数据模型

阶段 1 保留后续图模型的兼容形态，但只使用记忆、标签及 `has_tag` 关系。不要提前实现簇或未被使用的图算法。

```cpp
struct MemoryEntry {
    QString id;
    QString content;
    QString category;
    QStringList tags;

    enum class Type { Fact, Preference, Correction, Negative } type;
    enum class Scope { Global, Pet } scope;
    enum class Provenance { UserStated, UserCorrected } provenance;

    QString petId;
    qint64 createdAt;
    qint64 updatedAt;
    qint64 lastAccessed;
    quint32 accessCount = 0;
    quint32 strength = 0;
    float confidence = 1.0f;
    float trustScore = 1.0f;
    bool active = true;
};
```

`Inferred` 统一视为未来的 `Provenance`，而不是 `MemoryType`。只有引入自动提取后，才需要低置信度推断记忆及其较短半衰期。

`MemoryGraph` 内部至少维护：

```cpp
QHash<QString, MemoryEntry> entries;
QHash<QString, QSet<QString>> tagIndex;
QHash<QString, QSet<QString>> keywordIndex;
```

阶段 2 图遍历需要的边应使用明确的边对象或边索引，不能将邻接表定义成 `QVector<QString>` 后又将元素作为 `_tagMemoryEdge` 使用。

## 4. 存储与隐私

### 写入目录

默认数据目录应为：

```text
QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)/memory/
```

目录内的阶段 1 文件：

```text
memory/
  graph.json
```

不能复用可执行文件、工作目录或配置文件搜索路径作为用户数据写入目录。它们只适用于读取应用配置。

### JSON 契约

`graph.json` 必须含有 `schemaVersion`，并通过 `QSaveFile` 写入：

```json
{
  "schemaVersion": 1,
  "entries": [],
  "updatedAt": 0
}
```

读取失败、字段缺失或 JSON 损坏时：保留损坏文件副本、记录诊断信息、以内存空图启动。记忆故障不能阻止应用对话。

### 隐私过滤

所有写入必须经过单一过滤入口，至少拒绝：

- API key、access token、password、secret 等明确凭证模式。
- PEM 私钥、JWT、疑似 `.env` 内容。
- 身份证号、银行卡号等高风险个人标识符。
- 超过上限的原始文本；阶段 1 应拒绝而非尝试自动摘要。

过滤规则应保守：不确定时不存储，并在日志中只记录拒绝原因类别，不能回写敏感原文。

## 5. 异步服务模型

`MemoryService` 是记忆系统的唯一主入口。生产应用中创建一个服务实例；测试可以注入 fake service。不要依赖“整个进程永远只有一个 AgentRuntime”的假设。

```text
AgentRuntime (主线程)
  |  TryEnqueueRetrieve / TryEnqueueStore
  v
有界任务队列
  v
MemoryService worker (后台线程，独占 graph/repository)
  |
  v
按 petId + triggerType 分区的结果 mailbox
  |
  v
AgentRuntime (下一次调用时 TakeLatestReadyResult)
```

### 关键约束

- Qt 的 `QQueue` 没有 `try_enqueue`；服务需使用 `QMutex + QQueue + capacity` 自行实现非阻塞有界队列。
- 队列满时，提交返回失败并记录日志；DAG 必须继续回答。
- 不使用 `runtime.async.pending`。该机制会暂停当前 DAG，违背记忆服务的非阻塞要求。
- 结果携带 `requestId`、`petId`、`triggerType` 和查询时间。
- “第 N+1 轮可用”定义为“下一轮开始时消费当前最新已完成结果”，不承诺后台工作恰好一轮完成。
- 用户输入、视觉主动发话等不同 trigger 不能共享同一个未分区 mailbox，避免串入无关记忆。
- 应用关闭时停止接收任务、处理已提交写入并在有限等待时间内 flush；超时也不能阻止进程退出。

## 6. DAG 集成

用户输入链调整为：

```text
user.input -> web.research -> memory.retrieve -> llm.chat
           -> emotion.rewrite -> output.format -> memory.store
```

第一版不接入视觉主动发话链，避免不同触发源共用检索结果。等 mailbox 分区与触发策略稳定后再决定是否接入。

### `memory.retrieve`

输入：

- `semantic.text.prompt`
- `runtime.trigger.type`
- 当前 `petId`

行为：

1. 读取 mailbox 中已完成的最新结果。
2. 立刻提交当前 prompt 的检索任务。
3. 将已完成记忆按固定预算组装成只读提示段。
4. 若无结果、队列满、服务禁用或检索失败，保留原 prompt 并成功返回。

输出上下文键：

```text
semantic.memory.entries
semantic.memory.prompt
```

该节点注入 prompt 时必须同步更新下列四个键：

```text
semantic.text.prompt
node.input.prompt
node.output.prompt
prompt.text
```

当前 `llm.chat` 优先读取 `node.input.prompt`。只更新 `semantic.text.prompt` 不会将记忆传给模型。

### `memory.store`

位于 `output.format` 后，读取：

```text
user.input
semantic.text.final
runtime.trigger.type
pet.id
memory.store.intent
```

它只构造并提交本轮 DTO，绝不提交完整的 `conversation.history`，否则会在后续轮次反复保存历史对话。写入候选仅能来自明确命令或上游已判定的显式记忆意图。

新增的上下文键需统一登记在 `agent_context_keys.h`，并仅使用 `QString`、`QStringList`、`QVariantMap` 或 `QVariantList`，避免将自定义结构跨线程放入 `QVariant`。

## 7. 配置

新增 `memory_config.json` 和 `memory_config.example.json`：

```json
{
  "enabled": true,
  "data_dir": "",
  "queue_capacity": 64,
  "max_results": 6,
  "prompt_budget_chars": 1200,
  "default_scope": "pet",
  "automatic_extraction": false,
  "embedding": {
    "enabled": false,
    "backend": "local_onnx",
    "model": "BAAI/bge-small-zh-v1.5",
    "model_dir": "models/embedding/bge-small-zh-v1.5",
    "onnx_model": "model.onnx",
    "tokenizer_file": "tokenizer.json",
    "vector_store": "sqlite",
    "vector_db": "",
    "max_sequence_length": 512,
    "device": "cpu"
  }
}
```

`automatic_extraction` 必须默认为 `false`。`data_dir` 为空时使用 AppDataLocation；非空时必须是用户明确指定的位置。
`embedding.model_dir` 为空时可解析为安装目录下的默认模型目录；`embedding.vector_db` 为空时使用
`<data_dir>/memory/vectors.sqlite3`。embedding 阶段不得把用户记忆文本发送到远端服务。
模型目录至少需要包含 ONNX 导出文件 `model.onnx`、`tokenizer.json` 以及模型配置和许可证文件。
原始 Hugging Face PyTorch/safetensors 权重不能直接由桌面端推理，需要在发布前转换为 ONNX。

## 8. 阶段计划

### 8.1 阶段 1：受控记忆闭环

> **状态：** 已完成（2026-08-10）

1. 实现 `MemoryGraph`、`MemoryRepository` 和 `MemoryService`。
2. 实现 `remember`、`recall`、`list`、`forget`、`tag` 的内部 API。
3. 完成 JSON schema、原子保存、隐私过滤和关键词/标签召回。
4. 在 `AgentRuntime` 注入服务，并在应用入口完成启动与关闭。
5. 注册 `memory.retrieve` 和 `memory.store` 节点，更新真实与示例 DAG。
6. 仅接受显式记忆写入。

阶段 1 验收标准：用户可明确要求记住或删除内容；重启后数据存在；后续对话可召回相关标签/关键词记忆；任何记忆服务故障都不影响正常回复。

**实施记录（2026-08-10）：**
- 模块：`include/vpet/memory/` 与 `src/memory/`（graph / repository / service）；节点：`memory_retrieve_node`、`memory_store_node`；运行时注入构造器与生命周期方法。
- 内部 API：`remember`→`TryEnqueueStore`、`recall`→`TryEnqueueRetrieve`、`list`→`TryEnqueueList`、`forget`→`TryEnqueueForget(ById/ByKeyword)`、`tag`→`TryEnqueueTag`；结果统一经 `petId|triggerType` 分区 mailbox 消费。
- 检索：中文按单字建索引，英文按单词（≥2 字符），辅以子串回退；scope（Pet/Global）与 petId 过滤；标签索引多标签取并。
- 持久化：`<dataDir>/memory/graph.json`，`schemaVersion=1`，`QSaveFile` 原子写；损坏或未知版本时保留 `.corrupt.<epoch>` 副本并以空图启动。
- 异步：条件变量 worker 循环 + 有界队列（满则拒绝不阻塞）+ mailbox 分区；晚到低 requestId 不覆盖更新结果；`Shutdown` 排空已提交写入后退出。
- 隐私过滤：拒绝 credential / private_key / jwt / env_file / id_card / bank_card / too_long（>2000 字符），日志只记类别。
- 测试：`memory_core_tests`（图/仓库）、`memory_service_tests`（异步与持久化，含 list）、`memory_dag_integration_tests`（DAG 集成），全部通过。

### 8.2 阶段 2：本地 BGE embedding 与级联检索

> **状态：** 已完成（2026-08-10）

1. 引入 ONNX Runtime 和 `tokenizers-cpp`，从本地目录加载 `BAAI/bge-small-zh-v1.5`。（实施差异：不引入 `tokenizers-cpp`，内置 BERT WordPiece 分词器读取 `tokenizer.json`，与模型 vocab_size 校验一致。）
2. 对新记忆延迟生成 512 维 mean-pooled embedding，并执行 L2 归一化。
3. 将向量以 `float32` BLOB 写入本地 SQLite `vectors.sqlite3`，按模型 ID 和维度校验。
4. 使用本地向量库的精确 cosine 相似度产生 top-k 初始命中。
5. 引入带明确 edge 对象的图模型，实施最大深度 2 的 BFS 与边权重衰减。
6. 本地模型不可用时回退到阶段 1 的关键词/标签检索，不调用远端 embedding 服务。
7. 对最终注入设置条目和字符预算，默认最多 6 条、1200 字符。

**实施记录（2026-08-10）：**
- 模块：`embedding_client`（ONNX 会话 + WordPiece 分词 + 查询指令前缀 + L2 归一化）、`vector_store`（SQLite 向量库，cosine top-K）。
- 模型：`scripts/download_bge_model.ps1` 从 hf-mirror.com 拉取 `bge-small-zh-v1.5` ONNX（含量化版）到 `models/embedding/bge-small-zh-v1.5/`。
- 级联：`EmbedQuery` → top-K 向量召回（score>0 为种子）→ `ExpandByEdges` BFS（深度 2、每跳 ×0.6）→ 稳定排序截断；向量不可用回退关键词检索。
- 测试：`memory_embedding_tests`（fake embedder + 向量库）、`memory_service_tests` 级联用例；真实模型 e2e（`tests/manual/test_embedding_e2e.cpp`）5 段全 PASS。
- 阶段 3 才引入的主题变更检测、置信度衰减不在本阶段范围（见架构文档阶段 2 清单未勾选项）。

### 8.3 阶段 3：LLM 巩固

> **状态：** 已完成（2026-08-10）

1. 由伴生 LLM 从每轮增量中输出结构化候选 JSON。
2. 本地校验 schema、scope、类型、长度和隐私规则后才允许写入。
3. 相似记忆优先强化，避免新增重复条目。
4. 处理可确认的覆盖关系；无法确认时保留两者并标记冲突。
5. 引入 `Observed`、`Extracted`、`Inferred` provenance 和对应信任分数。

**实施记录（2026-08-10）：**
- 模块：`memory_consolidator` 复用已配置的文本 `LlmClient`，只提交当前轮 `user.input` 与 `semantic.text.final`；请求不写入 `runtime.async.pending`，因此不会暂停主 DAG。
- 开关：`automatic_extraction` 保持默认 `false`；显式启用后，每轮最多产生 `consolidation_max_candidates`（默认 4，范围 1-8）个候选。
- 防护：发往伴生 LLM 前，以及模型输出落盘前均经过统一隐私过滤；模型必须返回严格的单 JSON 对象 schema，未知字段、无效类型/scope/置信度、越界数量、重复候选和不可引用关系目标均会整批拒绝，诊断不记录模型原文。
- 合并：完全相同或 Jaccard 相似度 >= 0.80 的候选仅提升已有条目 `strength` 并合并标签；明确 `supersedes` 时新增替代条目、写入有向覆盖边并逻辑删除旧条目；`conflicts` 时保留两条记忆并建立冲突边。
- 来源：新增 `Observed`、`Extracted`、`Inferred` provenance；LLM 巩固写入统一标记为 `Extracted`，`trustScore=0.7`，并由 schemaVersion 3 JSON 持久化。
- 测试：`memory_service_tests` 覆盖 schema/隐私/关系校验、重复强化、覆盖和冲突保留；与 `memory_core_tests`、`memory_dag_integration_tests`、`memory_embedding_tests` 一并通过。

### 8.4 阶段 4：检索后维护

> **状态：** 已完成（2026-08-10）

1. 检索前按记忆类型和 provenance 半衰期增量衰减置信度，并将置信度、信任分、访问次数和近期访问用于排序。
2. 检索后更新访问元数据，按命中集合创建或加强 `related` 边，并以共同标签支持数进行保守标签推断。
3. 无命中时只持久化查询 SHA-256、宠物、触发类型和计数，不落盘查询原文，形成缺口记录。
4. 按检索次数定期从活跃 `related` 边重建连通簇，原子写入 `memory/clusters/cluster_metadata.json`。
5. 通过有界异步 `TryEnqueueFeedback` 接收有帮助/无帮助反馈，更新 `strength`、`confidence` 并持久化。

**实施记录（2026-08-10）：**
- 模块：`memory_maintenance`，由 `MemoryService` worker 独占调用；新增 `MemoryEdge::Type::Related` 和 `confidenceUpdatedAt`，graph schema 升级为 v4，兼容 v1-v3 读取。
- 维护配置：`decay_interval_hours`、`cluster_update_retrievals`、`max_gap_records`、`inferred_tag_min_support`、related 边权重参数均有范围校验。
- 隐私：gap 只写查询 SHA-256，不写入查询原文；维护文件使用 `QSaveFile` 原子更新。
- 测试：新增 `MemoryMaintenanceTest` 与异步 feedback 持久化用例；全量 10 个 CTest 目标通过。

### 8.5 阶段 5：高级记忆与用户控制

> **状态：** 已完成（2026-08-11）

1. schema v5 新增负面记忆 `triggerPatterns` 与结构化 `Procedure`（名称、触发、步骤、前置和警告）。
2. 检索优先匹配当前上下文触发的负面/流程记忆；延迟一轮的普通结果须通过保守话题相关性检查，避免跨主题注入。
3. `AgentRuntime` 提供异步 list / update / forget / feedback / import / export 门面，所有图和文件操作仍由 worker 独占。
4. 桌宠右键菜单新增长期记忆管理窗口，可查看、编辑、逻辑删除、反馈、导入和导出；最近一次回答所用记忆也可直接提交有帮助/无帮助反馈。
5. 导入先在临时图中完成 schema、隐私、索引和边校验，再原子替换 `graph.json`；失败时保留当前图不变，向量作为可重建缓存清空并回填。
6. embedding 首次检索时扫描活跃条目，对缺失或模型/维度不匹配的向量执行一次延迟回填。

**实施记录（2026-08-11）：**
- 模块：新增 `memory_manager_dialog`；扩展 `memory_graph`、`memory_repository`、`memory_service`、`memory_maintenance` 和 Runtime 管理门面。
- 安全：触发正则限制长度并预编译校验；流程至少含一个非空步骤；管理更新/删除/标签操作校验 pet scope；导入内容重新经过隐私过滤。
- 测试：覆盖负面/流程触发、schema v5 round-trip、导入导出、流程命令解析、scope 隔离、缺失向量回填；全量 10 个 CTest 目标通过。

### 8.6 阶段 6：保守深度巩固

> **状态：** 已完成（2026-08-11）

1. 默认每 50 次检索运行一次全图维护，仅合并同作用域、同类型且 Jaccard 相似度至少 0.95 的 Fact / Preference / Correction。
2. 合并保留更强、更可信或更早的条目，合并标签和统计，建立 `Supersedes` 边并逻辑删除重复项。
3. 对 `confidence < 0.05 && strength <= 1` 的非冲突弱记忆执行逻辑删除；停用条目的传播边和向量同步清理。
4. `Conflict` 边两端不参与自动合并或弱剪枝，并在管理列表中标记 `[冲突]`，由用户编辑或删除完成裁决。
5. 深度维护后立即从活跃 `Related` 边重建簇元数据，形成簇重组和知识图传播边清理闭环。

### 8.7 明确保留的后续项

- `Session` scope：当前运行时没有用户可控的会话开始/结束身份，不能用进程生命周期替代会话契约。
- reinforcement 面包屑：需要先引入稳定的 `sessionId + messageIndex` 协议，避免记录伪来源。
- 跨设备同步、加密备份和物理清理/恢复 UI。

## 9. 测试计划

### 单元测试

- JSON round-trip、未知字段兼容、schema version、损坏文件恢复。
- 重复 ID、逻辑删除、标签维护、scope 过滤和中文关键词检索。
- 凭证/敏感模式、超长文本与日志不泄露原文。
- 原子写失败时保持原有数据完整。
- 图索引的一致性和查询排序稳定性。

### 异步测试

- 队列满时 `TryEnqueue` 立即失败且不阻塞。
- 一轮提交后，在后续调用消费最新可用检索结果。
- 晚到结果不会覆盖更高 requestId 的新结果。
- 不同 `petId`、不同 trigger 的结果不会串流。
- 关闭流程 flush 已接收写任务，超时正常退出。

### DAG 回归测试

- 无记忆、服务禁用或服务故障时 prompt 和回答与现有行为一致。
- `memory.retrieve` 同步四个 prompt alias，`llm.chat` 确实收到带记忆的 prompt。
- `memory.store` 只接收当前 turn，不重复处理滚动 history。
- 记忆节点不设置 `runtime.async.pending`，不暂停 invocation。
- 现有 history、情感改写、web research 和 output.format 行为不回归。

## 10. 实施决策清单

开始编码前需要固定以下契约：

1. `petId` 的来源、默认值以及宠物切换时的行为。
2. 显式记忆意图由哪一个节点解析，初版是否只支持内部命令。
3. 记忆提示段的固定文案、最大条目数及最大字符数。
4. 逻辑删除的保留时间和用户恢复策略。
5. `Global` 记忆是否跨宠物共享，以及 UI 中的可见范围。

上述五项明确后，可以直接开始阶段 1，实现不依赖 embedding 提供商或伴生 LLM。
