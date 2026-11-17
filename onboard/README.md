# 妙算3采集适配器（开发阶段）

2026-11-17点云优先更新：`--stream --streams points`只初始化点云路径，不调用Liveview；默认`both`保持双流。两模式均会启动录制，因此仍受默认关闭的SDK修复门槛限制。新增纯Core诊断在本项目与官方socket收发下都复现28→4096越读，见[最新对照结果](../docs/TEST_REPORT_2026-11-16.md#2026-11-17无相机无录制的最小对照)。本机ROS/RViz与4G合成点流已通过，真实XYZ未取得。

2026-11-17：双流核心已接入。`l3_stream.c/.h`是SDK无关的双TCP分发器，`metadata_inspect.c --stream`将Liveview和持续点云读取接入该核心；`--stream`明确会启动点云录制，仍由默认关闭的`L3_ENABLE_POINT_RECORDING_PROBE`门槛拒绝。原PSDK内存错误未有已验证修复，当前未启动真实采集。完整构建、离线集成测试和电脑接收命令见[开发方案](../docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md#7-已实现代码运行与实测边界)。下方11月16日描述保留历史背景，适配器不再只是未集成源文件。

最新状态见[首次测试记录](../docs/TEST_REPORT_2026-11-16.md)：8 秒 L3 DEFAULT H.264 已解码确认为 1920×1080 可见光预览，但发现 PSDK 内部越界读取，尚未通过持续实时回传或真实点云验收。当前构建默认关闭会启动录制的点云入口；不要在未验证修复的 SDK 上打开它。本轮实际点云录制未执行。

这里包含专用元数据程序和待集成的采集适配器，尚不是可直接安装的 DPK。2026-11-16，`metadata_inspect.c` 已在妙算原生链接完整 PSDK 并正常运行，确认 M400 与 PSDK 负载位置 1 的 L3；未启动视频、点云或录制。`l3_pointcloud_psdk.c`、`l3_rgb_probe.c` 和 Linux TCP sink 则仅通过 GCC 9.4.0 的 `-std=c11 -Wall -Wextra -Werror -c` 原生编译，尚未集成为采集程序；点云及视频适配器使用官方 PSDK 3.18.0 头文件，并增加 `-pthread`。当前实机妙算接在 M400 的 E2，SSH 为 `192.168.42.130`，详见[本轮连接检查](../docs/MANIFOLD_DIRECT_LINK.md#2026-11-16-实机连接与编译检查)和[总体开发方案](../docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md)。

1. 从审查过的 PSDK 初始化流程启动应用，使用本机私有配置，不运行综合 Demo。调用方负责 `DjiCameraManager_Init`、`DjiMopChannel_Init` 或 `DjiLiveview_Init` 和各自清理。按实际机型、端口和安装核对 L3；适配器遇到非 L3 会拒绝操作。
2. 点云：`l3_pointcloud_psdk_read(mount, max_attempts, max_packets, sink, context, &report)` 只尝试一次 L3 的 MOP 49154 连接。调用方应确认录制已由授权流程开始、Pilot 2 已退出，再在专用线程读取。函数不开始或停止录制。SDK 的阻塞 connect/recv 不提供公开超时/取消参数，收包次数上限不能保证墙钟时间上限。第一轮应采用单独前台测试进程，操作方保有独立终止能力，不配置开机自启。
3. 点云传输：先启动 Linux 的 `pc_viewer/l3_points.py listen`，由妙算程序主动建立到这个已知地址的 TCP 连接。以已连接的 fd 创建 `L3p1TcpSink { .fd = fd, .timeout_ms = 200 }`，将 `l3p1_tcp_sink` 与其上下文传给 `l3_pointcloud_psdk_read`。首轮仅用于短批诊断，每包写入有超时；发送失败应退出、关闭连接。持续流版本再使用单独的发送线程和有限队列，不在取数线程积压。
4. 图像：`l3_rgb_probe_start(mount, sink, context)` 尝试 `DEFAULT` H.264 订阅，明确相机端口为 L3 后才执行。回调提供 SDK 原始编码字节，**一个回调不等于一帧**。调用方及时复制到有界缓冲并按顺序写到短时临时样本，禁止在回调里做阻塞网络操作。使用 `ffprobe -f h264 -show_streams sample.h264` 和实际解码画面确认内容。每进程只尝试一次，结束时调用 `l3_rgb_probe_stop()` 并检查返回；没有成功解码出可见光前，不把它当作 RGB。测试片段只留在本机。

镜头与录制状态保持由独立的授权操作流程管理。先分别验证两种数据，再测并发。全程不启动电机、不起飞、不操作飞控、云台或对频。详细接线见 [直传开发基线](../docs/MANIFOLD_DIRECT_LINK.md)。

## 专用 SDK 识别程序

`metadata_inspect.c` 是独立诊断程序，使用本目录的 CMake，**不要构建或执行官方综合示例来替代**。默认无参数和 `--help` 只显示帮助；`--check-config` 只校验配置文件，不初始化 SDK。显式 `--inspect` 初始化 PSDK、确认 M400、读取飞机版本和三个已知 PSDK 负载挂载位置的相机型号，并清理退出；该模式不订阅视频、点云或开启录制。`DjiCore_ApplicationStart()` 仅通知 SDK 软件会话开始。另有独立的 RGB 模式及默认禁用的点云模式，不能把整个程序统称为“纯元数据只读”。

在妙算3或明确配置好的 Linux AArch64 工具链下构建：

```bash
cmake -S onboard -B build/metadata -DPSDK_ROOT=/path/to/Payload-SDK-3.18.0
cmake --build build/metadata -j2
build/metadata/l3_metadata_inspect --help
build/metadata/l3_metadata_inspect --config /private/path/psdk.conf --check-config
```

`--sdk-diagnostic --seconds 60`仅做Core/ApplicationStart、飞机元数据和有界等待，不调用CameraManager、Liveview或录制。增加`--diagnostic-video --mount 1`才初始化视频并计数后丢弃数据。两者均需新建0700私有工作目录，外部`timeout --signal=TERM --kill-after=3s 90s`限制SDK阻塞。诊断会实际初始化SDK，不能把构建或`--help`通过当作诊断通过。

`-DL3_USE_OFFICIAL_SOCKET=ON`供故障对照：使用匹配SDK的官方阻塞socket收发，仅在该编译单元将`system()`重定向到拒绝函数，避免执行其全局sysctl写入；事件明确标注`sysctl_writes=blocked`。这不是SDK修复或正式部署必选项。Liveview适配已改用3.18的`DjiLiveview_StartStream/StopStream`，不声称接口迁移解决MOP内存错误。

`--rgb-probe --mount 1 --seconds 8` 会在精确确认 L3 后尝试 DEFAULT H.264 订阅，在内存中收集有限数据，Stop/排空后保存 `l3-rgb.private.h264`，不调用录制和相机源写入。该模式仅作 SDK 故障复现和短样本验证；当前 SDK 已有运行期崩溃证据，不当作稳定采集器。

`--point-probe --mount 1 --seconds 20 --tcp-host 127.0.0.1 --tcp-port 9010 --exclusive-test` 是准备中的点云探测模式。CMake 的 `L3_ENABLE_POINT_RECORDING_PROBE` 默认为 `OFF`，在配置读取、TCP 连接、SDK 初始化和 Start 之前拒绝此命令；只有 SDK 问题取得可验证修复后才评估启用。`--stop-pointcloud-only --mount 1 --exclusive-test` 仅用于本项目拥有的录制的应急 Stop，不能和另一 SDK 进程同时运行，也不能接管用户原有录制。

`psdk.example.conf` 仅包含占位值。真实配置必须是当前用户拥有、权限为 `0600` 的非符号链接普通文件；所有字段按首个 `=` 拆分，值不加引号，支持 UTF-8 中文应用名。使用 **Payload SDK-Manifold 3** 应用的准确名称、ID、Key、Advanced License、开发者账号及 `460800` 波特率；不要复用 MSDK 应用信息或把真实值放到命令参数、源码和 Git 中。配置格式通过不等于 SDK 身份认证成功。

`--inspect` 必须在新建、当前用户拥有、权限为 `0700` 的工作目录中运行。程序会新建最多 4 MiB 的 `sdk.private.log`，权限为 `0600`；SDK 原始日志可能包含应用密钥，不要直接展示或上传。SDK 初始化、清理及 USB 调用可能阻塞，单次运行必须由外部守护限定时间，例如：

```bash
# 先将以下两条路径替换为真实绝对路径，再在一个全新的 0700 目录中运行。
timeout --signal=TERM --kill-after=3s 45s /absolute/path/l3_metadata_inspect \
  --config /private/path/psdk.conf --inspect
```

原始 stdout/stderr 也应仅保存在私有目录，对外只提取程序固定的 `event=...` 白名单结果。超时强制退出不保证 SDK 完成清理；不要配置自启动或同时启动第二个 SDK 应用。

本项目的 `metadata_socket.c` 替代官方会写入全局网络缓冲设置的 socket 示例，只做常规 POSIX socket 操作。`tests/test_metadata_socket.c` 只使用本机回环，不链接 PSDK、不访问飞机，可独立复现：

```bash
mkdir -p .validation
gcc -std=gnu11 -Wall -Wextra -Werror -I onboard \
  -isystem /path/to/Payload-SDK-3.18.0/psdk_lib/include \
  onboard/metadata_socket.c onboard/tests/test_metadata_socket.c \
  -o .validation/test_metadata_socket
timeout 10s .validation/test_metadata_socket
```

`tests/test_point_probe_controller.c` 仅用 stub 测试本项目线程与停止逻辑，不链接 DJI 库或访问飞机：

```bash
gcc -std=gnu11 -Wall -Wextra -Werror -pthread -I onboard \
  -isystem /path/to/Payload-SDK-3.18.0/psdk_lib/include \
  onboard/l3_point_probe_controller.c onboard/tests/test_point_probe_controller.c \
  -o .validation/test_point_probe_controller
```

每个场景用独立进程执行 `timeout 10s .validation/test_point_probe_controller CASE`，CASE 为 `normal`、`reader-block`、`late-start`、`start-error`、`stop-failure`、`stop-block`、`wrong-type`、`pre-cancel`、`prepare-cancel`、`sink-failure`。模拟通过只证明控制器逻辑，不证明闭源 SDK 的启动、停止和清理安全。
