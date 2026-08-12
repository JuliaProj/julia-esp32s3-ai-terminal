# Julia UI 功耗测试固件包 v1.0.0

## 适用范围

本包用于微雪 ESP32-S3 1.85 英寸 LCD 开发板的 UI 和功耗测试。固件重点覆盖角色静态画面、状态切换、背光控制、休眠画面、约 5 分钟无操作呼吸模式和串口维护命令。

## 文件

| 文件 | 用途 |
|---|---|
| `bootloader.bin` | ESP32-S3 启动程序 |
| `julia-ui.bin` | UI/系统应用固件 |
| `partition-table.bin` | 分区表 |
| `ota_data_initial.bin` | OTA 初始选择数据 |
| `srmodels.bin` | 语音前端模型分区，供当前固件启动使用 |
| `flash_args.txt` | 烧录地址和 Flash 参数 |
| `flash_windows.bat` | Windows 一键烧录脚本 |
| `SHA256SUMS.txt` | 文件校验值 |

## 下载

在 GitHub 仓库的 `Releases` 页面下载 `Julia-UI-Firmware-v1.0.0` 目录中的全部文件。必须保持文件名不变，并将全部 `.bin` 文件放在同一目录。

## Windows 烧录

1. 安装 ESP-IDF 5.5.x，确保 `esptool.exe` 在 PATH 中。
2. 使用 USB 数据线连接开发板，确认串口号，例如 `COM5`。
3. 打开 PowerShell 或 CMD，进入本目录。
4. 执行：

```text
flash_windows.bat COM5
```

如需手动烧录，按 `flash_args.txt` 中的地址执行 `esptool.py`。

## 烧录后验证

打开串口监视器，波特率 `115200`，执行：

```text
status
state 3
screen-timeout 10
screen-wake
doze enter
doze status
```

`screen-timeout 10` 仅用于快速验证；正式功耗测试使用 `screen-timeout 300`，等待约 5 分钟后观察呼吸模式。

## 重要限制

本包来源于当前已有的稳定构建产物，构建时间以固件文件实际时间为准；它不包含 Wi-Fi 密码、API Key 或其他生产配置。ASR/TTS/LLM 和家居控制服务需要另行配置，暂不属于 UI 功耗测试包的验收范围。
