# DEC-016：kDisplay 坐标空间变换来源与融合开放契约

> 状态：Accepted（2026-09-15，M5-05 内冻结）
> 日期：2026-09-15
> 负责人：linductor
> 冻结里程碑：M5
> 替代/被替代：无

## 背景与问题

设计 §7 定义了五个已知坐标空间，其中 `kDisplay`（最终显示空间）在 M4 冻结时被显式
挂起：融合输入坐标空间限定 kFrame 与 kOriented，"kDisplay 待 M5 平台适配定义变换来源
后开放"（设计 §16 M4 冻结补充）。`EvidenceSet::add_*` 与 `fuse_evidence` 因此拒绝
kDisplay。

kDisplay 与 kFrame↔kOriented 有本质差异：后者由 `Frame` 自带的 rotation 唯一确定，
而"采集帧在屏幕上的位置与缩放"没有 core 内的权威来源——窗口原点、虚拟屏幕偏移、
DPI 缩放都只有平台适配层知道。M5-05 要求把该变换来源契约落地，否则上层 Agent 拿到的
区域无法安全映射为点击坐标。

## 决策

1. **变换来源是适配层/调用方提供的 `Transform2D`**，方向冻结为 `kOriented → kDisplay`
   （与 `Transform2D` "正向预处理方向"约定一致：外部区域自显示空间进入管线时取逆，
   输出恢复时正向使用）。适配层（如 X11 窗口采集的窗口原点平移）负责产出该变换；
   core 不从平台 API 推断它。
2. **`FusionOptions` 新增 `std::optional<Transform2D> display_transform`**：
   - `from/to` 必须恰为 kOriented→kDisplay，矩阵元素必须有限，否则 kInvalidArgument；
     奇异矩阵在求逆时按既有契约报 kCoordinateTransform。
   - 证据项与目标空间允许 kDisplay；只要任一证据项或目标空间为 kDisplay，
     `display_transform` 必须存在（即使无需实际转换），保证 kDisplay 快照总是自描述的。
   - 转换链：kDisplay↔kOriented 经 `display_transform`（或其逆），kDisplay↔kFrame
     再复合视图旋转。
3. **`EvidenceSet::add_*` 接受 kDisplay**（kFrame/kOriented/kDisplay 三者）；空间与
   变换的一致性由消费方 `fuse_evidence` 校验，`EvidenceSet` 保持纯容器语义。
4. **`run_ocr`/`run_detector` 输出空间保持 {kFrame, kOriented, kCropped} 不变**：
   Backend 坐标恢复属于帧族空间；显示映射是融合输出的职责，调用方需要时对结果施加
   `display_transform` 即可。这是刻意的边界收窄，避免预处理链与显示缩放语义纠缠。
5. 快照 `coordinate_space` 允许为 kDisplay；stable ID 跟踪、generation 与融合门控
   （IoU/包含均在同一目标空间内计算）不受影响。

## 备选方案

- core 从平台查询显示几何（如 Xinerama/DisplayMetrics）：core 必须链接平台 API，
  违反 `RULE-01`/`RULE-02`，被否。
- `EvidenceSet` 携带 per-item 任意 Transform：证据集退化为变换计算器，坐标语义分散
  在每个条目上，门控正确性难以保证，被否。
- kDisplay 仅作为适配层私有空间（kUserBase 起）不动 core：M4 冻结说明明确要求
  "开放" kDisplay，且五空间枚举已在设计 §7 公开，私有空间方案无法让公共快照自描述，
  被否。

## 影响与风险

- M4 的 kDisplay 拒绝断言（`evidence_set_test`、`perception_session_test`）按新契约
  更新；融合转换链新增 4 条路径，坐标正确性由方向与往返容差测试矩阵覆盖（`DOD-03`：
  0/90/180/270 度旋转复合、往返误差容限）。
- 适配层提供错误变换（错原点/错缩放）时 core 无法察觉——契约上这是调用方输入错误，
  与错误 ROI 同级；冒烟测试断言适配层产出的变换与几何查询一致。

## 验证方式

`M5-05`：kDisplay 方向（kFrame/kOriented/kDisplay 两两转换）与往返容差测试、
缺失/错向/奇异变换负路径、session 融合端到端（`DOD-03`）；X11 冒烟断言
`display_transform` 与窗口几何一致。见 `tests/fusion/`、`tests/adapters/`。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §7、§16（M4/M5 冻结补充）；
[DEC-012](DEC-012-backend-spi-contract.md)（请求层空间契约）；
[DEC-013](DEC-013-perception-session-in-fusion.md)；总计划 `M5-05`、`RULE-05`。
