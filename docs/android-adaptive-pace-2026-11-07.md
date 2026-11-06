# Android 自适应限帧等待（2026-11-07）

基线已提交为 `10a69d7`，包含已验证的 versionCode 23、宽屏/手柄修复、
Vulkan 热路径与寄存器 TLS 优化。本轮只调整限帧等待；CPU 锁频、温控、
GPU 状态和 guest 握手语义均未改动。

Android 现在复用 Linux 已有的单调时钟绝对时间休眠和自适应自旋窗口：
从 300 μs 开始，休眠接近或超过窗口时扩大，正常运行时逐步缩短，最大
自适应窗口 1200 μs。逐毫秒的 vblank 服务和 guest 锁释放/重取继续保留。
`debug.jsrf.recomp_pace_spin_us=1200` 可在下次启动固定回原自旋预算，
清空该属性恢复自适应。Windows、macOS 和 iOS 行为保持不变。

## 验证与限制

- `test_android_pace.py` 提取实际 helper，生成 Android 可执行测试；自适应
  扩大/收缩、固定/零/上下界参数、绝对时间休眠及 EINTR 重试通过。
- 三轮独立等待模型均保持平均 60 Hz，但 CPU 和 p99 结果随设备负载波动；
  一轮自适应未省下 CPU。测试时设备前台还运行过另一游戏，不能据单轮结果
  宣称所有负载下都有收益，也不把模型 p99 当成实际游戏的帧间隔。
- NDK 29 完整原生库增量编译链接、APK 签名与 16 KiB ZIP 对齐检查通过。
  versionCode 24 已同签名覆盖安装，保留游戏数据和存档。
- JSRF 实际运行中采集到 748 个 SurfaceFlinger 实际 present 时间戳，覆盖
  12.450 秒：约 60.001 FPS，间隔 p95 16.670 ms、p99 16.671 ms、
  最大 16.673 ms，窗口内无超过 25 ms 的间隔。这是短时现场检查，不是长测。
- 同次 root CPU-clock 500 Hz 采样约 15 秒，11,437 个样本、0 丢样；
  渲染线程时钟读取占全进程 CPU self 样本约 1.78%。前次 versionCode 23
  的白天地图为 5.45%，场景不同，不能当作严格的同视角 A/B 或整机功耗结论。
- 用户反馈“目前感觉很不错”，要求到此停止优化并提交当前版本。

记录：`build/logs/20261107-adaptive-pace/`，包含三轮模型测试、编译/打包日志、
实际 present 时间戳、`present-summary.json` 和匹配符号的 `profile24/`。
自动按键和诊断属性已清空，Vulkan 后端保留；没有继续修改性能参数。

APK：`build/dist/0.6.6-cn.1-adaptive-pace/JSRF-Unleashed-android-arm64-CN.apk`

版本：本地测试 `0.6.6-cn.1` / versionCode 24。

SHA256：`97c6f81357b01272041c0edbddb3cfda8a6c50df7bfe92afd9c60bf0b3fed89d`。
