# DEC-003：公共 API 不暴露 OpenCV 类型，ImageView 是图像边界

> 状态：Accepted
> 日期：2026-09-13
> 负责人：linductor
> 冻结里程碑：M0（随设计文档生效）
> 替代/被替代：无

## 背景与问题

设计文档（第 5、6、21 节）要求：OpenCV 只能作为部分模块的可选实现依赖，公共头文件不得
暴露 `cv::Mat`，以防止 ABI、版本与构建选项绑定；调用方通过适配函数把 `cv::Mat`、Android
`AHardwareBuffer`、连续内存等转换为 `ImageView`。

## 决策

1. 公共 API 的图像输入边界是 `ImageView`（非拥有视图：数据指针、宽高、步长、像素格式、
   旋转）与 `Frame`（附加序号、时间戳、来源 ID 与 `owner` 生命周期）。
2. OpenCV 类型只允许出现在 `adapters/opencv/` 与对应模块的可选编译单元中，CMake 选项默认
   关闭；公共头不得包含任何 OpenCV/第三方头。
3. 第一阶段所有算法支持普通 CPU 连续内存；NV12 等多平面格式经 `ImagePlane` 扩展表示，
   不假定单连续平面（`DEC-007`）。

## 备选方案

- 直接使用 `cv::Mat` 作为公共类型：被否决——绑定 OpenCV ABI 与版本，排除无 OpenCV 的
  Android/嵌入式宿主，违背依赖方向（设计 §5）。
- 自建拥有型 `Image` 类并在库内深拷贝：被否决——阻断零拷贝路径，增加终端内存压力。

## 影响与风险

- 调用方需要一次视图适配（成本近似为零）。
- stride、旋转与多平面的正确性成为核心责任，必须有方向/stride/奇数尺寸/往返容差测试矩阵。

## 验证方式

架构测试断言公共头无第三方类型；坐标与图像测试矩阵（M0-04/M1）；`adapters/opencv` 以
可选模块独立验证。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §5、§6、§21；`AGENTS.md`；总计划
`RULE-02`、`RULE-04`、`SCOPE-07`；`M0-03`、`M0-05`。
