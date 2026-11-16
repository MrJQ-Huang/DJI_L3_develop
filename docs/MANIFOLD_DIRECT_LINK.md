# 妙算3 → Linux 直传开发基线

2026-11-16 后续总体方案已整理为[空地多模态数据传输开发方案](REMOTE_SYSTEM_DEVELOPMENT_PLAN.md)，远程出口优先验证原生机载4G与 Tailscale。本文保留 USB 联调和早期独立无线候选的技术基线。

2026-11-10。用户要求数据不经过遥控器，可增加独立无线设备。只做地面 L3 取数，不启电机、不起飞、不操作飞控、云台或对频。

```text
L3 → M400 机内数据链路 → 妙算3 PSDK 采集程序
                                 │
                地面调参 USB 虚拟网络（先验收）
                或独立 Wi-Fi / 点对点网桥（最终直传候选）
                                 │
                       Linux PC（Ubuntu 22.04 / Humble）
                        ├── XYZ → PointCloud2 → RViz2
                        └── 编码视频 → 解码 → Image
```

没有查到公开的 Linux 接收器或任意 IP 透传接口，可让普通 PC 网卡直接接入 M400 原厂 O4。给电脑加 Wi-Fi 天线不会使其识别 O4。DJI O4 地面站的网口也不能仅凭存在就认定可以接收妙算自定义数据。若必须用原厂天线且不允许任何中继，当前没有已核实的方案；独立无线方式使用自己的无线网卡/网桥和天线。

## 2026-11-16 实机连接与编译检查

- 用户报告固件升级完成。原接 E3 时妙算3指示灯完全不亮、Pilot 2 无设备卡片；更换到 **E2** 后亮灯，Pilot 2 可见卡片。E3 此前异常的根因尚未确认，不据此判断端口损坏。
- Windows PC 仍连接 M400 调参 USB 口。虚拟网卡地址为 `192.168.42.1/24`，实际 E2 地址 `192.168.42.130:22` 已连通，SSH 登录成功。后续接续不要继续沿用 E3 的 `.140` 地址。E2 是妙算的接入位置，L3 的负载挂载位置仍须独立查询。
- 实机环境：Ubuntu **20.04.6 LTS**，`aarch64`，内核 `5.10.192-tegra`，NVIDIA L4T **35.5.0**；GCC/G++ **9.4.0**、CMake **3.16.3**、Make **4.2.1**、Python **3.8.10**。`nvidia-jetpack` 元包未查询到，不能仅凭 L4T 结果声称全部 JetPack 组件已安装。
- 本轮查看时，`dji_app_ctl status` 中已安装应用均为 `Not Running`。未启动、停止或更改任何应用的自启动设置。
- 在独立目录 `/open_app/workspaces/dji/l3-dev/preflight-20261116` 上传了三个适配模块及官方 PSDK 3.18.0 的依赖头文件。`l3p1_tcp_sink.c`、`l3_pointcloud_psdk.c`、`l3_rgb_probe.c` 均通过 `gcc -std=c11 -Wall -Wextra -Werror -c` 原生编译；后两个模块增加 `-pthread -I psdk-3.18-include`。**仅生成目标文件，未链接 PSDK、未执行采集程序，也未向飞机发送采集或控制指令。**该头文件集不含完整 SDK 静态库、HAL 或 OSAL，不能据此认为已可部署运行。
- PC 离线验证：`pc_viewer/test_l3_points.py` 的 3 项测试通过，`pc_viewer/test_protocol.py` 全部通过。合成数据测试不能证明真实 L3 数据链路已打通。

同日后续已完成 **Payload SDK-妙算3** 应用的私有配置，并下载固定提交的完整 PSDK 3.18.0。新增独立 `onboard/metadata_inspect.c` 与 CMake，原生编译链接成功；一次有外部超时守护的实机检查约 8.99 秒正常退出：Core Init、CameraManager Init、软件 ApplicationStart 及清理均成功，确认 M400 版本 19.01.10.03，PSDK 负载位置 1 为精确 L3 类型 117。其余位置返回 `0xe1` 超时，不是“不支持”的证据。该程序只检查元数据，不接视频、点云或录制。上述三个采集适配模块仍未链接到采集入口，不能把元数据成功写成双流已接通。真实凭据及 SDK 原始日志只存私有文件，不进入 Git。本轮未启动录制、扫描、云台或飞控操作，新的取流测试按用户要求暂停，下一步以总体方案为准。

短距地面联调：让妙算3接 DJI 已明确支持的 USB Wi-Fi 网卡（文档列 RTL8852BU、RTL88X2BU 芯片）并加入已知局域网；PC 同网即可，不要求再配同款天线。需要稳定的点对点链路时，再按距离、环境和实测吞吐选一对以太网无线网桥，妙算3经兼容 USB 以太网适配器接机载侧。不要仅按射频距离、图传清晰度或标称速率承诺净吞吐。机载 4G 或外置 CPE 是可选的独立 IP 链路，不属于 O4 直连。

## 两路数据的验收条件

| 数据 | 当前证据 | 首轮验收 |
| --- | --- | --- |
| L3 真实 XYZ／强度／逐点 RGB | 官方明确支持点云录制期间经 PSDK MOP 49154 订阅；教程要求关闭 Pilot 2 | 确认 L3 类型与端口、连续取得有效原始包、点数和序号变化。校验轴向、单位、原点与时间含义 |
| L3 独立可见光画面 | RC 上的 MSDK `WIDE_CAMERA` 已实机验证；PSDK 3.18.0 头及机载视频支持表没有明确列出 L3 的可见光源 | 在确认为 L3 的负载端口探测 PSDK Liveview，解码并目视核对实际场景；再测与点云订阅同时运行 |
| 两路同时无线接收 | 尚未实测；不存在已验收的原厂 O4 直收方案 | 确认两源均有效，再验证有线传输、无线吞吐、断流和恢复。视频优先传压缩流，点云量与发送频率按测试值设置 |

点云包中逐点 RGB 不能替代独立的 RGB 相机画面。SDK 的点云是实时预览级数据，不是存储卡里的全量测绘数据。SDK 接收需要 L3 在录制点云；本阶段探测代码不会自己开始或停止录制。先核实设备状态和本轮的采集授权，禁止直接运行综合飞控示例。没有取到点也不能以起飞绕开限制。

## 开发顺序

1. 电脑用数据线接 **M400 调参 USB 口**，先确认妙算3的 SSH 登录与版本。不要插妙算3自身 USB-C 当电脑设备口。官方 E1/E2/E3 默认地址为 `192.168.42.120`／`.130`／`.140`；只尝试实际接线所对应的已知地址，不扫描网络。不为了使用最新版 PSDK 自动升级固件或刷系统。
2. 妙算端使用其预装 AArch64 环境，核对 PSDK App 的有效凭据和高级权限。已有遥控器 MSDK Key 不能拿来替代 PSDK 应用配置，不把 Key 放进 Git 或聊天。首轮只用[专用采集模块](../onboard/README.md)，沿用审计过的 SDK 初始化流程；不配置自启动。
3. Linux 点云接收的第一版使用 TCP。`L3P1` 是本项目开发协议：大端 4 字节魔数 + 4 字节包长 + 一份完整原样的 DJI MOP 点云包。Python 验证包长、标志和坐标范围，默认监听回环，最多 512 KiB/包；供地面有线或隔离的无线网段使用。读取 28 字节头与 16 字节点记录，保留源时间与 CRC 原值。CRC 算法、真实包分片规律及坐标尺度仍需实机核对，不能用模拟数据声称已实机验证。
4. 相机探测使用官方 Liveview 的 `DEFAULT` 候选源，仅作能力核对。成功回调也要实际解码，确认收到的是 L3 的可见光画面，而非 FPV 或点云预览。在两个通道均通过前，不发布为完整双流方案。
5. 有线稳定后改为独立 Wi-Fi/IP 网桥。以 PC 实测的点包速率、视频码率、排队延时与断线恢复决定采样；不要把接收时钟当曝光时间、硬件同步或经校准的单向延迟。现有 Ubuntu 22.04 / Humble 继续使用，无需为了本阶段更换发行版。

离线执行（不会访问设备）：

```bash
python3 pc_viewer/test_l3_points.py
python3 pc_viewer/l3_points.py listen --count 30 --timeout 30
```

第二条只是有界接收诊断，无发送端时会超时。真实数据有线测试时用 `--bind` 指定经过确认的 Linux 调参网络本机地址，连接到对应 TCP 端口（默认 9010）；这个端口与 DJI MOP 通道 ID 49154 无关。本阶段不产生 ROS 点云话题或启动相机录制。

官方参考：[L3 FAQ](https://enterprise.dji.com/zenmuse-l3/faq)、[PSDK 相机管理](https://developer.dji.com/doc/payload-sdk-tutorial/en/function-overview/advanced-function/basic-camera-management.html)、[PSDK Liveview](https://developer.dji.com/doc/payload-sdk-tutorial/cn/function-overview/advanced-function/liveview.html)、[PSDK 3.18.0](https://github.com/dji-sdk/Payload-SDK/releases/tag/3.18.0)、[妙算3连接](https://developer.dji.com/doc/payload-sdk-tutorial/en/manifold-quick-start/manifold-connect.html)、[外置网络设备](https://developer.dji.com/doc/payload-sdk-tutorial/en/manifold-quick-start/manifold-platform-capabilities/system-tools.html)、[O4 地面站规格](https://enterprise.dji.com/o4-ground-station/specs)。
