# 公共 API 接口参考

Mirador 的公共 API 即 [`include/mirador/`](../../include/mirador/) 下的头文件；每个
声明的权威契约（语义、错误码、预算、不变量）在头文件注释内维护，本文是模块级导航
索引，不复制契约正文。接口变更受 [工程规范](../project/project-standards.md) 约束：
公开契约变更必须同步决策记录与设计文档。

## 模块与头文件

### mirador::core（零依赖基础层）

| 头文件 | 内容 |
| --- | --- |
| `status.hpp` / `result.hpp` | `Status`/`ErrorCode`/`Result<T>` 错误模型（`DEC-004`，无异常边界） |
| `image_view.hpp` / `frame.hpp` / `image_buffer.hpp` | 非拥有图像视图、帧上下文与生命周期（设计 §6） |
| `pixel_format.hpp` | 像素格式与多平面表示（`DEC-007`） |
| `geometry.hpp` | `PointF`/`RectI`/`RectF`/`LineSegment` 与基础几何谓词 |
| `transform.hpp` | `CoordinateSpaceId`、`Transform2D`、compose/inverse 与点/矩形/线段映射（设计 §7，`RULE-05`） |
| `backend_info.hpp` | `BackendInfo` 能力与线程安全声明（`DEC-012`） |
| `execution_context.hpp` | 取消与 deadline 的轻量通道（`RULE-03`） |
| `ocr_backend.hpp` / `detector_backend.hpp` / `line_detector.hpp` | 同步 Backend SPI：请求分层、prepared 空间输出契约（`DEC-012`，设计 §9/§13/§14/§15） |
| `version.hpp` | 库版本查询 |

### mirador::image（变换与变化检测）

`color_convert.hpp`、`resize.hpp`、`letterbox.hpp`、`crop.hpp`、`crop_refine.hpp`、
`transform.hpp`（图像级重采样入口）、`fingerprint.hpp`、`patch_fingerprint.hpp`、
`visual_fingerprint.hpp`、`change_detection.hpp`（分层变化检测与忽略区域，设计 §11）、
`detection_postprocess.hpp`（NMS/类别过滤）、`text_postprocess.hpp`（DB 后处理、
轮廓框恢复）、`text_normalize.hpp`、`grid_partition.hpp`、`frame_cache.hpp`（有界帧级缓存）。

### mirador::cache（有界缓存与视觉索引）

`cache_policy.hpp`（读写/刷新策略）、`capability_cache.hpp`（能力结果缓存：
`RULE-07` 键字段、128 位摘要、字节预算，设计 §12）、`visual_index.hpp`（三层
证据的有界视觉索引：精确哈希/感知哈希/模板 NCC，`DEC-014`）。

### mirador::geometry（线段检测与几何过滤）

`segment_growing_line_detector.hpp`（一方确定性线段检测器）、`line_detector.hpp`
SPI 的过滤/共线合并自由函数（`filter_segments`、`merge_collinear`，设计 §15，
`DEC-009`）、`geometric_proposal.hpp`（`propose_regions`、
`GeometricRegionProposal`，闭合/近闭合结构 → closure/rectangularity/edge_support
评分与 Tight/Context 双 ROI，设计 §24 M6、issue #11；M6 实验轨道交付，
`DEC-018` 阶段 1 冻结为正式契约，计入兼容性承诺）。

### mirador::fusion（融合、稳定 ID 与会话）

`evidence.hpp`（`EvidenceSet`/`EvidenceItem`，kFrame/kOriented/kDisplay 三空间）、
`fusion.hpp`（`fuse_evidence`/`FusionOptions`/`FusionTrace`，kDisplay 变换契约见
`DEC-016`）、`semantic_snapshot.hpp`（`RegionSource` 位掩码、`VisualRegion`、
`SemanticSnapshot`、generation 校验）、`stable_id_tracker.hpp`（`DEC-010`）、
`object_tracker.hpp`（跨帧目标跟踪：`TrackState`/`TargetTrack` 有界目标池与
`ObjectTracker` 生命周期，设计 §24 M7、`DEC-019`；**Experimental**：随 M7
go/no-go 判定冻结，冻结前不计兼容性承诺）、
`perception_session.hpp`（感知入口：变化分析 → 按需 Backend → 坐标恢复 → 缓存 →
融合发布，`DEC-013`）。

### mirador::render（SoM 与调试）

`set_of_mark.hpp`（标号图像与 `mark_id -> stable_id` 映射）、`grid_partition.hpp`
（纯几何网格划分与坐标回映，设计 §17）。

## 适配层与 integrations（默认构建零获取）

| 目录 | 内容 | 开关 |
| --- | --- | --- |
| `adapters/opencv/` | `cv::Mat` ↔ `ImageView` 互操作（`RULE-02`） | `MIRADOR_BUILD_ADAPTERS_OPENCV` |
| `adapters/capture-linux/` | X11 窗口/整屏采集 → `Frame`，含 `window_display_transform`（`DEC-016`） | `MIRADOR_BUILD_ADAPTERS_CAPTURE_LINUX` |
| `adapters/capture-windows/` | GDI 采集适配示例（编译验证级） | `MIRADOR_BUILD_ADAPTERS_CAPTURE_WINDOWS` |
| `adapters/capture-android/` | MediaProjection + Accessibility 适配示例（NDK 部分编译验证级） | `MIRADOR_BUILD_ADAPTERS_CAPTURE_ANDROID` |
| `integrations/ocr_ppocr/`、`integrations/detector_yolo/` | ncnn 参考能力后端（冒烟层无权重可运行；`DEC-015`） | `MIRADOR_BUILD_INTEGRATIONS` |

## 使用示例索引

| 示例 | 演示路径 |
| --- | --- |
| `examples/change_detection_tour.cpp` | 提交帧 → 变化检测 → ROI/指纹 → 帧缓存 |
| `examples/perception_session_tour.cpp` | 会话闭环：变化分析 → Stub OCR → 缓存复用 |
| `examples/road_segments_tour.cpp` | 线段检测 → 过滤 → 共线合并（非 Agent 语义） |
| `examples/icon_state_index_tour.cpp` | 图标状态入库 → 扰动查询 → 复用策略 |
| `examples/hybrid_localization_tour.cpp` | 融合 → 稳定 ID → SoM → generation 校验 |

运行方式见根 [README](../../README.md#examples)。基准入口与数字发布见
[docs/benchmarks/](../benchmarks/)；评测集场景约定见
[evaluation-scenes](../benchmarks/evaluation-scenes.md)。
