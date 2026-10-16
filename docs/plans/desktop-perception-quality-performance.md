# 桌面感知质量与性能维护

> 状态：In Progress
> 日期：2026-10-16
> 负责人：Mirador 维护者
> 总计划：[实施计划](mirador-implementation-plan.md)
> 依据：用户授权直接修复 Mirador 并走 PR；[issue #36](https://github.com/Linductor-alkaid/mirador/issues/36)、DEC-009/010/019

## 范围

同步、无 runtime、无内部并发的几何/变化检测/跟踪路径。保持默认变化门控语义、
跟踪 NCC/PSR 判定及预算/取消契约；不将 Mirage 的产品策略下沉，也不以放宽
PSR 门槛代替优化。Mirage pin 的升级与合并不在本 PR 范围。

## 工作项

- [x] `DP-01` 以相同 Release harness 记录优化前后数据及可复现基准。
- [x] `DP-02` 修复细描边线段遗漏，消除不必要的分配/拟合工作，覆盖描边/抗锯齿/噪声/取消。
- [x] `DP-03` 显式选择 block diff，消除 view 路径重复指纹，保持缺省兼容。
- [x] `DP-04` 减少逐候选跟踪 crop/灰度/无用哈希开销，证明与公开旧组合路径等价。
- [ ] `DP-05` 完整测试、ASAN/UBSAN、契约与证据同步，提交并创建 PR/检查 CI。

## 验收与风险

正确性优先于耗时；公开字段追加保持 aggregate 初始化兼容，但结构大小变化需 ABI
说明。细粒度模式仍是缩略图分块，不宣称逐像素等价。跟踪优化保留保守 work meter，
不改变预算接受集合；跨格式/stride/旋转/分数边界须有差分验证。性能只报告实测平台。

## 验证记录

2026-10-16：从 upstream master `ae8cf87581c1ebe393a67374d9977a38354d0911`
建立 `perf/desktop-perception` 独立工作树。Mirage 的真实帧已在本机受控目录，
不提交原图；baseline 与 candidate 用相同输入、预热和重复次数。

2026-10-16：DP-01…04 Done。Release（warnings-as-errors）、ASAN、UBSAN 各
53/53 CTest 目标通过。新增三项回归分别链接基线实现均失败；保留原始断言。
ASAN 初轮并行编译触发 `/tmp` 隐私检查噪声，全量串行重跑通过。
真实图与合成图的[对照基准](../benchmarks/linux-x64-desktop-perception-2026-10.md)
及 CSV 已保存；原图未入仓。真实截图线段叠加已视觉复核。DP-05 等待静态检查与 PR CI。
