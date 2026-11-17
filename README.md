# DJI L3 开发交接

2026-11-17最新要求：只用妙算3/PSDK，把二维点云画面按普通高清视频显示，不接RViz，不二次编码或降低分辨率/码率。已修正固定SDK的USB越读、退出适配，并加入断线后完整重新订阅；90秒实测解码543帧、两次恢复画面，机载正常退出。**持续原码率回传与软件切换仍未通过**：原流约15Mbps，当前路径约5Mbps，SDK切源仍未成功；SDK退出仍有泄漏报告。当前部署、修复边界见[最新修复记录](docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md#10-sdk越读退出与视频恢复修复2026-11-17)。

```bash
bash scripts/start_l3_preview_4g.sh --host MANIFOLD_TAILNET_IP --seconds 300
```

以下为历史阶段记录。

2026-11-17点云优先：已实现仅点云/双流机载入口、TCP分发、接收解码及本机ROS/RViz桥接。实际机载4G合成点流在本机RViz运行32秒，接收/发布319包，无点包或ROS发布错误；14项妙算原生测试及6项新增ROS测试通过。真实XYZ仍未取得：纯Core无相机/无录制诊断在本项目与官方socket收发下都复现SDK内部28→4096越读，录制门槛保持关闭。构建、测试、接收命令与最新状态统一见[开发方案](docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md#7-已实现代码运行与实测边界)，下方旧日期记录作为历史参考。

2026-11-16 测试更新：[L3 视频与点云首次测试记录](docs/TEST_REPORT_2026-11-16.md)。已从妙算取得并解码 L3 1920×1080 可见光预览；发现 PSDK 内部运行期越界读取，实际点云录制和稳定双流回传尚未执行。先解决 SDK 问题，禁止把短样本或纯模拟测试写成实时双流通过。

2026-11-16 当前开发方案见 [空地多模态数据传输开发方案](docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md)：保留单仓库的协议、机载、地面三个构建模块；先验证真实双流，再完成有线、机载4G与 Tailscale、多 PC 和 ROS 2。妙算 E2 的 SSH 与专用 PSDK 元数据程序已实机通过，L3 精确确认在 PSDK 负载位置 1；PSDK 视频和点云尚未取流。全程地面，严禁起飞。下方旧日期内容作为历史交接记录。

2026-11-10 接续：妙算3和 L3 已接入 M400。现阶段数据链路不经过遥控器，参见 [妙算3 → Linux 直传开发基线](docs/MANIFOLD_DIRECT_LINK.md)。当前新增离线点云解析与接收诊断，双流实机取数及无线直传尚未验证。

2026-11-04 更新：取得妙算 3后的硬件准备、PSDK 真实点云订阅、电脑传输与 PointCloud2/RViz 验收流程见 [妙算 3点云开发流程](docs/MANIFOLD3_POINTCLOUD_WORKFLOW.md)。本机 Linux RGB/RViz 已验证，真实 XYZ 尚未接通；下文未更新条目保留原交接阶段的记录。

本仓库保存 2026-11-03 在 Windows 上完成的 Matrice 400 / Zenmuse L3 / DJI RC Plus 2 图像取帧原型，以及交给 Linux 开发环境的接续说明。

**用户硬性要求：严禁起飞，只允许获取 L3 数据。所有验证均为地面验证。**

## 从这里开始

1. 下一位 agent 先读 [AGENTS.md](AGENTS.md) 和 [HANDOFF.md](HANDOFF.md)。
   当前目标、已验证状态和阶段验收以 [开发方案](docs/REMOTE_SYSTEM_DEVELOPMENT_PLAN.md) 为准。
2. Linux 环境和操作步骤见 [docs/LINUX_SETUP.md](docs/LINUX_SETUP.md)。
3. 已验证的 PC 接收器是 [pc_viewer/receiver.py](pc_viewer/receiver.py)。
4. 遥控器专用页面见 [android/L3ReadOnlyActivity.kt](android/L3ReadOnlyActivity.kt)；对固定 DJI 上游提交的完整修改见 [Android 补丁](android/patches/msdk-5.18-l3-read-only.patch)。

## 当前状态

| 项目 | 状态 |
|---|---|
| L3 可见光帧经遥控器、USB 传至 Windows 并显示 | 已实机验证，1920×1080，采样上限 5 fps |
| PC 获取可供 OpenCV 使用的 BGR NumPy 数组 | 已实现 |
| Linux 历史遥控器图像链路 | Ubuntu 22.04 / ROS 2 Humble 的 RGB/RViz 已验证；新 PSDK 双流链路另行验收 |
| L3 真实 XYZ / intensity / RGB 点云 | 尚未接入 |
| MSDK MOP 直接连接 L3 | 仅发现可验证的接口路径，尚未成功取点 |
| 妙算3 → 4G → 本机ROS 2 / RViz2 | 已实现并通过合成点流；真实PSDK点云受SDK内存错误阻塞 |
| 妙算3 SSH、PSDK 初始化、M400 与 L3 精确识别 | 2026-11-16 已实机通过；E2 / 192.168.42.130，L3 位于 PSDK 负载位置 1 |
| 图像和点云硬件同步 / 高精度建图 | 尚未实现 |

## Linux 最小启动

按 Linux 文档安装 ADB、Python 和依赖后：

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python pc_viewer/test_protocol.py
bash scripts/start_linux.sh
```

遥控器需已安装对应 APK、通过新电脑的 USB 调试授权，且前台为 `L3ReadOnlyActivity`。脚本只设置 USB 端口映射并启动相机读取页面、电脑接收器。

本仓库没有上传 App Key、APK、签名文件、设备序列号、原始运行日志或实际拍摄的画面。构建需要从原持有人处取得匹配包名的 App Key；同一遥控器上已安装的 APK 可以直接用于 Linux 接收，不需要为了换电脑重新编译。

