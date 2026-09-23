# 调研报告：trycua/cua 视觉能力实现

> 状态：Active（信息性调研，本身不构成契约；契约变更经 [DEC-021](../decisions/DEC-021-backend-output-trust-boundary.md) 落地）
> 日期：2026-09-23
> 调研对象：[trycua/cua](https://github.com/trycua/cua) `17c5444375ca8b5213a0c29982166246c052c4df`（浅克隆，文件路径以该修订为准）
> 调研方式：源码级阅读（克隆 + 逐文件），未运行其代码；所有行为描述的证据等级为"源码支持"
> 关联文档：[设计文档](../design/mirador-development-design.md) §7/§9/§16/§17/§23；[DEC-012](../decisions/DEC-012-backend-spi-contract.md)、[DEC-016](../decisions/DEC-016-display-space-transform-contract.md)

## 1. 调研目的

cua（原 c/ua）是与 Mirador 处在同一问题域的成熟开源项目：把桌面/终端画面转换为可供 Agent 使用的结构化视觉信息，并驱动输入动作。Mirador 已完成变化检测、Backend SPI、缓存、融合、稳定 ID 与 SoM（M0–M5，M7 进行中），本次调研回答三个问题：

1. 同类项目如何组织"截屏 → 视觉解析 → 结构化区域 → 支撑动作"链路，有哪些 Mirador 尚未固化的工程契约？
2. Mirador 的核心闭环（变化门控、结果复用、稳定 ID）在同类项目中处于什么位置？
3. 哪些机制值得吸收进 Mirador 的决策与设计，哪些因边界不同而不适用？

## 2. 视觉链路三代演进

cua 仓库内并存三代视觉实现，分别对应不同的成熟度与依赖策略：

### 2.1 第一代：Python `som` 包（OmniParser 路线）

`libs/python/som/`。YOLO 模型检测可交互图标，EasyOCR 识别文本；两路结果先做"文本框中心点落入图标框则丢弃该图标框"的去重，再用 NMS 合并。输出 `UIElement{type: icon|text, bbox(归一化), confidence, interactivity}`，同时渲染标注图供 VLM 消费。依赖 torch/ultralytics（AGPL 义务），是该仓库正在淡出的路线。

### 2.2 第二代：Rust `cua-driver` + `cua-perception` 扩展（当前主力）

`libs/cua-driver/`。driver（Rust 守护进程，MCP over stdio）拥有截屏、输入注入、Accessibility 与浏览器状态；视觉解析被拆分为**独立进程的 ONNX worker**（`rust/crates/cua-perception`），通过长度前缀 stdio 帧协议通信。推理管线为自研组装：YOLO 系图标检测 + PP-OCR 系 DB 文本检测（概率图 → unclip 框）+ CTC 识别器，纯 CPU。模型与 ONNX Runtime 库均经 SHA-256 manifest 校验、版本精确匹配后加载，worker 无网络、无 Python 依赖。默认分发只有确定性 fixture 后端（只接受固定测试图并返回固定结果），生产推理是显式安装、逐 artifact 记账的扩展。

### 2.3 第三代：`cua-s1` 小型专用决策模型

`libs/cua-s1/`。研究方向：855K 参数的 option-attention 分类器与 Qwen 4B LoRA 适配器，对屏幕状态一次性给所有 (element, action) 候选打分。定位为"System 1 快决策"，其 runtime 集成同样强制 snapshot 绑定的 element token 与动作后重观测。

## 3. 值得吸收的工程机制

### 3.1 capture 绑定的一次性动作授权

`docs/perception-extension.md`（driver 仓库内）定义了"观测 → 动作"的信任协议：

- 每次截屏登记为带 `capture_id` 的 capture；解析结果绑定该 `capture_id`。
- 从区域派生的像素点击必须携带同一 `capture_id`；**一个 capture 至多派生一个动作**。
- 动作、超时、结果未知、窗口尺寸/位置/焦点变化之后必须重新截屏；禁止对过期、陈旧或世代不匹配的 capture 重试动作。
- 陈旧性有显式错误码：`capture_expired` / `capture_stale` / `capture_generation_mismatch` / `capture_not_found`。
- "worker 失败、超时、资源超限后不得从部分结果行动"是硬性规则。

这是 Mirador generation 陈旧性模型的**动作侧延伸**：Mirador 的 generation 防"读陈旧快照"，cua 证明同一机制可以防"基于陈旧观测执行动作"。

### 3.2 坐标契约（`rust/crates/cua-driver-contract/src/visual.rs`）

- **半开像素边界**：`VisualRegionBounds` 是 `[x, x+width) × [y, y+height)`，文档注释精确到"覆盖所有中心落在半开框内的像素；几何中心可能落在像素之间"。
- **半像素中心不取整**：`center()` 返回 `(x + width/2.0, y + height/2.0)` 的 f64；奇数尺寸产生半像素中心是**有意行为**；注释明确"DTO 层从不取整，取整只发生在受平台动作契约约束的原生分发层，且在 V1 之外"。
- **动作空间自描述**：每个 capture 携带 `screenshot → action` 的 2×3 仿射变换（处理 HiDPI/缩放）；恒等时声明为 `ScreenshotPixels`，否则 `Affine{m11,m12,m21,m22,tx,ty}`。与 Mirador `DEC-016` 的 `display_transform` 同构，但作为 capture 溯源的一部分随结果下发。
- **输出几何显式声明**：结果携带 `text_geometry: "axis_aligned_bounds"` 与 `limitations: ["rotated_text_is_returned_as_an_axis_aligned_bound"]`——把"已知输出局限"作为身份的一部分上报。

### 3.3 资源预算协议化（`cua-perception/src/lib.rs`）

所有防护不是散在实现里，而是写入协议与清单：帧上限 16 MB、图像 8 MB、最大边 8192、最大 32M 像素、解码内存上限；请求带 `kinds`/`min_confidence`/`max_regions` 过滤；检测后处理带 `max_candidates`/`max_detections`；capture registry 为 TTL（默认 60 s）+ `max_captures`（默认 32）的有界惰性清理。另有**声明-一致性校验**：声明的尺寸/字节长度必须与实际解码结果一致（base64 长度、PNG 头尺寸逐一核对），防 zip-bomb 与元数据欺骗。层级区域被过滤时同步清理悬挂的 `parent_id`/`group_id` 引用。

### 3.4 实现身份溯源（`VisualParserMetadata`）

每次解析结果携带完整身份链：extension id/version、model id/version、`model_source_revision`、`model_manifest_sha256`、ONNX Runtime 版本与库 sha256、fixture sha256，外加 runtime 与 `text_geometry` 声明。`cua-perception/src/runtime.rs` 在加载时按 manifest 逐一校验模型的输入输出名/形状/类别数与 runtime 版本，不匹配即拒绝启动；`self_test` 提供"manifest 哈希 → 会话 IO → 合成推理"三级自检。

### 3.5 worker 回声校验（`cua-driver-core/src/perception_tools.rs`）

driver 采纳 worker 结果前验证其**回声**：回显的 `capture_id`、图像 sha256、宽高、`coordinate_space` 必须与登记的 capture 一致，否则按 `artifact_invalid` 整体拒绝。核心思想：**结果与输入的绑定关系由调用方校验，不信任后端自觉**。

### 3.6 确定性合成质量语料（`cua-perception/tests/quality-corpus/`）

九张纯标准库生成的确定性合成截图（原生控件、canvas、远程桌面风、深浅主题、高 DPI、小字、纯图标、OCR/控件框重叠、空表面、噪声），`manifest.json` 作为封闭契约记录 SHA-256、尺寸、场景标签与期望标注；评分器按 IoU ≥ 0.5 一一匹配；`generate.py --check` 校验字节级可复现。感知质量回归因此不依赖真实截图（无隐私入仓问题），且生成器本身可审计。

### 3.7 评测分类学（`cua-s1/evals/metrics.py`）

把"准确"拆开统计：accuracy、abstention（弃权率）、coverage、wrong_action、wrong_target、以及"该弃权时行动"单独计数，并按桶分组输出。避免单一准确率掩盖"错目标"与"不敢动"两类性质不同的失败。

### 3.8 许可证分级账本

`docs/perception-third-party-notices.md` 按 **artifact** 记账：OmniParser 检测权重 AGPL-3.0-only、PP-OCR 检测/识别 Apache-2.0、ONNX Runtime 以精确版本 + 哈希记账，发布需附 notices、SBOM 与对应源码要约。粒度是"每个模型 artifact"而非"每个依赖包"。

## 4. 与 Mirador 现状的对照

### 4.1 Mirador 已覆盖、无需变更

| cua 机制 | Mirador 现状 |
| --- | --- |
| 半开像素边界 `RectF`/`RectI` | `include/mirador/geometry.hpp` 已固定同语义（M3-02），含双精度边缘比较 |
| 模型修订使缓存失效 | `BackendInfo.model_id/model_revision` 全部参与能力结果缓存键（RULE-07，M2） |
| 显示空间仿射变换 | `DEC-016` `FusionOptions::display_transform`（kOriented→kDisplay）已冻结 |
| 确定性 fixture 后端 | M2 起 Fake Backend 注入固定结果是强制测试方式 |
| 输入尺寸防护与预算 | §20 + `kBudgetExceeded` + `DEC-008` 字节预算 |
| 变化门控与结果复用 | §10/§11 + M7 变化门控短路（m7-03），**cua 完全没有这一层**（见第 5 节） |
| 证据有限性校验 | `src/fusion/evidence.cpp` `validate_bounds` 已拒绝非有限/负尺寸 |

### 4.2 本次吸收（经 DEC-021 与设计修订落地）

1. **Backend 输出信任边界**（§3.5 回声校验的 Mirador 化）：Backend 结果按不可信输入处理，非法区域在证据采纳处显式拒绝，不钳位、不部分采纳——现状已实现，冻结为契约。
2. **`BackendInfo::known_limitations` 限制自述**（§3.2/§3.4 的 Mirador 化）：声明式输出限制（如"旋转文本仅返回轴对齐外接框"），供诊断与文档消费；不参与缓存键与门控。
3. **观测绑定动作协议**（§3.1 的边界内部分）：Mirador 不执行动作，但把"每个已发布快照至多派生一个动作、动作/超时/未知结果后必须重新提交帧"冻结为对调用方的协议建议，原语即既有 `generation` + stable ID。
4. **坐标中心语义文档化**（§3.2 的增量部分）：几何中心保持连续坐标（`x + width/2`），奇数尺寸产生半像素中心是有意行为，管线内不取整，取整责任在调用方动作分发层。
5. **合成质量语料与评测分类**（§3.6/§3.7）：写入设计 §23 作为测试设计要求，随首个真实感知质量评测工作项立项。

### 4.3 不适用或越界（明确不吸收）

- **进程外 worker 与 stdio 帧协议**：Mirador Backend 是进程内同步 SPI（`DEC-001`/`DEC-012`）；cua 拆进程的动因（AGPL 隔离、崩溃隔离、MCP 生态）不适用于核心。集成层若需要，可在自己的 Backend 实现内自选进程模型，核心不感知。
- **动作执行与 capture registry**：动作执行、截屏保留、TTL 注册表是调用方/适配层职责（AGENTS.md 边界）；Mirador 只提供 generation 与指纹原语。
- **OmniParser 权重**：AGPL-3.0-only，Mirador 适配包不应引入；`DEC-014` 已把 OmniParser 类模型限定为评测候选。
- **`VisualParserMetadata` 全字段照搬**：Mirador Backend 在进程内、artifact 完整性由集成层与供应链文档（§22）负责，`model_revision` 字符串足以承载摘要身份；新增结构化身份链是过度设计。

## 5. cua 的空白与 Mirador 的差异化

- **无变化检测与复用闭环**：agent 主循环是"动作 → 固定 `sleep(0.5)` → 截屏 → 全量重解析"；driver 层无帧间 diff、无哈希短路。画面未变时依然全量重跑 OCR/检测。Mirador 的变化门控短路（m7-03）与全局平移估计（m7-04，滚动场景下复用未滚出视口的区域证据）正是补这个缺口的，且已有同行项目背书"没有它，每次感知都是全价"。
- **视觉侧 `interactive` 恒为 false**：可交互性完全依赖 Accessibility 融合判定，印证 Mirador"视觉推测不伪装成平台保证"（§8）与多源证据分层的架构判断。
- **fixed-delay 重观测**：cua 用固定延时 + capture 过期近似"画面稳定"，Mirador 的 ChangeReport 提供的是内容驱动的等价判断，成本与准确性上限更高。

## 6. 结论

cua 的强项是**安全边界与契约工程**（capture 绑定动作授权、身份溯源、预算协议化、合成语料），弱项是**感知经济学**（无变化检测、无复用）。前者中的四项已通过 [DEC-021](../decisions/DEC-021-backend-output-trust-boundary.md) 与设计文档 §7/§9/§16/§23 修订吸收；后者确认了 Mirador 核心闭环的差异化价值，m7-03/m7-04 方向不变。

后续建议（不在本次范围，供立项参考）：

- 建立 Mirador 感知质量合成语料（§3.6 模式）作为首个真实 OCR/Detector Backend 集成（`POST-04` 邻近工作）的验收基础设施。
- 若未来引入层级区域（`parent_id`/`group_id`），同步吸收"过滤时清理悬挂引用"规则（§3.3）。
- `integrations/` 的参考 Backend 发布时按 artifact 粒度建立许可证账本（§3.8），与 `THIRD_PARTY_NOTICES` 对齐。
