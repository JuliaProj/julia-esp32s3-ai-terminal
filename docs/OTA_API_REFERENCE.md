# Julia OTA 接口参考

本文是 OTA 开发与联调的接口索引，也是 Doxygen HTML 文档首页。真实下载、固件分区写入、素材目录切换及 Bootloader 回退由 OTA 实现组负责；当前 Julia 模块只维护状态快照并同步发布事件。

## 快速入口

- 开发接入流程：[OTA_INTEGRATION_GUIDE.md](OTA_INTEGRATION_GUIDE.md)
- 硬件测试步骤：[OTA_HARDWARE_TEST_GUIDE.md](OTA_HARDWARE_TEST_GUIDE.md)
- 测试记录模板：[ota_test_report_template.md](ota_test_report_template.md)
- 注释与文档规范：[CODING_STYLE.md](CODING_STYLE.md)

## 版本和头文件

| 契约 | 头文件 | 当前版本 | 编译期主版本宏 |
|---|---|---:|---|
| OTA | `julia_ota.h` | 1.0.0 | `JULIA_OTA_VERSION_MAJOR` |
| 事件总线 | `julia_event_bus.h` | 1.0.0 | `JULIA_EVENT_BUS_VERSION_MAJOR` |

主版本不一致表示接口不兼容，OTA 组必须在编译期拒绝构建。启动顺序固定为事件总线初始化、OTA 初始化、订阅回调注册、OTA 工作任务启动。

## 公开接口

| 接口 | 用途 | 成功结果 | 主要失败结果 |
|---|---|---|---|
| `julia_ota_init()` | 清空快照并进入空闲状态 | `ESP_OK` | 当前无失败路径 |
| `julia_ota_get_snapshot()` | 将当前状态复制到调用方 | `ESP_OK` | 输出地址为空时 `ESP_ERR_INVALID_ARG` |
| `julia_ota_is_active()` | 判断是否正在下载、校验或安装 | `true/false` | 无错误码 |
| `julia_ota_publish()` | 更新快照并同步发布阶段事件 | `ESP_OK` | 阶段、类型、进度非法或总线未初始化时 `ESP_ERR_INVALID_ARG` |

所有接口均禁止从 ISR 调用。当前快照和事件总线不含内部锁，只能由单一 OTA 工作任务调用，或由调用方提供外部互斥。事件回调在发布者上下文同步执行，回调不得阻塞、递归发布或保存载荷地址。

## 快照数据

| 字段 | 格式 | 有效范围与生命周期 |
|---|---|---|
| `status` | `julia_ota_status_t` | 空闲、下载、校验、安装、完成或失败 |
| `type` | `julia_ota_type_t` | 固件或素材 |
| `progress` | `uint8_t` | 0~100；只在进度阶段有业务含义 |
| `error_code` | `julia_ota_error_t` | 0~4；只在失败或回退阶段有业务含义 |
| `version` | NUL 结尾字符串 | 最长 31 字节；空串表示未知 |
| `message` | NUL 结尾字符串 | 最长 95 字节；用于简短状态或错误摘要 |

快照由 OTA 模块持有。事件载荷指向该快照，只在同步发布调用期间有效；订阅方如需异步处理，必须复制值而不是保存指针。

## 状态与事件映射

| 发布事件 | 快照状态 | 进度要求 | 错误码要求 |
|---|---|---|---|
| `JULIA_EVENT_OTA_START` | `JULIA_OTA_DOWNLOADING` | 通常为 0 | 0 |
| `JULIA_EVENT_OTA_PROGRESS` | 保持下载状态 | 0~100，单调不减 | 0 |
| `JULIA_EVENT_OTA_VERIFY` | `JULIA_OTA_VERIFYING` | 通常为 100 | 0 |
| `JULIA_EVENT_OTA_INSTALL` | `JULIA_OTA_INSTALLING` | 通常为 100 | 0 |
| `JULIA_EVENT_OTA_COMPLETE` | `JULIA_OTA_COMPLETE` | 100 | 0 |
| `JULIA_EVENT_OTA_FAIL` | `JULIA_OTA_FAILED` | 最近有效进度 | 1~3 |
| `JULIA_EVENT_OTA_ROLLBACK` | `JULIA_OTA_FAILED` | 最近有效进度 | 4 |

终态 `COMPLETE`、`FAIL` 和终止性 `ROLLBACK` 互斥。新的流程必须以 `START` 开始；状态机顺序、进度单调性和终态唯一性由 OTA 工作流保证，基础发布接口只校验单次参数范围。

## 错误码

| 数值 | 枚举 | 含义 |
|---:|---|---|
| 0 | `JULIA_OTA_ERROR_NONE` | 无错误 |
| 1 | `JULIA_OTA_ERROR_DOWNLOAD` | 网络、超时或服务端下载失败 |
| 2 | `JULIA_OTA_ERROR_VERIFY` | SHA、签名、manifest 或文件校验失败 |
| 3 | `JULIA_OTA_ERROR_INSTALL` | 分区写入、空间、权限或目录切换失败 |
| 4 | `JULIA_OTA_ERROR_ROLLBACK` | 旧分区或素材备份不可用 |

## 串口 Mock 契约

Mock 命令格式为：

`ota mock <stage> <firmware|asset> <progress> <error> [version] [message]`

`stage` 支持 `start`、`progress`、`verify`、`install`、`complete`、`fail` 和 `rollback`。非失败阶段错误码必须为 0；`fail` 和 `rollback` 的错误码必须为 1~4。可用 `ota mock reset` 清空快照，用 `ota status` 查询结果。

Mock 只更新快照和发布事件，不联网、不写 Flash、不操作 SD、不切换分区且不重启。`ota check`、`ota start`、`ota cancel` 和真实 `ota rollback` 尚未实现，不能用于当前验收。

## 生成 HTML

安装 Doxygen 后，在仓库根目录执行 `doxygen Doxyfile`；本地已存在便携工具时也可执行 `tools\doxygen\doxygen.exe Doxyfile`。成功后打开 `build_doxygen\html\index.html`。配置将 Doxygen 告警视为失败，告警日志为 `build_doxygen\doxygen-warnings.log`。
