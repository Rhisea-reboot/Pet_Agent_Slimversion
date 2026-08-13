# VPet 项目减负与发行优化计划

> 状态：已确认（2026-08-06）
> 目标：将当前约 16.6 GB 的工作区精简为可发行的 **5-6 GB** 本地捆绑包（方案 B：本地精简捆绑）
> 关联文档：[架构评审](architecture_review_2026-08-01.md)、[项目评估](project_evaluation_2026-08-04.md)、[TTS 集成计划](tts_integration_plan.md)

---

## 1. 背景与目标

VPet 应用本体（Qt6/C++）源码仅约 1 MB，但发行时需捆绑 GPT-SoVITS 本地 TTS/ASR 子系统及动画资源，导致工作区膨胀至 **16.6 GB**，无法直接分发。

本计划在不改变产品形态（本地 TTS + 本地 ASR + PNG 动画）的前提下，通过精简依赖、剔除无用组件、重建最小运行时，将发行包压缩至 **5-6 GB**，并建立可复现的打包流程。

### 1.1 已确认的决策（2026-08-06）

| 决策项 | 结论 |
|---|---|
| TTS 发行策略 | **方案 B：本地精简捆绑**（不采用按需下载，也不更换轻量 TTS） |
| 语音输入（ASR） | **保留**（项目其他模块亦有使用，titles 不可删） |
| 动画资源格式 | **保持 PNG**（不转 WebP） |

### 1.2 非目标

- 不更换 TTS 引擎（GPT-SoVITS 保留）
- 不改变动画资源格式
- 不做在线按需下载机制
- 不删除语音输入功能

---

## 2. 现状盘点

实测工作区构成（2026-08-06）：

| 内容 | 大小 | Git 跟踪 | 运行时必需 | 处置 |
|---|---|---|---|---|
| `GPT-SoVITS/runtime`（Python 环境） | 7.4 GB | 否 | 是 | 重建精简（阶段 1） |
| `GPT-SoVITS/GPT_SoVITS/pretrained_models` | 4.2 GB | 否 | 部分 | 只留 v2 链路 + fast_langdetect（阶段 2） |
| `GPT-SoVITS/tools` | 2.0 GB | 否 | 部分 | 删 uvr5，改造删 AP_BWE_main，ASR 全留（阶段 3） |
| `GPT-SoVITS/GPT_SoVITS/text`（字典） | 751 MB | 是（约 65 MB） | 部分 | 裁剪验证（阶段 4） |
| `build/` + `build-clean/` | 995 MB | 否 | 否 | 删除（阶段 0） |
| `vendor/open-webSearch` | 87 MB | 部分 | 可选功能 | 维持现状（node_modules 已忽略） |
| `Animation/` | 73 MB | 是（539 PNG） | 是 | 移出版本库（阶段 6） |
| `.git` | 66 MB | — | 否 | 清理（阶段 6） |
| `src/` + `include/` + `tests/` | ~1 MB | 是 | 是 | 不动 |

### 2.1 关键事实

- `tts_infer.yaml` 实际只用 **v2 链路**：`chinese-roberta-wwm-ext-large`(621M) + `chinese-hubert-base`(180M) + `gsv-v2final-pretrained`(338M) ≈ **1.1 GB**
- 机内存在但运行时用不到的模型：`gsv-v4-pretrained`(789M)、`v2Pro`(587M)、`pretrained_models/sv/` 权重(103M)、`models--nvidia--bigvgan_v2_24khz_100band_256x` 权重(215M) ≈ **1.7 GB**
- **`fast_langdetect`(125M) 为强依赖，不可删除**（2026-08-06 代码核对）：`TTS_infer_pack/TextPreprocessor.py:12` 无条件 `from text.LangSegmenter import LangSegmenter`；`text/LangSegmenter/langsegmenter.py:10-11` 模块加载即 `import fast_langdetect` 并实例化检测器（`cache_dir` 指向 `pretrained_models/fast_langdetect`），无 try/except 保护，删除将导致 api_v2 启动崩溃
- 注意区分代码与权重：`GPT_SoVITS/BigVGAN/`、`GPT_SoVITS/sv.py` 为**代码目录/文件**，被 `TTS.py:25,37` 无条件 import，删除权重时切勿误删
- `tools/AP_BWE_main`(114M) 为隐藏强依赖：`TTS.py:33` 无条件 import `tools/audio_sr`，其模块级代码 import AP_BWE_main 内模块；直接删除会崩溃，需先改代码（见阶段 3.2）
- `TEMP/ref_audio.wav` 是 **TTS 音色样本（强依赖）**：`tts_config.json:6` 的 `ref_audio_path` 指向它，`TTS.py:1136` 文件不存在即抛错 → `/tts` 全部 400；该文件未入 git、src 无生成代码，删 TEMP 时须排除
- `pytorch-lightning`、`tensorboard`、`torchmetrics` 是**推理链强依赖**（非训练专属）：`TTS.py:24` 无条件导入 `t2s_lightning_module.py`，其 `:11` `from pytorch_lightning import LightningModule`，阶段 1.3 删"训练相关包"时勿删
- `sv.py:5-8` 为模块级 import：`sys.path.append(GPT_SoVITS/eres2net)` 后 `from ERes2NetV2 import ERes2NetV2` + `import kaldi`（kaldi 内嵌于 `eres2net/` 代码目录，非 pip 包）；`GPT_SoVITS/eres2net/` 必须保留
- `runtime/nltk_data` 中的 `averaged_perceptron_tagger` 是英文前端依赖；实测现有中文与英文回归未使用 `punkt`，但重建时仍须整体迁移已有 `nltk_data`
- C++ 与启动脚本现优先使用 `GPT-SoVITS/runtime/python.exe`，并回退到标准 venv 的 `runtime/Scripts/python.exe`；重建 runtime 后仍须核对两者至少存在其一
- `runtime/Lib` 中 torch 占 4.8 GB（含 CUDA 库）；日/韩语前端依赖约 550 MB（`mecab_ko_dic`、`eunjeon`、`pyopenjtalk`、`ipadic`、`wordfreq`）
- ASR 固定中文（`voice_input_manager.cpp` 中 `ASR_LANGUAGE = "zh"`），`tools/asr/models` 1.13 GB 已是最小中文集（paraformer 848M + 标点 283M + VAD 4M），**无冗余，全部保留**
- `tools/uvr5`(718M) 为人声分离训练工具，推理不依赖
- 动画为运行时磁盘加载（`main.cpp:GetAnimationBasePath`），不编译进二进制

---

## 3. 分阶段执行计划

### 阶段 0：本地即刻清理（预计节省 ~1 GB）

| # | 操作 | 节省 | 说明 |
|---|---|---|---|
| 0.1 | 删除 `build/`、`build-clean/` | 995 MB | 纯构建产物，CMake 可重建 |
| 0.2 | 删除 `GPT-SoVITS/output`、`logs`、`TEMP` 内缓存（`jieba.cache`、`gradio/`、`torch/`） | ~20 MB | ⚠️ `TEMP/ref_audio.wav` 必须保留：`tts_config.json` 的 `ref_audio_path` 指向它（`TTS.py:1136` 不存在即 /tts 全部 400），手工音色样本，未入 git、无生成代码 |
| 0.3 | `git gc --prune=now` | 若干 | 压缩对象库 |

**验收**：`ctest` 全绿（在临时构建目录重新 configure 验证一次）。

### 阶段 1：Python 运行时重建（7.4 GB → 1.5-3 GB）

**背景**：现有 `runtime` 是全量训练环境（torch 4.8 GB 含 CUDA、训练工具、多语言依赖），推理只需 api_v2 + funasr 子集。

| # | 操作 | 节省 | 说明 |
|---|---|---|---|
| 1.1 | 新建干净 venv，安装已验证的 api_v2 推理依赖集 | ~4 GB | 排除训练/WebUI 依赖；C++ 与启动脚本兼容 `runtime/python.exe` 和标准 venv 的 `runtime/Scripts/python.exe` |
| 1.2 | 删除日/韩语 TTS 前端依赖：`mecab_ko_dic`(213M)、`eunjeon`(112M)、`pyopenjtalk`(105M)、`ipadic`(50M)、`wordfreq`(57M) | ~550 MB | 中文 TTS/ASR 不依赖（`cleaner.py:37` 延迟 import），需回归验证 |
| 1.3 | 删除训练/WebUI 相关包：`cmake`(78M)、`gradio`(61M) 等 | ~200 MB | ⚠️ 勿删 `pytorch-lightning`/`tensorboard`/`torchmetrics`——推理链强依赖（`t2s_lightning_module.py:11`） |
| 1.4 | torch 按目标机选型 | 2.5-4.5 GB | 已确认实际走 `custom` 段（`TTS.py:318` 取 `configs_.get("custom", "v2")`，当前 device: cuda）；`TTS.py:322` 有 CUDA 不可用自动降 CPU 的保护，但纯 CPU 推理显著变慢，默认保留 CUDA 版 ~2.5 GB |
| 1.5 | 产出 `scripts/rebuild_runtime.ps1` | — | 环境可复现，要求完整 Python 3.9；迁移 `runtime/nltk_data`，并保留兼容的 Python 启动路径 |

**风险**：api_v2.py 与 torch/Python 版本兼容性；重建后必须回归 TTS 发声与 ASR 识别。**重点核对**：① `runtime/python.exe` 或 `runtime/Scripts/python.exe` 存在且可被启动器解析；② `pytorch-lightning` 链完整；③ `runtime/nltk_data` 已迁移（中英混读回归项）；④ `sv.py` 的 `eres2net/` 代码目录未动。
**验收**：GPT-SoVITS API 启动成功；`/tts` 合成出 wav；`funasr_asr.py` 中文识别正常。

### 阶段 2：模型权重瘦身（4.2 GB → ~1.3 GB）

| # | 操作 | 节省 | 说明 |
|---|---|---|---|
| 2.1 | 删除 `gsv-v4-pretrained`、`v2Pro`、`pretrained_models/sv/`（权重）、`models--nvidia--bigvgan_v2_24khz_100band_256x`（权重） | ~1.7 GB | 核对 `tts_infer.yaml` 各段与代码无引用后执行；仅删权重目录，`BigVGAN/`、`sv.py` 代码必须保留 |
| 2.2 | 保留：`chinese-roberta-wwm-ext-large`、`chinese-hubert-base`、`gsv-v2final-pretrained`、`fast_langdetect` | — | v2 链路必需 + 语言检测强依赖（不可删，见 2.1 关键事实） |
| 2.3 | 清理空权重目录（`GPT_weights*`、`SoVITS_weights*`） | — | 仅保留实际引用的声音模型目录 |

**风险**：若未来切换 v3/v4 音色需要重新下载。删前整体备份。
**验收**：`/tts` 使用 v2 配置合成正常；`tts_infer.yaml` 指向的文件全部存在；`api_v2.py` 启动无 ModuleNotFoundError（`fast_langdetect`、`BigVGAN/`、`sv.py` 代码链正常加载）。

### 阶段 3：tools 瘦身（2.0 GB → ~1.13 GB）

| # | 操作 | 节省 | 说明 |
|---|---|---|---|
| 3.1 | 删除 `tools/uvr5` | 718 MB | 人声分离，推理不依赖；grep 确认仅 `webui.py` 引用 |
| 3.2 | 改造后删除 `tools/AP_BWE_main` | 114 MB | 超分模型，`super_sampling` 默认 False 不加载权重；但 `TTS.py:33` 无条件 import `tools/audio_sr`，其模块级代码 import AP_BWE_main 内模块（`datasets1`/`models`），**直接删会崩**，需先给 `audio_sr.py` 的 import 加 try/except 降级 |
| 3.3 | ASR 保留：`tools/asr/models`（1.13 GB） | — | 已是最小中文集，不动（`voice_input_manager.cpp:27` 固定调用 `tools/asr/funasr_asr.py`）；⚠️ `funasr_asr.py:29-40` 每次调用都执行 `snapshot_download`（modelscope 联网检查），离线发行时可能抛异常被 `only_asr` 吞掉 → 语音输入静默失败，发行前需离线实测一次，必要时改代码跳过下载检查 |

**验收**：语音输入功能回归正常；webui 相关入口（如无使用）保持禁用；`api_v2.py` 启动正常（若执行 3.2 需回归 `super_sampling=True` 路径按预期报错或跳过）。

### 阶段 4：字典裁剪（可选，751 MB → ~715 MB，风险中等）

| # | 操作 | 节省 | 说明 |
|---|---|---|---|
| 4.1 | 验证中文推理不加载日/英文资源后删除：`ja_userdic`(37M) | ~37 MB | 含 git 内字典文件；`cleaner.py:37` 为延迟 import，zh 推理不加载日文模块；⚠️ cmudict 系列**不可删**：`text/english.py:19-20` 加载 `cmudict.rep`/`cmudict_cache.pickle`，中英混读与纯英文 TTS 依赖，删除将导致英文路径报错（中英混读是验收项） |
| 4.2 | 复核其余可裁剪部分 | 视验证结果 | ⚠️ `text/g2pw/`（polyphonic.rep/pickle）与 `G2PWModel_1.1.zip`(85M) 是中文强依赖（`chinese2.py:28-34` 模块加载即实例化 `G2PWPinyin`），**不可删**；剩余可动空间有限，收益低于原估 |

**风险**：文本前端可能在启动时无条件加载字典，删除会导致启动报错。此步放在其他阶段全部完成、回归稳定后执行。
**验收**：TTS 中文合成、情感朗读、中英混读行为与裁剪前一致。

### 阶段 5：发行包脚本（新增）

| # | 操作 | 说明 |
|---|---|---|
| 5.1 | 新建 `packaging/` 目录与 `Build-Release.ps1` | CMake Release 构建 → `windeployqt` 收集 Qt DLL → 组装发行目录 |
| 5.2 | 发行目录结构 | `VPet.exe` + `Qt6/*.dll` + `platforms/` + `Animation/` + `GPT-SoVITS/`（精简版）+ `config/*.json` |
| 5.3 | Inno Setup 脚本 | 一键生成安装包 |
| 5.4 | 发行自检脚本 | 校验 DLL 齐备、动画目录完整、模型文件存在、`TEMP/ref_audio.wav` 存在 |

**验收**：在干净机器（无 Qt、无 Python 环境变量）上安装后可启动、可 TTS、可语音输入。

### 阶段 6：Git 仓库减负（66 MB → ~10 MB）

| # | 操作 | 节省 | 说明 |
|---|---|---|---|
| 6.1 | `Animation/` 移出跟踪（`git rm -r --cached` + `.gitignore`） | ~60 MB | 动画随发行包分发，不走版本库 |
| 6.2 | GPT-SoVITS 大字典移出跟踪 | ~5 MB | 同上 |
| 6.3 | 历史清理（可选） | 视仓库增长 | `git filter-repo` 彻底清除，或保留历史仅停更 |

**风险**：新克隆者需单独获取动画资源；filter-repo 重写历史需团队同步。
**验收**：`git clone` 体积显著下降；`Animation/` 可从发行包恢复用于开发调试。

---

## 4. 预期收益

| 指标 | 当前 | 目标 |
|---|---|---|
| 工作区总大小 | 16.6 GB | ~6 GB |
| 发行包大小 | 无（无法发行） | 5-6 GB |
| Git 仓库（.git） | 66 MB | ~10 MB |
| 发行流程 | 手工复制 | 一键脚本 |

保留的硬性成本（不可再减）：v2 模型 1.1 GB + `fast_langdetect` 0.13 GB + ASR 中文模型 1.13 GB + 精简运行时 1.5-3 GB + 动画 73 MB。

---

## 5. 风险与回滚策略

| 风险 | 等级 | 缓解措施 |
|---|---|---|
| venv 重建后 api_v2 兼容性失败 | 高 | 重建前整体备份 `runtime`；逐包安装验证；核对 `python.exe` 路径、`pytorch-lightning` 链、`nltk_data` |
| 误删 `TEMP/ref_audio.wav` 导致 TTS 全部 400 | 高 | 阶段 0.2 按文件粒度清理缓存，音色样本保留并纳入发行自检（5.4） |
| 离线环境 ASR 静默失败（modelscope 联网检查） | 中 | 发行前断网实测语音输入；必要时改 `funasr_asr.py` 跳过 `snapshot_download` |
| 字典裁剪导致 TTS 启动失败 | 中 | 阶段 4 放最后执行；删除前备份 `text/`；cmudict 系列不删（中英混读依赖） |
| torch 选型后推理设备不可用 | 中 | 先实测当前走 CPU 还是 CUDA；发布两套配置文档 |
| 删除模型后需切换 v3/v4 音色 | 低 | 模型备份至外置盘，保留下载说明 |
| filter-repo 重写历史影响协作者 | 低 | 阶段 6.3 与协作者确认后执行 |

**备份目录约定**：所有删除项在动手前复制到 `F:\Pet Agent-备份\`（或外置盘），命名 `yyyymmdd-<名称>`。

---

## 6. 验收清单（全部阶段完成后）

- [ ] `ctest` 全绿（当前机器未发现 Qt6 开发包，待配置 `CMAKE_PREFIX_PATH` 后执行）
- [ ] 桌宠启动、动画播放正常（PNG 资源完整）
- [ ] TTS 发声正常（v2 链路，中文 + 中英混读）
- [ ] 语音输入（ASR 中文）正常
- [ ] 断网（离线）环境下 TTS 与 ASR 均可用（modelscope 不触发联网下载）
- [ ] Agent 对话 / Web 搜索功能不受影响
- [ ] 干净环境安装发行包可完整运行
- [x] Git 索引不再跟踪 `Animation/` 与已验证的大字典资源；历史清理待团队确认后执行
- [ ] `scripts/rebuild_runtime.ps1` 可在具备完整 Python 3.9 的新机器复现环境

---

## 7. 执行记录

| 日期 | 阶段 | 状态 |
|---|---|---|
| 2026-08-06 | 计划定稿 | 待执行 |
| 2026-08-07 | 阶段 0 本地清理 | 已完成：`build/` 已备份后删除；`TEMP/ref_audio.wav` 已保留 |
| 2026-08-07 | 阶段 1 运行时重建 | 脚本已实现；当前仅有嵌入式 Python 3.9 和系统 Python 3.14，待完整 Python 3.9 环境执行 |
| 2026-08-07 | 阶段 2 模型瘦身 | 已完成：v4、v2Pro、BigVGAN v3 权重已备份后删除；`sv` 权重保留 |
| 2026-08-07 | 阶段 3 tools 瘦身 | 已完成：UVR5、AP_BWE 已备份后删除；默认 v2 TTS 降级兼容；ASR 本地模型不再联网检查 |
| 2026-08-07 | 阶段 4 字典裁剪 | 已完成：日语用户词典、韩语词典与 `eunjeon` 已备份后删除；`wordfreq`、`ipadic` 经回归确认必须保留 |
| 2026-08-07 | 阶段 5 发行脚本 | 已实现：构建、运行时自检与 Inno Setup 脚本；待 Qt6 环境执行 |
| 2026-08-07 | 阶段 6 Git 减负 | 已完成索引移除与忽略规则；未执行历史重写或 `git gc --prune=now` |
