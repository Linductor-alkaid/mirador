# DEC-024：YOLO 显式预处理配置

> 状态：Accepted
> 日期：2026-10-16
> 依据：维护者要求修复并接入 Mirage；issue #38。
> 工作项：YP-01…03（见下）

`YoloDetectorOptions` 尾部追加 `interpolation` 与 `pad_value`，默认保留
area/pad0。新增 adapter-only `prepare_yolo_input`，提供 area 或 ncnn 字节双线性
插值、方形居中 letterbox 和 x/255；返回实际整数缩放/偏移的 Transform2D。
不增加 Core runtime 依赖，不扩展 Core letterbox/resize API，不修改模型权重。

Backend 与离线诊断工具必须使用同一公开预处理函数，避免诊断张量与实际检测输入
不同。Backend implementation_version 包含 side/interpolation/pad，调用方仍必须
用 model_revision 区分权重与其余模型配置。默认数值行为保留；工作预算覆盖同时存活
的 float 张量，超小预算可以比旧实现更早拒绝。结构尾部追加需消费者重新编译。

bilinear114 是明确配置，不称与 OpenCV 逐位等价；ncnn 字节插值存在定点舍入差异。
通过独立半像素仿射图验证误差，再用真实权重/独立推理/人工目标验证实际输出。
旋转元数据仍按像素已定向契约处理，不能称模型对物理旋转不变。

- [x] YP-01：实现 profile、预算、身份和同一路诊断输入。
- [x] YP-02：默认等价、非连续 stride/奇数尺寸/变换/取消/预算，真实模型回归。
- [ ] YP-03：ASAN/UBSAN、文档与 PR/CI 证据。

验证证据：[真实模型与坐标](../benchmarks/linux-x64-yolo-preprocessing-2026-10.md)。Release 与 ASAN/UBSAN 各 58/58；静态分析及 PR/CI 收尾中。
