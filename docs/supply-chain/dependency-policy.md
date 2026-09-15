# 依赖锁定与供应链策略

> 状态：Active
> 日期：2026-09-13
> 负责人：linductor

本文登记 Mirador 的依赖锁定机制与供应链文档索引，规则来源为
[工程规范](../project/project-standards.md)第 9 节与[设计文档](../design/mirador-development-design.md)第 21-22 节。

## 锁定机制

Mirador 采用 **Git submodule + 锁文件**：

- `.gitmodules` 声明 `third_party/<dep>`；克隆后需
  `git submodule update --init`（或 `git clone --recursive`）。
- 根目录 [dependencies.lock.json](../../dependencies.lock.json) 记录 source、精确
  commit、版本号、许可证及许可文件路径。
- configure 阶段校验 submodule HEAD 与 `pinned_commit`，不匹配即失败；缺失时给出
  初始化指引，不做静默下载。
- `MIRADOR_BUILD_TESTS=OFF`（以及未来的可选模块开关关闭）的最小核心构建不获取任何
  第三方依赖。

## 依赖清单

| 依赖 | 版本 | 许可证 | 使用范围 | 审计记录 |
| --- | --- | --- | --- | --- |
| googletest | v1.18.0 | BSD-3-Clause | 仅测试目标 | [googletest.md](googletest.md) |
| ncnn | 20260526（pinned commit `e54f7b1f`） | BSD-3-Clause | 仅 `integrations/` 参考后端（`MIRADOR_BUILD_INTEGRATIONS=ON` 时 FetchContent，默认构建零获取，`DEC-015`） | [ncnn.md](ncnn.md) |

可选实现依赖（OpenCV、ELSED 等）按里程碑引入时在此登记，并先完成许可证审查
（`DEC-009` 覆盖 ELSED）。模型 runtime 只出现在默认构建不获取的 `integrations/`
（`DEC-015`），永不进入核心构建图。

## 升级与审计

- 升级走独立 MR：更新 submodule 与 `pinned_commit`，在对应依赖的审计文件中记录版本
  差异、能力变化与回归结果。
- 新增依赖前按工程规范 §9.3 完成许可证、维护状态、平台支持与体积影响对比，并建立
  决策记录。
- `THIRD_PARTY_NOTICES` 与实际随源码/二进制分发的内容保持同步。
