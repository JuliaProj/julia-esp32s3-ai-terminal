# Julia UI OTA Mock 测试固件包 v1.1.0-ota-mock

## 用途

本包供 OTA 接口联调和硬件测试使用，增加 `ota status`、`ota mock reset` 及分阶段 `ota mock` 串口命令。Mock 只更新 OTA 快照并发布事件，不联网、不写 Flash、不修改 SD 素材目录、不切换启动分区且不重启。

本包不是完整 OTA 下载器。`ota check`、真实 `ota start`、`ota cancel`、固件安装和素材原子切换仍由 OTA 实现组后续接入。

## 文件

| 文件 | 用途 |
|---|---|
| `bootloader.bin` | ESP32-S3 Bootloader |
| `julia-ui.bin` | 包含 OTA Mock 命令的应用固件 |
| `partition-table.bin` | 16 MB Flash 分区表 |
| `ota_data_initial.bin` | OTA 初始选择数据 |
| `srmodels.bin` | 语音前端模型分区 |
| `flash_args.txt` | Flash 参数和烧录地址 |
| `flash_windows.bat` | Windows 一键烧录脚本 |
| `SHA256SUMS.txt` | 发布文件 SHA-256 校验值 |

## 烧录

安装 ESP-IDF 5.5.x 并确保 `esptool.exe` 在 PATH 中。连接设备后，在本目录执行：

```text
flash_windows.bat COM5
```

将 `COM5` 替换为实际串口。烧录会覆盖对应 Flash 分区，应先确认设备和端口正确。

## 快速验证

串口参数为 115200 bit/s、8N1、无流控。烧录并重启后发送：

```text
ota mock reset
ota mock start firmware 0 0 v-test start
ota mock progress firmware 50 0 v-test downloading
ota mock verify firmware 100 0 v-test verifying
ota mock install firmware 100 0 v-test installing
ota mock complete firmware 100 0 v-test complete
ota status
```

最后状态应包含 `status=4 type=0 progress=100 error=0 active=0`。

Windows 自动化测试：

```text
powershell -ExecutionPolicy Bypass -File ..\..\tools\test_ota_hardware.ps1 -Port COM5
```

完整步骤见 `docs/OTA_HARDWARE_TEST_GUIDE.md`，接口契约见 `docs/OTA_API_REFERENCE.md`。

## 构建信息

- 基础 Git commit：`8a011b1`
- 构建日期：2026-08-14
- ESP-IDF：5.5.4
- 目标芯片：ESP32-S3
- Flash：16 MB，DIO，80 MHz
- 应用大小：3,740,416 字节
- 最小应用分区剩余约 21%

固件来自包含 OTA 文档、接口模块和 Mock 命令的本地验证构建；完整 ESP-IDF 编译和链接已通过。
