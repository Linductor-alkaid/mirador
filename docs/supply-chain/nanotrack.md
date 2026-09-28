# NanoTrack 候选 port 审计记录（integrations/tracker_nanotrack 参考后端）

> 状态：Active（审查通过）
> 日期：2026-09-28（M7-12 引入审查）
> 负责人：linductor
> 决策依据：[DEC-020](../decisions/DEC-020-tracker-backend-spi.md)（备选更换条款）、
> [DEC-015](../decisions/DEC-015-reference-runtime-selection.md)（integrations 基础设施）
> 运行时依赖：[ncnn](ncnn.md)（pinned `e54f7b1f`，20260526）

## 审查结论

**通过**。候选 NanoTrack ncnn port（下表）许可证为 Apache-2.0（允许再分发与
构建树获取，含专利授权；附 NOTICE/归属义务），无传染性，与仓库既有供应链
口径（BSD-3/Apache 类）兼容；上游仓库可 pin 到精确 commit；模型权重来源与
获取方式明确。按 `DEC-020` 无需启用备选（LightTrack/Ocean ncnn 移植）。

## 引入信息

| 项 | 值 |
| --- | --- |
| 候选 | NanoTrack（HonglinChu，轻量 Siamese 单目标跟踪，参考 SiamBAN/LightTrack） |
| 来源 | https://github.com/HonglinChu/NanoTrack.git |
| 精确 commit | `76b1c6711ae11958ec592e525163826d357d5db5`（master head，2023-06-08） |
| 许可证 | Apache-2.0（仓库根 `LICENSE`，审查时点核对原文） |
| 获取方式 | **不获取**——本仓库对该 port 零构建期依赖（见下"使用范围"）；`deps.lock.json` 因此无新条目 |
| 模型权重 | 同仓库 `ncnn_android_nanotrack/app/src/main/assets/` 下的
`nanotrack_backbone_sim-opt` / `nanotrack_head_sim-opt` param/bin（port 作者转换并随仓库按 Apache-2.0 分发）。本仓库不入仓、不下载、不随任何机制分发；真实权重由使用者在
本机以显式路径提供（`DEC-015` 分层、`RISK-2026-13` 口径） |
| 使用范围 | **仅模型契约对齐**（本文件下方"冻结模型契约"节）：`integrations/tracker_nanotrack/` 的后端为本仓库自研实现，不编译、不 FetchContent、不链接该 port 的任何代码 |

## 与 pinned ncnn 20260526 的兼容核对

- 该 port 的 ncnn 模型仅使用标准层（`Input`/`Convolution`/`Pooling`/`Concat`
  等，无自定义层）；blob 名与拓扑即本仓库冻结模型契约的来源（backbone
  `input`→`output`，head `input1`+`input2`→`output1`+`output2`）。
- pin `e54f7b1f`（20260526）对这些标准层的参数字典完整覆盖；合成模型冒烟在
  该 pin 上实测通过（本地 integrations 构建，`MIRADOR_BUILD_INTEGRATIONS=ON`）。
  未升级 ncnn pin，无需走 ncnn.md 升级流程。
- 已核对的 pinned-ncnn 行为差异（后端实现已按其调整，见后端头注释）：
  extractor 单输出语义（多输出模型逐输出独立 extractor 求值）与 packing
  layout（关闭以保平面 CHW 张量面精确）。

## 分发与义务

- Mirador 核心源码与发布包不包含、不分发该 port 的代码或权重；默认构建
  不获取、不引用（`MIRADOR_BUILD_INTEGRATIONS` 默认 OFF，架构测试锁定
  core 闭包）。
- 本仓库交付的 `integrations/tracker_nanotrack/` 代码为自研（本仓库许可
  覆盖）；仅其"冻结模型契约"文档性对齐该 port。使用者按契约自行获取并
  转换/提供权重时，直接与上游 Apache-2.0 及其模型上游许可发生关系，义务
  在使用者侧，与 `THIRD_PARTY_NOTICES` 第 3 节（integrations 分节）现状
  一致，无新增分发物。

## 冻结模型契约（对齐结论）

- backbone 模型：全卷积（模板 crop 与搜索 crop 共享同一 param/bin），
  输入 blob `input`（RGB 3×side×side，值 0..255 float），输出 blob
  `output`（CHW float 特征）。
- head 模型：输入 blob `input1`（模板特征）+ `input2`（搜索特征）；输出
  blob `output1`（分类响应，2×score_size×score_size，前景 logit 为
  channel 1）与 `output2`（框回归响应，4×score_size×score_size，channels =
  left/top/right/bottom 距离，搜索 crop 像素单位）。
- 解码参考（尺寸/比率惩罚 + cosine 窗 + 行主序首最大 + ltrb 映射回
  prepared 像素空间）以后端头注释为权威；对 port 实现的两处规范化：尺寸
  学习率单次施加（port 的附带双次施加不采纳）、位置先验逐帧由调用方经
  `prior_bounds` 供给（`DEC-020` 契约，port 内部位置状态不迁移）。

## 验证

- 许可证与来源核对：commit、LICENSE 原文、模型文件路径于审查时点经 GitHub
  API 核对（2026-09-28）。
- 默认构建零获取：`MIRADOR_BUILD_INTEGRATIONS=OFF` 的 debug 构建图不含
  `tracker_nanotrack` 与 ncnn（架构测试口径不变）。
- 管线验证：合成模型冒烟（运行时生成权重，零权重入仓）在 integrations
  专用 job 运行；合成口径只验证管线与解码正确性，**不代表跟踪质量**——
  质量证据按 `DOD-05` 归 M7-13 的 D+ 深度基准列报与后续真实数据评估
  （`RISK-2026-14`）。

## 后续更换/升级流程

- 候选更换（许可证失效、上游删除、pinned-ncnn 不兼容且无法在独立 MR 内
  解决）：按 `DEC-020` 备选（LightTrack/Ocean ncnn 移植）同走本 SPI，契约
  不变；更换决策记录进里程碑验证记录并更新本文件。
- 本文件为文档性对齐登记，无构建期 pin 可"升级"；若未来决定 FetchContent
  该 port（当前无此需要），须先补 `integrations/deps.lock.json` 条目与
  integrations CMake 编译期 pin（同 ncnn 机制），并走独立 MR。
