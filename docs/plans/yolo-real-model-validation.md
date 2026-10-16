# YOLO 真实权重验证

> 状态：In Progress
> 日期：2026-10-16
> 负责人：Mirador 维护者
> 依据：维护者要求补齐 M5-04 的真实权重验收；DEC-015、RISK-2026-13

- [x] YR-01 固定模型、转换工具、输入图的来源、版本、摘要与许可证；原图/权重不入仓。
- [x] YR-02 可选离线真实模型 harness，检测结果及坐标与独立推理参考对照。
- [x] YR-03 正例、空白负例、桌面截图、裁切/缩放/stride/旋转、阈值与取消/预算验证，逐图视觉复核。
- [x] YR-04 记录冷加载、预热后 p50/p95、RSS、推理与外围处理开销及模型适用边界。
- [ ] YR-05 必要修复、回归和 sanitizer、文档与 PR；真实模型测试显式启用，不让默认 CI 下载权重。

不将合成模型冒烟结果替代真实模型质量，不将少量样本报告称作标准数据集 mAP。
默认 Core 不加载模型或联网。Mirage pin 与产品装配不在此 PR 范围。


2026-10-16：YR-01…04 Completed。固定 YOLOv5n 官方权重和 OmniParser UI 权重，
14 个样本 / 57 个框全部通过同输入 ONNX 对照；逐图复核区分模型质量和适配正确性。
性能另采样 11 次。Release+integrations 57/57，ASAN/UBSAN 各53/53，真实模型 CLI
与 adapter 额外 sanitizer 通过（ncnn 仍 Release）。
[报告](../benchmarks/linux-x64-yolo-real-model-2026-10.md)和外部资产锁已保存。
最终 clang-format / clang-tidy 与 Python 编译检查均通过，未知转换器摘要拒绝路径通过。
[PR #39](https://github.com/Linductor-alkaid/mirador/pull/39)已提交；YR-05 等待远端 CI，
负责人为 Mirador 维护者，CI 结束后按 PR checks 验收，失败则按对应日志修复/补跑。
预处理组合差异 [#38](https://github.com/Linductor-alkaid/mirador/issues/38) 属后续明确模型配置能力，
本项完成定义为真实模型测试和问题取证，不保证模型达到产品质量，也不修改冻结 pad0 行为。
