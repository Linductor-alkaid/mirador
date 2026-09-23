# DEC-021：Backend 输出信任边界、限制自述与观测绑定动作协议

> 状态：Accepted（2026-09-23，随 [cua 视觉能力调研](../research/trycua-cua-vision-survey.md)评审冻结）
> 日期：2026-09-23
> 负责人：linductor
> 冻结里程碑：信任边界与观测绑定协议即日生效（现状已实现，本决策冻结为契约）；`known_limitations` 字段为增量契约，随下一次公共头变更 MR 落地
> 替代/被替代：无

## 背景与问题

对同类项目 trycua/cua（`17c5444`）的源码调研（见调研报告 §3）暴露了三个 Mirador 契约中
"实现已有行为、但未上升为决策"或"完全缺失"的点：

1. **Backend 输出的信任边界**。`EvidenceSet::add_*` 已经拒绝非有限或负尺寸区域
   （`src/fusion/evidence.cpp` `validate_bounds`），但"拒绝而非钳位、整体而非部分采纳"
   从未被任何决策冻结；对照 cua 的回声校验（worker 返回结果必须与登记的输入绑定一致，
   否则按 `artifact_invalid` 整体拒绝，且"worker 失败后不得从部分结果行动"），该行为
   是公共契约而不仅是实现细节。
2. **Backend 无法自述输出局限**。`BackendInfo`（`DEC-012`）只有身份与能力字段。OCR 类
   Backend 常见"旋转文本仅返回轴对齐外接框、`polygon` 为空"这类输出局限，目前只能靠
   文档或口头约定；cua 把它作为身份的一部分显式上报（`text_geometry` +
   `limitations`），诊断与上层可据此解释结果。
3. **"动作绑定观测"缺少 Mirador 侧立场**。cua 用 capture 绑定的一次性动作授权
   （一个截屏至多派生一个动作，动作后强制重观测）防止陈旧动作。Mirador 不执行动作，
   但 `generation` + stable ID 正是为此设计的原语；需要决策明确"不内建动作绑定，
   只提供原语与协议建议"，避免未来被迫在核心里长出动作执行职责。

## 决策

1. **Backend 输出按不可信输入处理**：Mirador 管线在证据采纳点校验区域的有限性与
   非负尺寸；不合法结果**显式拒绝**（`kInvalidArgument`），不静默钳位、不丢弃单条后
   继续采纳其余、不降级为警告。校验责任固定在证据采纳处（`EvidenceSet::add_*`），
   Backend 侧与融合引擎侧不重复实现。"拒绝而非修复"是不可协商项：任何"宽容模式"
   都必须先修订本决策。
2. **`BackendInfo` 新增 `std::vector<std::string> known_limitations`（默认空）**：
   实现用它声明已知的输出局限（如 `"rotated text is returned as an axis-aligned
   bound; polygon is empty"`）。约束：
   - 纯声明式元数据：**不参与**能力结果缓存键（不改变 `RULE-07` 键字段集）、不参与
     `validate` 门控（空列表合法）；
   - 消费方是诊断、文档与上层结果解释，Mirador 不依据它改变行为；
   - 语义要求：条目为稳定英文短句，格式对齐 cua 的 `limitations` 风格，便于跨项目
     对照与日志输出。
3. **观测绑定动作是调用方协议，不是 Mirador 能力**：Mirador 不引入动作执行、截屏
   保留或 capture 注册表（AGENTS.md 边界不变）。冻结以下调用方协议建议，写入设计
   文档并在集成文档中引用：
   - 动作必须携带其依据的 `generation` 与 `stable_id`，执行前由调用方对当前快照校验；
   - 每个已发布快照至多派生一个动作；动作执行、超时或结果未知后必须重新提交帧，
     依据新快照决策；
   - 管线失败（`kBackendFailure`/`kTimeout`/`kCancelled`）后得到的部分结果不得作为
     动作依据。
   - 内容等价复用（变化门控返回缓存快照）不视为新观测：`generation` 未递增时，上述
     "一观测一动作"约束按同一观测计。

## 备选方案

- **信任边界改为"钳位到图像范围并继续"**：与 cua 的失败实践相悖——错误 Backend 输出
  往往是系统性错位而非个别越界，钳位会把可诊断的失败变成不可诊断的错误动作，被否。
- **限制自述用结构化枚举**（`enum class Limitation`）：扩展每个新局限都要改公共 ABI，
  与 `backend_params` 用字符串换演进自由的既有取舍（`DEC-012`）不一致，被否。
- **在 `PerceptionSession` 内建 capture 式注册表与 TTL**：引入时间驱动的状态清理，
  与"核心无内部并发设施/定时器"及内容驱动（指纹）复用模型冲突，且动作执行本属调用方，
  被否。
- **身份链照搬 cua `VisualParserMetadata`**（manifest sha256、runtime 库 sha256 等全字段）：
  Mirador Backend 在进程内同步执行，artifact 完整性由集成层与供应链文档（设计 §22）
  负责；`model_revision` 字符串已承载摘要身份并参与缓存键，全字段照搬是过度设计，被否。

## 影响与风险

- `BackendInfo` 增加成员属于公共结构体变更：v0.3.0 后源代码兼容（新增带默认值的尾部
  成员），已编译消费者存在 ABI 影响；项目处于 1.0 前次版本号阶段，接受为 minor 变更，
  落地 MR 需同步 [CHANGELOG](../../CHANGELOG.md) 与兼容性说明。
- `known_limitations` 为自由文本，存在被滥用为日志垃圾桶的风险；靠"稳定英文短句"约定
  与评审约束，不做 schema 化（必要时未来经新决策收紧）。
- 信任边界冻结为契约后，现有依赖"宽容解析"的第三方 Backend（若有）会从静默错误变为
  显式 `kInvalidArgument`；这是有意的失败可见化，符合完成定义"失败对调用方可见"。
- 协议建议第 4 条把"缓存命中 + generation 未变"解释为同一观测，调用方若需要更保守的
  语义（如时间上限），可在自身层加 TTL，Mirador 不提供。

## 验证方式

- 信任边界：`tests/fusion/evidence_set_test.cpp` 既有非有限/负尺寸拒绝负路径保持通过，
  补充"合法与非法混合提交时整批拒绝"的断言（落地 MR 内）。
- `known_limitations`：`tests/core/backend_test.cpp` 补充默认空、`validate` 不受影响、
  不进入缓存键（`RULE-07` 键敏感性矩阵不含该字段）三项断言。
- 观测绑定协议：无核心代码变更，语义由 `tests/fusion/perception_session_test.cpp` 既有
  generation 递增/复用测试间接覆盖；协议文本的正确性在集成文档评审中把关。

## 关联文档和工作项

[调研报告](../research/trycua-cua-vision-survey.md)（证据来源）；
[设计文档](../design/mirador-development-design.md) §7/§9/§16/§23（本次同步修订）；
[DEC-008](DEC-008-cache-default-byte-budgets.md)、[DEC-010](DEC-010-stable-id-matching.md)、
[DEC-012](DEC-012-backend-spi-contract.md)、[DEC-016](DEC-016-display-space-transform-contract.md)；
总计划 `RULE-07`、`POST-04`。无新增里程碑工作项：信任边界为现状冻结；`known_limitations`
落地 MR 由负责人在下次公共契约触点安排。
