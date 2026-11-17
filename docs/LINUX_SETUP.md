# Linux 接续环境与验收

本流程是准备方案，尚未在 Linux 物理设备上执行验证。硬件限制先读仓库 `AGENTS.md`：全程地面，只获取 L3 数据，严禁启电机、起飞或调用飞控。

## A. 地面电脑：先复现 RGB，不急着安装 ROS 或 Android Studio

建议 Ubuntu 24.04 LTS Desktop / amd64。使用桌面图形会话显示 OpenCV 窗口；无显示器/SSH 环境先用 `--no-display` 验证。

先检查实际系统，不把以下 PC 安装命令用于秒算3：

```bash
cat /etc/os-release
uname -m
df -h
```

安装最小环境：

```bash
sudo apt update
sudo apt install -y git adb android-sdk-platform-tools-common \
  python3 python3-venv python3-pip libgl1 libglib2.0-0t64
sudo usermod -aG plugdev "$USER"
```

注销并重新登录，`id` 确认组权限。接遥控器的 USB 数据线，在遥控器上授权这台 Linux 电脑的 USB 调试；Windows 的授权不会自动迁移。

```bash
adb version
adb devices -l
adb -d get-state
```

验收必须显示 `device`。`unauthorized` 应在遥控器确认授权；`no permissions` 应检查 plugdev / udev 规则，必要时用 `lsusb` 读取本机实际 VID/PID 后增加精确规则，不猜设备 ID，不默认使用 sudo 运行 ADB。

获取交接包并建立 Linux 自己的 Python 环境：

```bash
git clone https://github.com/MrJQ-Huang/DJI_L3_develop.git
cd DJI_L3_develop
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -c 'import cv2,numpy; print(cv2.__version__, numpy.__version__)'
.venv/bin/python pc_viewer/test_protocol.py
```

不要复制 Windows Anaconda 或 `.venv`。GUI 使用 `opencv-python`；不要同时安装多个提供 `cv2` 的 headless/contrib 发行包。

遥控器原有 APK 可以继续使用；无需为更换电脑而重建或覆盖安装。核实专用入口存在并建立映射：

```bash
adb -d shell pm path com.dji.sampleV5.aircraft
adb -d reverse tcp:9000 tcp:9000
adb -d reverse --list
adb -d shell am start -n \
  com.dji.sampleV5.aircraft/dji.sampleV5.aircraft.L3ReadOnlyActivity
```

先做有界收帧验收（只保存一张本地测试图像，不提交仓库）：

```bash
.venv/bin/python pc_viewer/receiver.py --no-display --max-frames 30 \
  --save-first /tmp/l3-first.jpg --stats-json /tmp/l3-30frames.json
```

停止条件：若在合理等待后没有帧，Ctrl+C 停止并排查，不能让验证无限等待。成功条件：正常退出、frames_received≥30、protocol_errors=0；图像确认来自 L3 可见光，而不是屏幕截图或点云预览视频。

完成后启动实时窗口：

```bash
bash scripts/start_linux.sh
```

ESC、关闭窗口或 Ctrl+C 停止电脑接收。遥控器页面的按钮可暂停向电脑传输。只允许一份接收器占用 9000 端口。USB 重连或 ADB 重启后检查 reverse。多台 USB Android 设备时先明确目标，再将脚本中的 `-d` 改为本地 `-s SERIAL`，不要公开 SERIAL。

Linux 首次通过后记录：系统/架构、Python/OpenCV/NumPy/ADB版本、相机类型/端口/源、图像尺寸、连续观测帧数/时长/实际fps、协议错误、重连结果。不要将初次启动等待算入稳定帧率；不要把协议错误为0说成无线零丢包。

## B. 仅在要修改遥控器 App 时准备 Android 构建

所需版本：JDK17、Gradle Wrapper8.12、AGP8.7.0、Kotlin2.1.0、MSDK5.18.0、Android compile/target35、min24。无需先装模拟器，目标是实际 RC Plus 2。

```bash
sudo apt install -y openjdk-17-jdk unzip
export JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
export ANDROID_HOME="$HOME/Android/Sdk"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
java -version
```

如果机器同时装了多个 JDK，确认上述 javac 实际是17；否则明确选择已安装的JDK17路径，不盲目覆盖系统默认 Java。

通过 Android Studio SDK Manager 或官方 Linux Command-line Tools 安装工具；命令行工具必须先正确解压到 `$ANDROID_HOME/cmdline-tools/latest`，再运行：

```bash
sdkmanager "platform-tools" "platforms;android-35" "build-tools;34.0.0"
sdkmanager --licenses
```

Android SDK工具的下载从[官方页面](https://developer.android.com/studio#command-line-tools-only)选择当前Linux包；不要使用本次Windows工具路径或复制Windows SDK。无需仅因上游的NDK_VERSION属性提前下载旧NDK，确有构建任务需求再补。

取得与本次补丁完全匹配的上游提交：

```bash
HANDOFF_REPO="$PWD"
mkdir -p vendor/dji-msdk
git -C vendor/dji-msdk init
git -C vendor/dji-msdk remote add origin https://github.com/dji-sdk/Mobile-SDK-Android-V5.git
git -C vendor/dji-msdk fetch --depth 1 origin 91bf2d957e74635932ae639bc9bc9ac70eb200f4
git -C vendor/dji-msdk checkout --detach FETCH_HEAD
git -C vendor/dji-msdk apply --check "$HANDOFF_REPO/android/patches/msdk-5.18-l3-read-only.patch"
git -C vendor/dji-msdk apply "$HANDOFF_REPO/android/patches/msdk-5.18-l3-read-only.patch"
```

这是首次初始化步骤；目录若已存在，先检查状态与远端，不能反复重置或覆盖已有修改。补丁已应用时不要再执行第二次apply；失败先查看差异，不能升级到新SDK后强行忽略所有冲突。

配置与构建：

```bash
cd vendor/dji-msdk/SampleCode-V5/android-sdk-v5-as
printf 'sdk.dir=%s\n' "$ANDROID_HOME" > local.properties
chmod +x gradlew
./gradlew --version
```

App Key 通过用户本地配置提供，对应包名必须为 `com.dji.sampleV5.aircraft`。在上游工程本机 `gradle.properties` 的 `AIRCRAFT_API_KEY` 项填真实值，保持该值不进入提交；不要另建一个不会自动加载的配置文件后误以为已生效。

不要把真实Key放进命令行参数、公开构建日志、提交或本仓库。不要把Windows的JDK/SDK/代理绝对路径复制到Linux。

```bash
./gradlew :sample:assembleDebug --console=plain
```

APK在兄弟模块路径：

```text
../android-sdk-v5-sample/build/outputs/apk/debug/sample-debug.apk
```

先确认产物包名、签名和专用launcher，再按实际设备覆盖安装；保持原测试签名兼容，不因 `INSTALL_FAILED_UPDATE_INCOMPATIBLE` 直接卸载用户现有App。

```bash
adb -d install -r ../android-sdk-v5-sample/build/outputs/apk/debug/sample-debug.apk
adb -d shell am start -n \
  com.dji.sampleV5.aircraft/dji.sampleV5.aircraft.L3ReadOnlyActivity
```

本次已授权的遥控器覆盖安装保留了权限；新设备或全新安装需要检查SDK所需权限与联网注册，不能把Windows注册成功当成另一设备也已注册。

## C. 当前二维视频预览（不使用RViz）

最新用户要求：二维点云画面与可见光都按普通视频显示，使用妙算3/PSDK，不用MSDK，不再为本预览启动ROS/RViz。原始H.264直通，不调整源分辨率/帧率/码率；本地窗口可按窗口大小缩放显示，网络收到的仍是原图。需要`.venv`中的PyAV/OpenCV及系统`python3-tk`；本机已具备。两端沿用已授权的私有Tailnet/SSH。

```bash
bash scripts/start_l3_preview_4g.sh \
  --host MANIFOLD_TAILNET_IP --seconds 300
```

替换私有地址，必要时传`--identity KEY_PATH`、`--ssh-host SSH_ALIAS`。当前机载部署默认`/open_app/workspaces/dji/l3-dev/repair-20261117`，可用`--remote-root`覆盖。只接收不显示加`--no-display`。如需核对本轮已确认的点云画面源，可加`--expected-source 14`；不匹配会拒绝启动，不会自动切源。预览最多300秒，停止按钮/关窗请求结束Liveview，SDK阻塞仍依赖机载外部超时。连接中断后等待旧SDK正常退出，再用剩余时间重新订阅启动IDR；坏帧只丢弃该帧。末帧停更超过3秒时命令以失败结束，不能用历史收到过画面当作实时成功。

窗口显示实际源、切换按钮、原始分辨率、接收Mbps和解码fps。**切换按钮已连接PSDK，但SDK本地功能检查返回`0x00000021000000e0`，尚未实现软件切换成功。** 私有诊断越过这一检查后，命令返回参数错误且源仍14，没有将未成功方案加入默认构建。当前源14的二维点云画面不能当作XYZ。修正库构建、SDK剩余泄漏、高清带宽和实测边界见[开发方案第10节](REMOTE_SYSTEM_DEVELOPMENT_PLAN.md#10-sdk越读退出与视频恢复修复2026-11-17)。旧`--rviz`和码率参数不再属于此启动入口。

### 历史ROS 2 / RViz2通路

以下桥接保留给此前XYZ目标；本轮普通预览不调用它们。

2026-11-17本机实际环境为Ubuntu22.04/ROS 2 Humble，已经安装，无需重装。新增`pc_viewer/l3_ros_bridge.py`直接构造Image/PointCloud2，复用本机`.venv`，不依赖`cv_bridge`；已通过ROS发布订阅和4G合成点流RViz验证，真实XYZ仍待SDK修复。默认仅点云启动：

```bash
bash scripts/start_l3_rviz.sh --host MANIFOLD_TAILNET_IP \
  --socks5 127.0.0.1:1055 --raw-coordinates --seconds 60
```

替换实际私有地址；`--raw-coordinates`明确表示源坐标诊断显示，不能视为米制空间验收。双流加`--streams both`，无界面加`--no-rviz`。脚本只启动本机接收与显示，不开启机载录制。以下Jazzy命令仅供新Ubuntu24.04环境参考，不用于当前本机。

Ubuntu24.04对应ROS2 Jazzy。按[官方Ubuntu安装说明](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html)配置当前ROS软件源与签名，再执行：

```bash
sudo apt install ros-jazzy-desktop ros-dev-tools \
  python3-numpy python3-opencv ros-jazzy-cv-bridge
source /opt/ros/jazzy/setup.bash
ros2 run rviz2 rviz2
```

不要在尚无ROS源时盲目安装；不要将安装成功或合成ROS消息通过等同于已取得真实DJI点云。

桥接发布图像 `sensor_msgs/msg/Image`、点云 `sensor_msgs/msg/PointCloud2`。必须标明坐标轴/单位、frame_id、点字段和时间来源；无标定和硬件同步时不能假装RGB/点云已经对齐。

ROS2预编译Python包需要匹配系统解释器。当前使用系统Python3.10创建的`.venv`，source Humble后加载rclpy/sensor_msgs，直接序列化消息；不要用conda解释器替换，也不要为本桥接额外安装可能与NumPy ABI不匹配的cv_bridge。

## D. 秒算3：单独管理机载环境

秒算3不是上面的amd64地面PC。保留厂商系统、固件及JetPack；官方文档列Kernel5.10 / JetPack5.1.3并说明已预装PSDK开发环境，不要为Jazzy重刷Ubuntu24.04。

接入后首先只读记录：

```bash
cat /etc/os-release
uname -a
uname -m
df -h
```

再核对实际PSDK、JetPack和固件，选择与设备相匹配的编译/部署方法。不要预先照抄地面PC的包和架构。

电脑接M400调参USB口，通过虚拟网络访问装在E-Port上的秒算3。官方E1/E2/E3地址分别为192.168.42.120/.130/.140，仍应按实际接线与官方当前文档确认网卡、地址与路由，不直接覆盖Linux网络配置。秒算3自身USB-C不支持直接接电脑。

只部署经过安全审查的L3取数程序。先实现秒算3→电脑的有线接收；真XYZ经遥控器中继和MSDK直接订阅L3都是另需验证的项目，不能复用JPEG协议后就宣称点云接通。

## 官方来源

- [Ubuntu USB调试权限/udev](https://developer.android.com/studio/run/device)
- [Linux Platform-Tools](https://developer.android.com/tools/releases/platform-tools)
- [ADB reverse](https://developer.android.com/develop/ui/views/layout/webapps/access-local-server)
- [Python venv](https://docs.python.org/3/library/venv.html)
- [OpenCV Python包](https://pypi.org/project/opencv-python/)
- [AGP8.7兼容表](https://developer.android.com/build/releases/agp-8-7-0-release-notes)
- [ROS2 Jazzy Ubuntu安装](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html)
- [ROS2 Python解释器与依赖](https://docs.ros.org/en/jazzy/How-To-Guides/Using-Python-Packages.html)
- [秒算3开发环境](https://developer.dji.com/doc/payload-sdk-tutorial/en/manifold-quick-start/manifold-development.html)
- [秒算3连接方法](https://developer.dji.com/doc/payload-sdk-tutorial/en/manifold-quick-start/manifold-connect.html)
- [秒算3硬件](https://developer.dji.com/doc/payload-sdk-tutorial/en/manifold-quick-start/manifold-product.html)
