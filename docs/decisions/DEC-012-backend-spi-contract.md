# DEC-012：Backend SPI 公共契约（请求分层、坐标契约、取消通道与线程安全声明）

> 状态：Accepted（2026-09-14，M2 内冻结）
> 日期：2026-09-14
> 负责人：linductor
> 冻结里程碑：M2
> 替代/被替代：无

## 背景与问题

设计文档 §9 给出了 `BackendInfo`、`OcrBackend`、`DetectorBackend` 的代码草案，但 M2 冻结
公共契约前必须补齐四类草案未回答的问题：

1. 请求类型同时被管线（Mirador 侧预处理与缓存）和 Backend 消费，哪些字段由谁解释？
2. Backend 返回区域的坐标落在哪个空间、由谁恢复到调用方要求的空间（`RULE-05`）？
3. `ExecutionContext`（设计 §18）如何进入 SPI 方法签名？
4. "Backend 是否线程安全必须显式声明"（AGENTS.md 同步边界第 3 条）落在哪个类型上？

## 决策

1. **请求分层**：每能力一个请求类型（`OcrRequest`/`DetectionRequest`），同时作为会话层
   入参（`run_ocr(frame, backend, request)`）与 SPI 入参
   （`recognize(prepared_image, request, context)`）。字段消费方在契约中固定：
   - Mirador 管线消费：`roi`/`roi_space`（裁剪）、`max_side`（最长边缩放上限）、
     `output_space`（坐标恢复目标）、`cache_policy`（读/写/强制刷新）；Backend 不得解释
     这四组字段。
   - Backend 消费：`min_confidence`（结果过滤）、`language_hint`（仅 OCR）、
     `backend_params`（不透明实现私有参数）。`backend_params` 进入缓存键参数摘要，
     供实现扩展而不破坏 ABI。
   - `cache_policy` 不进入缓存键：同一内容的 `kRefresh` 与 `kReadWrite` 指向同一缓存
     条目，刷新是覆盖写而非新键。
2. **坐标契约**：Backend 返回的区域（bounds 与 polygon）一律落在 `prepared_image` 的
   像素空间（[0, w) × [0, h)）；Mirador 依据预处理链（旋转 → 裁剪 → 缩放，均为
   `Transform2D`）的逆变换把结果恢复到 `request.output_space`（M2 支持 `kFrame`、
   `kOriented`、`kCropped`）。缓存存储恢复后的结果，命中即可直接使用。
3. **取消通道**：SPI 执行方法以 `const ExecutionContext&` 参数接收取消与 deadline
   （`DEC-001` 同步边界不变）；无取消需求时传默认构造值。实现必须周期检查并在观测到
   取消/超时时报 `kCancelled`/`kTimeout`；Mirador 侧在管线各阶段边界同样检查。
4. **线程安全声明**：`BackendInfo` 新增 `bool thread_safe`（默认 false）。Backend 实现
   必须显式声明；Mirador 不做任何加锁，调用方依据声明自行编排并发。
5. **能力门控**：`BackendInfo::accepted_formats` 是预处理目标格式的选择依据（按声明顺序
   取第一个可达格式）；`info()` 返回值经 `validate` 检查（名称与实现版本非空、格式列表
   非空且全部为已定义格式），不合法报 `kBackendUnavailable`（与 `Status` 枚举语义一致）。
6. **确定性要求**：缓存语义要求"相同 prepared 图像 + 相同 Backend 参数 → 相同结果"；
   违反确定性的实现不得接入带缓存感知的会话。
7. **Embedder SPI 延后**：按 `POST-04` 触发条件（M3 图标索引命中率不足）再定义；本决策
   的请求分层与坐标契约即未来 Embedder SPI 必须遵循的模板。

同时延后：逐字符 OCR 结果（设计 §13"可选字符级结果"）随首个真实需要的 Backend 引入，
M2 冻结的请求不含该开关；NMS/letterbox 组合器（§14）随 M3+ 通用模块落地。

## 备选方案

- 管线请求与 SPI 请求拆成两个类型（如 `OcrQuery`/`OcrRequest`）：避免"字段消费方"注释，
  但类型翻倍、会话层与 SPI 层需要转换样板，且设计 §9/§13 的草案即单类型，被否。
- Backend 直接接收原始帧并自行预处理：预处理在每个 Backend 重复实现，缓存键无法统一
  描述"预处理版本"，与 §9"Mirador 负责通用前后处理"相悖，被否。
- `ExecutionContext` 作为 `OcrRequest` 字段：取消是执行期关注点而非请求内容，混入后
  缓存键序列化必须显式排除它，易错，被否。
- `OcrBackend::recognize` 不带 context、由外层超时兜底：超时后 Backend 仍在执行，违背
  "取消必须可在长任务内生效"（AGENTS.md 同步边界第 4 条），被否。

## 影响与风险

- Backend 实现者必须理解"字段消费方"契约；测试用 Fake Backend 固化该行为（消费字段
  被误用时测试失败）。
- 输出空间契约把坐标恢复责任固定在 Mirador 侧；若未来某类 Backend 必须输出非
  prepared 空间（如带自身配准的几何 Backend），需修订本决策并在设计中补充对应空间链。
- `backend_params` 是字符串，不提供类型安全；换取的是 Backend 演进不破坏公共 ABI。

## 验证方式

`M2-01` 单元测试（`validate`、请求默认值）；`M2-02` 键字段敏感性矩阵（`RULE-07` 逐项
失配）；`M2-05` Fake Backend 端到端矩阵：坐标恢复（0/90/180/270、stride、奇数尺寸、
ROI+缩放）、取消/超时、`kRefresh`/`kReadOnly`、格式门控。见
`tests/core/backend_test.cpp`、`tests/cache/capability_cache_test.cpp`、
`tests/fusion/perception_session_test.cpp`。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §9、§13、§18、§19；总计划
`RULE-03`、`RULE-05`、`RULE-07`、`RULE-08`、`POST-04`/`POST-05`；`M2-01`~`M2-05`。
