# L3 视频与点云首次测试记录

日期：2026-11-16，Asia/Shanghai。设备：M400、禅思 L3、妙算3；妙算接 E2，电脑通过 M400 调参 USB 联调。所有操作均在地面完成，没有电机、飞控、云台动作或点云录制。

## 测试结论

L3 的 PSDK `DEFAULT` 视频已取得 H.264，并解码确认是当前室内场景的可见光预览。当前完成短时机载采集、样本传回电脑与解码验证，**尚未完成稳定的持续实时窗口、真实点云或双流回传验收**。

PSDK 3.18.0 运行中发现 SDK 内部越界读取，可能导致进程异常退出；实际点云录制没有开始。点云程序、独立停止控制器和回传适配器已准备并编译，不能将模拟测试通过写成取得真实 XYZ。

## 2026-11-17：无相机、无录制的最小对照

用户批准诊断和修复验证后的有界采集，并要求优先点云。本轮在同一妙算上原生编译新增`--sdk-diagnostic --seconds 60`，只注册平台、初始化Core、查询飞机元数据、ApplicationStart、等待及清理；没有调用CameraManager、Liveview、应用MOP读写或任何录制接口。两次运行顺序执行，各有90秒外部期限，SDK库与下表既有SHA256一致。

| 平台socket适配 | 进程观测时长 | 结果 |
| --- | --- | --- |
| 本项目非阻塞socket | 14.300秒 | SIGABRT；ASan heap-buffer-overflow，28字节分配上READ 4096 |
| 官方阻塞socket收发实现 | 14.649秒 | 相同错误尺寸、分配链和`DjiMopChannel_UsbBulkSendData`调用链 |

两轮均已完成CoreInit、飞机查询和ApplicationStart，进入等待约5秒后异常；未到正常DeInit。官方socket对照仅屏蔽该源码中的两次`system()`全局网络缓冲修改，其余socket收发逻辑保持官方实现，USB仍使用现有参数保护包装。故这不是未经修改的完整官方示例，但结果表明替换本项目socket收发不能修复当前错误。

两轮分配链均含`Osal_Malloc → mop_flow_ctrl_clear_buffer → mop_flow_ctrl_thread`。本次把必要触发条件缩小到SDK基础运行，排除了本项目相机订阅、点云读取/录制、TCP分发和ROS作为必要条件；闭源内部原因和修复仍需DJI确认。没有已验证的公开API可关闭该内部任务。保持真实点云录制门槛关闭，不将不崩溃的短运行或屏蔽ASan当作修复。

源文件及Release、两种ASan构建位于妙算`/open_app/workspaces/dji/l3-dev/points-20261117`；14项原生CTest通过，默认仅点云业务入口已实测在读取配置和初始化SDK前拒绝。两次诊断已结束，没有启动点云录制。原始日志保留在各自0700私有运行目录，不上传。

独立的软件/网络验证也已完成：本机新增6项ROS转换与实际发布订阅测试通过；在妙算运行不链接SDK的合成回放器，经机载4G/Tailscale到本机RViz，32.016秒内接收并发布319个合成点包，点包和ROS发布错误均为0，视频连接为0。另一个本机ROS订阅者检查了50条消息的XYZ、RGB和接收时间，RViz实际渲染合成点。该记录证明点流转发与显示可工作，**不等于已经取得真实L3点云**。

## 2026-11-17：3.14.0 / 3.17.0 / 3.18.0 ARM64 版本对照

为判断是否可用降级规避，同一最小`--sdk-diagnostic --seconds 60`（只初始化Core、查询元数据、ApplicationStart、等待，不启动相机/视频/点云录制）分别在以下官方 ARM64 库上原生编译并运行：

| PSDK | ARM64 lib SHA256 | 结果 |
| --- | --- | --- |
| 3.14.0 | `d792c896f007f496d3755ce3d1dd6c45730c57bf7c9166f59d1f65c2815525a3` | 相同`mop_flow_ctrl_thread → DjiMopChannel_UsbBulkSendData`，28字节分配上READ 4096 |
| 3.17.0 | `274a5782632dd3bbbe4acd011196173a56db149f9e7b215041163051df84365d` | 同上 |
| 3.18.0 | `5725cc72824d915410be59e98cb31b4d269510c4615c1033da8d5fffbffdb5e7` | 同上（既有记录） |

3.14.0 是官方开始支持 Zenmuse L3 的版本，因此当前没有“支持 L3 且避免该越界”的可用降级版本。3.17.0 x86_64 的比较此前已发现相同分支，本次在 ARM64 上实际复现，进一步把结论从代码比较升级为实机证据。3.14.0 的构建因 Liveview 接口名不同（`DjiLiveview_StartH264Stream`）为本次最小诊断加入了不调用视频路径的占位，不影响 Core/MOP 诊断结论。

结论不变：这是 SDK 内部的 `DjiMopChannel_UsbBulkSendData` 内存越界，发生在 `DjiCore_Init` 内部 WebSocketOverMOP 流控线程，应用层与 HAL/OSAL 适配无法关闭或修复。真实点云录制入口继续默认关闭，等待 DJI 提供适用于妙算3 / M400 / L3 的 ARM64 修复库或官方规避方案。

## 环境与初始化

| 项目 | 结果 |
| --- | --- |
| 妙算系统 | Ubuntu 20.04.6 / AArch64 / kernel 5.10.192-tegra / L4T 35.5.0 |
| 工具链 | GCC 9.4.0 / CMake 3.16.3 |
| SDK | 官方 PSDK 3.18.0，提交 `f3938d881e1628fc9a12e1b982b77060b9006311` |
| 飞机 | SDK 确认为 M400，版本 19.01.10.03 |
| L3 | PSDK 负载挂载位置 1，`DJI_CAMERA_TYPE_L3`，类型值 117 |
| 元数据程序 | 初始化、ApplicationStart、查询和清理均成功；一次约 8.99 秒正常退出 |
| 采集前条件 | 用户已确认退出 Pilot 2，且没有其他录制任务 |

## 第一轮视频探测

专用程序执行 `--rgb-probe --mount 1 --seconds 8`，先精确核验 L3，再初始化 Liveview。回调只向预分配的 32 MiB 内存复制数据；停止并排空后保存本地私有 H.264。没有开始录制、切换相机源或调用飞控。

| 指标 | 观测结果 |
| --- | --- |
| 控制等待预算 | 8 秒，观测为 8000 ms；不含 SDK 初始化与清理 |
| SDK 回调 | 194 次；回调次数不是视频帧数 |
| 接收字节 | 518416 bytes |
| 最大单次回调 | 38407 bytes |
| 本项目内存缓冲丢弃 | 0 bytes，未溢出；不代表上游绝对无丢失 |
| Liveview Start / Stop / Deinit | 均返回成功 |
| CameraManager DeInit | 返回成功 |
| 程序退出 | CoreDeInit 阶段异常终止，SIGABRT；该轮整体不判稳定通过 |

样本通过 SSH/SFTP 在采集结束后传到本机，不将这次文件传输当作实时网络回传。FFprobe 识别 H.264 / 1920×1080 / yuv420p，报告 184 个可读帧；首帧解码后人工核对为真实可见光场景。解码存在警告，因此没有宣称零码流错误、固定帧率或稳定 1080p30。裸 H.264 里的 `r_frame_rate` 等字段不等于设备实测帧率。

真实 H.264、解码图和原始日志只保存在私有位置，不随本报告上传。

## 内存错误定位

随后仅对相同视频模式做了一轮 AddressSanitizer 诊断，没有运行点云录制。SDK 静态库没有重新编译插桩；应用与 HAL/OSAL 插桩，ASan 的 `memcpy` 拦截器捕获到闭源 SDK 调用的越界读取。

该轮约 18.52 秒异常终止，事件到达 Liveview Start 成功后，尚未到正常 Stop。错误类别为 `heap-buffer-overflow`，具体为：

```text
READ of size 4096
source allocation: 28 bytes
fault boundary: 0 bytes to the right of that region

read stack:
  __interceptor_memcpy
  DjiMopChannel_UsbBulkSendData

allocation stack:
  Osal_Malloc
  mop_osal_malloc
  mop_flow_ctrl_clear_buffer
  mop_flow_ctrl_thread
```

该 MOP 后台路径来自 `DjiCore_Init` 内部启动的 WebSocketOverMOP 任务，并非本项目调用了点云 MOP 或发送测试数据。因此不能只把第一轮现象解释为退出清理问题，也不能通过跳过 DeInit 认定持续运行已修复。

交叉检查官方 x86_64 库：3.18.0 与 3.17.0 的同名函数都存在把非零且不大于 4095 的长度改为 4096、随后从原始指针 `memcpy` 的分支，位置在 HAL 写回调之前。**3.17 的结果只是 x86_64 代码比较，没有在飞机上降级或验证 ARM64 3.17**，不能声称降级即可修复。V4.0.0 的公开支持范围也不足以证明可直接替换本组合。

当前结论是本项目已复现 SDK 内部的非法内存访问，仍需可核验的修复库或官方支持方案。没有通过扩大分配掩盖越读、关闭 ASan、修改设备固件或二进制打补丁。

### 可复现标识

| 内容 | SHA256 |
| --- | --- |
| 官方 3.18.0 ARM64 `libpayloadsdk.a` | `5725cc72824d915410be59e98cb31b4d269510c4615c1033da8d5fffbffdb5e7` |
| 视频初测的 normalized LF `metadata_inspect.c` 快照 | `fa7b8bb4e492921185a6035a898e2c3a055c63cafe16101f2196f226f9f045c1` |
| 用于比较的官方 3.17.0 x86_64 库 | `f89c2596560219260bf0e55be56baffaa73e5b2ff67847649ad5bc10d8c49105` |

诊断构建使用 `-fsanitize=address -fno-omit-frame-pointer -g` 和相同链接 sanitizer 参数。SDK 运行日志可能包含凭据，只能保存到私有目录；对外报告只提取上面的错误类别、尺寸和函数名，不展示原始内存转储、进程地址、设备身份或账号。

## 点云准备与未执行部分

控制器准备了独立的一次 Start、MOP 接收和 Stop 线程。计时从 Start 调用之前开始，目标在 20 秒期限请求停止，即使没有包或接收阻塞也不等待接收线程。Start 晚到时必须在其返回后再 Stop；只有此后的成功返回才标记停止命令已确认。线程未结束时不释放上下文，也不清理共享 SDK。

这些机制不能保证闭源 SDK 进程崩溃后仍能停录，也不能把停止命令 ACK 当作独立读取的传感器状态。因此在上述内存问题解决前，不开展真实点云录制；构建默认关闭会开启录制的点云探测入口。

收尾时设备的 SSH 连接在协议 banner 前被关闭，已停止重试，原因未确认。完整点云程序此前原生构建及参数检查通过；最后增加的默认禁用门槛版本尚未能同步到妙算并原生复核。该限制明确保留，不将源码默认值当作已更新的设备二进制状态，也不要运行设备上较早的点云测试文件。

| 验证 | 结果 |
| --- | --- |
| 现有点云协议离线测试 | 3 项通过 |
| 原图像协议测试 | 通过 |
| POSIX socket 纯回环 | UDP/TCP、截断、超时、EOF 和 accept 通过，无 SDK/飞机访问 |
| 点云控制器正常、阻塞、错误及取消模拟 | Linux 原生严格 GCC 构建；10 个场景均通过 |
| 点云完整程序 | 原生构建通过；帮助、配置和参数拒绝通过，无真实录制 |
| 真实 MOP 49154 接收、XYZ、持续点云回传 | 未执行、未验证 |

控制器模拟场景包括正常、reader 阻塞、迟到 Start、Start 错误、Stop 失败、Stop 阻塞、错误相机类型、进入前取消、准备中取消及 sink 失败。它们不链接 DJI 静态库，只测试本项目控制逻辑。

## 后续进入条件

先解决并复核 SDK 内存访问问题，使用无录制的短视频模式验证持续取流及正常退出，再恢复一次有界点云采集，最后进行真正的有线实时双流回传。不得将“收到过视频”“SDK 初始化成功”或“模拟 Stop 通过”替代这些条件。

本报告和代码只作本项目内的诊断与交接，没有自动向 DJI 发 Issue、邮件或上传原始数据。总体路线继续参见[开发方案](REMOTE_SYSTEM_DEVELOPMENT_PLAN.md)。
