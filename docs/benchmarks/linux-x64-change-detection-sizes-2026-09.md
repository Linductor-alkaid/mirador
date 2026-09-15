# Linux x64 基准报告：变化检测与模块体积（2026-09）

> 状态：Active（M5-06 收口数字）
> 日期：2026-09-15
> 负责人：linductor
> 环境：全部数字按 [DEC-011](../decisions/DEC-011-benchmark-environments.md)
> 采集，**仅对下述机器与构建有效**，不构成跨平台声明。

## 环境

| 项 | 值 |
| --- | --- |
| OS | Ubuntu 24.04 x64（Linux 6.x），与缓存报告同机 |
| 构建 | release（`cmake --preset release`），CMake 3.28.3 + Ninja，GCC 13.3.0 |
| 分支 | `feat/m5-display-contract-and-capture-adapters`（M5-06 收口点） |
| 方法 | 确定性合成帧；预热 + 固定迭代；p50/p95 墙钟（`DEC-011` §2 口径） |

## 变化检测（`mirador_bench_change_detection`，1280x720 RGBA8，300 iterations）

| 场景 | 分类 | p50 | p95 |
| --- | --- | --- | --- |
| unchanged（不变画面） | kUnchanged | 1.64 ms | 3.50 ms |
| partial-change（局部变化） | kPartial | 10.81 ms | 10.97 ms |
| rotation-180（旋转） | kGlobal | 10.77 ms | 10.92 ms |
| dynamic-ignored（忽略区域内动态） | kUnchanged | 10.78 ms | 10.96 ms |

口径说明：

- 上表"分类"为各场景的期望分类，与基准运行时打印值一致（0=kUnchanged、
  1=kPartial、2=kGlobal）；基准仅对 `detect_change` 错误非零退出，分类正确性
  由 M1 单元/集成测试锁住，此处为人工核对项。
- unchanged 路径 p50 ≈ 1.6 ms——低分辨率签名快速路径；其余场景进入分块差分
  全量路径（≈ 10.8 ms，1280x720 一次全帧扫描）。**"不变画面近零成本"的层级
  结构成立：便宜路径先走，贵的路径只在画面变化时付出。**
- dynamic-ignored 走全量路径（忽略区域判定本身需要分块差异），但分类仍为
  kUnchanged——忽略区域机制防止无意义失效，代价可控。

## 模块静态体积（`benchmarks/measure_sizes.sh build/release`）

| 构件 | 字节 |
| --- | --- |
| libmirador_core.a | 39,664 |
| libmirador_image.a | 203,052 |
| libmirador_cache.a | 83,744 |
| libmirador_geometry.a | 50,592 |
| libmirador_fusion.a | 236,498 |
| libmirador_render.a | 25,346 |
| 全部模块静态库合计 | 638,896（0.61 MiB） |
| 基准/示例可执行文件（7 个） | 1,119,344 |
| 合计（全部列出构件） | 1,758,240 |

复现命令：

```sh
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/mirador_bench_change_detection
./benchmarks/measure_sizes.sh build/release
```

口径说明：静态库为 GCC release（-O2/-DNDEBUG）产物；模块按需裁剪
（`MIRADOR_BUILD_*` 全开为上图口径）；核心 + image + cache（最小感知闭环）
静态体积 ≈ 326 KiB，符合"轻量核心"的体量预期。

## 限制（`DEC-011` 补跑条件）

- 物理 Android 设备与 Windows 桌面数字缺失；补跑前体积与延迟结论限定 Linux x64。
- CI runner 数字不采信；Android 功耗/温升挂起至物理设备补跑。
