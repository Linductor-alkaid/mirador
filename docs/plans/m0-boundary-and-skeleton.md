# M0：边界与骨架

> 状态：Planned
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：无
> 建议发布点：`v0.1.0-alpha`
> 更新日期：2026-09-13

## 目标

建立 C++20/CMake 项目、模块目标与最小可验证骨架，冻结最容易传播到所有模块的公共数据类型
（`Status`/`Result`、`ImageView`/`Frame`、坐标空间与变换模型），并以架构测试锁住依赖规则；
完成 Linux、Windows、Android NDK 的最小 CI。本阶段不接入任何模型（设计文档 §24 M0）。

## 范围与非目标

范围：公共数据类型与错误模型、模块目标与构建预设、架构测试、三平台最小 CI、仓库卫生。
非目标：任何图像算法实现（M1）、Backend SPI 的请求/结果类型细化（M2）、缓存与视觉索引
（M1/M2）、融合与渲染（M4）。

## 设计与决策依据

- [设计文档](../design/mirador-development-design.md) 第 6-7 节（输入模型与坐标）、第 18-19 节
  （会话状态与错误模型）、第 21 节（构建与目录）、第 24 节 M0、第 26 节（验收标准）。
- 已生效：`DEC-001`（同步 API 与无 executor）、`DEC-002`（Core 不链接模型 runtime）、
  `DEC-003`（公共 API 不暴露 OpenCV 类型）。
- 待冻结：`DEC-004`（异常与错误模型）、`DEC-005`（CMake 基线）、`DEC-006`（测试框架），
  均须在本里程碑内落地。

## 工作项

- [ ] `M0-01` 建立 CMake 项目与模块目标：`mirador::core`/`image`/`cache`/`geometry`/`fusion`/
  `render` 目标与按模块开关，`CMakePresets.json` 提供 `debug`/`release`/`asan`/`ubsan`/
  `tsan` 预设，警告策略与 `MIRADOR_WARNINGS_AS_ERRORS`。
- [ ] `M0-02` 冻结 `Status`/`Result<T>` 与错误码分类（设计 §19，依据 `DEC-004`）：覆盖无效
  输入、不支持格式、坐标错误、Backend 不可用/失败、超时、取消、缓存损坏、预算超限；
  错误不泄漏 runtime 枚举，异常不穿越公共 API。
- [ ] `M0-03` 定义 `PixelFormat`/`Rotation`/`ImageView`/`Frame`（设计 §6）与多平面 `ImagePlane`
  扩展表示（`DEC-007` 草案）；文档化非拥有语义与 `Frame::owner` 生命周期。
- [ ] `M0-04` 定义 `CoordinateSpaceId`/`Transform2D`/`RectF`/`PointF`（设计 §7）与点/矩形/
  多边形/线段的统一变换接口，实现并通过方向/stride/奇数尺寸/往返容差测试矩阵。
- [ ] `M0-05` 架构测试：Core 链接闭包仅含标准库（无 OpenCV、模型 runtime、线程/调度设施）；
  公共头不含第三方类型；CI 必须运行。
- [ ] `M0-06` 最小 CI：Linux（GCC/Clang）、Windows（MSVC）、Android NDK（arm64-v8a）的
  configure/build/test；测试标签机制（`unit`/`architecture` 等）可用且默认套件非空。
- [ ] `M0-07` 仓库卫生：`.clang-format`/`.clang-tidy` 纳入 CI、`.gitignore`、`LICENSE`（MIT）、
  `THIRD_PARTY_NOTICES`、`README` 与文档骨架。
- [ ] `M0-08` 测试框架选型落地（`DEC-006`）并迁移现有 smoke 测试到统一框架与标签。

## 风险与阻塞

- `RISK-2026-01`：MSVC/NDK CI 环境受限时，对应验证保持未完成并记录补跑条件。
- `DEC-004` 未冻结会阻塞 `M0-02`；`DEC-005` 未冻结会阻塞预设形态；两项须在 M0 内关闭。
- `RISK-2026-03`：坐标/变换建模过度设计或表达力不足，`M0-04` 需以设计 §7 测试矩阵为验收。

## 测试与退出条件

- [ ] `mirador-core` 在无 OpenCV、executor、模型 runtime 的环境独立配置、构建并测试通过
  （Linux GCC 与 Clang，`debug` 预设）。
- [ ] 公共头在 GCC、Clang、MSVC 下编译通过，NDK arm64-v8a 交叉编译通过（各留 CI 证据；
  环境受限时按规范记录原因与补跑条件）。
- [ ] 架构测试证明核心链接闭包仅含标准库，并在 CI 运行。
- [ ] 坐标变换测试矩阵（0/90/180/270 度、非连续 stride、奇数尺寸、往返容差）通过。
- [ ] `ctest` 标签机制可用，默认套件非空；适用 sanitizer 预设通过。
- [ ] 文档同步：总计划、本里程碑、相关决策记录与 `README` 一致。

## 验证记录

2026-09-13：工作空间初始化。基于 `~/cpp-project-template` 治理框架并按设计文档裁剪
（模板的 Executor 强制章节替换为同步 API/依赖边界章节，见工程规范引言）。创建：治理文档
（`AGENTS.md`、工程规范、实施总计划、本里程碑、`DEC-001`~`DEC-006`）、构建骨架（CMake
项目 + `mirador::core` 目标 + 全部 Linux/Windows 预设 + smoke 测试 + GitHub Actions 三平台
CI 定义）、目录骨架（设计 §21）。`M0-01`（仅 core 目标，其余模块目标未建）、`M0-07`
（CI 未实际运行）为部分就绪，工作项保持未勾选；`M0-02`~`M0-06`、`M0-08` 未开始。

2026-09-13：初始化骨架的本地构建与静态检查验证（工作树状态：与初始提交同一内容）。

- 环境：Ubuntu 24.04 x64，CMake 3.28.3 + Ninja 1.13.2，GCC 13.3.0，clang-format/
  clang-tidy 18.1.3。
- 命令与结果：`cmake --preset {debug,release,warnings,asan,ubsan,tsan}` 及对应
  `cmake --build` 全部成功；`ctest` 每预设 1/1 通过（`mirador.core.smoke`，标签 `unit`）。
  `clang-format --dry-run --Werror`（include/src/tests 全部 C++ 文件）与
  `clang-tidy --warnings-as-errors='*' -p build/debug` 无告警。
- 限制：tsan 预设在本机（高熵 ASLR）需 `setarch "$(uname -m)" -R ctest --preset tsan`，
  该写法已写入 CI；Windows MSVC 与 Android NDK 仅有 CI 定义、未实际运行（本机无对应
  环境），负责人 linductor，补跑条件：GitHub Actions 首次运行或本地配置 MSVC/NDK。
- 同步：总计划"当前状态"与验证记录已更新；`DEC-005` 的验证方式得到本地证据支撑。
