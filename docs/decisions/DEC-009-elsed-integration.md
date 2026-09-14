# DEC-009：ELSED 集成方式

> 状态：Accepted（2026-09-14，M3 内冻结）
> 日期：2026-09-14
> 负责人：linductor
> 冻结里程碑：M3
> 替代/被替代：无

## 背景与问题

设计 §15 与 `SCOPE-04` 要求 `mirador::geometry` 提供 `LineDetector` SPI，并允许实现
选择 ELSED、OpenCV LSD 或其他算法；「ELSED 或等价线段实现（可选依赖）」。总计划将
`DEC-009`（ELSED 集成方式）的最迟冻结点定为 M3。需要决定：

1. ELSED 是否进入 M3 交付；
2. 若引入，以何种形式（源码 vendor、可选适配、独立仓库）；
3. 一方等价实现是否满足 `SCOPE-04`。

## 事实与审查结论

- 上游 [iago-suarez/ELSED](https://github.com/iago-suarez/ELSED) 许可证为
  **Apache-2.0**（仓库许可证标识与 LICENSE 一致，2026-09-14 审查），允许源码再分发，
  义务为保留 LICENSE/NOTICE 归属声明。
- ELSED 上游实现依赖 **OpenCV（4.x）**：其核心数据通路基于 `cv::Mat`。引入 ELSED
  源码必然把 OpenCV 拉进对应可选目标的构建图。
- AGENTS.md Runtime 与依赖边界：可选实现依赖必须默认关闭、pin 到精确版本、登记来源
  与许可证；ELSED 引入前必须完成许可证审查（本决策即该审查记录）。
- M3 的验收语义是「ELSED **或等价**线段实现可以作为可选模块工作」（设计 §26），并非
  绑定 ELSED 本体。

## 决策

1. M3 交付 `mirador::geometry` 的一方等价线段检测器：梯度幅值播种、方向对齐区域
   生长、逐区域最小二乘拟合的确定性纯 C++ 实现（零第三方依赖、零 libm 热路径），
   满足 `SCOPE-04` 与设计 §26 的「等价实现」语义；`LineDetector` SPI 保证后续可替换
   或并存具体实现。
2. ELSED 本体**不进入 M3**，作为可选适配保留引入窗口：形式为默认构建不获取的可选
   目标（`adapters/` 下的 ELSED 适配，OpenCV 经 `find_package` 可选提供，或独立
   集成仓库），通过 `ImageView` → `cv::Mat` 既有适配层对接，公共 API 不出现
   `cv::Mat`（`DEC-003`）。
3. 触发条件（满足其一时立项引入 ELSED）：
   - M5 基准证明一方检测器在目标场景（屏幕分隔线、道路边界）的检出质量或性能
     不达标；
   - 出现明确调用方要求 ELSED 语义（如已有 ELSED 调用资产迁移）。
4. 引入时必须同时完成：上游 pin 到精确 commit 并登记 `dependencies.lock.json`、
   `THIRD_PARTY_NOTICES` 追加 Apache-2.0 归属、架构测试确认 ELSED/OpenCV 令牌仅
   出现在适配目录、稀疏 vendor 或外部获取方式与 Mimosa 门禁交互评估。

## 备选方案

- M3 直接 vendor ELSED 源码进 `third_party/`：拉 OpenCV 进可选目标构建图，CI 矩阵
  与门禁成本前置，而 M3 没有消费其差异能力的真实场景；被否。
- 以 OpenCV LSD 为等价实现：同样引入 OpenCV 依赖且 LSD 上游维护状态不稳定；被否。
- 仅交付 SPI 不交付任何实现：`SCOPE-04` 退出条件与设计 §26 验收明确要求至少一个
  可工作的线段实现；被否。

## 影响与风险

- `RISK-2026-02`（ELSED 许可证与集成方式未定）解除：审查完成，集成方式冻结。
- 新增 `RISK-2026-09`：一方检测器的真实场景检出质量未与 ELSED 对齐，以 M5 基准与
  上述触发条件收口。
- `mirador::geometry` 在 M3 转编译目标后，其链接接口恰为 `mirador::core`（架构测试
  同步），不引入任何新的外部链接闭包。

## 验证方式

`M3-02`/`M3-03`：一方检测器与 SPI 的确定性测试（合成线段图、奇数尺寸、非连续
stride、输出位稳定）；架构探针 `link_closure_geometry`。ELSED 引入窗口按本决策触发
条件另行立项并补齐 §9 供应链登记。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §15、§21、§26；总计划 `SCOPE-04`、
`DEC-009` 行、`RISK-2026-02`/`RISK-2026-09`；`M3-02`、`M3-03`、`M3-12`。
