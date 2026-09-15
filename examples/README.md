# examples

无 runtime 的基础示例：使用公共 API 演示变化检测、缓存与融合，纳入编译验证。示例不链接
模型 runtime、不下载权重；需要外部资源的示例通过用户显式提供的路径运行。

| 示例 | 模块 | 场景 |
| --- | --- | --- |
| `change_detection_tour` | image + cache | 提交帧、分层变化检测与帧级缓存 |
| `perception_session_tour` | fusion | 感知闭环：变化分析 → 桩 Backend 执行 → 能力结果缓存复用 |
| `road_segments_tour` | geometry | 合成道路灰度图上的线段检测 → 角度/长度过滤 → 共线合并（道路边界/屏幕分隔线语义） |
| `icon_state_index_tour` | cache + image | 图标状态入库（`VisualPatchFingerprint`）→ 扰动查询 → 三层证据命中与调用方复用策略 |
| `hybrid_localization_tour` | render（→ fusion） | 混合定位：Accessibility 区域 + OCR/检测证据融合 → 稳定 ID → SoM 渲染 → generation 拒绝陈旧引用 |

`road_segments_tour` 与 `icon_state_index_tour`（M3-11）刻意覆盖两个差异明显的领域，
证明 Mirador 不是只服务 Android Agent 的专用封装（设计 §24 M3）；
`hybrid_localization_tour`（M4-07）验证设计 §24 M4 的混合定位退出场景：外部区域、
OCR 与 Detector 在不侵入上层 Agent 的情况下组合，SoM 输出离散区域而 Mirador 不调用
VLM、不执行动作。
