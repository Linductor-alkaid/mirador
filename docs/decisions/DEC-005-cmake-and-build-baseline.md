# DEC-005：CMake 与构建基线

> 状态：Proposed（暂定默认值，冻结里程碑 M0）
> 日期：2026-09-13
> 负责人：linductor
> 冻结里程碑：M0
> 替代/被替代：无

## 背景与问题

设计文档（§21）建议"CMake 3.16 以上版本并提供按模块开关"；工程基线（工程规范 §11）要求
CMakePresets 及 `debug`/`release`/`asan`/`ubsan`/`tsan` 预设。CMakePresets 的条件字段与
ctest 预设需要 CMake ≥ 3.21，较新的预设 schema 需要更高版本。两者需要统一成一条基线。

## 决策（暂定默认值）

1. `cmake_minimum_required(VERSION 3.16)`：遵循设计文档，保持对较旧发行版/CI 镜像的兼容。
2. `CMakePresets.json` 使用 schema version 3：消费预设需 CMake ≥ 3.21；开发与 CI 环境使用
   ≥ 3.25 并以其验证警告与预设行为。
3. 预设提供 `debug`、`release`、`asan`、`ubsan`、`tsan`、`warnings`（Linux/macOS，Ninja）与
   `windows-debug`（MSVC）；Android NDK 交叉编译经工具链文件直接配置，不进预设。
4. 警告策略：GCC/Clang `-Wall -Wextra -Wpedantic -Wshadow`，MSVC `/W4 /permissive-`；
   `MIRADOR_WARNINGS_AS_ERRORS` 选项默认 OFF，CI 的 `warnings` 预设开启。
5. 按模块开关（`MIRADOR_ENABLE_<MODULE>` 等）随各模块落地引入；可选依赖对应选项默认
   关闭。

## 备选方案

- 统一最低 3.25：更简单，但放弃设计文档对旧环境的兼容意图；若 M0 期间确认无旧环境需求，
  可在冻结时上调并更新本记录。
- 3.16 且不使用 presets：违背工程基线，放弃可复现的构建/测试入口。

## 影响与风险

- 预设 schema 决定 CI 脚本形态；冻结后变更需同步 `.github/workflows/ci.yml`。
- 3.16 下部分生成器表达式与特性受限，模块 CMake 需保持在该子集内。

## 验证方式

本地（CMake 3.28/Ninja/GCC 13）与 CI 实测全部预设 configure/build/test 通过。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §21；工程规范 §11；总计划 `SCOPE-10`；
`M0-01`、`M0-06`。
