# Julia OTA 硬件测试手册

本文供硬件测试人员在 ESP32-S3 实机上验证 OTA 接口、串口协议、状态快照和 UI 事件链路。当前阶段提供安全 Mock，不执行真实下载、Flash 擦写、素材目录切换或设备重启。

## 1. 测试范围

当前可以自动验证：

- 串口命令可达性和 OTA 初始化状态。
- 固件、素材两类事件的开始、进度、校验、安装和完成链路。
- 下载、校验、安装和回退错误码进入失败状态。
- 进度范围、成功 ACK、最终快照和设备异常复位关键字。

当前只能人工观察或等待后续模块实现：

- UI 是否订阅 OTA 事件并显示升级页。当前代码尚无 OTA UI 订阅者，此项预期为“未接入”。
- 真实 HTTP 下载、SHA/签名校验和固件分区切换。
- 素材解压、原子目录切换和真实回退。
- `ota check`、`ota start`、`ota cancel` 和真实 `ota rollback` 命令。

## 2. 测试准备

| 项目 | 要求 |
|---|---|
| 设备 | ESP32-S3 Julia 主板，供电稳定 |
| 固件 | 包含 `ota mock` 命令的当前构建 |
| 串口 | 默认 COM5，115200 bit/s，8N1，无流控 |
| 主机 | Windows PowerShell 5.1 或更高版本 |
| 显示 | LCD 可正常进入待机画面，便于记录 UI 行为 |
| 日志 | 测试期间关闭其他占用同一 COM 端口的软件 |

先手工发送 `ota status`。预期收到以 `OTA status=` 开头的一行，并包含 `type`、`progress`、`error`、`active`、`version` 和 `message`。

## 3. 安全说明

- `ota mock` 是非破坏性命令，不写 Flash、不改 SD、不联网、不重启。
- 本手册不会自动发送刷机、擦除、真实升级或真实回退命令。
- 后续执行断电和真实回退测试前，必须确认拥有可恢复固件和串口刷写条件。
- 自动化脚本检测到异常复位、panic、watchdog、命令无 ACK 或状态不匹配时判定失败。

## 4. Mock 命令

| 命令 | 作用 | 预期输出 |
|---|---|---|
| `ota mock reset` | 清空快照并回到空闲 | `OTA_MOCK stage=reset result=ESP_OK` |
| `ota mock start firmware 0 0 v-test start` | 模拟固件升级开始 | `OTA_MOCK ... result=ESP_OK` |
| `ota mock progress firmware 50 0 v-test downloading` | 模拟 50% 进度 | ACK 中进度为 50 |
| `ota mock verify firmware 100 0 v-test verifying` | 模拟校验 | 状态变为校验中 |
| `ota mock install firmware 100 0 v-test installing` | 模拟安装 | 状态变为安装中 |
| `ota mock complete firmware 100 0 v-test complete` | 模拟完成 | 状态变为完成且 active 为 0 |
| `ota mock fail firmware 40 1 v-test download_failed` | 模拟下载失败 | 状态变为失败，错误码为 1 |
| `ota mock rollback asset 100 4 v-test rollback_failed` | 模拟回退失败 | 状态变为失败，错误码为 4 |

消息参数不允许空格；需要多个词时使用下划线。非法参数只打印 Usage，不更新快照。

## 5. 自动化测试

从仓库根目录执行：

`powershell -ExecutionPolicy Bypass -File tools\test_ota_hardware.ps1 -Port COM5`

只查看脚本将发送的命令，不连接设备：

`powershell -ExecutionPolicy Bypass -File tools\test_ota_hardware.ps1 -DryRun`

脚本依次执行固件成功链路、素材成功链路和四类失败链路。输出保存在 `tmp/ota-hardware-tests/<timestamp>/`：

- `serial.log`：完整串口文本。
- `OTA_TEST_REPORT.md`：自动判定、设备信息和人工 UI 检查表。

脚本退出码为 0 表示自动项目通过，非 0 表示至少一个 ACK、状态、进度或稳定性检查失败。退出码不代表 UI 人工观察通过。

## 6. 手工测试用例

### HW-OTA-001 初始状态

操作：发送 `ota mock reset`，再发送 `ota status`。

预期串口：reset 返回 `ESP_OK`；状态为 0，进度为 0，active 为 0。

预期 UI：不应因 reset 闪屏或离开当前页面。

通过标准：串口字段匹配，设备无复位、panic 或 watchdog。

### HW-OTA-002 固件成功链路

操作：依次发送 start、进度 25/60/100、verify、install、complete，每步后查询 status。

预期串口：所有 ACK 为 `ESP_OK`；进度单调；状态依次为下载、校验、安装、完成；最终 error 为 0、active 为 0。

预期 UI：当前版本尚无 OTA UI 订阅者，应记录“未接入”，不能误判成事件发布失败。UI 接入后应显示升级页、进度、校验和安装状态。

通过标准：接口链路通过；UI 项单独记录当前实现状态。

### HW-OTA-003 素材成功链路

操作：将 HW-OTA-002 的类型改为 asset。

预期串口：最终 type 为 1、status 为 4、progress 为 100、error 为 0。

预期 UI：当前记录“未接入”；未来应在完成事件后重载 manifest 并恢复先前状态。

通过标准：接口快照正确，画面无异常闪烁或设备复位。

### HW-OTA-004 下载失败

操作：reset、start，然后发送 fail，错误码 1。

预期串口：最终 status 为 5、error 为 1、active 为 0。

预期 UI：接入后显示下载失败和重试选项；当前记录“未接入”。

通过标准：失败后不出现 complete，设备保持可响应 `ota status`。

### HW-OTA-005 校验失败

操作：reset、start、progress 100、verify，然后发送 fail，错误码 2。

预期串口：最终 status 为 5、error 为 2。

通过标准：快照正确且设备稳定；真实 OTA 接入后还需确认旧分区或旧素材保持可用。

### HW-OTA-006 安装失败

操作：reset、start、progress 100、verify、install，然后发送 fail，错误码 3。

预期串口：最终 status 为 5、error 为 3。

通过标准：快照正确且设备稳定；真实素材 OTA 接入后还需验证生产目录恢复。

### HW-OTA-007 回退失败

操作：reset、start，然后发送 rollback，错误码 4。

预期串口：最终 status 为 5、error 为 4。

通过标准：快照正确，设备仍可响应命令。

### HW-OTA-008 非法参数

操作：分别发送进度 101、未知阶段、未知类型、成功阶段携带非零错误码。

预期串口：打印 Usage；随后 status 与非法命令前一致。

通过标准：无数组越界、panic、watchdog 或异常复位。

## 7. 真实 OTA 后续验收

真实 OTA 模块完成后追加以下破坏性或存储相关测试，不能用 Mock 结果代替：

1. 下载 30% 时断网，确认错误码 1、旧固件可启动。
2. 提供错误 SHA/签名，确认错误码 2 且目标分区不被设为启动分区。
3. 分区写入中断电，确认 Bootloader 仍选择有效分区。
4. 新固件首次自检失败，确认自动回退并报告结果。
5. 素材 manifest 或单文件 CRC 错误，确认生产目录完全不变。
6. 素材生产目录改名成功、临时目录改名失败，确认立即恢复旧生产目录。
7. 连续安装三个素材版本，确认只保留最近两个有效备份并可回退。
8. OTA 全程记录 UI 帧率、最小堆和 watchdog，确认没有阻塞 UI tick。

## 8. 故障排查

- 无输出：确认端口号、115200 波特率、端口未被监视器占用。
- 只看到 Usage：检查参数顺序、消息中是否含空格，以及失败错误码是否为 1~4。
- ACK 成功但 status 不变：确认查询发生在 ACK 后，并保存完整串口日志。
- UI 不变化：当前固件尚无 OTA UI 订阅者，这是已知限制；先以 ACK 和快照判断接口链路。
- 设备复位：在日志中查找 `rst:0x`、`Guru Meditation`、`panic` 和 `watchdog`，测试直接判定失败。
