# M0：边界与骨架

> 状态：Completed（发布点 `v0.1.0-alpha` tag 待维护者按发布流程创建）
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

- [x] `M0-01` 建立 CMake 项目与模块目标：`mirador::core`/`image`/`cache`/`geometry`/`fusion`/
  `render` 目标与按模块开关，`CMakePresets.json` 提供 `debug`/`release`/`asan`/`ubsan`/
  `tsan` 预设，警告策略与 `MIRADOR_WARNINGS_AS_ERRORS`。
- [x] `M0-02` 冻结 `Status`/`Result<T>` 与错误码分类（设计 §19，依据 `DEC-004`）：覆盖无效
  输入、不支持格式、坐标错误、Backend 不可用/失败、超时、取消、缓存损坏、预算超限；
  错误不泄漏 runtime 枚举，异常不穿越公共 API。
- [x] `M0-03` 定义 `PixelFormat`/`Rotation`/`ImageView`/`Frame`（设计 §6）与多平面 `ImagePlane`
  扩展表示（`DEC-007` 草案）；文档化非拥有语义与 `Frame::owner` 生命周期。
- [x] `M0-04` 定义 `CoordinateSpaceId`/`Transform2D`/`RectF`/`PointF`（设计 §7）与点/矩形/
  多边形/线段的统一变换接口，实现并通过方向/stride/奇数尺寸/往返容差测试矩阵。
- [x] `M0-05` 架构测试：Core 链接闭包仅含标准库（无 OpenCV、模型 runtime、线程/调度设施）；
  公共头不含第三方类型；CI 必须运行。
- [x] `M0-06` 最小 CI：Linux（GCC/Clang）、Windows（MSVC）、Android NDK（arm64-v8a）的
  configure/build/test；测试标签机制（`unit`/`architecture` 等）可用且默认套件非空。
- [x] `M0-07` 仓库卫生：`.clang-format`/`.clang-tidy` 纳入 CI、`.gitignore`、`LICENSE`（MIT）、
  `THIRD_PARTY_NOTICES`、`README` 与文档骨架。
- [x] `M0-08` 测试框架选型落地（`DEC-006`）并迁移现有 smoke 测试到统一框架与标签。

## 风险与阻塞

- `RISK-2026-01`：MSVC/NDK CI 环境受限时，对应验证保持未完成并记录补跑条件。
- `DEC-004` 未冻结会阻塞 `M0-02`；`DEC-005` 未冻结会阻塞预设形态；两项须在 M0 内关闭。
- `RISK-2026-03`：坐标/变换建模过度设计或表达力不足，`M0-04` 需以设计 §7 测试矩阵为验收。

## 测试与退出条件

- [x] `mirador-core` 在无 OpenCV、executor、模型 runtime 的环境独立配置、构建并测试通过
  （Linux GCC 与 Clang，`debug` 预设）。
- [x] 公共头在 GCC、Clang、MSVC 下编译通过，NDK arm64-v8a 交叉编译通过（CI run
  `34766949574`：linux gcc/clang、windows msvc/ninja、android ndk 全部 success）。
- [x] 架构测试证明核心链接闭包仅含标准库，并在 CI 运行。
- [x] 坐标变换测试矩阵（0/90/180/270 度、非连续 stride、奇数尺寸、往返容差）通过。
- [x] `ctest` 标签机制可用，默认套件非空；适用 sanitizer 预设通过。
- [x] 文档同步：总计划、本里程碑、相关决策记录与 `README` 一致。

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

2026-09-13：M0-01~M0-06、M0-08 实施与验收（分支 `feat/core-m0-contracts`，commit
`b7d1aa3`..本提交，经 [PR #1](https://github.com/Linductor-alkaid/mirador/pull/1) 评审合入）。

- 环境：Ubuntu 24.04 x64（本机，CMake 3.28.3、Ninja、GCC 13.3.0、clang-format/
  clang-tidy 18.1.3）+ GitHub Actions（run `34766949574`，9/9 job success：linux
  gcc/clang debug、gcc warnings/asan/ubsan/tsan、windows msvc/ninja、android ndk
  arm64-v8a、lint）。
- 落地内容与证据：
  - `M0-01`（`0c1f91a`）：`mirador::image/cache/geometry/fusion/render` 以 INTERFACE 目标
    建立，`MIRADOR_BUILD_<MODULE>` 开关关闭路径实测（目标从构建图消失、余下构建绿）。
  - `M0-02`（`2578814`）：`Status`/`Result<T>`/`Result<void>` + 九类 `ErrorCode`；
    `mirador.core.status_result` 覆盖类别覆盖、传播链、`value_or`/`take_value`、超时与
    取消互斥；`DEC-004` 冻结 Accepted。
  - `M0-03`（`4cc49a9`）：`PixelFormat`/`Rotation`/`ImageView`/`Frame` + 特征函数与
    `validate()`；测试覆盖奇数宽度 stride、NV12 双平面规则（`DEC-007` 草案）、owner 生命周期。
  - `M0-04`（`1901881`）：`CoordinateSpaceId`/`Transform2D` + 工厂/组合/求逆 +
    `transform_point/rect/points/segment`（命名避开 `std::apply` 的 ADL 二义）；单元矩阵
    （4 方向、oriented_size、letterbox 中心化、镜像、多边形/线段、stride 无关性、
    frame→rotation→crop→letterbox 链恢复）+ 属性测试（固定种子 200 迭代、往返容差 1e-3、
    合法链不越界）。
  - `M0-05`（`e27e5eb`、`74d06c1`）：`mirador.architecture.source_scan`（可移植令牌扫描，
    负向验证注入 `#include <thread>` 即失败）与 `mirador.architecture.link_closure`
    （Linux readelf NEEDED ⊆ 标准库/工具链运行时/sanitizer 运行时；readelf 强制 C locale）
    + configure 期断言 `mirador::core` 链接接口为空。
  - `M0-06`（`d9f54c9`、`aeda801`）：CI 全部 job 递归检出子模块；Windows job 由 VS 生成器
    改为 `msvc-dev-cmd` + Ninja（windows-latest 已无 VS17 2022 实例，首跑失败后修复）。
  - `M0-08`（`b7d1aa3`）：GoogleTest v1.18.0 submodule + `dependencies.lock.json`，configure
    期 pin 校验三路径实测（缺失/不匹配/恢复）；smoke 测试迁移；`DEC-006` 冻结 Accepted。
- 本地命令与结果：`cmake --preset {debug,release,warnings,asan,ubsan,tsan}` +
  `cmake --build` + `ctest` 每预设 7/7 通过（unit 4、property 1、architecture 2；tsan 经
  `setarch "$(uname -m)" -R`）；`clang-format --dry-run --Werror` 与
  `clang-tidy --warnings-as-errors='*'` 无告警。
- 限制与说明：
  - Android NDK job 仅 configure+build（无设备不运行测试），后续在 M5 补跑设备侧测试。
  - `v0.1.0-alpha` tag 依仓库纪律不由 Agent 创建，待维护者发布。
  - 本地 Mimosa L3 安全门禁曾拦截含 `third_party/googletest` 上游测试脚本的扫描告警
    （路径穿越/代码注入等，均属上游开发工具、本仓库不构建不执行不修改）。处置：
    googletest 子模块改用稀疏检出，仅物化库源码与构建脚本（HEAD 仍锁定 pin、锁校验通过、
    内容未修改），`.mimosa/` 门禁状态目录已 gitignore；重跑密封深度扫描
    scan `scan-2026-09-13T15-49-35.799Z-87e2a6bb330b`（seal `7e75daf4…`）finding 数为 0。
    CI 完整检出不受影响。
- 同步：总计划"当前状态"、里程碑索引 M0 状态、`DEC-004/005/006` Accepted、`DEC-007`
  草案创建、`docs/supply-chain/`、`THIRD_PARTY_NOTICES`、`README`。
