# DEC-007：多平面图像（NV12 等）在公共输入模型中的表示

> 状态：Accepted（2026-09-14，M1 内冻结）
> 日期：2026-09-13（创建）；2026-09-14（冻结）
> 负责人：linductor
> 冻结里程碑：M1
> 替代/被替代：无

## 背景与问题

设计文档 §6 要求"对于 NV12 等多平面格式……通过扩展的 `ImagePlane` 表示，而不能假定所有
图像只有一个连续平面"。`PixelFormat` 中只有 `kNv12` 是多平面格式，但公共输入模型一旦定
型，后续新增平面格式（如 I420）都会受其约束。M0-03 按草案落地该表示；M1 的颜色转换与
裁剪首次实际消费平面数据，按计划在本里程碑内冻结。

## 决策

1. `ImageView` 固定携带一个 `ImagePlane secondary_plane` 成员：多平面格式（当前仅
   `kNv12`）使用它承载色度平面；单平面格式必须保持默认空值，由 `validate()` 强制。
2. 主平面字段（`data`/`row_stride_bytes`）仍直接位于 `ImageView` 上，单平面格式零额外
   开销，与设计 §6 的代码示例保持一致。
3. 平面语义由特征函数表达：`plane_count(format)`、`bytes_per_pixel(format)`（主平面）、
   `min_row_stride_bytes(format, plane, width)`；所有平面 stride 非负，负 stride 的
   自底向上布局由平台适配层归一化。
4. 视图维度描述"呈现后的视图"（应用 `rotation` 之后），不描述原始采集帧。
5. **冻结时修正（2026-09-14）**：NV12 色度行承载 `ceil(width / 2)` 个交错 UV 对，因此
   最小色度行 stride 为 `width + (width % 2)`（奇数宽度需要 1 个尾部字节）。M0 草案的
   "色度行 = width 字节"在奇数宽度下无法容纳最后一个 V 分量，属实现缺陷，随本决策冻结
   一并修正（`pixel_format`、`ImageBuffer`、颜色转换与裁剪同步更新，M0 测试矩阵相应
   重新基线，见 M1 里程碑验证记录）。
6. 拥有式缓冲 `ImageBuffer`（M1-01）按同一规则布局：主平面行（紧凑 stride）之后连续
   存放色度行，`view()` 输出的 secondary_plane 语义与第 5 条一致。

## 备选方案

- `std::vector<ImagePlane> planes`：任意平面数、表达力最强——被否：视图是按值传递的
  轻量非拥有类型，堆分配引入分配、失效与拷贝语义负担，而当前格式集合最多 2 平面。
- 固定 `std::array<ImagePlane, N>`（N≥3）：为尚不存在的格式预付复杂度，且仍需"哪些
  索引有效"的约定，本质与 2 等价。
- 为多平面格式定义独立类型（如 `Nv12View`）：Backend SPI 需要同时接受两类视图，接口
  翻倍，违背单一输入模型目标。
- 保留"色度行 = width 字节"：奇数宽度下色度数据越界，被否（缺陷）。

## 影响与风险

- 若未来引入 ≥3 平面格式（I420 等），必须修订本决策并同步 `validate()`、特征函数与
  架构测试；在此之前不预付复杂度。
- `secondary_plane` 对单平面格式必须为空的约束是新增的合法性规则，调用方从聚合初始化
  迁移时可能遇到显式校验失败，属预期行为。
- 色度 stride 取偶规则使奇数宽度的 NV12 缓冲比 M0 草案多分配每行 1 字节；对外部
  NV12 缓冲的包装（平台适配）必须遵守同一最小 stride。

## 验证方式

`M0-03` 测试矩阵（奇数高度、最紧/宽松 stride、缺失 secondary plane、单平面携带
secondary plane）继续通过；M1 新增：奇数宽度最小色度 stride 断言（`width=7 → 8`）、
`ImageBuffer` NV12 布局断言（色度平面偏移与 stride）、颜色转换/裁剪的奇数 ROI 色度
子采样正确性。全部见 `tests/core/image_view_test.cpp`、`tests/image/image_buffer_test.cpp`、
`tests/image/color_convert_test.cpp`、`tests/image/crop_test.cpp`。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §6；总计划 `RULE-04`、`DEC-007` 行；
`M0-03`、`M1-01`~`M1-03`。
