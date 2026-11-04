# 本次开发 handoff

2026-11-04 妙算路线接续：请先阅读 [妙算 3点云开发流程](docs/MANIFOLD3_POINTCLOUD_WORKFLOW.md)，按设备上线、PSDK 实际收点、有线传输、ROS/RViz、遥控器转发的顺序开发验收。当前本机 Ubuntu 22.04 / ROS 2 Humble 的 RGB/RViz 链路已验证，真实 XYZ 尚未取得，妙算 3尚未接入；此前已批准的一次及后续三次地面录制均已停止，授权已用完。该流程为实施计划，不是已完成的驱动。下文保留历史交接记录，以新流程的状态与边界为准。

更新：2026-11-03，Asia/Shanghai。本文供在 Linux 电脑继续开发的 agent 使用。

## 1. 用户目标与当前阶段

最终目标是 Matrice 400 挂载禅思 L3，实时获取可见光图像和真实激光点云到电脑处理/展示，之后集成秒算3和 Linux。用户不熟悉 Android 开发，需要给出可直接执行的说明。

本次先完成了可见光链路：

```text
L3 → M400 → 无线图传 → RC Plus 2 Android App（MSDK 5.18）
                                  ↓ NV21 回调 → JPEG
                            USB / ADB reverse
                                  ↓
                         Windows Python → BGR / OpenCV
```

**严禁起飞，只允许获取 L3 数据。不要为了取点云启电机、解锁、起飞或调用飞控。**

## 2. 已实机确认的事实

- 设备：Matrice 400、Zenmuse L3、DJI RC Plus 2；遥控器 Android 11 / API 30。
- 遥控器已允许 ADB 调试；安装的 App 已通过 MSDK 注册，运行时确认产品为 `DJI_MATRICE_400`。
- 包名：`com.dji.sampleV5.aircraft`。SDK 版本：5.18.0；Android build compile/target API 35、min API 24、arm64-v8a。
- 相机精确类型 `ZENMUSE_L3`；本机实际端口 `PORT_1`；当前源 `WIDE_CAMERA`，硬件返回范围 `[WIDE_CAMERA, POINT_CLOUD_CAMERA]`。下一设备需重新枚举。
- 已收到并显示真实可见光场景。图传帧为 1920×1080，画面本身可能含黑边，不等于相机原始测绘照片分辨率。
- SDK 解码回调为 NV21。发送端最多每 200 ms 取一帧，复制 SDK 缓冲后交给容量为 1 的最新帧队列；编码 JPEG quality 80；PC 解码为 `uint8` BGR 数组。
- 18:28 左右的 24.09 秒观测窗口约 4.57 fps，协议错误 0。
- 本地最终统计：887 帧、242.109 秒（包含启动等待）、平均 3.664 fps、1 次连接、0 个协议错误、superseded_frames=19，结束状态 `stopped`。不能把这写成相机原生帧率或稳定 5 fps，0 协议错误也不代表无线链路零丢包。
- APK 已编译、签名校验、覆盖安装并在真机运行。软件中没有起飞、电机、飞控、对频、云台动作或相机源写操作。
- Python 离线协议测试及 Windows GUI 烟测已通过，覆盖拆包、重连、无效长度/尺寸拒绝、BGR 解码、有限帧退出、首帧 Unicode 路径写入和有界缓冲。

统计是历史验收记录，不是现在设备还在线的承诺。不要把真实图像、日志或设备身份上传仓库。

## 3. 代码与上游

本仓库不是整份 DJI SDK。它保留自定义代码、可复现补丁、接收器与文档：

| 文件 | 用途 |
|---|---|
| `android/L3ReadOnlyActivity.kt` | 专用遥控器页面：SDK 就绪后枚举 L3，异步读元数据，预览并取帧传输 |
| `android/patches/msdk-5.18-l3-read-only.patch` | 对固定上游提交的完整修改，包含新 Activity、Manifest、USB 入口与 networkImp |
| `pc_viewer/receiver.py` | 接收、校验 JPEG、解码 BGR、显示、状态 JSON；可作为模块调用 |
| `pc_viewer/test_protocol.py` | 无硬件合成数据集成测试 |
| `scripts/start_linux.sh` | Linux USB 映射、启动专用页面、启动接收器 |
| `docs/LINUX_SETUP.md` | Linux 最小环境、可选 ROS 2、Android 构建和点云接续计划 |

官方上游：[dji-sdk/Mobile-SDK-Android-V5](https://github.com/dji-sdk/Mobile-SDK-Android-V5)。本次下载分支 `dev-sdk-main`，ZIP 注释给出的准确提交为：

```text
91bf2d957e74635932ae639bc9bc9ac70eb200f4
```

原始 ZIP SHA256：`bcaef6597d374ac3e901037b39e2f5ebe4b39aa71990a1e08ef34cb3525b3f2e`。构建入口是 `SampleCode-V5/android-sdk-v5-as`，任务 `:sample:assembleDebug`。补丁不包含个人机器的 Gradle 配置或 App Key。

版本：JDK 17、Gradle 8.12、AGP 8.7.0、Kotlin 2.1.0、Android API 35，已有 build-tools 34.0.0 成功构建。上游虽然定义 NDK 21.4 变量，但本次工程没有使用它编译原生源码，不要提前下载大体积旧 NDK。

补丁做了四件事：

1. 新增 `L3ReadOnlyActivity`，只用相机读取接口，精确匹配 L3。
2. Launcher 指向专用页面，保留原示例源代码但不从其控制界面操作。
3. `UsbAttachActivity` 同样跳转专用页面，避免 USB 事件切回原主页。
4. 添加 `runtimeOnly deps.networkImp`，提供注册所需 SDK 联网能力。

Windows 本地还配置过 JDK 路径、SDK 路径、代理和 `android.overridePathCheck=true`。这些不是跨机配置，不应直接抄到 Linux。上游带旧 JCenter/Fabric/Bintray 仓库，本次在已有本地代理下解析成功；以后如依赖解析失败，应根据实际错误处理，不要盲目升级整套 SDK/Gradle。

## 4. 两端分工和通信协议

MSDK 始终在遥控器 Android App 中运行。电脑只提供 ADB USB 端口映射、TCP 接收、处理和显示；电脑由 Windows 换成 Linux 不要求重写 APK。

遥控器 App 连接自身 `127.0.0.1:9000`。电脑运行：

```bash
adb reverse tcp:9000 tcp:9000
```

ADB 把这个连接送到电脑 `127.0.0.1:9000` 的接收服务。接收器绑定 loopback，不需要打开公网端口。更换电脑、重新连接 USB 或 ADB 重启后要检查映射与授权。

每帧由 32 字节大端头和 JPEG 组成，Python 格式 `>4sIIIQQ`：

| 字段 | 类型 | 含义 |
|---|---|---|
| magic | 4 bytes | ASCII `L3F1` |
| payload_length | uint32 | JPEG 长度 |
| width / height | uint32 各一个 | 解码图像尺寸 |
| sequence | uint64 | 本次 App 进程的采样序号 |
| remote_callback_monotonic_ns | uint64 | 遥控器接收回调时的单调时钟 |

**时间戳不是传感器曝光时间，不是硬件同步时间，不能直接与 PC 时钟相减来测单向延迟。**

PC 限制 JPEG ≤8 MiB，尺寸 ≤8192、总像素 ≤32 Mi，验证 JPEG 头尺寸与协议尺寸一致；断线重新 accept，最新帧单槽避免积压。GUI 使用显示副本，`Frame.image` 保留无叠字 BGR。

接收器模块用法（先关闭独立 viewer，避免端口占用）：

```python
from receiver import Receiver  # 在 pc_viewer 目录运行，或将该目录加入模块路径
rx = Receiver()
rx.start()
try:
    frame = rx.latest_frame()
    if frame is not None:
        bgr = frame.image  # uint8, H×W×3；修改前 copy()
finally:
    rx.stop()
```

这是单次读取示例；实际算法需要自行循环获取最新帧，并跳过重复序号。

## 5. 尚未完成的工作

- Linux 电脑的 ADB、真实 USB 链路、Python GUI 和持续收帧验收。
- 真实 L3 XYZ / intensity / RGB 点云的订阅、解包、坐标系和单位验证。
- ROS 2 `PointCloud2` / `Image` 发布、RViz2 配置。
- 秒算3应用部署、点云经 MOP 下传、带宽和延迟测试。
- 点云与 RGB 硬件时间同步、标定和高精度建图。

**不要把 `POINT_CLOUD_CAMERA` 的图像当作 XYZ 数据；换 Linux 或装 RViz 不会自动增加点云接口。**

## 6. 真实点云：已知证据与下一步

官方 L3 FAQ 明确：点云录制期间，可由 OSDK/PSDK 设备订阅实时点云。实时流是用于现场预览的抽稀数据，不能等同于存储卡内的高精度测绘数据。

官方 PSDK `DjiTest_CameraManagerSubscribePointCloud()` 示例使用 MOP 通道 **49154**，UNRELIABLE，按负载挂载端口寻址，接收彩色点云结构。49154 是 DJI MOP channel ID，不是 PC TCP/UDP 端口。该示例会调用启动/停止点云录制，只接收 30 包，然后停止；**不要照搬综合示例运行**。

本机 5.18.0 provided JAR 的 `javap` 和 `MopVM.kt` 已确认存在：

```text
connectPipeline(ComponentIndexType, int, PipelineDeviceType, TransmissionControlType)
```

因此可提出有界实验候选 `已确认L3端口 / 49154 / PAYLOAD / UNRELIABLE`。注意线上及随包 HTML 文档仍可能只列旧三参数重载；这是发现候选路径的依据，**不是 MSDK 已成功直接取 L3 点云的证据**。

下一位 agent 按顺序执行：

1. 先在 Linux 复现已有 RGB 路径，保存脱敏测试结论。
2. 重新核对当前 SDK、固件和官方点云协议；确认 L3 扫描/录制状态满足订阅前提，清楚说明必要采集操作，遵守用户授权和地面限制。
3. 在专用页面内做一次有界 MOP connect/read/disconnect 实验，不发送测试数据，不调用飞控，不盲扫 ID。
4. 接收线程使用循环而非递归；只解析 `result.length` 有效字节，处理部分包、边界、断流和取消。按匹配版本的 packed 定义核对 flag/seq/timestamp/dataByte、点结构和单位，不能凭点数看起来合理就宣布成功。
5. 只有持续收到有效坐标、点数和序号，才认定 MSDK 直取成功；只连接成功或没有数据都不能证明全部支持/不支持。
6. 若该路径没有成功，采用标准机载路线：`L3 → M400 → 秒算3 PSDK → 网络 → Linux PC`，先验证有线，再考虑 `秒算3 → 自定义MOP → RC App → USB → PC`。
7. 取得真实数据后再封装 `sensor_msgs/msg/PointCloud2`，明确 frame_id、坐标轴、单位和时间戳含义，配置 RViz2。只有本地显示时可以先用固定 L3 frame，不要伪造世界坐标或硬件同步。

当前 M400 标准 PSDK 相机管理路线要求秒算3。MOP 的基础互联能力不能简单等同于全部相机高级能力，所以也不能断言任何客户端都绝对必须使用秒算3。

PC 可通过 **M400 调参 USB 口** 的虚拟网络访问已经装在 E-Port 上的秒算3，再接收秒算3应用发出的数据。飞机 USB 不直接变成 L3 XYZ 设备。**秒算3自身 USB-C 不支持直接接电脑**，不要把两个 USB 口混淆。

本轮点云调研未操作设备。当前点云教程要求关闭 Pilot 2；历史 L2 通道说明提到冲突优化，不能据此保证 L3 可与 Pilot 2 并发。不要为了得到机身感知数据执行文档中的解锁/起飞流程，本任务只允许 L3。

## 7. 原 Windows 工作目录（仅供定位，不是 Linux 路径）

```text
E:\如影随形\DJI_MSDK\
  Mobile-SDK-Android-V5-dev-sdk-main\SampleCode-V5\android-sdk-v5-as\
  Mobile-SDK-Android-V5-dev-sdk-main\SampleCode-V5\android-sdk-v5-sample\
  pc_viewer\
  build-debug.cmd
  start-l3-viewer.cmd
```

Windows 工具位置：JDK17 在 `E:\javaJDK`，SDK 在 `D:\AndroidSDK`，Python 在 `D:\anaconda\python.exe`，Android Studio 在 `F:\Android\Android Studio`。不要把这些路径写进 Linux 配置。

实际 App Key 只存在原 Windows 本地工程；不要复制进 Git。新建应用时包名须匹配 `com.dji.sampleV5.aircraft`。已有 APK 使用同一个包名/签名，覆盖安装要保持签名一致，不能未经评估卸载已有 App 来规避签名问题。

## 8. 官方参考

- [MSDK 官方源码](https://github.com/dji-sdk/Mobile-SDK-Android-V5)；[运行 Sample](https://developer.dji.com/doc/mobile-sdk-tutorial/en/quick-start/run-sample.html)
- [相机视频与取帧 API](https://developer.dji.com/api-reference-v5/android-api/Components/IMediaDataCenter/ICameraStreamManager.html)
- [ADB 反向端口映射](https://developer.android.com/develop/ui/views/layout/webapps/access-local-server)
- [L3 官方 FAQ](https://enterprise.dji.com/zenmuse-l3/faq)
- [PSDK 点云示例](https://github.com/dji-sdk/Payload-SDK/blob/master/samples/sample_c/module_sample/camera_manager/test_camera_manager.c)
- [PSDK 相机管理](https://developer.dji.com/doc/payload-sdk-tutorial/en/function-overview/advanced-function/basic-camera-management.html)
- [PSDK 开放能力](https://developer.dji.com/doc/payload-sdk-tutorial/en/development-preparation/development-platform-overview/open-capability-list.html)
- [秒算3连接方式](https://developer.dji.com/doc/payload-sdk-tutorial/en/manifold-quick-start/manifold-connect.html)
- [秒算3互联能力](https://developer.dji.com/doc/payload-sdk-tutorial/cn/manifold-quick-start/psdk-capabilities-overview.html)
- [实时预览数据质量的官方支持答复](https://github.com/dji-sdk/Payload-SDK/issues/480)

