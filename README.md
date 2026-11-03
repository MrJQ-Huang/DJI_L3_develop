# DJI L3 开发交接

本仓库保存 2026-11-03 在 Windows 上完成的 Matrice 400 / Zenmuse L3 / DJI RC Plus 2 图像取帧原型，以及交给 Linux 开发环境的接续说明。

**用户硬性要求：严禁起飞，只允许获取 L3 数据。所有验证均为地面验证。**

## 从这里开始

1. 下一位 agent 先读 [AGENTS.md](AGENTS.md) 和 [HANDOFF.md](HANDOFF.md)。
2. Linux 环境和操作步骤见 [docs/LINUX_SETUP.md](docs/LINUX_SETUP.md)。
3. 已验证的 PC 接收器是 [pc_viewer/receiver.py](pc_viewer/receiver.py)。
4. 遥控器专用页面见 [android/L3ReadOnlyActivity.kt](android/L3ReadOnlyActivity.kt)；对固定 DJI 上游提交的完整修改见 [Android 补丁](android/patches/msdk-5.18-l3-read-only.patch)。

## 当前状态

| 项目 | 状态 |
|---|---|
| L3 可见光帧经遥控器、USB 传至 Windows 并显示 | 已实机验证，1920×1080，采样上限 5 fps |
| PC 获取可供 OpenCV 使用的 BGR NumPy 数组 | 已实现 |
| Linux 复用同一 APK、ADB reverse 和 Python 协议 | 已准备操作流程，尚未在 Linux 真机验证 |
| L3 真实 XYZ / intensity / RGB 点云 | 尚未接入 |
| MSDK MOP 直接连接 L3 | 仅发现可验证的接口路径，尚未成功取点 |
| 秒算3 PSDK → ROS 2 / RViz2 | 官方路线已调研，本项目尚未部署 |
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

