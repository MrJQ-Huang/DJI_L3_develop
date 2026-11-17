# L3 实时点云订阅问题报告（PSDK 3.18.0 / M400 / 妙算3）

日期：2026-11-17（Asia/Shanghai）。本目录用于向 DJI PSDK 工程师反馈问题，
只包含脱敏后的标准输出，不包含 App Key、签名、设备身份、真实画面/点云或原始 SDK 日志。

## 1. 现场环境

- 飞机：Matrice 400，固件 `19.01.10.03`
- 负载：禅思 L3，PSDK 挂载位置 1，`DjiCameraManager_GetCameraType` 返回 `117`（L3）
- 机载计算：妙算3，Ubuntu 20.04.6 / AArch64 / kernel 5.10.192-tegra / L4T 35.5.0
- 工具链：GCC 9.4.0 / CMake 3.16.3
- PSDK：3.18.0，官方 commit `f3938d881e1628fc9a12e1b982b77060b9006311`
- 官方静态库：

```text
psdk_lib/lib/aarch64-linux-gnu-gcc/libpayloadsdk.a
SHA256: 5725cc72824d915410be59e98cb31b4d269510c4615c1033da8d5fffbffdb5e7
```

- 测试范围：全程地面，只取 L3 数据，不启电机、不起飞、不操作飞控/云台。
- 遥控器未开机，Pilot 2 未运行，排除 Pilot 2 占用。

## 2. 已经验证通过的环节

| 环节 | 结果 |
| --- | --- |
| `DjiCore_Init` | 成功 |
| `DjiAircraftInfo_GetBaseInfo` | 识别为 M400 |
| `DjiCameraManager_Init` | 成功 |
| `DjiCameraManager_GetCameraType` | L3，值 117 |
| `DjiCameraManager_StartRecordPointCloud` | 成功（返回 0） |
| `DjiMopChannel_Init` | 成功 |
| `DjiMopChannel_Create`（UNRELIABLE） | 成功 |
| `DjiMopChannel_Connect`（`DJI_CHANNEL_ADDRESS_PAYLOAD_PORT_NO1`，通道 `49154`） | 成功 |
| `DjiCameraManager_StopRecordPointCloud` | 成功，Stop ACK 为 yes |
| `DjiMopChannel_Close` / `Destroy` | 成功 |

可见光路径也独立验证过：L3 Liveview H.264 可以订阅并实时经 4G 回传、本机解码，
排除整条传输链路问题。

## 3. 故障现象

录制和 MOP 连接全部成功后，调用：

```c
uint32_t received = 0;
returnCode = DjiMopChannel_RecvData(channel, buffer, 512 * 1024, &received);
```

第一次调用即返回错误：

```text
returnCode = 0x0000000f00000001
received   = 0
```

按 PSDK 错误码格式拆解：高 32 位 `0x0F` 是 `DJI_ERROR_MODULE_MOP_CHANNEL`，
低 32 位是 `0x00000001`。该原始错误码在公开头文件 `dji_error.h` 中没有对应说明
（公开的 MOP 错误只有 `DJI_ERROR_MOP_CHANNEL_MODULE_RAW_CODE_CONNECTION_CLOSE = 0x00B`）。

我们还做了两组对照：

- 严格模式：`RecvData` 返回非 0 就停止 -> 得到 `0x0000000f00000001`，0 字节。
- 官方示例式容忍模式：`Create` 后等待 2 秒再 `Connect`，`RecvData` 返回非
  `CONNECTION_CLOSE` 时等待 200ms 后有界重试（最多 20 次）-> `RecvData` 阻塞，
  直到 20 秒外部时限，仍无任何点包返回。

也就是说：连接是通的，但 `RecvData` 要么立刻给一个未公开错误，要么阻塞收不到数据。

## 4. 最小复现命令

```bash
l3_metadata_inspect \
  --config <private-0600-config> \
  --point-probe --mount 1 --seconds 20 \
  --tcp-host 127.0.0.1 --tcp-port 9010 \
  --exclusive-test
```

本目录的日志文件：

- [point_stream_run.log](logs/point_stream_run.log)：`--stream --streams points` 的脱敏标准输出。
- [point_probe_first.log](logs/point_probe_first.log)：第一次 `--point-probe` 诊断输出，
  包含 `sdk_code=0x0000000f00000001`。
- [point_probe_retry.log](logs/point_probe_retry.log)：加入有界重试后的第二次诊断输出，
  包含 `reader_finished=no` 与阻塞到外部时限的情况。

## 5. 需要官方确认的问题

1. `DjiMopChannel_RecvData` 返回 `0x0000000f00000001`（MOP 模块，raw code 1）具体代表什么？
2. `DjiCameraManager_StartRecordPointCloud` 成功后，是否就应当有实时点云从 49154 通道推给 PSDK？
   还是需要额外的扫描/录制前置条件？
3. L3 实时点云订阅对固件版本、Pilot 2 状态、相机工作模式是否有隐藏要求？
4. 官方示例 `DjiTest_CameraManagerSubscribePointCloud` 在“连接成功后 RecvData 立即报非 0”时
   会直接跳过错误继续按 `recvBuf` 解析；这是否说明某些非 0 返回属于可忽略状态？
   官方建议的正确 RecvData 错误处理是什么？

## 6. 附带的另一处 MOP 越界（可能相关）

在同一台妙算上，ASan 还稳定复现了 PSDK 内部越界：即使不启动相机、不录制、
不使用任何 MOP 通道，只执行 `DjiCore_Init -> ApplicationStart -> 等待`，约 14 秒后崩溃：

```text
heap-buffer-overflow
READ of size 4096
source allocation: 28 bytes
read stack:
  __interceptor_memcpy
  DjiMopChannel_UsbBulkSendData
allocation stack:
  Osal_Malloc
  mop_flow_ctrl_clear_buffer
  mop_flow_ctrl_thread
```

该线程由 `DjiCore_Init` 内部的 `Dji_WebSocket_Over_Mop_Channel_init` 自动启动，
应用层没有公开接口关闭。官方 3.14.0、3.17.0、3.18.0 的 ARM64 库均复现同一越界。

## 7. 可补充提供

如需进一步定位，我们可以提供脱敏后的完整 SDK 日志、ASan 报告、精确到纳秒的
Start/Stop/Recv 时序，以及去掉所有私有凭据的最小复现源码工程。
