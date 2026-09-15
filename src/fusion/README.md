# src/fusion

`mirador::fusion` 模块实现目录：`PerceptionSession`（M2，`DEC-013`）承载变化分析、按需
Backend 执行与坐标恢复的会话闭环。M4 叠加统一输出模型与融合能力（设计 §8、§16）：

- `semantic_snapshot.cpp`：`RegionSource`/`VisualRegion`/`SemanticSnapshot` 契约与
  `find_region`/`is_generation_current` 查询辅助。
- `evidence.cpp`：`ExternalRegion` 属性袋（`RULE-11`）与有界 `EvidenceSet`（确定性
  证据 ID，kFrame/kOriented 输入空间）。
- `fusion.cpp`：`fuse_evidence` 确定性融合——IoU/包含门控 + 类别兼容关联、聚类聚合、
  来源权重置信度与可解释 `FusionTrace`（中心距离仅记录，不单独合并，`DEC-010`）。
- `stable_id_tracker.cpp` + 私有 `rect_math.h`：门控后贪心一对一稳定 ID 跟踪、
  分裂/合并事件与 generation 递增规则（`DEC-010`、`RULE-09`）。
- `perception_session.cpp`：`fuse()` 发布不可变 `SemanticSnapshot`（首次 generation
  为 1，仅 tracker 报告 bump 时递增），`latest_snapshot()` 供并发只读。

公共 API 位于 `include/mirador/`。
