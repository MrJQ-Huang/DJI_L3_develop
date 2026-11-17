# L3 点云与可见光实时传输：MVP 开发方案

更新：2026-11-17（Asia/Shanghai）。本方案替代本文件先前的多阶段扩展设计，作为当前实现顺序。

当前验收目标（2026-11-17用户收窄范围）：通过妙算3取得一路真实L3可见光H.264和一路真实XYZ预览点云，经M400机载4G传到本机，在现有ROS 2 Humble / RViz中同时显示并持续刷新。多电脑接收能力保留，不作为本轮验收前提。全程地面验证，沿用仓库禁止电机、起飞、飞控和云台运动的范围。

同日用户已批准 SDK 对照诊断、ROS/RViz 桥接和修复验证后的有界采集，要求**优先转发激光点云**。因此新增仅点云模式；视频不再是点云业务进程的必需模块。官方互联图中的负载↔妙算 MOP 和妙算↔自定义网络分别对应本项目取数与4G转发两段。

## 1. 先固定能力依据

采集、设备识别与机载网络能力以用户提供的 DJI PSDK 中文教程、API 及匹配版本官方头文件为依据。TCP 分发、电脑解码和 Tailscale 是本项目的工程选择，不属于 DJI 提供的现成双流服务。

| 能力 | DJI 官方依据 | 本项目采用方式 |
| --- | --- | --- |
| 精确识别设备 | [相机管理][camera]、`DjiCameraManager_GetCameraType()` | 查询实际挂载位置，必须确认为 `DJI_CAMERA_TYPE_L3` |
| 可见光视频 | [Liveview][liveview]、[固定版本头文件][liveview-header] | 复用已实测的 `DEFAULT` H.264 回调，电脑端解码 |
| L3 实时点云 | [相机管理][camera]、[固定点云示例][point-sample] | 按实际 L3 mount 连接 MOP `49154 / UNRELIABLE` |
| 机载4G上网 | [机载网络][internet]、[系统功能][system-features] | 飞机安装 DJI 增强图传模块、开通对应流量包并确认4G正常 |
| 自定义业务传输 | [PSDK 功能介绍][capabilities] | 妙算应用使用机载4G访问自定义网络服务 |

点云录制期间才提供 L3 实时点云；当前订阅教程要求退出 Pilot 2。该数据是实时预览点云，不能当成存储卡的全量测绘成果。[相机管理][camera]、[L3 FAQ][l3-faq]

当前 SDK 不作为依据承诺两路测绘相机视频、动态编码码率、SEI 曝光时间或硬件同步。第一版只交付已经有接口依据的一路可见光与一路预览点云。

## 2. 当前基线与采集前置阻塞

- 仓库基线：`5da04b4`；PSDK 3.18.0：`f3938d881e1628fc9a12e1b982b77060b9006311`。
- 妙算：Ubuntu 20.04.6 / AArch64 / GCC 9.4；M400 已读到 19.01.10.03。
- 妙算目前接 E2；L3 在 PSDK 负载位置 1。启动时仍须重新精确识别，不能将两种端口编号混用。
- 已验证短时可见光：8秒得到 518416 bytes H.264，解码为 1920×1080。尚无真实 XYZ 或持续双流验收。
- 2026-11-17 用户已安装4G模块。妙算经机载默认出口实际完成DNS解析与DJI/Tailscale HTTPS访问；尚未验收双流业务吞吐。USB管理链路出现过中断，不继续投入 Wi-Fi 配网。

**真实采集当前受 PSDK 内部内存错误阻塞。** [2026-11-16 测试报告](TEST_REPORT_2026-11-16.md)记录 ASan 捕获 `DjiMopChannel_UsbBulkSendData` 从28字节源读取4096字节。问题发生于运行期后台路径，不能通过跳过 DeInit 当作修复。

先用无录制最小模式和官方 socket 收发适配对照排除本项目适配原因；若官方路径仍复现，需取得适用于本组合的修复库/官方处理方案。修复候选先通过无录制视频持续运行和正常退出，再启用点云录制入口。协议、分发器和ROS显示以合成数据并行开发，分别记录软件与实际网络验证，不冒充真实L3采集。

## 3. MVP 网络架构

网络采用M400机载4G经运营商基站接入互联网，妙算3与本机通过Tailscale组网。此前已确认的多电脑分发能力保留；本轮仅验收本机接收与RViz显示，不再等待第二台电脑。

```mermaid
flowchart LR
    L[L3] --> M[M400 内部链路]
    M --> A[妙算3：PSDK双源采集]
    A --> Q[按客户端独立队列与发送]
    Q --> G[机载4G → 运营商基站 → 互联网]
    G --> T[Tailscale 私有网络]
    T --> P1[本机：图像解码 + 点云数组]
    P1 --> R[ROS 2 Image / PointCloud2]
    R --> V[RViz 图像和点云显示]
```

运营商基站提供蜂窝网络承载；本机分别建立视频与点云连接。本机不运行DJI SDK，也不经遥控器转发业务数据。

当前4G部署将业务端口绑定妙算`127.0.0.1`，由Tailscale用户态Serve转发；本机经SOCKS5连接。USB仅作可选管理和诊断路径，不能将飞机转发地址或用户态Tailnet地址当作妙算本机可绑定接口。

DJI 文档确认的是互联网和自定义网络通信能力，没有承诺公网固定地址、入站端口、Tailscale、TUN权限或业务带宽。现有Tailscale用户态路径已通过机载出口的合成流验证，真实L3码率仍待测；不扩展穿透或代理框架。

## 4. 核心代码只保留三个职责

保持现有 `onboard/`、`pc_viewer/` 目录，不迁移工程、不创建空模块。

| 职责 | 文件与开发内容 | 完成标准 |
| --- | --- | --- |
| 采集 | 复用 `onboard/l3_rgb_probe.c`、`l3_pointcloud_psdk.c`；初始化沿用 `metadata_inspect.c` 的独立流程 | SDK数据交给明确的字节回调，采集线程不直接操作网络 |
| 分发 | 已实现 `onboard/l3_stream.c/.h` | 两个独立TCP端口、每流最多两客户端、独立有界队列、发送与停止 |
| 接收与显示 | `pc_viewer/l3_stream.py`、`l3_points.py`、`l3_ros_bridge.py`及`l3.rviz` | 连续得到BGR及XYZ/强度/RGB数组，发布Image和PointCloud2，由本机RViz显示 |

上述核心源码与SDK无关集成测试已完成。`metadata_inspect.c --stream --streams points`只启动点云采集，`--streams both`为双流；原20秒探测模式保持不变。共享分发器保留两个监听端口，但仅点云模式没有视频采集或视频字节。SDK修复门槛仍默认关闭，尚未执行真实点云采集。

### 4.1 采集模块

- 视频保留一次 SDK Start/Stop 生命周期，网络客户端的加入或断线不触发反复初始化相机。
- H.264 回调的数据仅在调用期间有效；及时复制入有界缓冲。一个回调不是一帧，也不是一个 NAL。
- 点云复用精确 L3 验证、MOP 地址映射和包布局检查；给持续接收增加明确的停止信号与独立读取线程。
- 点云同时提供短时探测和持续读取接口。SDK Connect/Recv/Stop 没有可依赖的公开硬取消参数，不能把“循环30包”或设置很大包数当成持续服务或时间保证。
- 录制命令沿用现有明确入口与停止机制；连接电脑、重连网络不自动开启或停止用户已有录制。
- 持续入口支持显式60秒/5分钟测试时长并保留独立Stop；首次真实20秒取点验收后才能进入后续长测，不通过增大收包次数替代时长控制。

传输层只需要两个发布入口，接收借用缓冲并负责复制，不引入统一传感器对象模型：

```text
publish_h264(context, bytes, length)
publish_points(context, packet, length)
```

### 4.2 分发模块

首版固定两条TCP数据通道，复用已经存在的点包封装：

| 项目端口 | 数据 | 格式 |
| --- | --- | --- |
| 9101/TCP | 真实点云 | `L3P1` + 大端 uint32 长度 + 一份完整原始 DJI 点包 |
| 9102/TCP | 可见光 | 按源顺序发送 H.264 字节，不添加自定义头、不转 JPEG |

49154是DJI内部MOP通道，与上述TCP端口不同。旧点云诊断工具默认9010；用于MVP时显式指定9101。`L3P1`仍是项目自己的封装，不是DJI协议名。首版保留原包，暂不重新定义点云二进制格式。

每流最多两个客户端，共四条业务连接。连接的生命周期由电脑订阅决定；所有客户端共享同一份相机/点云采集，不重复订阅 SDK。

最小队列约定：

- 每客户端视频排队上限先设2 MiB、点云1 MiB，另限制记录数；实际速率确定后调整常量，不引入动态配置中心。
- 发送线程从队列取数据，SDK回调只复制入队；网络慢时不阻塞采集及另一台电脑。
- 点云仅丢弃完整、尚未发送的旧包；已部分发送的包失败后关闭该连接，避免后续解包错位。
- 视频不任意丢弃回调字节；队列满或发送超过预算时断开该慢客户端。
- 视频新连接先发送缓存的 SPS/PPS，再从完整 IDR 访问单元的起始边界接入，避免接入多slice图像的中间。用小型增量 Annex-B 解析处理跨回调起始码，不实现编码器或完整 H.264 解码器。
- 视频实际封装、SPS/PPS和IDR行为须用真实 SDK 流确认；“等到可解码起点”属于验收条件，不能只用TCP建连成功代替。

仅统计收到/发出的字节和点包、解码帧数、队列丢弃、慢客户端断开及重连次数。队列限制和正确分帧属于实时传输本身，不另起通用可靠性框架。

### 4.3 电脑接收模块

- 视频：使用现成 H.264 解码库增量解码，得到 `uint8 H×W×3` BGR 数组；OpenCV显示和算法读取共用该输出。
- 点云：复用现有 `parse_packet()`，输出XYZ、强度和RGB；保留源序号、时间原值及本机接收时间。
- 电脑端处理TCP拆包和断线；协议尺寸与包长上限沿用现有校验。
- `l3_ros_bridge.py`直接构造ROS消息，复用本机Humble和现有`.venv`，无需`cv_bridge`。只发布新收到的对象，不以相同旧包伪装刷新；源序号重置后仍接收新包。
- `/l3/camera/image_raw`发布`sensor_msgs/msg/Image`（bgr8），`/l3/points`发布`PointCloud2`（XYZ/强度float32，RGB uint32，20字节/点）。原机载16字节点结构和L3P1不变。RViz显示项和发布端均使用Best Effort、深度1。
- 点云必须显式选择`--raw-coordinates`诊断模式（frame=`l3_raw`，不承诺米/轴/原点），或在源单位核实后传`--xyz-scale-to-meters`（frame=`l3_local`，保持源轴）。本轮不要求SLAM、地图定位、投影或硬件同步。ROS时间为本机系统接收/解码时间，源时间原值保留。

DJI固定头定义20字节点云头、8字节CRC和16字节点记录。当前尚须核对真实包边界、坐标单位/原点、时间单位和CRC算法；不将未知时间重命名为曝光时间，也不把未经核对的XYZ直接标成米或世界坐标。[点云结构][point-header]

## 5. 4G接入只做必要步骤

1. 用户安装与 M400 配套的 DJI 增强图传模块，开通对应流量包，确认飞机4G正常。
2. 在妙算上验证DNS、HTTPS和实际业务连接。增强图传开关状态、遥控器有网或出现4G图标，都不能单独证明妙算业务通道可用。
3. 沿用飞机内部网络；官方说明负载网络访问在满足4G前提时默认启用。不安装AT拨号程序，不改`onboard`的IP/DHCP。
4. 当前账号无CAP_NET_ADMIN，两端已准备官方Tailscale用户态模式。妙算通过Serve转发到回环9101/9102；本机接收器显式使用`--socks5 127.0.0.1:1055`。两端登录同一Tailnet后再验收实际数据通道。
5. 测量本机接收真实双流的实际码率、刷新与队列情况，完成RViz显示；不以第二台电脑或新增传输协议作为前置条件。

网络工程选择与DJI能力分开记账：DJI文档提供网络出口能力依据，实际出口仍由本项目验证；本项目实现业务连接、双流分发及电脑解码。Tailscale访问范围限制在参与联调的设备与业务端口，不新建一套应用账号/控制协议。

### 上行与流量计算

本轮验收仅一个接收端。保留的多客户端能力会重复占用机载上行；设视频实际码率为 `Rv`，点云码率为 `Rp`：

```text
总业务上行 ≈ 客户端数量 × (Rv + Rp) + TCP/组网开销
点云封装字节/秒 ≈ 16 × 点数/秒 + 36 × 点包数/秒
流量 GB/小时 ≈ 平均总上行 Mbps × 0.45
```

36字节由28字节DJI包前缀和8字节L3P1封装组成，不含TCP/网络开销。这是计算方法，不是L3实测速率。已有8秒视频样本也不能作为持续带宽指标。

## 6. 按这个顺序实现与验收

| 顺序 | 工作 | 通过条件 |
| --- | --- | --- |
| 0 | SDK修复与数据源核验 | 可见光持续取流并正常退出；随后有界点云采集收到真实有效XYZ，停止结果可确认 |
| 1 | 核心传输离线实现，可与0并行 | 合成点包与有效合成H.264可经TCP连续接收；拆包、解码和停止通过 |
| 2 | 真实点云优先 | 有界20秒录制/收点/停录，校验原始XYZ，再经4G送至本机；不以视频并发或USB恢复为前提 |
| 3 | 本机ROS桥接 | 接收器输出转换为Image/PointCloud2，RViz显示项、编码和局部frame正确；合成数据仅用于软件验证 |
| 4 | 安装4G后的网络验证 | 妙算机载出口、Tailnet及两条业务TCP均可用，记录实际连接类型和上行 |
| 5 | 4G本机RViz验收 | 本机经4G实际收到来自L3的可见光及XYZ，RViz同时持续刷新图像与点云，保留来源及速率统计 |

60秒用于首轮持续观察，最终计划5分钟双流显示；这些是本项目验收窗口，不是DJI保证的稳定性指标。合成数据、旧录像和点云预览视频不能替代真实XYZ。全程保持地面测试。

分发器、增量解码和ROS桥接已实现；当前先处理源端SDK阻塞并验证真实点云。每项代码以真实数据或明确标注的合成流证明能工作。

ROS桥接与RViz已纳入当前必需工作；SRT/RTMP、全量遥测、同步标定、自动转码、自启动、录像管理和Web控制台仍不前置。

## 7. 已实现代码、运行与实测边界

已交付分发器、持续点云读取及停止控制、SDK双流入口、Python连续接收与解码。SDK双流入口`--stream`会启动点云录制，仍受`L3_ENABLE_POINT_RECORDING_PROBE=OFF`默认门槛限制；拒绝发生于读取私有配置、网络监听与SDK初始化之前。

新增仅点云接收/RViz启动命令（本机程序不启动机载录制）：

```bash
bash scripts/start_l3_rviz.sh --host MANIFOLD_TAILNET_IP \
  --socks5 127.0.0.1:1055 --raw-coordinates --seconds 60
```

默认`--streams points`，双流加`--streams both`；无界面检查加`--no-rviz`。原始坐标模式仅用于诊断，真实空间尺度须在单位核验后确定。合成ROS测试：

```bash
source /opt/ros/humble/setup.bash
.venv/bin/python pc_viewer/test_l3_ros_bridge.py
```

本机SDK无关编译与验证：

```bash
cmake -S onboard -B build/transport -DL3_BUILD_PSDK_APP=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/transport -j2
cd build/transport && ctest --output-on-failure
```

在仓库根目录准备Python环境后：

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python pc_viewer/test_l3_stream.py
.venv/bin/python pc_viewer/test_stream_integration.py
```

电脑接收命令（USB路径；实际采集程序尚未启用）：

```bash
.venv/bin/python pc_viewer/l3_stream.py --host 192.168.42.130 --seconds 60
```

Tailnet接收命令（先替换实际妙算Tailnet地址，并完成两端授权与Serve配置）：

```bash
.venv/bin/python pc_viewer/l3_stream.py --host MANIFOLD_TAILNET_IP --socks5 127.0.0.1:1055 --seconds 60
```

妙算程序仅Tailnet使用时绑定`127.0.0.1`；同时做USB测试时显式绑定`0.0.0.0`。`192.168.42.130`是飞机转发地址，不是妙算本机接口地址，不能直接绑定它。

本机和妙算已安装并启动经官方SHA256核对的Tailscale 1.104.7用户态进程，已授权到同一Tailnet；没有修改系统路由或配置开机自启。二进制位于`~/.local/opt/tailscale-1.104.7/`，私有状态与socket位于`~/.local/state/l3-tailscale/`。妙算Serve9101/9102已转发到本机回环对应端口，本机SOCKS5为1055；账号、授权链接和节点身份不写入仓库。用户态模式与TCP转发依据[官方userspace说明](https://tailscale.com/docs/concepts/userspace-networking)及[官方Serve说明](https://tailscale.com/docs/reference/tailscale-cli/serve)，不是DJI SDK能力。

已验证：接收器8项测试、C到两个Python接收器的有效合成H.264/BGR与点云字段集成、单客户端断开隔离/重连、旧图像/点云协议。C核心在本机及妙算原生严格GCC、ASan/UBSan/泄漏检测通过。最新源码在妙算原生编译链接，14项CTest（分发、reader、控制器11场景、诊断/点云模式隔离）通过；新增6项ROS消息及真实本机发布订阅测试通过。默认`--stream --streams points`已实测在配置读取和SDK初始化前返回2。

实际机载出口上的Tailnet验证已通过：两端口各两连接的合成TCP回传逐字节一致；随后在妙算运行不链接SDK的C回放器，以有效合成H.264和合成点包经Serve/4G送往本机两个接收实例，实际解码、XYZ/强度/RGB字段、单端断开隔离与重连解码均通过。两实例在同一台电脑，不替代两台物理电脑验收。该测试是小流量链路验证，不是实际L3码率的带宽测试。

本轮路径探测为DERP_RELAY，往返延迟721/313/354ms；未验证DIRECT或低延迟指标。USB管理中断后已通过Tailnet SSH完成最终原生编译和测试。VNC服务卡住的问题另行修复，不进入业务代码。

点云优先阶段新文件在`/open_app/workspaces/dji/l3-dev/points-20261117`。本次已运行两轮无相机、无录制的纯Core诊断：本项目socket与官方阻塞socket下分别在14.300/14.649秒复现相同28字节分配上READ4096，见[实测报告](TEST_REPORT_2026-11-16.md#2026-11-17无相机无录制的最小对照)。当前无已验证SDK修复，没有启动真实点云录制。

最新4G→ROS/RViz验证：妙算SDK无关回放器发送合成点包，本机默认仅点云桥接运行32.016秒，接收和发布319包，点包/ROS发布错误0，视频连接0；独立ROS订阅者检查50条消息的XYZ/RGB与时间，RViz实际显示合成点。真实XYZ及最终真实双流验收仍未完成，合成结果不能替代。

[camera]: https://developer.dji.com/doc/payload-sdk-tutorial/cn/function-overview/advanced-function/basic-camera-management.html
[liveview]: https://developer.dji.com/doc/payload-sdk-tutorial/cn/function-overview/advanced-function/liveview.html
[liveview-header]: https://github.com/dji-sdk/Payload-SDK/blob/f3938d881e1628fc9a12e1b982b77060b9006311/psdk_lib/include/dji_liveview.h
[point-sample]: https://github.com/dji-sdk/Payload-SDK/blob/f3938d881e1628fc9a12e1b982b77060b9006311/samples/sample_c/module_sample/camera_manager/test_camera_manager.c
[point-header]: https://github.com/dji-sdk/Payload-SDK/blob/f3938d881e1628fc9a12e1b982b77060b9006311/psdk_lib/include/dji_camera_manager.h
[internet]: https://developer.dji.com/doc/payload-sdk-tutorial/cn/advanced-function/access-internet.html
[system-features]: https://developer.dji.com/doc/payload-sdk-tutorial/cn/manifold-quick-start/manifold-platform-capabilities/system-features.html
[capabilities]: https://developer.dji.com/doc/payload-sdk-tutorial/cn/manifold-quick-start/psdk-capabilities-overview.html
[l3-faq]: https://enterprise.dji.com/zenmuse-l3/faq

## 8. 二维点云预览经妙算和4G回传（2026-11-17）

用户本阶段将优先目标调整为回传 Pilot 2 中的二维点云相机画面，先不处理存储，明确只用妙算3、不用MSDK。它通过 `sensor_msgs/msg/Image` 显示，不是包含XYZ的 `PointCloud2`，不替代此前真实点云验收。

### 已验证的源端与传输事实

- 当前 M400 / L3 / PSDK 3.18.0 的相机类型精确为 L3，CameraManager `GetStreamSourceRange` 返回 `[7,14]`，`GetCurrentCameraStatus` 返回源14。
- `DjiLiveview_StartStream(..., DJI_LIVEVIEW_CAMERA_SOURCE_DEFAULT, ...)` 取得了1920×1080真实二维彩色点云画面。本机实际解码并目视确认；`DEFAULT`不能固定标为“可见光”。CameraManager源ID与Liveview源枚举是不同参数域，不将14直接强转传入Liveview。
- 原始25秒订阅产生47,005,975字节，约15.0Mbps；本机只解码32帧，转发器因慢客户端断开1次。4G路径需压缩预览码流。
- 妙算FFmpeg以960×540、10fps、800kbps目标参数重编码后，60秒订阅产生5,342,336字节压缩码流。本机收到4,904,625字节、解码546帧、解码错误0，实际观察268次最新帧更新，覆盖52.98秒，最大观察更新间隔约1.02秒。10fps是编码配置，不是承诺每秒10次屏幕刷新。
- 该轮转发器接受1个真实视频客户端，慢客户端断开0；结束阶段send error=1。本机统计还包含发布服务启动前/停止后Tailscale Serve连接重试，不能把这些全部解释为中途断流，也不能宣称全程零网络错误。
- `SetStreamSource(7)`有界试验及恢复请求均返回`0x00000021000000e0`。后续源14检查及点云画面再次通过，未观察到源被改变。最终代码不保留切源写操作，仅核对当前源；不能承诺PSDK自动切换或两路同时输出。

### 当前链路

```text
L3 当前二维点云画面
  → 妙算 PSDK Liveview DEFAULT
  → 本地 TCP 19102（原始H.264，不出机载网络）
  → FFmpeg 内存解码/缩放/重编码
  → FIFO → l3_video_relay → 127.0.0.1:9102
  → 已配置的 Tailscale Serve / 机载4G
  → 本机 H.264 解码 → OpenCV 或 ROS Image / RViz
```

原始媒体不写文件。运行日志在两端私有目录；本轮少量画面验证快照仅留本地`.validation/`，不提交。没有启动视频或点云录制、扫描、电机、飞控或云台动作。PSDK已知内存错误没有被修复，当前短时视频实测不能当作长期SDK稳定性或真实XYZ验收，`L3_ENABLE_POINT_RECORDING_PROBE`仍OFF。

### 构建与启动

妙算保留现有PSDK及本地凭据，在独立目录构建本仓库`onboard`，不要启动综合示例：

```bash
cmake -S onboard -B build \
  -DPSDK_ROOT=/path/to/Payload-SDK-3.18.0 \
  -DL3_ENABLE_POINT_RECORDING_PROBE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
(cd build && ctest --output-on-failure)
```

本轮部署目录为妙算`/open_app/workspaces/dji/l3-dev/preview-20261117`。需要系统`ffmpeg`含`libx264`，本机已有，无需新增安装或修改系统配置。机载脚本默认只绑定loopback，继续使用已配置的Tailscale Serve9102；不新增公网监听、路由或服务自启动。

本机启动（替换本地私有地址；SSH别名和凭据保持本地配置）：

```bash
bash scripts/start_l3_preview_4g.sh \
  --host MANIFOLD_TAILNET_IP --expected-source 14 --seconds 60

# 同一路画面在现有ROS 2 Humble / RViz中显示：
bash scripts/start_l3_preview_4g.sh \
  --host MANIFOLD_TAILNET_IP --expected-source 14 --seconds 60 --rviz
```

`--expected-source 14`校验的是本轮设备已确认的源，检查失败会拒绝启动预览，**不会写入14或开启录制**。更换设备、固件或当前源后先用`--video-info`读取，不能按编号盲试。省略该参数允许回传当前源，但不预先认定图像内容。`--no-display`用于只收流验证。SSH只管理进程，业务图像走所指定的Tailnet/SOCKS连接。

妙算单独启动可调分辨率/码率，所有进程有有限运行时间和外部超时：

```bash
bash scripts/run_l3_preview_onboard.sh \
  --bin-dir /absolute/path/build --config /private/psdk.conf \
  --expected-source 14 --seconds 60 --width 960 --fps 10 --bitrate-kbps 800
```

正常结束会停Liveview、编码器和转发器。强制终止或本机窗口提前关闭时，远端可能继续到本次有界超时结束；不承诺关闭本机窗口就是机载立即停流。不能同时启动另一份占用相同SDK/端口的采集程序。

### 验证覆盖

原JPEG协议离线测试通过；H.264/点包接收与双客户端集成测试9项通过；新增stdin视频转发的真实合成H.264解码、断开重连、空闲取消测试通过；ROS桥接6项通过。妙算原生14项CTest通过，包含视频模式不调用点云录制控制器、视频源范围长度越界拒绝、精确L3检查及清理。`test_diagnostic_modes`修复了旧布尔参数仍被当作枚举使用的测试问题，明确分别验证`POINTS`、`VIDEO`、`BOTH`。

最终`--expected-source 14 --rviz --seconds 45`实测：当前源14核对通过；本机收到3,773,089字节、解码424帧、发布236条Image，解码错误/ROS发布错误/连接错误均0，ROS未发布任何PointCloud2。转发器输出3,968,951字节，1个连接，慢客户端断开0、send error=0。RViz界面已目视确认显示二维彩色点云；其三维主视图为空属于当前Image模式，不表示XYZ接通。发布器、FFmpeg、转发器、本机接收返回码均0。原始截图和日志仅本地保存，不随代码上传。
