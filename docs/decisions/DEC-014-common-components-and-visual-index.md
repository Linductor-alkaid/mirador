# DEC-014：检测/OCR 通用组件归属与视觉索引契约

> 状态：Accepted（2026-09-14，M3 内冻结）
> 日期：2026-09-14
> 负责人：linductor
> 冻结里程碑：M3
> 替代/被替代：无

## 背景与问题

总计划 1.1 修订将设计 §13/§14 的检测/OCR 通用组件并 M3（`SCOPE-11`），但明确「模块
归属随 M3 立项确定」；设计 §12 的视觉索引（精确哈希/感知哈希/模板匹配）落点为
`mirador::cache`，但其指纹归一化依赖 `mirador::image` 的转换/重采样能力，而模块依赖
图（设计 §5，`DEC-013` 允许集合表）中 image 与 cache 互不依赖。M3 立项必须决定：

1. `SCOPE-11` 八个组件（letterbox、NMS、类别过滤、小目标 crop-refine、DB 后处理、
   轮廓框恢复、行合并、文本规范化）的模块归属；
2. 视觉索引的分层契约与 image/cache 能力切分；
3. 参考组件与具体模型后处理的适配范围（总计划 `RISK-2026-08`）。

## 决策

### 1. `SCOPE-11` 全部归属 `mirador::image`

`mirador::image` 的章程从「缩放、裁剪、颜色转换、差分和哈希」（设计 §5）扩展为
「纯 CPU 图像操作与检测/OCR 通用组件」：

- **像素域组件**（letterbox、DB 后处理、轮廓框恢复）输入是图像/概率图，需要复用
  image 的格式转换、面积重采样与缓冲管理；DB 后处理与轮廓框恢复共享 8 连通域标记
  机制，必须同模块实现。
- **结果域组件**（NMS、类别过滤、行合并、文本规范化）与上述像素域组件构成同一条
  检测/OCR 后处理参考链，与 crop-refine（裁剪 + 重采样 + Backend 回调）一起保持
  集合内聚，避免把一条参考管线拆到三个模块。
- `mirador::geometry` 保持线段语义（设计 §15），不承载框/文本组件。

### 2. 视觉索引：指纹提取与索引存储分离，无依赖图变更

- `mirador::core` 新增数据契约 `VisualPatchFingerprint`（POD）：FNV-1a 64 内容哈希、
  dHash 9x8 感知哈希、归一化灰度缩略图字节（纯数据，无逻辑）。
- `mirador::image` 新增 `make_visual_patch_fingerprint`：补丁转灰 + 面积重采样归一
  到索引边长 + 计算两级哈希 + 物化缩略图（复用既有 `convert_color`/`resize_area`/
  `dhash` 公共 API）。
- `mirador::cache` 新增 `VisualIndex`：字节预算与 LRU 逐出、插入/替换/删除；查询按
  三层证据输出候选——`kExactContent`（内容哈希相等且缩略图逐字节相等）、
  `kPerceptualHash`（dHash 汉明相似度 ≥ 阈值）、`kTemplate`（缩略图 NCC ≥ 阈值），
  证据层优先级与相似度决定排序，数量上限显式。查询输出是候选列表 + 相似度 + 证据
  类型，不宣称语义相同（设计 §12）。
- image 与 cache 仍都只依赖 core（`DEC-013` 允许集合表不变）；Embedder 层按
  `POST-04` 延后，索引 API 不预留 runtime 类型。

### 3. 参考适配范围（`RISK-2026-08` 收口）

| 组件 | M3 参考实现 | 显式留待首个真实 Backend |
| --- | --- | --- |
| DB 后处理 | 概率图（Gray8）→ 8 连通域 → AABB 框 + 均值分数；unclip 采用 DB 公式的 AABB 形式（`area·ratio/perimeter` 外扩） | 任意四边形/旋转框、polygon 输出、逐字符结果 |
| 轮廓框恢复 | 二值图 → 8 连通域外接 AABB | 亚像素轮廓、旋转矩形 |
| 行合并 | 同行判定（竖直中心容差 + 水平间隙，参数化）+ 区间并 | 段落/栏结构分析 |
| 文本规范化 | UTF-8 安全的空白折叠、C0/C1 控制字符清理、全角 ASCII 折叠（含 U+3000） | Unicode NFC/NFKC 归一化 |
| letterbox | 单平面格式，任意通道同值填充，变换由实际 rw/rh/偏移推导 | NV12 色度填充（沿用 `convert_color` 的色度写入待决） |
| NMS | 分数贪心，IoU 阈值，可选类别感知 | Soft-NMS、旋转框 IoU |
| crop-refine | 置信度/面积阈值选候选 → 外扩裁剪 → 目标边长重采样 → Backend 重识别 → 逆变换恢复 | 多轮细化预算策略（属于上层调度，设计 §10） |

全部组件参数显式、确定性输出；候选数量、缓冲与迭代均有上限，超限显式
`kBudgetExceeded`（`RULE-06`）。

## 备选方案

- 轮廓框恢复放 `mirador::geometry`（贴合 §5「线段与轮廓」举例）：与 DB 后处理共享
  连通域机制会跨模块复制或引入 geometry→image 依赖（违背依赖图）；集合内聚优先，
  §5 的「等传统视觉能力」为非穷举示例；被否。
- 视觉索引整体放 `mirador::image`：违背设计 §5/§12 对 cache 章程的指定；被否。
- 允许 cache→image 依赖边：把 image 变成 cache 的底层依赖，破坏两个兄弟模块在
  fusion 汇合的依赖图，且仅为复用两个函数；POD 契约上移 core 的成本更小；被否。
- 检测/OCR 组件新建 `mirador::vision` 模块：模块清单是设计 §21 冻结的稳定目标集合
  （`DEC-013` 同理）；被否。

## 影响与风险

- `mirador::image` 公共 API 面显著扩大（预计新增 6 个公共头）；全部保持
  core-only 依赖，架构测试不需要新允许集合。
- `RISK-2026-08` 关闭：参考适配范围已冻结，四边形/逐字符等扩展以「首个真实
  Backend」为触发条件，按工程规范变更流程演进。
- `VisualPatchFingerprint` 进入 core 公共 API，属增量数据契约；M0 类型不受影响。

## 验证方式

`M3-04`~`M3-10` 各组件单元/属性测试（确定性、边界阈值、预算负向路径、坐标往返）；
`M3-02`/`M3-12` 架构探针与全预设矩阵；`M3-11` 两个示例纳入默认构建。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §5、§12、§13、§14、§24 M3；总计划
`SCOPE-03`/`SCOPE-11`、`RISK-2026-08`；`DEC-013`（允许集合表）；`M3-02`~`M3-11`。
