# DEC-023：桌面感知精细比较与热路径优化

> 状态：Accepted（维护者授权修复及性能优化）
> 日期：2026-10-16
> 负责人：Mirador 维护者
> 工作项：[DP-01…05](../plans/desktop-perception-quality-performance.md)

## 背景

真实桌面复核（issue #36）发现细描边在默认参数下没有线段、全图 dHash 相同时
可见局部修改被提前返回，以及逐目标验证对每个偏移重复生成完整 patch 指纹。
应用可以调整阈值或重跑局部 OCR，但库应提供明确的精细比较和避免无用工作。

## 决策

在 `ChangeDetectionParams` 末尾追加 `fingerprint_early_exit=true`。false 强制执行
现有缩略图 block diff，不改变指纹含义、忽略区域或默认行为。view/signature/
session 路径统一语义；它不是原始像素逐点比较，也不保证检出低于缩略图阈值的变化。
源码 aggregate 的原有字段顺序不变；结构大小变化要求消费者重新编译，不承诺旧 ABI。

细描边通过分离相反梯度的边缘区域修正，不提高残差或线段预算。区域内仍保留无向
线段角度、确定性遍历、长度/置信度和有界输出。长区域生长增加协作取消检查。

跟踪验证保持 NCC、PSR、阈值、搜索网格/排序和保守 planned-work 接纳公式不变。
只在实际搜索窗口的包围区域做一次 crop/逐像素灰度，逐候选使用非拥有灰度视图
生成同样的 thumbnail；不生成 NCC 未使用的 content hash/dHash，不改变模型或
PSR 默认值。不同格式/旋转/stride/边界需要和原公开 crop→fingerprint 组合差分。

## 验收与边界

新回归在旧代码上失败；默认变化检测旧测试保持通过；跟踪差分逐项检查 peak、PSR、
偏移和判定。相同 Release 输入/预热/次数提供前后 median/p95 与明确环境；全测试
及 ASAN/UBSAN 验证。无新依赖、线程、全局缓存、日志或磁盘图像行为。
