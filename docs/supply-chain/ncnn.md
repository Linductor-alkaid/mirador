# ncnn 审计记录（integrations 参考后端）

> 状态：Active
> 日期：2026-09-15（M5-02 引入）
> 负责人：linductor
> 决策依据：[DEC-015](../decisions/DEC-015-reference-runtime-selection.md)
> 锁定文件：[integrations/deps.lock.json](../../integrations/deps.lock.json)

## 引入信息

| 项 | 值 |
| --- | --- |
| 依赖 | ncnn（Tencent 移动端神经网络推理框架） |
| 版本 | 20260526 |
| 精确 commit | `e54f7b1f88434e1d844ea0551b880a1cfb079ce1`（轻量 tag `20260526`） |
| 来源 | https://github.com/Tencent/ncnn.git |
| 许可证 | BSD-3-Clause（上游仓库 LICENSE） |
| 获取方式 | configure 阶段 FetchContent（仅当 `MIRADOR_BUILD_INTEGRATIONS=ON`），不进源码树、不加 submodule |
| 使用范围 | `integrations/` 参考 Backend 专用；核心模块与默认构建图零引用 |

## 选型结论（摘要）

`DEC-015` 对比 ncnn / ONNX Runtime / MNN / TensorRT（许可证、维护状态、平台、体积、
模型链路，2026-09-15 复核）：ncnn 主选——BSD-3 干净、无第三方运行时依赖、体积最小、
三平台同一 C++ 代码路径；ONNX Runtime 为文档化备选（ncnn 算子覆盖遇阻时切换）。

## 分发与义务

- Mirador 核心源码与发布包**不包含、不分发** ncnn；默认构建不获取。
- 选项开启时 ncnn 被拉取进构建树：编译并分发 integrations 产物者即分发 ncnn，
  需随附其 BSD-3-Clause 许可证文本（`THIRD_PARTY_NOTICES` 第 3 节）。
- 模型权重不在本仓库、不随本机制分发；使用者显式路径提供。

## 验证

- configure 时校验 `deps.lock.json` 与 CMake 侧 pin 一致，不一致即失败。
- 合成 tiny 模型冒烟（`mirador.integrations.ncnn_smoke`）验证 forward → Mirador
  张量契约；在 integrations 专用 CI job 运行。
- 默认构建图由架构测试锁定零 runtime 令牌。

## 升级流程

独立 MR：更新 `deps.lock.json` 与 integrations CMake pin → 冒烟与基准回归 →
本文件记录旧/新版本、能力变化与回归结果（工程规范 §9、§10.7）。
