# Julia ESP32-S3 智能陪伴终端

## 稳定版本

`master` 分支仅用于发布当前稳定版本和功耗测试基线。当前正式版本标签为 `v1.0.0`。

本仓库面向微雪 ESP32-S3 1.85 英寸 LCD 开发板，屏幕目标分辨率为 360 x 360。仓库包含可复现的 ESP-IDF 源代码、稳定的 UI/状态运行时、非敏感配置模板和功耗测试说明。

已排除生产密钥、本地 `sdkconfig`、构建输出、调试日志、个人文件、生成缓存和设备固件二进制。

## 快速开始

1. 安装 ESP-IDF 5.5.x 及 ESP32-S3 工具链。
2. 使用 `sdkconfig.defaults` 生成本地配置。不要提交本地 `sdkconfig`。
3. 设置芯片目标并编译：

```text
idf.py set-target esp32s3
idf.py build
```

如需烧录，请根据开发板连接方式执行：

```text
idf.py -p <串口号> flash monitor
```

## 当前稳定范围

- UI 状态机和角色状态切换。
- LCD 显示、背光渐变和约 5 分钟无操作呼吸模式。
- 呼吸灯控制、低功耗显示处理和电源管理接口。
- 串口维护命令、状态切换和功耗测试辅助命令。
- 稳定版 UI 动画播放器、状态转换框架和资源加载接口。
- Wi-Fi、语音、云端模型和家居控制源代码框架。

语音云服务、LLM、网络服务器和家居控制模块需要测试账号或目标硬件，仓库不宣称这些外部服务已经完成生产验收。

## 功耗测试

请先阅读 [功耗测试指南](docs/power_test_guide.md)，其中包含：

- S0 至 S5 状态进入方法。
- 屏幕全亮、降亮度、休眠呼吸模式测试。
- Wi-Fi、语音和网络异常场景。
- 平均电流、峰值电流、测试电压和记录格式。
- 约 5 分钟无操作后进入呼吸模式的测试步骤。

## UI 固件包

本地发布包目录为 `release/Julia-UI-Firmware-v1.0.0/`，包含 Bootloader、应用固件、分区表、语音模型分区、Windows 烧录脚本、Flash 地址和 SHA-256 校验值。固件包的详细下载和烧录方法见其中的 `README_中文.md`。

由于固件二进制体积较大且构建产物不纳入源码仓库，发布包应作为 GitHub Release 附件或校内文件服务器下载，不应直接放入 Git 提交历史。

## 项目文档

- [状态素材矩阵](docs/state_asset_matrix.md)
- [状态转换素材流程](docs/transition_asset_pipeline.md)
- [待机动画规范](docs/idle_animation_guide.md)
- [串口维护命令](docs/avatar_serial_commands.md)
- [部署说明](docs/deployment_guide.md)
- [云端配置模板](docs/cloud_config_template.md)
- [ASR 配置模板](docs/asr_config_template.md)

## 仓库内容边界

仓库保留源代码、必要的 ESP-IDF 组件依赖、稳定版脚本、配置模板和测试文档。个人文件、外包需求文档、临时生成素材、日志、固件二进制、API Key、Wi-Fi 密码和备份目录均不纳入发布版本。

`.trn` 是设备端运行格式，不是外包源素材交付格式；外包源素材应按独立交付清单提供 PNG 序列、MP4 预览和可编辑工程，由研发端转换后再部署到设备。
