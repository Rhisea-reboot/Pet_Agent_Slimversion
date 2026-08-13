# Pet Agent 轻量版（slimVersion）构建计划

## 1. 定位与目标

轻量版是 [F:\Pet Agent](F:\Pet Agent) 全量版的精简分支，**只替换语音链路的两大件**，其余模块原样移植：

| 能力 | 全量版 | 轻量版 | 轻量化手段 |
|---|---|---|---|
| TTS | GPT-SoVITS（v2pro，数 GB 模型 + GPU/大显存） | **Kokoro**（https://github.com/hexgrad/kokoro，82M 参数，CPU 可跑） | 无需参考音频克隆，模型随 HuggingFace 自动下载，服务秒级启动 |
| ASR | GPT-SoVITS 自带 funasr_asr.py（large 模型，需 Python 环境内 GPU 或大内存） | **SenseVoiceSmall**（阿里 FunASR 的 sherpa-onnx int8 版本，228MB，纯 CPU 离线，**中/英/日/韩/粤混合识别**） | 小模型 CPU 实时，支持中英夹杂（如 "今天天气很好，Hello world"），无需 GPU |
| 其他 | — | 与全量版保持一致 | 原样拷贝：动画、DAG Agent、文本/视觉 LLM、联网研究、长期记忆、感知管道、UI、打包脚本、测试 |

**不变量**：所有 `include/vpet/*`、`src/*`、`CMakeLists.txt`、JSON 配置（除 `tts_config.json`）、`Animation/`、`packaging/`、`scripts/`、`tests/`、`docs/` 均从 `F:\Pet Agent` 原样拷贝，仅在下列"修改清单"中列出的文件做小范围改动。

---

## 2. 总体架构

```text
全量版                                 轻量版
─────────────────────                 ─────────────────────
GPT-SoVITS/ (数 GB)      ──移除──▶   （不存在）
  ├─ api_v2.py /tts                    tools/kokoro/kokoro_server.py   (Python 常驻服务, 端口 9880)
  └─ runtime/python.exe   ──替换──▶   runtime/ (Python venv: kokoro + sherpa-onnx + soundfile + numpy)
  └─ tools/asr/funasr_asr.py ──替换─▶  tools/asr/sensevoice_transcribe.py   (按需子进程)
                                      models/sensevoice/ (SenseVoiceSmall int8, 约 228MB)

C++ 侧沿用同一套接线，改动面最小：
  TtsServerManager (生命周期/健康检查) ──▶ kokoro_server.py
  TtsClient (POST /tts)              ──▶ kokoro_server.py
  VoiceInputManager (录音 + ASR 子进程) ──▶ sensevoice_transcribe.py
  TtsAudioPlayer / MainWindow / PetController ── 不动
```

关键设计决定：**Kokoro 由 C++ 通过 `QProcess` 拉起一个常驻 HTTP 服务**，而不是每次合成拉起进程——这样 `TtsServerManager` 的启动/健康检查/重启/退出清理逻辑可**原样复用**；ASR 维持"录音 → 子进程转写 → 读结果文件"的模式，`VoiceInputManager` 流程骨架不动。

---

## 3. 目录规划（E:\Pet Agent slimVersion）

```text
Pet Agent slimVersion/
├── runtime/                      # 新建：Python venv（供 TTS + ASR 共用）
│   └── Scripts/python.exe
├── tools/
│   ├── kokoro/
│   │   ├── kokoro_server.py      # 新建：Kokoro HTTP 服务（stdlib http.server，无 Web 框架依赖）
│   │   └── requirements.txt      # 新建：kokoro、soundfile、numpy 等
│   └── asr/
│       ├── sensevoice_transcribe.py    # 新建：SenseVoice 转写脚本（含 16kHz 重采样）
│       └── download_sensevoice.py      # 新建：下载 SenseVoiceSmall int8（sherpa-onnx）
├── models/
│   └── sensevoice/               # 新建：SenseVoiceSmall int8 ONNX + tokens.txt（下载产物，不入库）
├── setup_slim_runtime.bat        # 新建：一键创建 venv 并 pip 安装
├── start_tts_server.bat          # 修改：指向 kokoro_server.py
├── tts_config.json               # 修改：Kokoro 语义（voice/speed）
├── include/vpet/…                # 原样拷贝（仅 voice_input_manager.h 微调）
├── src/…                         # 原样拷贝（仅 3 个文件微调）
└── 其余全部（Animation/、agent_dag_structure.json、packaging/、scripts/、tests/、docs/…）
```

注意：`runtime/` 与 `models/` 为生成物，写入 `.gitignore`；`tools/` 两个 Python 脚本提交版本控制。

---

## 4. 轻量化 TTS：Kokoro

### 4.1 依赖与模型

- Python 包：`kokoro>=0.9.4`、`soundfile`、`numpy`（`torch` 随 kokoro 自动安装）。
- 为控制体积，torch 装 **CPU 版**：`pip install torch --index-url https://download.pytorch.org/whl/cpu`。
- 模型：`hexgrad/Kokoro-82M`（约 330MB），首次运行由 `huggingface_hub` 自动下载并缓存。为自包含可设 `HF_HOME=项目根/models/hf`（建议，可选）。
- 中文语音：`KFpipeline(lang_code='z')`，常用音色 `zf_xiaobei`（小北·女）、`zm_yunjian`（云健·男），可在 `tts_config.json` 配置。Kokoro 输出 **24kHz 单声道 WAV**，`TtsAudioPlayer`（QMediaPlayer）可直接播放，无需格式转换。

### 4.2 kokoro_server.py 服务契约（与现有 C++ 完全对齐）

| 端点 | 方法 | 请求 | 响应 |
|---|---|---|---|
| `/health` | GET | — | 200 `{"status":"ok","instance_id":"<VPET_TTS_INSTANCE_ID 环境变量>“}` |
| `/tts` | POST | JSON `{"text","lang","voice","speed"}` | 200 WAV 二进制流；400 JSON 错误 |

要点：
- **必须回显 `VPET_TTS_INSTANCE_ID`**——`TtsServerManager::PerformHealthCheck` 用它确认识别到的是本实例，否则 C++ 侧会一直认为健康检查未通过（见 `F:\Pet Agent\src\tts_server_manager.cpp:573-583`）。
- 服务器忽略 `ref_audio_path / prompt_text / prompt_lang / streaming_mode` 等旧字段，保证过渡期兼容。
- 单线程合成即可（桌宠说话是低频操作）；若想并发可用 `ThreadingHTTPServer`。
- 进程内 `KPipeline` 只初始化一次，避免逐句加载模型。

### 4.3 C++ 修改清单（仅 2 个文件 + 1 个配置）

**A. `src/tts_server_manager.cpp`（生命周期管理，骨架不变）**

| 位置 | 改动 |
|---|---|
| `LoadServerConfig()`（约 410-493 行） | 工作目录由 `<配置同目录>/GPT-SoVITS` 改为 `<配置同目录>` 项目根；Python 由 `runtime/python.exe` 改为项目根 `runtime/Scripts/python.exe`（兼容 `runtime/python.exe`）；脚本由 `api_v2.py` 改为 `tools/kokoro/kokoro_server.py`；启动参数由 `[-a host -p port -c yaml]` 改为 `[kokoro_server.py --host <host> --port <port>]`。去掉 PYTHONPATH 特殊拼接逻辑 |
| `FreeConflictingInstance()`（约 33-72 行） | PowerShell 匹配关键字由 `api_v2` 改为 `kokoro_server` |
| `STARTUP_TIMEOUT_MS`（158 行） | 120s 的模型加载时限可下调到 60s（Kokoro 秒级加载）；也可保留原值减少改动，二者皆可 |
| 错误文案 | "未找到 API 脚本" 等提示中的 api_v2 字样换成 kokoro_server.py |

**B. `src/tts_client.cpp` + `include/vpet/tts_client.h`（请求构造，改动很小）**

| 位置 | 改动 |
|---|---|
| `_tagTtsConfig`（头文件 17-23 行） | `refAudioPath/promptText/promptLang/textLang` → `voice`、`speed`、`lang` |
| `LoadConfig()`（41-121 行） | 删除 `ref_audio_path 非空`校验（119-117 行附近）；解析新增字段；`text_lang→lang` |
| `Synthesize()`（128-181 行） | 请求体改为 `{"text","lang","voice","speed"}` |

**C. `tts_config.json`（新语义）**

```json
{
    "server_url": "http://127.0.0.1:9880",
    "server_host": "127.0.0.1",
    "server_port": 9880,
    "voice": "zf_xiaobei",
    "speed": 1.0,
    "lang": "z"
}
```

> 说明：`TtsServerManager` 解析 `server_*` 字段、`TtsClient` 解析 `voice/speed/lang`，字段名与原实现尽量对齐，减少理解成本。

---

## 5. 轻量化 ASR：SenseVoiceSmall（中英混合）

> 决策记录：原计划用 Vosk small-cn（42MB），实测 **Vosk 单语模型无法处理中英夹杂**——中文模型把 "Hello world" 转成 "为了 我 而"，英文模型把中文句毁掉；双模型按置信度选也只对纯单语句有效。故改用 **SenseVoiceSmall**（sherpa-onnx int8，228MB，CPU 实时），支持中/英/日/韩/粤**句内混合**识别，中文精度也显著高于 vosk small。

### 5.1 依赖与模型

- Python 包：`sherpa-onnx`（1.13+，自带 onnxruntime），与 Kokoro 共用 `runtime/` venv。
- 模型：`csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17` 的 `model.int8.onnx`（228MB）+ `tokens.txt`，`tools/asr/download_sensevoice.py` 负责经 HF 镜像下载到 `models/sensevoice/`。
- 推理：`sherpa_onnx.OfflineRecognizer.from_sense_voice(model, tokens, use_itn=True)`；ITN 自动补标点与数字规范化（"二零二四年"→"2024年"）。

### 5.2 sensevoice_transcribe.py 契约

```
runtime\Scripts\python.exe tools\asr\sensevoice_transcribe.py -i <输入文件或目录> -o <输出目录> -m <模型目录>
```

- 输入：`VoiceInputManager` 录制的 WAV（QMediaRecorder 产物，通常 44.1/48kHz）；脚本内用 **numpy 线性重采样** 到 16kHz 单声道 float32。
- 输出：`<输出目录>/result.txt`，每个 wav 一行纯文本（相对 funasr 的 `path|duration|datalist|text` 格式是简化）。
- 退出码：0 = 成功（即使无文字也写空行）；非 0 = 失败，stderr 输出原因（模型缺失时给出 download_sensevoice.py 提示）。

### 5.3 C++ 修改清单（仅 1 个文件 + 1 个头文件）

**`src/speech/voice_input_manager.cpp` + `include/vpet/speech/voice_input_manager.h`（流程与状态机全部保留）**

| 位置 | 改动 |
|---|---|
| 匿名命名空间常量（cpp 27-35 行） | 删除 `GPT_SOVITS_DIRECTORY_NAME/ASR_SCRIPT_PATH/ASR_MODEL_SIZE/ASR_PRECISION`；新增 `TOOLS_DIRECTORY_NAME="tools"`、`ASR_SCRIPT_PATH="tools/asr/sensevoice_transcribe.py"`、`ASR_MODEL_DIR="models/sensevoice"`；`ASR_PROCESS_TIMEOUT_MS` 120000→60000 |
| `StartAsrProcess()`（330-381 行） | 定位改为：项目根（exe 同级/上级/上上级）→ `tools/asr/sensevoice_transcribe.py`；Python 改为 `runtime/Scripts/python.exe`（或 `runtime/python.exe` / 系统 `python`）；参数改为 `-i <input目录> -o <output目录> -m <models/sensevoice>`；`setWorkingDirectory(项目根)` |
| `FindGptSoVitsRootPath()`（383-405 行） | 语义改为"查找项目根目录"，函数更名 `FindProjectRootPath()`（头文件同步更名） |
| `FindPythonExecutable()`（407-431 行） | 候选：`<root>/runtime/Scripts/python.exe`、`<root>/runtime/python.exe`、`python` |
| `ReadTranscriptionText()`（433-485 行） | 简化为：读 `result.txt` → 取第一行 trim 后作为文本（删除 `|` 字段解析） |
| 头文件注释 | "调用 GPT-SoVITS 自带 ASR 脚本" → "调用 SenseVoice 转写脚本" |

**无需改动**：录音（`QMediaRecorder`）、热键、`MainWindow` 接线、`TranscriptionCompleted → OnVoiceTranscriptionCompleted → AgentRuntime` 全部原样。

> 备选（不做，仅记录）：Vosk 双模型（cn+en）按置信度分流。收益是省体积（42+40MB），但句内混合依然失败、置信度选择逻辑复杂，与"混合优先"目标相悖；子进程方案与现有 `VoiceInputManager` 架构完全同构，风险最小。

---

## 6. 原样拷贝清单（不变量，禁止改动）

| 内容 | 说明 |
|---|---|
| `Animation/` | 桌宠动画资源 |
| `include/vpet/` 除 `speech/voice_input_manager.h`、`tts_client.h` | 全部头文件 |
| `src/` 除 `tts_client.cpp`、`tts_server_manager.cpp`、`speech/voice_input_manager.cpp` | agent/memory/perception/sensor/llm/web 全部实现 |
| `CMakeLists.txt` | 目标、测试注册、onnxruntime/sqlite 部署逻辑全部保留（`vendor/onnxruntime` 一并拷贝，服务 embedding 功能；如追求极致体积可另出裁剪版，不在本计划范围） |
| `agent_dag_structure.json` 及 example | DAG 配置 |
| `llm_config.*` / `vision_llm_config.*` / `web_search_config.*` / `memory_config.*` / `context.md` / `情感.md` 等 | 配置文件与文档 |
| `packaging/`、`scripts/`、`tests/`、`tools/`（原工具） | 打包、测试脚本 |
| `.gitignore` | 追加 `runtime/`、`models/sensevoice/`、`models/hf/` |
| `main.cpp` / `main_window.cpp` / `pet_controller.cpp` / `tts_audio_player.cpp` | 启动/接线完全不动（TtsServerManager 接口未变，只是被管理的进程换了脚本） |

---

## 7. 实施步骤与验收标准

### Phase 0：环境准备（半天）
1. 本机已有 Python 3.14.2（`F:\python.exe`），直接用其创建 venv。注意：3.14 过新，torch（kokoro 依赖）若尚无 cp314 wheel 会安装失败，届时用 `py -V:3.11`（或安装 Python 3.11）降级，其余流程不变。
2. 运行 `setup_slim_runtime.bat`：`F:\python.exe -m venv runtime`；`runtime\Scripts\pip install torch --index-url https://download.pytorch.org/whl/cpu`；`pip install kokoro soundfile vosk`。
3. 运行 `tools/asr/download_sensevoice.py` 下载中英混合模型。
4. 编写 `tools/kokoro/kokoro_server.py`、`tools/asr/sensevoice_transcribe.py`。
- **验收**：`runtime\Scripts\python tools\asr\sensevoice_transcribe.py -i 一段中英混合test.wav -o out -m models\sensevoice` 输出正确文本（中英夹杂如 "今天天气很好，Hello world" 均正确）。

### Phase 1：TTS 服务独立验证（半天）
1. 手工启动 `runtime\Scripts\python tools\kokoro\kokoro_server.py --port 9880`（或 `start_tts_server.bat`）。
2. `curl -X POST http://127.0.0.1:9880/tts -H "Content-Type: application/json" -d "{\"text\":\"你好，我是小北\",\"lang\":\"z\",\"voice\":\"zf_xiaobei\"}" -o out.wav`。
- **验收**：返回 wav 可播放；`curl http://127.0.0.1:9880/health` 返回含 `instance_id` 的 JSON，且 `VPET_TTS_INSTANCE_ID` 设置时能回显。

### Phase 2：移植主工程并改造（1~2 天）
1. 把 `F:\Pet Agent` 全量（除 `.git/`、`GPT-SoVITS/`、`build*/`、`vendor/open-webSearch/` 按需保留）拷贝到 `E:\Pet Agent slimVersion`。
   - `vendor/open-webSearch/`：联网研究依赖，**保留**（全量版默认使用它，属于"其他部分不加变动"）。
2. 按 §4.3 / §5.3 修改 `tts_client.*`、`tts_server_manager.cpp`、`voice_input_manager.*`、`tts_config.json`、`start_tts_server.bat`。
3. `cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt>; cmake --build build`。
- **验收**：编译通过；`.\scripts\Run-Tests.ps1` 全部 CTest 通过（TTS/ASR 改动不触及 agent/memory/web 测试）。

### Phase 3：端到端联调（1 天）
1. 启动 VPet：启动画面应显示 Kokoro 服务快速就绪（预期 <10s，对比全量版 GPT-SoVITS 数分钟级）。
2. 说话验证：LLM 回复 → 气泡 + Kokoro 音频；连续多句验证音频不串、可打断。
3. 语音输入验证：Ctrl+Alt+V 录音 → Vosk 转写 → 对话链路 → 复读识别文本。
4. 降级路径验证：停掉 runtime（改名 runtime）→ 应用仍可启动，说话退化为纯气泡、语音输入报错提示（不崩溃）。
- **验收**：以上 4 项全部通过。

### Phase 4：打包与体积验收（半天）
1. 复用 `packaging/Build-Release.ps1`；发行目录放入 `tools/`、`runtime/`、`models/vosk/`（模型约 42MB）。
2. 冲击测试：连续会话 30 分钟无内存异常增长（Kokoro 常驻进程内存稳定）；退出程序后 TTS 进程被 `TtsServerManager::Stop()` 正常回收。
- **验收**：发行包体积对比全量版（GPT-SoVITS 数 GB）显著下降，目标 <500MB（不含 torch 时更小；torch CPU 约占 1.5GB 安装体积，属交换"数 GB 模型+GPU 需求"换"CPU 可跑"的必然成本）。

---

## 8. 轻量化收益测算

| 项 | 全量版 | 轻量版 |
|---|---|---|
| TTS 模型体积 | GPT-SoVITS v2pro 全目录数 GB | Kokoro-82M 约 330MB（HF 自动下载） |
| ASR 模型体积 | funasr large（GB 级） | SenseVoiceSmall int8 228MB（含中英日韩粤混合） |
| 硬件要求 | ASR/TTS 建议 GPU | 纯 CPU 可跑 |
| TTS 服务启动 | 数分钟（起大模型） | 数秒 |
| 每句合成速度 | 依赖 GPU | CPU 上 82M 模型仍实时以上 |
| 需用户准备的 | 参考音频 + 参考文本（音色克隆） | 无需，内置音色可选 |

**代价（务必认知）**：Kokoro 音色固定（不可克隆用户音色）、长文本需耐心（模型 82M 对多句短文本友好，桌宠场景句子短，不设分句边界亦可）；SenseVoiceSmall 体积 228MB（换取真中英混合与更高中文精度）；纯英文 TTS 需系统安装 espeak-ng（phonemizer 依赖，中文链路不受影响）。

---

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| Kokoro 首次运行需联网下载模型 | 首次启动 splash 提示；`HF_HOME` 指向项目内目录以支持离线迁移；失败降级为纯气泡（现有逻辑已具备） |
| 中文 `misaki[zh]` 依赖 | `pip install kokoro` 自动带 misaki；若中文 G2P 异常（如依赖 jieba/pypinyin 缺失），在 requirements.txt 显式补 `misaki[zh]`、`jieba`、`pypinyin` |
| QMediaRecorder 录得 48k 而模型要 16k | 已在 `sensevoice_transcribe.py` 内做 numpy 重采样，C++ 无感知；重采样段单独单元验证 |
| 9880 端口残留旧实例 | `FreeConflictingInstance` 已改为匹配 `kokoro_server`，健康检查 instance_id 双重保险 |
| torch CPU 安装体积大（~1.5GB） | 明确属于"可接受的交换"；如需进一步压缩，后续可评估 kokoro-onnx（onnxruntime 后端），列为独立后续任务，不阻塞本计划 |
| health 契约不一致导致 C++ 永不就绪 | §4.2 已把 `instance_id` 回显列为硬性契约，Phase 1 单独验收后才进入联调 |
| SenseVoice 模型未下载就启动 | 在 `StartAsrProcess` 中显式检查模型目录存在，缺失时给出下载提示（复用现有 TranscriptionFailed 通道） |

---

## 10. 反例（不做的事）

- 不重写 `TtsServerManager`/`TtsClient`/`VoiceInputManager` 架构：只改"被管理的进程/请求体/解析"，类、信号槽、状态机、超时看门狗全部复用。
- 不做进程内 Vosk 集成（见 §5.3 备选说明）。
- 不给桌宠加流式/边录边识别：超出轻量版范围。
- 不裁剪 vision/记忆/联网研究/embedding（onnxruntime）等既有能力：即使体积换大一点点，也遵守"其他部分不需要有大的变动"的边界。