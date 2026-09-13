# DEC-004：异常与错误模型——公共边界使用 Result/Status

> 状态：Accepted
> 日期：2026-09-13
> 负责人：linductor
> 冻结里程碑：M0（随 `M0-02` 实现与测试冻结）
> 替代/被替代：无

## 背景与问题

设计文档（第 18-19 节）指出异常策略需要在项目早期确定，并建议面向 Android NDK 的公开
边界提供 `Result<T>` 与 `Status`，避免强迫使用者启用异常。M0-02 需要冻结该模型后才能定义
公共 API 签名形态。

## 决策

1. 公共 API 返回 `Result<T>`，携带 `Status`（稳定错误码 + 短消息）；错误码分类覆盖设计
   §19：无效输入、不支持的像素格式、坐标变换错误、Backend 不可用、Backend 执行失败、
   超时、取消、缓存损坏、资源预算超限。
2. 异常不穿越公共 API 边界；库内部不使用异常表达常规错误流。不要求编译期禁用异常
   （`-fno-exceptions`），但公共边界不依赖异常机制。
3. 错误码不泄漏任何 runtime 的错误枚举；Backend 可在诊断字符串中附加实现私有信息。
4. `Status` 支持与 `ExecutionContext` 取消/超时的明确映射（取消不冒充失败，反之亦然）：
   `ErrorCode::kTimeout` 与 `ErrorCode::kCancelled` 是互斥的独立类别。

落地形态：`include/mirador/status.hpp` 定义 `ErrorCode`（`uint8_t` 基类型）与 `Status`，
`include/mirador/result.hpp` 定义 `Result<T>`（含 `Result<void>` 特化）；访问器带前置
条件断言，不抛异常；`error_code_name()` 提供稳定诊断名。

## 备选方案

- 直接抛异常：被否——Android/嵌入式调用方可能整体禁用异常，且错误路径成本不可预测。
- 全局错误码 + out 参数：被否——缺少组合性与 payload 表达力，调用方容易忽略检查。

## 影响与风险

- 决定所有公共函数签名形态与 Backend SPI 返回类型；M0-02 之后修改成本高，须在冻结前
  完成评审。
- `Result<T>` 的模板设计需要避免把实现细节（如异常栈）带入公共头。

## 验证方式

`M0-02` 单元测试覆盖每个错误类别的构造、传播与映射；文档给出错误处理示例。

2026-09-13：`mirador.core.status_result` 测试通过——全部九类错误码可构造并可经
`error_code_name()` 输出稳定诊断名，`Result<T>`/`Result<void>` 值与错误路径、
`value_or`、`take_value`、错误传播链均有断言；超时与取消的互斥性有专项断言。
证据见 M0 里程碑验证记录。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §18、§19；总计划 `RULE-08`、`DOD-02`；
`M0-02`。
