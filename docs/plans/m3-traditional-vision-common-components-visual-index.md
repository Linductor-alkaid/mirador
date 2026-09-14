# M3：传统视觉、检测/OCR 通用组件与视觉索引

> 状态：In Progress
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：M2
> 建议发布点：`v0.1.0-beta.3`
> 更新日期：2026-09-14

## 目标

落地设计 §24 M3 的三条能力线，全部保持纯 CPU、零模型 runtime、零第三方默认依赖：

1. `mirador::geometry` 转编译目标：`LineDetector` SPI 与统一 `LineSegmentSet` 输出、
   线段几何过滤（角度/长度/共线合并）、一方等价线段检测器（梯度区域生长 + 最小二乘
   拟合），满足 `SCOPE-04`「ELSED 或等价线段实现」；ELSED 本体按 `DEC-009` 作为可选
   适配延后引入。
2. `SCOPE-11` 检测/OCR 通用组件（归属 `mirador::image`，`DEC-014`）：letterbox 预处理
   组合、NMS、类别过滤、小目标 crop-refine、DB 后处理（AABB 参考适配）、轮廓框恢复、
   行合并、文本规范化。
3. `SCOPE-03` 视觉索引（归属 `mirador::cache`，`DEC-014`）：精确内容哈希 / 感知哈希 /
   模板匹配三层证据的有界视觉索引；指纹提取与索引存储分离，不改变模块依赖图。
   Embedder 层按 `POST-04` 维持延后。

并用界面图标状态与道路线段两个差异明显的示例证明 Mirador 不是只服务 Android Agent
的专用封装（设计 §24 M3 退出要求）。

## 范围与非目标

范围：`mirador::core` 新增 `make_translation` 工厂与 `VisualPatchFingerprint` 数据契约；
`mirador::geometry` 转编译目标并承载线段 SPI、过滤与检测器；`mirador::image` 新增
letterbox、NMS/类别过滤、DB 后处理/轮廓框恢复、行合并/文本规范化、crop-refine、
视觉指纹提取；`mirador::cache` 新增 `VisualIndex`；架构测试新增 geometry 链接闭包
探针；两个新示例；文档与决策同步。
非目标：ELSED 源码引入（`DEC-009`，可选适配窗口未开）、Embedder Backend 与嵌入
索引（`POST-04`）、证据融合/稳定 ID/`SemanticSnapshot`/SoM 渲染（M4）、缓存命中路径
基准（M5）、DB 任意四边形 unclip 与逐字符 OCR 结果（随首个真实 Backend 引入，
`DEC-014` 参考适配范围）。

## 设计与决策依据

- [设计文档](../design/mirador-development-design.md) §12（视觉索引三层与查询输出）、
  §13（OCR 通用后处理组件）、§14（检测通用组件：letterbox、NMS、类别过滤、
  crop-refine）、§15（`LineDetector`/`LineSegmentSet` 与线段过滤）、§20（预算保护）、
  §24 M3。
- 已生效：`DEC-001`~`DEC-008`、`DEC-012`、`DEC-013`。
- 本里程碑冻结：`DEC-009`（ELSED 集成方式：一方等价实现进 M3，ELSED 本体为可选
  适配延后）、`DEC-014`（`SCOPE-11` 组件归属 `mirador::image`、视觉索引分层契约与
  参考适配范围）。

## 工作项

- [ ] `M3-01` 立项：里程碑文档、`DEC-009`/`DEC-014` 冻结、总计划状态更新。
- [ ] `M3-02` geometry SPI：`LineSegmentSet`、`LineDetectRequest`（ROI、阈值参数、
  取消通道）、`LineDetector` 抽象接口（`info()` + `detect()`）、线段过滤（角度/长度/
  共线合并）；geometry 转编译目标（链接接口恰为 core）并新增链接闭包探针。
- [ ] `M3-03` geometry 一方线段检测器：梯度幅值播种 → 方向对齐区域生长（8 连通）→
  逐区域最小二乘拟合与离群拆分 → 端点投影输出；全整数/双精度确定性算术，无 libm
  热路径；预算保护。
- [ ] `M3-04` image `letterbox`：面积重采样 + 居中填充到目标尺寸，返回缓冲与由实际
  像素操作推导的精确 `Transform2D`；单平面格式。
- [ ] `M3-05` image `nms_indices`（按分数贪心、可选类别感知、确定性次序）与
  `filter_detections`（置信度/类别过滤）。
- [ ] `M3-06` image 共享 8 连通域标记 + `db_postprocess_aabb`（概率图 → 文本框，
  DB unclip 公式的 AABB 参考适配）与 `recover_contour_boxes`（二值图 → 外接框）；
  盒数上限显式 `kBudgetExceeded`。
- [ ] `M3-07` image `merge_text_lines`（同行竖直容差 + 水平间隙合并）与
  `normalize_text`（UTF-8 安全的空白折叠/控制字符清理/全角 ASCII 折叠）。
- [ ] `M3-08` image `refine_small_detections`：候选选择（置信度/面积阈值、数量上限）→
  外扩 ROI 裁剪 → 目标边长重采样 → Backend 格式门控与执行 → 坐标逆变换恢复 →
  确定性合并；候选间轮询 `ExecutionContext`。
- [ ] `M3-09` core `VisualPatchFingerprint` POD 与 image `make_visual_patch_fingerprint`
  （转灰 + 面积归一化 + FNV-1a 内容哈希 + dHash 9x8 + 缩略图字节）。
- [ ] `M3-10` cache `VisualIndex`：字节预算与逐出（LRU）、插入/删除/替换、三层证据
  查询（精确内容哈希 → 感知哈希相似度 → 缩略图 NCC），候选输出有序确定、数量上限。
- [ ] `M3-11` 示例：`road_segments_tour`（合成道路灰度图上线段检测与过滤）与
  `icon_state_index_tour`（合成图标补丁入库与扰动查询复用）；纳入默认构建与编译验证。
- [ ] `M3-12` 收尾：全 Linux 预设矩阵与 lint 通过、跨平台 CI 证据回填、验证记录与
  计划状态更新、CHANGELOG/README 同步。

## 风险与阻塞

- `RISK-2026-01`：MSVC/NDK 跨平台编译证据依赖 CI，受限时按规范记录补跑条件。
- `RISK-2026-02`：ELSED 许可证与集成方式 — 已解除：`DEC-009` 完成审查（Apache-2.0）
  并冻结集成方式；本体引入不再阻塞 M3。
- `RISK-2026-08`：检测/OCR 通用组件与具体模型后处理的参数化差异 — 处置：`DEC-014`
  冻结参考适配范围（AABB 框、8 连通域、参数化阈值；四边形/逐字符留待首个真实
  Backend），组件契约全部显式参数化。
- 新增 `RISK-2026-09`：一方线段检测器在合成图上的召回/精度未与真实场景对齐 — 处置：
  M3 以确定性正确性测试为准，性能与检出质量基准按 `SCOPE-08` 在 M5 评测；触发条件
  （基准不达标）时按 `DEC-009` 窗口引入 ELSED。

## 测试与退出条件

- [ ] 全部 6 个 Linux 预设（debug/release/warnings/asan/ubsan/tsan）配置、构建、ctest
  通过；触及文件 `clang-format`/`clang-tidy` 无告警。
- [ ] LineDetector SPI 可被伪实现注入并经基类指针调用；一方检测器在合成图（水平/
  垂直/对角线段、奇数尺寸、非连续 stride）上确定性检出，输出位稳定。
- [ ] 线段过滤：角度、长度、共线合并的包含/排除边界均有正负用例。
- [ ] letterbox 变换与像素操作一致（缩放/居中偏移按实际 rw/rh 推导），与
  `make_letterbox` 连续映射在 1px 容差内一致；填充区域与预算上限有负向用例。
- [ ] NMS：重叠抑制、分数排序稳定性、类别感知隔离；类别过滤：置信度与类别边界用例。
- [ ] DB 后处理/轮廓框恢复：合成概率图上确定性恢复框，盒数超限显式报错、缓存输入
  不被修改（`RULE-04`/`RULE-06`）。
- [ ] crop-refine：候选选择边界、坐标往返（恢复框与解析期望一致，`DOD-03`）、
  取消/超时零 Backend 调用、预算超限。
- [ ] 视觉索引：三层证据各自的命中/不命中边界（阈值上下）、相同内容精确命中、扰动
  内容感知命中、字节预算逐出与单条目超预算显式错误（`DOD-04` 思路的负向路径）。
- [ ] 架构测试演进后：geometry 链接闭包仅 core + 标准库；公共头与 `src/` 无第三方与
  线程令牌；CI 全部 job 运行。
- [ ] `DEC-009`/`DEC-014` 冻结为 Accepted；设计文档 §12/§14/§15 按冻结契约同步。

## 验证记录

2026-09-14：里程碑创建。依据设计文档 §12/§13/§14/§15/§24 M3 与总计划
`SCOPE-03`/`SCOPE-04`/`SCOPE-11` 拆分工作项 `M3-01`~`M3-12`；`DEC-009` 完成 ELSED
许可证审查（上游 iago-suarez/ELSED 为 Apache-2.0，依赖 OpenCV 4.x）并冻结集成方式；
`DEC-014` 冻结 `SCOPE-11` 归属与视觉索引契约。Embedder 层按 `POST-04` 维持延后。
