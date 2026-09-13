# 更新日志

本文件格式参考 Keep a Changelog，版本号遵循语义化版本；每个里程碑的发布点与
[实施总计划](docs/plans/mirador-implementation-plan.md)的里程碑索引一一对应。

## [0.1.0-alpha] - 2026-09-14

M0「边界与骨架」：公共数据类型冻结、依赖边界锁定与三平台 CI
（[PR #1](https://github.com/Linductor-alkaid/mirador/pull/1)）。

### 新增

- 公共错误模型 `Status` / `Result<T>` / `Result<void>`：九类稳定错误码（无效输入、
  不支持的像素格式、坐标变换、Backend 不可用、Backend 执行失败、超时、取消、缓存损坏、
  预算超限）；异常不穿越公共 API，错误不泄漏 runtime 枚举（`DEC-004`）。
- 公共输入模型 `PixelFormat` / `Rotation` / `ImagePlane` / `ImageView` / `Frame`：
  非拥有视图、NV12 多平面表示（`DEC-007` 草案）、结构性校验 `validate()` 与
  `kMaxImageDimension` 尺寸保护。
- 坐标模型 `CoordinateSpaceId` / `Transform2D`：仿射变换与旋转/缩放/裁剪/镜像/letterbox
  工厂、空间校验的组合与求逆，点/矩形/多边形/线段统一映射（`transform_*` 命名避开
  与 `std::apply` 的 ADL 二义）。
- 架构测试（ctest 标签 `architecture`）：公共头与核心源码的线程设施/第三方库令牌扫描、
  Linux readelf 链接闭包检查、`mirador::core` 链接接口为空的 configure 断言
  （`RULE-01`~`RULE-03`）。
- CMake 模块目标 `mirador::core/image/cache/geometry/fusion/render` 与
  `MIRADOR_BUILD_<MODULE>` 按模块开关；`debug`/`release`/`warnings`/`asan`/`ubsan`/`tsan`
  及 `windows-debug` 构建预设（`DEC-005`）。
- 测试基线：GoogleTest v1.18.0 + ctest 标签机制（`unit` / `property` / `architecture`），
  坐标往返容差矩阵与固定种子属性测试。

### 兼容性影响

- 首个版本，无既有 API 影响。
- 公共头在 GCC、Clang、MSVC 下编译通过；Android NDK arm64-v8a 交叉编译通过。NDK 侧
  仅做 configure/build 验证，设备侧测试按计划在 M5 补跑。
- 测试默认开启（`MIRADOR_BUILD_TESTS=ON`），需要递归检出 googletest 子模块；最小核心
  构建可用 `-DMIRADOR_BUILD_TESTS=OFF`，不获取任何第三方依赖。

### 依赖变化

- 新增 googletest v1.18.0（BSD-3-Clause，submodule + `dependencies.lock.json` 锁定，
  仅测试目标，不进入 `mirador-core` 链接闭包）。审计记录见
  [docs/supply-chain/googletest.md](docs/supply-chain/googletest.md)。
