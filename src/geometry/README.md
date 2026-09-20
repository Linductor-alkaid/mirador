# src/geometry

`mirador::geometry` 模块实现目录：`LineDetector` SPI、几何过滤与 ELSED 等传统视觉适配
（可选依赖，许可证审查先行）。内容随 M3 里程碑落地；M6 追加
`geometric_proposal.cpp`——闭合/近闭合线段结构的区域 proposal 分析（issue #11 假设
验证，`DEC-017` 交付；`DEC-018` 阶段 1 已冻结为正式契约，计入兼容性承诺）。
