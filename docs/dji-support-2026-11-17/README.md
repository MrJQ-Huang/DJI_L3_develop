# L3 实时点云订阅问题报告（PSDK 3.18.0 / M400 / 妙算3）

## 开发目标与希望获得的建议

我们的目标是：通过妙算3获取 M400 上禅思 L3 的实时可见光视频和真实激光点云，经飞机机载 4G 网络回传到地面 Linux 电脑，供本机程序处理，并在 ROS 2 Humble / RViz 中同时持续显示图像与点云。业务数据由妙算3直接通过网络回传，地面电脑不依赖遥控器转发。

```text
L3 可见光 H.264 → 妙算3 PSDK Liveview → 机载4G / Tailscale → Linux电脑解码显示（已跑通）
L3 真实激光点云 → 妙算3 PSDK / MOP 49154 → 同一4G链路 → Linux电脑 / PointCloud2 / RViz（待跑通）
```

**当前已经跑通真实 L3 可见光视频的实时 4G 回传及本机解码显示；本次希望重点解决剩余的 L3 点云取流与 4G 回传。** 这里的点云是包含真实 XYZ 坐标的数据，以及接口提供的强度和逐点 RGB，不是 `POINT_CLOUD_CAMERA` 的预览视频。点云网络封装、接收和 ROS/RViz 显示已通过合成点流验证，但尚未收到真实 L3 点包，不能将合成测试视为真实点云链路通过。

目前阻塞发生在 L3 到妙算3的源端取数阶段：`StartRecordPointCloud` 和 MOP `Connect` 返回成功，随后 `RecvData` 返回 `0x0000000f00000001` 或阻塞，未取得有效点包。因此尚未进入真实点云经 4G 回传的验收阶段；可见光链路已提供同一机载网络能够回传业务数据的实测基础，但不代表真实点云的速率和持续性已经验证。

希望 DJI 工程师结合下方环境、调用过程和日志，围绕上述目标给出建议：

1. 当前通过 PSDK / MOP 49154 获取 L3 实时点云、再由妙算3通过机载 4G 转发的路线是否适用？如有更合适的官方接口或示例，请指出。
2. 在本项目地面、未启电机、遥控器未开机 / Pilot 2 未运行的测试条件下，L3 输出实时点云还需要满足哪些明确的设备状态、配置、SDK或固件条件？
3. 请解释 `RecvData` 的上述返回码与阻塞现象，并提供正确的调用顺序、参数和错误处理方式，帮助我们先取得连续有效的真实点包，再完成与可见光并行的 4G 回传及本机显示。

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

## 关键异常发生时间（Asia/Shanghai）

| 现象 | 时间 |
| --- | --- |
| `--stream points` 收到 0 点包 | 2026-11-17 14:47:21 |
| `--point-probe` 首次 `RecvData` 返回 `0x0000000f00000001` | 2026-11-17 14:48:30 |
| 加入有界重试后 `RecvData` 阻塞到 20 秒时限 | 2026-11-17 15:01:11 |
| 最小 Core 诊断 MOP 越界（本项目 socket） | 2026-11-17 11:39:33 |
| 最小 Core 诊断 MOP 越界（官方阻塞 socket） | 2026-11-17 11:40:38 |

## 固件/版本清单（不含遥控器）

| 对象 | 版本 |
| --- | --- |
| 飞机 Matrice 400 | `19.01.10.03` |
| 禅思 L3 相机 | `17.2.0.59` |
| PSDK | `3.18.0`（commit `f3938d881e1628fc9a12e1b982b77060b9006311`） |
| PSDK ARM64 库 | SHA256 `5725cc72824d915410be59e98cb31b4d269510c4615c1033da8d5fffbffdb5e7` |
| 妙算3 系统 | Ubuntu 20.04.6 / L4T R35.5.0 / kernel 5.10.192-tegra / GCC 9.4.0 |
| 妙算3 整机 DJI 固件 | 设备本地未暴露，需在 DJI Assistant 2 中读取 |

遥控器固件未采集（遥控器未开机）。

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
