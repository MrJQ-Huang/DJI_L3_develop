# DJI L3 机载 4G 实时预览

本项目使用 DJI Matrice 400、禅思 L3 和妙算3，将 L3 的原始 H.264 码流经机载 4G 和 Tailscale 传到 Linux 电脑。当前窗口支持在 **可见光广角**和**二维点云画面**之间切换。回传保留相机原始分辨率和码率，窗口缩放只影响本地显示。

**全程地面联调。严禁起飞、启用电机或调用飞控。**

## 启动高清预览

地面电脑需要 Python 3、Tk、SSH 和已连通的私有 Tailscale。妙算3接 M400 的 E-Port V2，运行基于固定版本 **PSDK 3.18.0** 构建的 `l3_metadata_inspect`。本机通过 SSH 启动机载会话，并通过 SOCKS5 接收从妙算 TCP 9102 转发的码流。接线、构建和网络设置见 [Linux 环境](docs/LINUX_SETUP.md#c-当前二维视频预览不使用rviz)、[机载程序](onboard/README.md)和[部署说明](docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md)。

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
bash scripts/start_l3_preview_4g.sh \
  --host MANIFOLD_TAILNET_IP --ssh-host l3-manifold --seconds 300
```

将 `MANIFOLD_TAILNET_IP` 换成妙算3的私有 Tailscale 地址，`l3-manifold` 换成已配置的 SSH 别名。默认使用本机 SOCKS5 `127.0.0.1:1055` 和机载部署 `repair-20261117`，可用 `--socks5`、`--remote-root` 和 `--remote-config` 指定自己的部署。预览直接使用妙算3上的 PSDK，地面端只需普通视频窗口。

- 开启时读取 L3 当前源。源 `1` 为可见光广角，源 `14` 为二维点云画面。
- 点击“切到可见光”或“切到点云画面”。窗口在设备确认后重新订阅视频，取得首个可解码的关键帧。
- **按 Esc 停止预览**，也可点击“停止预览”或关闭窗口。前台终端可按 Ctrl+C。预览时长参数上限为 300 秒。

支持 `--fast-switch` 的机载构建可试用同进程切换。地面脚本和机载程序需要来自同一版本。用 `--remote-root` 指定新构建所在目录。

```bash
bash scripts/start_l3_preview_4g.sh \
  --host MANIFOLD_TAILNET_IP --ssh-host l3-manifold --seconds 300 \
  --remote-root /absolute/path/to/updated/onboard-deployment --fast-switch
```

切换时保留上一帧，等待新源首帧。模式会在原 SDK 会话内停止并重新订阅 Liveview，建立新的视频 TCP 连接。若重订阅失败或等待首帧超时，在机载进程清理并正常退出后回退到完整重连。

## 机载 YOLO 人员检测

在妙算3的独立部署目录构建同版本的机载程序，并保留该目录下的 `onboard/` 和 `scripts/`。通过有线 SSH 上传构建所需文件和模型，在目标妙算3上生成 TensorRT FP16 引擎。这里的构建命令对应 TensorRT 8.5 和 CUDA 11。机载程序需要 Python 3 的 TensorRT、OpenCV、NumPy 和 GStreamer GI 绑定，以及 NVIDIA 的 `nvv4l2decoder`、`nvvidconv` 和 `nvv4l2h264enc` 插件。[官方 YOLOX-s ONNX 下载](https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_s.onnx)

```bash
/usr/src/tensorrt/bin/trtexec --onnx=/absolute/path/yolox_s.onnx \
  --saveEngine=/absolute/path/yolox_s.fp16.engine --fp16 --buildOnly --heuristic \
  --avgTiming=1 --workspace=1024 --tacticSources=-CUDNN \
  --precisionConstraints=obey '--layerPrecisions=*:fp16'
```

有线部署完成后，用机载 4G 上的 Tailscale 地址验收。先让 L3 处于可见光源，再从 PC 启动。

```bash
bash scripts/start_l3_preview_4g.sh \
  --host MANIFOLD_TAILNET_IP --ssh-host l3-manifold --seconds 180 \
  --remote-root /absolute/path/to/updated/onboard-deployment \
  --expected-source 1 --yolo-engine /absolute/path/yolox_s.fp16.engine
```

- 默认关闭 YOLO，转发原始 H.264。点击“开启 YOLO”后，妙算3对可见光画面运行人员检测、画框并硬编码 1920×1080 H.264。检测输入为 640×640，视频回传仍用 TCP 9102。
- 处理后的码率默认 4 Mbps，可用 `--yolo-bitrate` 指定 bit/s。点击“关闭 YOLO”后恢复相机原码流。按钮在本机解出新模式的首帧后更新。
- YOLO 开关只对源 `1` 可用。要切到二维点云画面，先关闭 YOLO。按 **Esc** 停止预览。

## 实机验证和当前限制

| 项目 | 状态 |
| --- | --- |
| 可见光和二维点云画面切换 | 2026-11-17 实机完成 `14 → 1 → 14 → 1 → 14`，每段收到至少 30 帧新的 1920×1080 画面。 |
| 机载 4G 回传 | 2026-11-18 的预览会话实际解码 2,995 帧 1920×1080 视频，包含 3 次重订阅，机载程序正常退出。长时间稳定回传仍待验收。 |
| 机载 YOLO 人员检测 | 2026-11-27 有线部署后，妙算3用公开样片输出带框 1080p 视频，PC 经 4G 解码并确认画框。实际 L3 可见光也经 4G 两次完成原画 → YOLO → 原画，其中一次使用 PC 窗口按钮。每段验证 60 帧。开启至首帧约 6.5–6.8 秒，关闭至恢复原画约 3.1 秒。 |
| 实时 XYZ 激光点云 | 尚未取得有效点包。录制启动和 MOP 49154 连接成功后，`RecvData` 返回 `0x0000000f00000001` 或阻塞。详见[故障记录](docs/dji-support-2026-11-17/README.md)。 |
| ROS 2 `PointCloud2` / RViz | 桥接与机载 4G 通路已用合成点流验证。真实 XYZ 接入仍依赖 L3 输出有效点包。 |

源 `14` 输出的是供人观看的二维 H.264 画面。真实 XYZ 取数通过独立的 MOP 接收路径，默认构建保持 `L3_ENABLE_POINT_RECORDING_PROBE=OFF`。接收重试用于诊断，实机点云问题仍待解决。可用独立的 `l3_metadata_inspect --config PATH --camera-firmware --mount 1` 查询所选位置上已确认的 L3 固件。

项目在固定 PSDK 库的构建副本中修复已复现的 USB 越读，并修正线程退出适配。后续视频诊断仍有 SDK 内存泄漏报告。版本约束、切源兼容处理和实验边界见[修复与验收记录](docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md#11-切源按钮修复2026-11-17)。

## 项目入口

| 路径 | 用途 |
| --- | --- |
| [onboard/](onboard/README.md) | 妙算3 PSDK 应用、L3 预览与点云诊断、TCP 分发 |
| [pc_viewer/l3_preview.py](pc_viewer/l3_preview.py) | 4G 高清预览窗口、切源、YOLO 开关与断线恢复 |
| [pc_viewer/l3_ros_bridge.py](pc_viewer/l3_ros_bridge.py) | ROS 2 图像和点云桥接 |
| [android/](android/L3ReadOnlyActivity.kt) 和 [pc_viewer/receiver.py](pc_viewer/receiver.py) | 早期 DJI RC Plus 2 / MSDK 可见光实验及 USB 接收器，操作步骤见 [Linux 环境](docs/LINUX_SETUP.md#a-地面电脑先复现-rgb不急着安装-ros-或-android-studio) |
| [HANDOFF.md](HANDOFF.md) 和 [AGENTS.md](AGENTS.md) | 工程交接、验收证据及实机操作范围 |

仓库使用占位配置和合成测试数据。应用凭据、签名、设备身份、原始日志和实机画面保留在私有目录。
