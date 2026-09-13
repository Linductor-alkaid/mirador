# DEC-001：同步 API 基线，核心不绑定 executor 或并发框架

> 状态：Accepted
> 日期：2026-09-13
> 负责人：linductor
> 冻结里程碑：M0（随设计文档生效）
> 替代/被替代：无

## 背景与问题

项目模板（`~/cpp-project-template`）要求以 pinned `third_party/executor` 作为唯一并发基础
设施。Mirador 设计文档（第 3、18 节）则明确：Mirador 是可被任意调度环境嵌入的视觉能力库，
"Mirador 不依赖 executor。同步 API 是能力边界的基准"，且核心不创建常驻工作线程、不隐藏
后台轮询、不要求全局单例。

## 决策

1. 公共 API 采用同步基线；核心不创建线程、线程池或定时器，不使用 `std::thread`、
   `std::jthread`、`std::async` 或任何调度框架，不暴露 future/executor/协程 ABI。
2. 异步执行、优先级、超时与实时调度由调用方负责；Mira 等上层系统在自己的集成层完成调度。
3. Backend 实现可在内部使用模型 runtime 自带异步能力，但必须在同步边界返回前完成。
4. 取消与 deadline 仅通过轻量 `ExecutionContext`（`is_cancelled` 回调 + 可选 deadline）传递。
5. 该边界写入根 `AGENTS.md` 强制条款，并由 M0 架构测试锁住（核心链接闭包无线程/调度设施）。

## 备选方案

- 引入 pinned executor（模板默认方案）：被否决——会把调度 ABI 传播给所有使用方，与设计
  文档的零拷贝/异步 runtime 兼容目标冲突，也阻碍 Android NDK 与纯 OpenCV 程序直接复用。
- 公共 API 提供 future/异步重载：被否决——异步扩展接口留待 `POST-02`，且不得破坏核心 ABI。

## 影响与风险

- 并发责任完全落在调用方；Mirador 无法在内部并行加速（符合"避免工作优先于加快工作"的
  性能原则）。
- 与项目模板的 Executor 强制条款不兼容；本文与工程规范第 1 节的效力顺序共同裁决。
- 测试策略相应调整：并发类测试聚焦 session 并发读取、共享缓存与 `ExecutionContext` 竞态。

## 验证方式

M0 架构测试断言核心目标的链接闭包不含线程/调度设施；`AGENTS.md` "同步 API 是强制并发
边界"章节作为日常检查依据。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §3、§9、§18；`AGENTS.md`；总计划
`RULE-03`；`M0-05`。
