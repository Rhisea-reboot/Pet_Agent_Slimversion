# LLM 流式输出、流式断句 TTS 与逐句气泡架构设计方案

## 1. 概述与目标

### 1.1 现状分析
当前项目的 LLM 与 TTS 交互采用**全文本批处理模式**：
- **LLM 客户端 (`LlmClient`)**：请求体显式设置 `"stream": false`，通过 `QNetworkReply::finished` 一次性接收完整 JSON 并提取整段文本。
- **Agent DAG 运行时 (`AgentRuntime`)**：等待 LLM 节点整体完成后将完整文本写入 Context（如 `llm.text.output` / `semantic.text.final`），下游节点才能继续执行。
- **TTS 语音合成 (`TtsClient` / `PetController`)**：收到完整文本后发起一次单体 HTTP 合成请求，等到整段 WAV 生成并下载完成后才开始播放并显示完整气泡。
- **瓶颈**：当 LLM 生成较长回复（例如 100~200 字）时，端到端延迟（用户输入 -> 听到首字声音 / 看到气泡）高达 **3~8 秒**。

### 1.2 改造目标
1. **支持 LLM 流式输出 (SSE Stream)**：实时接收 Token/Delta，降低首字感知延迟。
2. **流式断句器 (Stream Sentence Splitter)**：在 LLM 流式生成过程中，按照标点符号与语义停顿实时切分句子。
3. **分句流水线 TTS 与音频平滑串联播放 (Pipelined TTS & Playback Queue)**：
   - 收到第 1 句立即向 TTS 服务发起合成请求；
   - 第 1 句合成完毕即刻开始播放与动画；
   - 后续第 2、3 句在后台并行排队合成，并在前一句播放完毕后无缝衔接，实现“首字发音端到端延迟降低至 **< 800ms**”。
4. **逐句冒气泡 (Sentence-by-Sentence Bubble Sync)**：
   - 聊天气泡与当前正在播放的句子严格同步（或流式打字与分句切换），当前句子播放时展示对应气泡。

---

## 2. 总体架构设计

```
[ LLM API (OpenAI 兼容 SSE) ]
              │ (stream chunks: delta.content)
              ▼
    ┌──────────────────┐
    │    LlmClient     │  (SSE 解析器: 解析 "data: {...}")
    └────────┬─────────┘
             │ emit StreamDelta(requestId, delta) / StreamFinished(requestId)
             ▼
    ┌───────────────────────────┐
    │  StreamSentenceSplitter   │  (流式断句器: 缓冲区 + 正则断句规则)
    └────────┬──────────────────┘
             │ emit SentenceReady(requestId, sentenceIndex, sentenceText, isLast)
             ▼
    ┌───────────────────────────┐
    │  StreamDialogueCoordinator│  (流式对话编排调度器)
    └───────┬───────────┬───────┘
            │           │
            │ 1. 提交分句合成请求
            ▼           ▼
┌──────────────────┐  ┌──────────────────────────────────────────────┐
│   TtsClient      │  │  SentenceAudioQueue (音频与气泡播放队列)      │
│ (异步流水线合成) │  │  - sentenceIndex 严格保序                         │
└───────┬──────────┘  │  - 预加载/预缓冲机制 (Pre-buffering)          │
        │              └───────┬──────────────────────────────┬───────┘
        │ 合成完成通知          │ 触发播放                      │ 触发同步
        └──────────────────────►│                              │
                                ▼                              ▼
                     ┌──────────────────┐           ┌────────────────────┐
                     │  TtsAudioPlayer  │           │  ChatBubbleWindow  │
                     │  (顺序连贯播放)  │           │   (逐句显示/打字)  │
                     └──────────────────┘           └────────────────────┘
```

---

## 3. 核心模块详细设计

### 3.1 模块一：LLM 客户端流式扩展 (`LlmClient`)

#### 3.1.1 接口与信号扩展
在 `include/vpet/llm/llm_client.h` 中增加流式支持：
```cpp
struct _tagLlmRequestOptions
{
    // ... 原有参数
    bool stream = false; ///< 是否开启流式传输
};

signals:
    void ChatDelta(int requestId, const QString &deltaContent);        ///< 流式增量内容
    void ChatStreamFinished(int requestId, const QString &fullContent); ///< 流式全部接收完毕
    void ChatCompleted(int requestId, const QString &content);         ///< 兼容非流式
    void ChatFailed(int requestId, const QString &errorMessage, int httpStatusCode);
```

#### 3.1.2 SSE (Server-Sent Events) 数据流解析
- 修改 `SendChat`，当 `options.stream == true` 时，请求体写入 `"stream": true`。
- 连接 `QNetworkReply::readyRead` 信号：
```cpp
void LlmClient::OnReplyReadyRead()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) return;

    int requestId = reply->property("requestId").toInt();
    QByteArray &buffer = m_streamBuffers[requestId];
    buffer.append(reply->readAll());

    // 按行切分 SSE 格式 ("data: {...}\n\n")
    while (true)
    {
        int newlineIndex = buffer.indexOf('\n');
        if (newlineIndex < 0) break;

        QByteArray line = buffer.left(newlineIndex).trimmed();
        buffer.remove(0, newlineIndex + 1);

        if (line.isEmpty() || line.startsWith(':')) continue; // 注释或心跳

        if (line.startsWith("data: "))
        {
            QByteArray jsonData = line.mid(6).trimmed();
            if (jsonData == "[DONE]")
            {
                // 流结束
                continue;
            }

            // 解析 JSON: choices[0].delta.content
            QJsonDocument doc = QJsonDocument::fromJson(jsonData);
            if (doc.isObject())
            {
                QString delta = doc.object()["choices"].toArray()[0]
                                   .toObject()["delta"].toObject()["content"].toString();
                if (!delta.isEmpty())
                {
                    m_accumulatedTexts[requestId] += delta;
                    emit ChatDelta(requestId, delta);
                }
            }
        }
    }
}
```

---

### 3.2 模块二：流式断句器 (`StreamSentenceSplitter`)

#### 3.2.1 断句原则与阈值规则
流式断句的关键在于**在保证首句延迟最小化的同时，避免切出过短或无意义的破句**。

1. **强终止标点 (Immediate Split)**：
   - 标点：`。`、`！`、`？`、`!`、`?`、`\n`、`；`、`;`
   - 规则：遇到此类标点，若当前累积字符数 $\ge 4$（中文字符/单词），立即切分触发 `SentenceReady`。
2. **弱停顿标点 (Soft Split)**：
   - 标点：`，`、`,`、`、`、`：`、`:`、`—`
   - **首句策略（极速发音）**：若为第 1 句，累积字符数达到 **6~10 个字** 且遇到弱停顿标点时，立即切出第 1 句，让首字发音在几百毫秒内产生。
   - **后续句策略（听感连贯）**：第 2 句及以后，累积字符数达 **15~25 个字** 时再在逗号处切分，避免过于细碎导致语音卡顿或语调割裂。
3. **最大保底长度 (Max Buffer Force Split)**：
   - 若 LLM 连续输出无标点文本超过 **35 个字符**，寻找最近的空格或语法停顿进行强制截断。
4. **尾包冲刷 (End of Stream Flush)**：
   - 当收到 `ChatStreamFinished` 时，若缓冲区内残留任何未切分文本（无论多短），全部作为最后一句 `isLast = true` 冲刷送出。

#### 3.2.2 断句器数据结构与方法
```cpp
struct SentenceChunk
{
    int requestId;
    int index;          ///< 句子序号 (0, 1, 2...)
    QString text;       ///< 句子纯文本
    bool isFirst;       ///< 是否为首句
    bool isLast;        ///< 是否为末句
};

class StreamSentenceSplitter : public QObject
{
    Q_OBJECT
public:
    explicit StreamSentenceSplitter(QObject *parent = nullptr);
    void AppendDelta(int requestId, const QString &delta);
    void FinalizeStream(int requestId);
    void Reset(int requestId);

signals:
    void SentenceReady(const SentenceChunk &chunk);

private:
    void ProcessBuffer(int requestId, bool forceFlush = false);
    bool IsStrongDelimiter(QChar c) const;
    bool IsSoftDelimiter(QChar c) const;

    QMap<int, QString> m_buffers;
    QMap<int, int> m_sentenceCounters;
};
```

---

### 3.3 模块三：分句流水线 TTS 与播放队列调度器 (`StreamDialogueCoordinator`)

#### 3.3.1 流水线工作时序图

```
时间线 (ms)  0ms       300ms      600ms      900ms      1500ms     2200ms     3000ms
LLM 生成:    [字...] -> [句1完成] -> [句2生成中...] -> [句2完成] -> [句3生成中...]
TTS 任务:                [合成句1...] -> [句1完成, 发起句2合成...] -> [句2完成, 发起句3...]
音频播放:                           [▶ 播放句1音频 (1.6s) ────────►] [▶ 播放句2音频 (1.8s) ──►]
气泡显示:                           [💬 气泡显示句1 ──────────────►] [💬 气泡切换为句2 ────────►]
```

#### 3.3.2 队列状态机设计 (`SentenceQueueItem`)
每个句子在队列中维护独立的状态：
```cpp
enum class SentenceState
{
    PENDING_SYNTHESIS,  ///< 等待/正在合成 TTS
    SYNTHESIS_FINISHED, ///< 合成完成，WAV 音频就绪
    PLAYING,            ///< 正在播放音频并展示气泡
    PLAYED,             ///< 播放完毕，待清理
    FAILED              ///< 合成失败
};

struct SentenceTask
{
    SentenceChunk chunk;
    QString audioFilePath;
    SentenceState state = SentenceState::PENDING_SYNTHESIS;
    int ttsRequestId = -1;
};
```

#### 3.3.3 核心流转逻辑
1. **入队并触发合成**：
   - 收到 `SentenceReady`，生成 `SentenceTask` 压入当前会话的队列；
   - 立即调用 `TtsClient::Synthesize(chunk.text, tempAudioPath)` 发起异步 HTTP 合成（支持并发发起或按序管道化发起）。
2. **合成完成回调**：
   - 标记对应 `SentenceTask` 为 `SYNTHESIS_FINISHED`；
   - 触发播放调度函数 `TryPlayNextSentence()`。
3. **播放与气泡同步调度 (`TryPlayNextSentence`)**：
   - 若当前 `TtsAudioPlayer::IsPlaying()` 为 `true`，则静默等待当前句播放完毕；
   - 若当前空闲，且队列头部的下一句状态已为 `SYNTHESIS_FINISHED`：
     - 将该句状态改为 `PLAYING`；
     - 驱动桌宠进入 `SAYING` 动画动作（首句进入，后续句维持）；
     - 更新 `ChatBubbleWindow`：显示该句文本（支持逐句冒出或平滑淡入切换）；
     - 调用 `TtsAudioPlayer::Play(task.audioFilePath)` 播放音频。
4. **单句播放结束 (`OnPlaybackFinished`)**：
   - 将当前播放完成的句子标记为 `PLAYED` 并移出播放队列，删除对应临时 WAV 文件；
   - 立即检查下一句：
     - 若下一句已经就绪（`SYNTHESIS_FINISHED`）：**无缝直接播放**，气泡立即刷新为下一句文本，宠物维持说话动作，实现极为自然的连贯发音；
     - 若下一句还在合成中（短暂停顿）：气泡保持显示，桌宠保持待机/停顿，等待合成完成后瞬间接上；
     - 若队列为空且 LLM 流已结束：宠物退出 `SAYING` 状态恢复 `IDLE`，气泡延时一段时间后隐藏。
5. **打断/抢占机制 (User Interruption)**：
   - 若用户在播放中途点击宠物、拖拽或输入了新消息：
     - 立即调用 `LlmClient::CancelRequest` 取消在途 SSE；
     - 清空句子队列，调用 `TtsClient::Cancel`（或忽略未完成回调）；
     - 停止 `TtsAudioPlayer` 并清理已生成的临时 WAV；
     - 气泡立即隐藏或重置。

---

### 3.4 模块四：逐句聊天气泡视觉与动效设计 (`ChatBubbleWindow`)

#### 3.4.1 逐句气泡展示模式设计
提供两种优雅的逐句呈现方式（可配置）：

- **模式 A：单句轮播替换模式（推荐，适合桌面小巧宠物）**
  - 气泡同一时间只展示**当前正在念的那一句话**。
  - 当第 1 句念完、开始念第 2 句时，气泡以微小的弹性淡入动画（Fade-in & Scale）切换为第 2 句文本。
  - **优势**：气泡尺寸小巧紧凑，不遮挡屏幕大面积内容，用户视线聚焦，与当前听到的声音 100% 对应。
  - **视觉体验**：听一句、看一句，节奏明快。

- **模式 B：历史累积 + 当前高亮/追加模式**
  - 第 1 句播放时气泡展示第 1 句；
  - 第 2 句播放时，第 1 句变为灰色弱化字体，第 2 句在下方高亮冒出，并自动向上滚动/自适应撑开气泡；
  - 适合希望保留完整上下文对话阅读习惯的用户。

#### 3.4.2 气泡平滑动画与过渡
在 `ChatBubbleWindow` 中引入 `QVariantAnimation` / `QGraphicsOpacityEffect`：
- 句子切换时执行 150ms 快速淡入淡出（Cross-fade）；
- 气泡外框矩形尺寸平滑过渡（Size transition），避免文字长度变化时瞬间跳变。

---

## 4. 与现有 Agent DAG / Context 体系的兼容与协同

当前系统的核心架构基于 DAG (`AgentRuntime` / `AgentContext`)，需确保流式改造与现有架构完美契合：

1. **DAG 节点流式事件传播**：
   - 现有的 `llm.text.output` 属于终态上下文键。在流式执行过程中，增加临时上下文/事件通道：
     - `runtime.stream.active = true`
     - `semantic.text.stream_delta`
   - LLM 节点在流式执行时可以提前将流式调度句柄移交给 `StreamDialogueCoordinator`，无需阻塞整个 DAG 到最后才发出声音。
2. **记忆系统与情感分析兼容**：
   - **记忆沉淀 (`MemoryConsolidator`)** 与 **情感分析** 仍然需要在 **完整文本 (`fullContent`)** 产生之后执行。
   - 流式不会破坏记忆模块：当 LLM `ChatStreamFinished` 触发后，将完整文本回写至 `llm.text.output` 与 `semantic.text.final`，记忆提取与反思节点在后台照常触发，与前端的前台发音并行不悖。

---

## 5. 实施步骤与演进计划

| 阶段 | 阶段目标 | 涉及模块与改动 | 验收标准 |
| :--- | :--- | :--- | :--- |
| **Phase 1** | **LLM 客户端流式改造** | `LlmClient` | - 支持 `"stream": true`<br>- 能够稳定按 SSE 协议解析 `readyRead` 数据流<br>- 正确发出 `ChatDelta` 与 `ChatStreamFinished` 信号 |
| **Phase 2** | **流式断句器实现** | `StreamSentenceSplitter`<br>（新建头文件与单元测试） | - 编写完整单元测试，验证中英文标点、中途换行、无标点超长文本的切分逻辑<br>- 验证第 1 句在 6~10 字遇到逗号时能快速切出 |
| **Phase 3** | **流式分句 TTS 与音频队列** | `StreamDialogueCoordinator`<br>`TtsClient`<br>`PetController` | - 实现分句任务队列与预合成机制<br>- 前一句播完瞬间无缝起播下一句，无爆音、无重叠<br>- 首句端到端发音延迟由 4s 降低至 < 1s |
| **Phase 4** | **逐句气泡同步与打断交互** | `ChatBubbleWindow`<br>`PetController` | - 气泡随音频播放逐句精准切换<br>- 用户点击/拖拽桌宠时能瞬间打断 LLM 流、TTS 合成与音频播放 |

---

## 6. 异常处理与边界情况保护

1. **TTS 服务合成慢于朗读速度 (Under-run)**：
   - 若某一句较长，TTS 服务负载高导致下一句尚未合成完成：
   - 播放器暂时保持在 `SAYING` 的最后一帧（或循环动作），等待当前句合成完成后即刻播放，不发生异常中断。
2. **极短回复或单字语气词**：
   - 如回复仅有“好的。”，断句器直接触发一次切分（`isFirst=true, isLast=true`），走单句合成流程。
3. **网络断流与 LLM 报错**：
   - 若流式中途断开（如 HTTP 错误、Token 截断）：
   - 冲刷当前缓冲区已有文本，完成最后一批音频播放与气泡呈现；随后发出 `ChatFailed` 通知，并安全重置状态。
4. **并发与临时文件清理**：
   - 每轮对话使用唯一递增 `sessionId` / `requestId` 作为临时 WAV 前缀（如 `stream_req12_seq0.wav`）；
   - 音频播放完毕或会话被打断时，立即异步安全删除临时文件，防止磁盘堆积。
