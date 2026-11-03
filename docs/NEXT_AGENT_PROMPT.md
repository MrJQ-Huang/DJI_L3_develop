# 给下一位 Linux agent 的启动提示

请接续本仓库 DJI M400 / L3 数据采集开发，先完整阅读 `AGENTS.md`、`HANDOFF.md`、`docs/LINUX_SETUP.md`。

用户硬性限制：严禁启电机和起飞，只允许获取 L3 数据；不得对频、运行飞控/航点/虚拟摇杆/云台动作或综合自动示例。

Windows 已实机跑通 L3 WIDE_CAMERA → RC Plus 2 上的 MSDK 5.18 App → USB ADB reverse → Python/OpenCV，1920×1080、采样上限5fps，可获得真实BGR数组。真实XYZ点云和Linux真机迁移尚未完成。

先只检查本机 Linux 发行版/架构、ADB、Python依赖与USB授权，运行离线协议测试；随后复用遥控器已有APK和本仓库接收器，完成Linux的RGB链路验收。不要先重建整个Android工程，也不要为此重刷秒算3。

Linux RGB通过后，按HANDOFF中的证据审查MSDK MOP的L3直连候选（已确认L3端口、49154、PAYLOAD、UNRELIABLE，只connect/read/disconnect），不要把POINT_CLOUD_CAMERA视频当成XYZ。当前没有直连成功证据，先做有界验证。涉及L3扫描/录制状态变化时明确说明数据采集前提并核对用户授权，不得隐式照跑官方示例。

如果MOP直取没有成功，转向秒算3PSDK机载取点，先通过M400调参USB虚拟网络做有线地面联调，再考虑遥控器中继。取得真实XYZ后才发布ROS2 PointCloud2并接RViz2；不要伪造坐标系、曝光时间或硬件同步。

保留App Key、身份、实拍画面和真实点云在本地，公开仓库只提交脱敏说明与代码。持续汇报“已验证事实、当前阻塞、下一项验收”，不要声称未做过的Linux/点云实验成功。
