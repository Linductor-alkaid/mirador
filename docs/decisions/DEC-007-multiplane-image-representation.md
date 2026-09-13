# DEC-007：多平面图像（NV12 等）在公共输入模型中的表示

> 状态：Proposed（暂定默认值，冻结里程碑 M1）
> 日期：2026-09-13
> 负责人：linductor
> 冻结里程碑：M1
> 替代/被替代：无

## 背景与问题

设计文档 §6 要求"对于 NV12 等多平面格式……通过扩展的 `ImagePlane` 表示，而不能假定所有
图像只有一个连续平面"。`PixelFormat` 中只有 `kNv12` 是多平面格式，但公共输入模型一旦定
型，后续新增平面格式（如 I420）都会受其约束。M0-03 需要按草案落地该表示。

## 决策（暂定默认值）

1. `ImageView` 固定携带一个 `ImagePlane secondary_plane` 成员：多平面格式（当前仅
   `kNv12`）使用它承载色度平面；单平面格式必须保持默认空值，由 `validate()` 强制。
2. 主平面字段（`data`/`row_stride_bytes`）仍直接位于 `ImageView` 上，单平面格式零额外
   开销，与设计 §6 的代码示例保持一致。
3. 平面语义由特征函数表达：`plane_count(format)`、`bytes_per_pixel(format)`（主平面）、
   `min_row_stride_bytes(format, plane, width)`；所有平面 stride 非负，负 stride 的
   自底向上布局由平台适配层归一化。
4. 视图维度描述"呈现后的视图"（应用 `rotation` 之后），不描述原始采集帧。

## 备选方案

- `std::vector<ImagePlane> planes`：任意平面数、表达力最强——被否：视图是按值传递的
  轻量非拥有类型，堆分配引入分配、失效与拷贝语义负担，而当前格式集合最多 2 平面。
- 固定 `std::array<ImagePlane, N>`（N≥3）：为尚不存在的格式预付复杂度，且仍需"哪些
  索引有效"的约定，本质与 2 等价。
- 为多平面格式定义独立类型（如 `Nv12View`）：Backend SPI 需要同时接受两类视图，接口
  翻倍，违背单一输入模型目标。

## 影响与风险

- 若未来引入 ≥3 平面格式（I420 等），必须修订本决策并同步 `validate()`、特征函数与
  架构测试；在此之前不预付复杂度。
- `secondary_plane` 对单平面格式必须为空的约束是新增的合法性规则，调用方从聚合初始化
  迁移时可能遇到显式校验失败，属预期行为。
- 最迟冻结里程碑 M1：颜色转换与差分实际消费平面数据时定稿。

## 验证方式

`M0-03` 测试矩阵覆盖：NV12 双平面（奇数高度、最紧/宽松 stride）通过校验；NV12 缺失
secondary plane、secondary stride 过小、单平面格式携带 secondary plane 均返回
`kInvalidArgument`；特征函数对全部格式与奇数宽度给出正确最小 stride。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §6；总计划 `RULE-04`、`DEC-007` 行；
`M0-03`、`M1` 颜色转换与差分工作项。
