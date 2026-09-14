# Mirador 实施总计划

> 状态：Active
> 版本：1.0
> 负责人：linductor
> 设计依据：[Mirador 低负载终端视觉基础设施库开发设计方案](../design/mirador-development-design.md)
> 协作约束：根 [AGENTS.md](../../AGENTS.md) 与[项目管理与工程规范](../project/project-standards.md)
> 更新日期：2026-09-13

## 当前状态

M0「边界与骨架」已完成并发布 `v0.1.0-alpha`（PR #1 全部工作项与退出条件通过，CI 9/9
绿）：公共类型（`Status`/`Result`、`ImageView`/`Frame`、`Transform2D`）、架构测试、
GoogleTest v1.18.0、三平台 CI 已冻结，发布说明见 [CHANGELOG](../../CHANGELOG.md)。
M1「基础图像与变化检测」已完成（2026-09-14，PR #3/#4/#5 合入，跨平台 CI 全绿；发布点
`v0.1.0-beta.1` 待发布流程启动）。M2「Backend SPI 与能力结果缓存」已完成（2026-09-14，PR #6 CI 全绿；工作项与退出
条件全部通过，里程碑文档见 [m2-backend-spi-and-result-cache.md](m2-backend-spi-and-result-cache.md)，
发布点 `v0.1.0-beta.2` 待发布流程启动）。
整体路线沿用设计文档第 24 节的 M0-M5。

## 交付边界

### 包含

- [ ] `SCOPE-01` `mirador-core`：基础类型、`Status`/`Result`、`ImageView`/`Frame`、坐标空间与
  `Transform2D`、Backend SPI 接口与能力查询（设计 §5-§9、§18-§19）。
- [ ] `SCOPE-02` `mirador-image`：颜色转换、缩放、裁剪、指纹、分块差分、变化 ROI、帧级有界
  缓存（设计 §11、§24 M1）。
- [ ] `SCOPE-03` `mirador-cache`：有界缓存与字节预算、能力结果缓存键、语义快照缓存、有界
  视觉索引（精确哈希/感知哈希/模板匹配）（设计 §12、§24 M2-M3）。
- [ ] `SCOPE-04` `mirador-geometry`：`LineDetector` SPI、几何过滤、ELSED 或等价线段实现
  （可选依赖）（设计 §15、§24 M3）。
- [ ] `SCOPE-05` `mirador-fusion`：多源证据关联、确定性融合、来源追踪、稳定 ID 与 generation
  （设计 §16、§24 M4）。
- [ ] `SCOPE-06` `mirador-render`：SoM 渲染、调试叠加、网格划分与坐标回映工具（设计 §17、
  §24 M4）。
- [ ] `SCOPE-07` `adapters/opencv`：`cv::Mat` ↔ `ImageView` 互操作（可选依赖，非公共 API）
  （设计 §5、§21）。
- [ ] `SCOPE-08` `benchmarks`：变化检测、缓存命中路径、Backend 调用外层耗时的基准入口与
  评测集组织（设计 §20、§23、§24 M5）。
- [ ] `SCOPE-09` `examples`：无 runtime 的基础示例，使用公共 API 并纳入编译验证（设计 §21）。
- [ ] `SCOPE-10` 多平台验证：Linux、Windows、Android NDK 的构建、测试与基准证据及 CI 门禁
  （设计 §21、§24 M0/M5）。

### 明确不包含

- 模型 runtime 链接进 Core、模型权重分发与下载；runtime 适配位于独立仓库或默认构建不获取
  的 `integrations/`（见 `POST-05`）。
- 平台采集实现、Accessibility 服务、窗口/投影权限、输入注入与点击执行。
- VLM 调用、Agent 决策、Workflow 状态机与任务调度框架。
- 常驻线程、后台轮询、网络请求与默认持久化。

## 不可破坏约束

- `RULE-01` 依赖方向始终指向 Mirador 抽象；`mirador-core` 只依赖 C++20 标准库，不依赖
  OpenCV、ELSED、executor 或任何模型 runtime，由架构测试锁住（设计 §3、§21、§24 M0）。
- `RULE-02` 公共头不暴露 `cv::Mat`、runtime 类型或第三方私有类型；平台与第三方互操作只经
  `adapters/`（设计 §5）。
- `RULE-03` 公共 API 同步基线；核心不创建线程/定时器，不暴露 executor、future 或协程 ABI；
  取消与 deadline 仅经 `ExecutionContext` 传递（设计 §3、§9、§18）。
- `RULE-04` `ImageView` 非拥有；算法不得静默修改输入；NV12 等多平面格式不假定单连续平面
  （设计 §6）。
- `RULE-05` 所有区域输出携带 `CoordinateSpaceId`，预处理产生可组合 `Transform2D`；坐标恢复
  是核心正确性能力，不得下沉为 Backend 私有细节（设计 §7）。
- `RULE-06` 所有缓存有字节预算；输入尺寸、候选数量与细化流程有保护与预算；超限是显式淘汰
  或明确错误，不得无界增长（设计 §20）。
- `RULE-07` 能力结果缓存键至少包含规范化图像指纹、源 ID、ROI、预处理版本、Backend 名称、
  实现版本、模型 ID、模型修订与请求参数摘要（设计 §12）。
- `RULE-08` `Status`/`Result` 错误模型覆盖无效输入、不支持格式、坐标错误、Backend 不可用/
  失败、超时、取消、缓存损坏与预算超限；不泄漏 runtime 错误枚举（设计 §19）。
- `RULE-09` 稳定 ID 只在跟踪会话或缓存代际内稳定；SoM 与上层动作必须携带 `generation`
  （设计 §8、§16）。
- `RULE-10` 隐私默认：内存内处理、不落盘、不联网、不记录原始帧；持久化由调用方显式启用
  （设计 §22）。
- `RULE-11` `clickable` 等平台语义不得作为视觉内生推断；外部区域属性袋保留 `interactive`、
  `role`、`enabled` 等调用方语义（设计 §8）。
- `RULE-12` 是否调用某 Backend、任务优先级与超时由上层决定；Mirador 只提供执行组件与指标，
  不硬编码全局感知策略（设计 §10）。

## 里程碑索引

| 里程碑 | 名称 | 前置 | 建议发布点（暂定） | 状态 | 文档 |
| --- | --- | --- | --- | --- | --- |
| M0 | 边界与骨架 | — | `v0.1.0-alpha` | Completed | [m0-boundary-and-skeleton.md](m0-boundary-and-skeleton.md) |
| M1 | 基础图像与变化检测 | M0 | `v0.1.0-beta.1` | Completed | [m1-image-and-change-detection.md](m1-image-and-change-detection.md) |
| M2 | Backend SPI 与能力结果缓存 | M1 | `v0.1.0-beta.2` | Completed | [m2-backend-spi-and-result-cache.md](m2-backend-spi-and-result-cache.md) |
| M3 | 传统视觉与视觉索引 | M2 | `v0.1.0-beta.3` | Planned | 启动时创建 |
| M4 | 融合、稳定 ID 与 SoM | M3 | `v0.1.0` | Planned | 启动时创建 |
| M5 | 平台适配与产品化基准 | M4 | `v0.2.0` | Planned | 启动时创建 |

里程碑划分、范围与退出条件以设计文档第 24 节为准；发布点为暂定映射，里程碑启动时确认
并与 tag 一一对应。M4 完成设计文档第 26 节的"首个可用版本"验收。

## 尚未冻结的决策（暂定默认值）

| 编号 | 主题 | 暂定默认值 | 负责人 | 最迟冻结 |
| --- | --- | --- | --- | --- |
| [DEC-007](../decisions/DEC-007-multiplane-image-representation.md) | NV12 等多平面格式表示 | 扩展 `ImagePlane`，不假定单连续平面（M0-03 已按草案实现） | linductor | M1 |
| `DEC-008` | 缓存默认字节预算 | 已冻结：帧 4 MiB、能力结果 16 MiB（见 [DEC-008](../decisions/DEC-008-cache-default-byte-budgets.md)） | linductor | M2 |
| `DEC-009` | ELSED 集成方式 | 源码引入可选模块并完成许可证审查 | linductor | M3 |
| `DEC-010` | 稳定 ID 匹配算法 | 门控后贪心匹配起步 | linductor | M4 |
| `DEC-011` | 基准设备清单 | 设计 §20 三平台中端代表设备 | linductor | M5 |

已生效决策见 [docs/decisions/](../decisions/)：`DEC-001` 同步 API 与无 executor、`DEC-002`
Core 不链接模型 runtime、`DEC-003` 公共 API 不暴露 OpenCV 类型、`DEC-004` 公共边界
`Result<T>`/`Status`、`DEC-005` CMake 与构建基线、`DEC-006` GoogleTest 测试框架、
`DEC-007` 多平面图像表示。M2 新增：`DEC-012`（Backend SPI 契约）、`DEC-013`
（`PerceptionSession` 归属 fusion 与模块依赖演进）。

## 通用完成定义

- `DOD-01` 架构测试证明 Core 链接闭包仅含标准库；公共头无第三方类型。
- `DOD-02` 公共头经 GCC、Clang、MSVC 编译（Android NDK 为 Clang 交叉验证）。
- `DOD-03` 新公共行为有与风险相称的自动化测试；坐标相关变更覆盖方向、stride、奇数尺寸
  与往返容差矩阵。
- `DOD-04` 缓存相关变更验证失效路径（模型修订、参数、ROI、预处理版本），不只验证命中。
- `DOD-05` 性能声明附基准方法、环境与数字；未验证声明明确限定。
- `DOD-06` 隐私约束有负向测试（默认不落盘、不联网、日志脱敏）。
- `DOD-07` 按工程规范第 8 节完成文档同步；计划状态与验证记录更新。
- `DOD-08` 适用预设（`debug`/`asan`/`ubsan`/`tsan`）构建与测试通过。

## 建议拆分顺序

先契约后实现：每个里程碑先冻结类型与接口（含文档与伪实现测试），再填充算法实现。
先 SPI 与 Fake Backend，后真实依赖；可选依赖模块保持默认关闭并可独立裁剪。
架构边界（`RULE-01`~`RULE-03`）在 M0 用架构测试固化，后续里程碑只增不改。

## 延后项与触发条件

- `POST-01` GPU/native buffer 零拷贝路径 — 触发：M5 测量证明 CPU 连续内存路径不达标。
- `POST-02` 异步扩展接口（不破坏核心 ABI）— 触发：多个调用方证明同步封装不足。
- `POST-03` 光流/轻量特征增强变化检测 — 触发：M1 基准漏检/误检率超标。
- `POST-04` Embedder Backend 与嵌入视觉索引 — 触发：M3 图标索引命中率不足。
- `POST-05` ncnn/ONNX Runtime 示例适配包（独立仓库或 `integrations/`）— 触发：M2 需要外部
  runtime 可适配性验证。

## 风险

- `RISK-2026-01` Windows MSVC 与 Android NDK CI 环境可得性受限，跨平台证据可能延迟 —
  跟进：M0-06；受限时按工程规范第 4 节记录补跑条件。
- `RISK-2026-02` ELSED 许可证与集成方式未定，影响 M3 — 跟进：`DEC-009`。
- `RISK-2026-03` 坐标/变换建模在 M0 过度设计或表达力不足 — 跟进：M0-04 与设计 §7 测试矩阵。
- `RISK-2026-04` 缓存键设计遗漏导致跨模型/参数误命中 — 跟进：`RULE-07` 与 `DOD-04` 负向测试。

## 验证记录

2026-09-13：工作空间初始化并完成骨架验证——6 个 Linux 预设（debug/release/warnings/asan/
ubsan/tsan）配置、构建、ctest 全部通过，clang-format/clang-tidy 无告警；Windows 与 Android
NDK 仅有 CI 定义未实测。证据与限制详见 [M0 里程碑验证记录](m0-boundary-and-skeleton.md)。

2026-09-13：M0-01~M0-06、M0-08 实现完成（分支 `feat/core-m0-contracts`，commit
b7d1aa3..d9f54c9）：公共类型（`Status`/`Result`、`ImageView`/`Frame`、`Transform2D`）、
架构测试、GoogleTest v1.18.0 引入与全部模块目标落地。本地 6 预设 ctest 7/7 通过，
clang-format/clang-tidy 无告警；`DEC-004`/`DEC-005`/`DEC-006` 冻结为 Accepted。跨平台
编译证据随 PR #1 的 CI 运行回填，详见 [M0 里程碑验证记录](m0-boundary-and-skeleton.md)。
