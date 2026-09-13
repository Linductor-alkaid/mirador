# GoogleTest v1.18.0 依赖审计

> 状态：Active
> 日期：2026-09-13
> 负责人：linductor
> 决策依据：[DEC-006](../decisions/DEC-006-test-framework.md)
> 工作项：`M0-08`

## 来源与锁定

| 项 | 值 |
| --- | --- |
| 名称 | googletest（含 googlemock） |
| 上游 | <https://github.com/google/googletest.git> |
| 锁定版本 | v1.18.0 |
| 锁定 commit | `063de7e9578f82b369302001269680b4b1553359` |
| 引入方式 | Git submodule `third_party/googletest` + [dependencies.lock.json](../../dependencies.lock.json) |
| 许可证 | BSD-3-Clause（`third_party/googletest/LICENSE`） |

configure 阶段（`tests/CMakeLists.txt`）校验 submodule HEAD 与 `pinned_commit` 一致，
不匹配即报错；`MIRADOR_BUILD_TESTS=OFF` 的最小核心构建不获取、不编译、不链接该依赖。

## 选型对比与审查结论

选型对比记录见 [DEC-006](../decisions/DEC-006-test-framework.md)（Catch2 v3 / doctest /
自研断言库为备选）。审查结论：

- 许可证 BSD-3-Clause，允许商用与再分发，义务仅保留版权与许可声明；已登记
  `THIRD_PARTY_NOTICES`。
- 维护状态：Google 官方维护，v1.18.0 为最新稳定 tag（引入时点 2026-09-13）。
- 平台支持：CMake 构建覆盖 Linux GCC/Clang、Windows MSVC、Android NDK（Clang），与
  `SCOPE-10` 三平台验证矩阵一致。
- 体积影响：仅进入测试目标（`EXCLUDE_FROM_ALL`，不安装），不影响库产物体积。

## SBOM 影响

- 组件仅出现在测试构建图，不进入 `mirador-core` 链接闭包（由架构测试 `M0-05` 锁住）。
- 随源码分发（submodule 指针 + lock），`THIRD_PARTY_NOTICES` 已登记。
- 升级流程：更新 submodule 与 `pinned_commit`，本文件记录版本差异，独立 MR 并跑全量回归。

## 验证

2026-09-13：`cmake --preset debug && cmake --build --preset debug && ctest --preset debug`
在本机（CMake 3.28.3 / Ninja / GCC 13.3.0 / Ubuntu 24.04）通过；configure 时 commit 校验
生效（见 M0 里程碑验证记录）。
