# DEC-002：mirador-core 不链接模型 runtime，Backend 由调用方注入

> 状态：Accepted
> 日期：2026-09-13
> 负责人：linductor
> 冻结里程碑：M0（随设计文档生效）
> 替代/被替代：无

## 背景与问题

Mirador 需要接入 OCR、检测等模型能力，但设计文档（第 3、9、21 节）明确其定位是"视觉能力
库"而非"推理运行时"：不加载或执行神经网络模型，不链接 ncnn、ONNX Runtime、MNN、
TensorRT 等 runtime，不管理模型权重，且核心 API 不得被某类 runtime 的概念（如统一
`Tensor`）污染。

## 决策

1. `mirador-core` 只依赖 C++20 标准库；模型能力通过 `OcrBackend`、`DetectorBackend` 等
   Backend SPI 以图像和领域结果为稳定边界注入。
2. 核心 Backend 接口保持同步；`BackendInfo` 携带实现身份与模型修订，供缓存键与诊断使用。
3. 具体 runtime 的 Backend 示例只存在于独立仓库或默认构建不获取的 `integrations/`；
   模型权重不进仓库，示例通过用户显式提供的路径运行。
4. 该边界由 M0 架构测试锁住（核心链接闭包仅含标准库），CI 必须运行。

## 备选方案

- Core 内置 ncnn/ORT 适配：被否决——ABI、体积、许可证与构建选项被 runtime 绑定，Android
  与桌面场景无法统一。
- 提供通用 `Tensor` 公共 API：被否决——各模型输入输出差异大，会把预处理、量化和设备内存
  细节推给 Mirador（设计 §9）。

## 影响与风险

- 每个具体模型都需要一个适配包；换取 Core 体积小、可独立构建、被任意宿主复用。
- 缓存键必须显式包含 Backend 身份与模型修订，否则模型升级后出现陈旧命中（`RULE-07`）。

## 验证方式

无 runtime 环境下独立构建核心并通过测试（CI Linux/Windows/NDK 任务）；架构测试断言链接
闭包；`BackendInfo` 字段单测。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §3、§9、§21、§22；`AGENTS.md`
"Runtime 与依赖边界"；总计划 `RULE-01`、`POST-05`；`M0-05`。
