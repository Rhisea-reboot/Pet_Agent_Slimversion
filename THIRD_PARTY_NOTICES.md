# VPet 第三方组件声明（Third-Party Notices）

> 更新日期：2026-08-13
> 本文件列出 VPet 项目中随附、依赖或再分发的第三方组件及其许可证，并说明各自的合规义务。
> VPet 自有代码（`src/`、`include/`、`tests/`、`tools/` 等由本仓库作者编写的内容）以根目录
> [LICENSE](LICENSE)（Apache License 2.0）授权。
> 完整许可证全文见根目录 `licenses/` 与各组件自带的许可证文件。

---

## 1. 总览表

| 组件 | 版本/来源 | 许可证 | 形态 | 随发行包分发 |
|---|---|---|---|---|
| Qt 6 | 6.9.2 (MinGW 64-bit) | **LGPL-3.0** | 动态链接 DLL + 插件 | ✅ windeployqt 部署 |
| FFmpeg | Qt Multimedia ffmpeg 后端 | **LGPL-2.1-or-later** | 动态链接 DLL（avcodec/avformat/avutil/swscale/swresample） | ✅ |
| onnxruntime | Microsoft，仓库 vendor | **MIT** | 头文件 + 二进制库 | ✅（本地向量检索） |
| open-webSearch | v2.1.11（仓库 vendor） | **Apache-2.0** | Node.js daemon 源码 | ⚠️ 仅随源码仓库，发行包不内置 |
| VPet 动画资产 | 开源 VPet (C#) 项目内置动画 | **自定义条件授权**（见 §2.6） | PNG 序列帧（约 73MB） | ✅ |
| Kokoro TTS | kokoro 0.7.16 + Kokoro-82M 权重 | **Apache-2.0** | Python 包 + 模型权重 | ✅（runtime/，权重可选内置） |
| sherpa-onnx | 1.13.5 | **Apache-2.0** | Python 包 | ✅（runtime/） |
| SenseVoiceSmall 模型 | sherpa-onnx 导出版 | **MIT**（代码）；以模型卡声明为准 | ONNX 模型 | ✅（models/sensevoice/） |
| BGE 嵌入模型 | BAAI/bge-small-zh-v1.5 | **MIT** | ONNX 模型 | ⚠️ 按需下载（models/embedding/） |
| PyTorch | 2.13.0+cpu | **BSD-3-Clause** | Python 包（torch CPU） | ✅（runtime/） |
| 其他 Python 包 | numpy/scipy/spacy/transformers 等 | 各自许可证 | Python 包 | ✅（runtime/，dist-info 自带） |
| SQLite | — | **Public Domain** | Qt SQL 驱动 + 向量库存储 | ✅ |
| Mesa llvmpipe | Qt 附带 | **MIT** | opengl32sw.dll（软渲染） | ✅ |
| Inno Setup | 构建期工具 | 见其官网 | 安装包制作工具 | ❌ 不随产品分发 |

> 注：发行包 `runtime/Lib/site-packages/` 中每个 Python 包均随附其 `*.dist-info/licenses/` 或
> 包内 LICENSE 文件（组装发行包时整体复制，已核实存在），本文件仅汇总主要组件的许可类型，
> 不作为完整许可文本清单。

---

## 2. 各组件详情与合规义务

### 2.1 Qt 6 —— LGPL-3.0

- **使用方式**：`Core / Gui / Widgets / Network / Multimedia / Concurrent / Sql / Svg`，全部为**动态链接**
  （发行包内 Qt6*.dll 与 platforms/、imageformats/、sqldrivers/ 等插件）。
- **义务**：
  1. 随发行物提供 LGPL-3.0 全文（本仓库 `licenses/LGPL-3.0.txt`，由发行脚本复制进发行包）；
  2. 允许用户替换/重新链接 Qt 库（MinGW 运行时与 DLL 均已随附，天然满足可重链接性）；
  3. 不得修改后仍以 Qt 名义分发；如修改 Qt 源码，需按 LGPL 公开修改。
- **更多信息**：https://www.qt.io/licensing 、https://www.gnu.org/licenses/lgpl-3.0.html

### 2.2 FFmpeg —— LGPL-2.1-or-later

- **使用方式**：Qt Multimedia 的 ffmpeg 媒体后端随附 avcodec-61 / avformat-61 / avutil-59 /
  swresample-5 / swscale-8 动态链接库。
- **义务**：随发行物提供 LGPL-2.1 全文（`licenses/LGPL-2.1.txt`）；允许用户替换库。
- **注意**：若未来改用含 GPL 组件的 FFmpeg 构建，需重新评估许可证义务。
- **更多信息**：https://ffmpeg.org/legal.html

### 2.3 onnxruntime —— MIT

- **来源**：Microsoft onnxruntime（仓库 `vendor/onnxruntime/`，头文件 + 二进制）。
- **用途**：本地 BGE 向量模型推理（长期记忆 embedding）。
- **义务**：保留 MIT 版权与许可文本（`licenses/MIT.txt`，源文本见 `vendor/onnxruntime/LICENSE`）。
- **版权**：Copyright (c) Microsoft Corporation

### 2.4 open-webSearch —— Apache-2.0

- **来源**：固定版本 v2.1.11（仓库 `vendor/open-webSearch/`，含其自带 `LICENSE`）。
- **用途**：本地联网搜索 daemon（默认回环 127.0.0.1:3210，引擎 Bing）。
- **义务**：
  1. 保留其 LICENSE（已在 vendor 目录内随附）与任何 NOTICE；
  2. 如有修改，按 Apache-2.0 标注。
- **分发说明**：该 daemon **不随发行包分发**（发行包无 `vendor/`），联网搜索功能要求用户按
  `docs/open-websearch-local-deployment.md` 单独部署；其 `node_modules/` 内各依赖遵循各自许可证。
- **更多信息**：https://www.apache.org/licenses/LICENSE-2.0

### 2.5 Python 运行时组件

发行包 `runtime/`（自包含 Python 便携运行时）内各组件：

| 组件 | 许可证 |
|---|---|
| torch (2.13.0+cpu) | BSD-3-Clause |
| kokoro (0.7.16) / misaki / Kokoro-82M 权重 | Apache-2.0 |
| sherpa-onnx (1.13.5) | Apache-2.0 |
| numpy / scipy / sympy / joblib | BSD-3-Clause |
| spacy / thinc / pydantic / rich / click | MIT |
| huggingface_hub / transformers / safetensors / fsspec | Apache-2.0 |
| phonemizer (3.4.0) | **GPL-3.0** |
| 其余包 | 各自许可证（dist-info 内） |

- **GPL 边界说明**：`phonemizer` 为 GPL-3.0 的独立 Python 模块，仅作为运行时内被
  Kokoro/misaki 调用的独立程序随附，**不与 VPet 主程序（C++ 二进制）链接或合并**，
  因此不构成对 VPet 自有代码的传染；分发时须保留其 GPL-3.0 许可证文本（dist-info 已自带）。
- **义务**：所有包的许可证文本已随 `runtime/Lib/site-packages/*.dist-info/licenses/` 整体分发，无需额外动作；
  升级/更换依赖时应保持该结构。

### 2.6 VPet 动画资产 —— 自定义条件授权（重要）

- **来源**：开源 VPet (C#) 项目（[LorisYounger/VPet](https://github.com/LorisYounger/VPet)）内置动画
  （`Animation/` 目录：Nomal / Raise / Say / Touch_Body / Touch_Head / Walk 序列帧）。
- **版权归属**：虚拟主播模拟器制作组（Virtual Streamer Simulator Team / VUP-Simulator）。
- **授权条款**（依据 VPet 项目 README「动画版权声明与授权」，非 OSI 开源许可）：
  - **非商业用途**：须向使用者告知动画文件来源，并提供 [VPet 项目页面](https://github.com/LorisYounger/VPet) 的链接；
    满足后即可免费使用。
  - **商业用途**：首次使用时须弹窗醒目告知来源并附链接；在用户可访问的页面告知来源并附链接；
    不得以出售动画文件营利；使用前须邮件联系作者（zoujin.dev@exlb.org）。
  - **转发动画文件**：须告知上述全部授权信息并提供页面链接，禁止任何付费/收费行为。
  - 内置图片版权授权同上。
- **本仓库的落实**：本文件与 README「许可证」章节即为"告知来源并提供链接"的载体，并随发行包分发；
  **若未来进行商业分发，必须先按上款联系原作者并增加首启弹窗**。

### 2.7 模型权重

| 模型 | 许可证 | 位置 |
|---|---|---|
| SenseVoiceSmall（sherpa-onnx 导出，csukuangfj） | MIT（源自 FunAudioLLM/SenseVoice）；以模型卡声明为准 | models/sensevoice/ |
| Kokoro-82M（hexgrad） | Apache-2.0 | models/hf/（HF 缓存） |
| BGE bge-small-zh-v1.5（BAAI） | MIT | models/embedding/（按需下载） |

### 2.8 其他

- **SQLite**：Public Domain；用于 Qt SQL 驱动与记忆向量库（`vectors.sqlite3`）。
- **Mesa llvmpipe**（opengl32sw.dll，Qt 附带软件渲染）：MIT。
- **D3Dcompiler_47.dll / dxcompiler.dll**：Microsoft 可再分发组件，随 windeployqt 部署。
- **Inno Setup**：仅用于构建安装程序（`packaging/VPet.iss`），不随产品分发，遵循其自身许可
  （https://jrsoftware.org/）。
- **LLM API（文本/视觉）**：OpenAI 兼容云端服务，由用户自行配置 API Key；不随分发，但
  **对话内容与屏幕截图会明文发送至用户配置的云端服务**，隐私边界见 README「当前限制」。

---

## 3. 发行合规自查清单

随发行包（`build/Release/VPet-*/` 与安装程序）必须包含：

- [x] `LICENSE`（VPet 自有代码，Apache-2.0）——由 `packaging/Build-Release.ps1` 复制
- [x] `THIRD_PARTY_NOTICES.md`（本文件）——由发行脚本复制
- [x] `licenses/`（LGPL-3.0、LGPL-2.1、MIT 全文）——由发行脚本复制
- [x] Python 运行时各包 dist-info 许可证文本（随 `runtime/` 整体复制）
- [x] 动画来源告知（本文件 §2.6 + README 许可证章节，随发行包分发）
- [ ] **商业分发前**：按 §2.6 联系动画版权方 + 首启弹窗（当前仅非商业使用）

---

## 4. 免责声明

本文件由项目作者依据仓库内证据与上游公开信息整理，**不构成法律意见**。许可证义务以各组件
官方许可证全文与上游项目声明为准；公开分发前如有疑问，请咨询法律专业人士。
