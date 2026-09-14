# DEC-013：PerceptionSession 归属 fusion 模块与模块依赖演进

> 状态：Accepted（2026-09-14，M2 内冻结）
> 日期：2026-09-14
> 负责人：linductor
> 冻结里程碑：M2
> 替代/被替代：无

## 背景与问题

`PerceptionSession`（设计 §18、§25）编排变化检测（`mirador::image`）、帧级与能力结果
缓存（`mirador::cache`）和 Backend SPI（core），是设计 §5 依赖图中 image 与 cache 的
汇合点（`image → fusion`、`cache → fusion`）。但 M1 冻结的架构测试断言"每个一方模块的
链接接口恰为 `mirador::core`"，当时 image/cache 尚无兄弟依赖，该断言成立。M2 引入
会话时必须决定其归属模块，并同步架构测试，否则会话无处安放或架构测试与设计冲突。

## 决策

1. `PerceptionSession` 落在 `mirador::fusion`：它是设计中消费 image 与 cache 输出的
   汇合模块，M4 的证据融合与稳定 ID 也在该模块；M2 先落地会话闭环（变化分析、按需
   Backend 执行、坐标恢复、缓存集成），M4 在同一模块上叠加融合与快照。
2. 架构测试按模块声明允许的链接接口：core 为空、image/cache/geometry/render 恰为
   `mirador::core`、fusion 恰为 `mirador::core + mirador::image + mirador::cache`
   （与设计 §5 依赖图一致）；新增 fusion 链接闭包探针（NEEDED 仍仅标准库，image/cache
   为静态库不产生额外动态依赖）。
3. 依赖方向保持指向 Mirador 抽象（`RULE-01`）：fusion 只消费 image/cache 的公共 API 与
   core 的 SPI，任何模块不得反向依赖 fusion；render（M4）依赖 fusion，不直接依赖
   image/cache。
4. M1 记录的"每个模块链接接口恰为 core"是该阶段事实的快照而非永久约束；本决策将其
   演进为按模块的允许集合表，演进本身同步设计 §5 与架构测试（`M2-04`）。

## 备选方案

- 会话落在 `mirador::cache` 并允许 cache→image：违背设计 §5 依赖图（cache 不依赖
  image），且缓存机制层被迫携带编排职责，被否。
- 新建 `mirador::session` 模块：模块清单是设计 §21 冻结的稳定目标集合，为单一类型增加
  模块破坏"小而稳定"的模块划分，被否。
- 会话放 core、以模板/回调注入 image 与 cache 能力：core 不得依赖任何一方模块，回调
  注入会把预处理与缓存细节倒置给调用方，公共 API 退化为不可用的裸 SPI，被否。

## 影响与风险

- fusion 在 M2 落地后即成为编译目标，`MIRADOR_BUILD_FUSION=OFF` 时会话不可用（模块
  开关语义与 image/cache 一致）。
- 架构测试的允许集合表须随模块依赖图演进；后续里程碑（M3 geometry、M4 render）按同一
  表扩展，不得静默偏离设计 §5。

## 验证方式

`M2-04`：`tests/architecture/CMakeLists.txt` 的按模块接口断言与
`fusion_link_closure_probe`（Linux readelf NEEDED 仅标准库）；全预设 ctest 通过。
见 `tests/architecture/`。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §5、§18、§25；总计划 `RULE-01`；
`M2-04`、`M2-05`。
