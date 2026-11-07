# JSRF Unleashed CN

基于 [JuMatt/JetSetRadioFuture-Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed) 的中文 Android 分支，由 081944 维护。本项目保留上游代码、许可证及第三方署名，提供中文资源适配、Vulkan 渲染、双屏地图及手柄设置。

## 下载与使用

从 [本分支 Releases](https://github.com/081944/JetSetRadioFuture-Unleashed-CN/releases) 下载 `JSRF-Unleashed-android-arm64-CN.apk`。当前正式版需要 Android 10 或以上、ARM64，以及能够运行随包 Turnip 驱动的 Adreno GPU；副屏地图还使用 OpenGL ES。实机验证设备为 AYN Thor，不保证所有 GPU 兼容。

1. 安装 APK，把自己持有的受支持中文版 ISO 复制到设备。
2. 启动应用，选择中文版 ISO，等待资源提取完成。
3. 后续直接启动游戏，无需再提供美版镜像。更新 APK 可保留已提取资源和存档。

仓库及 Release 不提供游戏镜像、提取资源、字库、汉化补丁或镜像下载地址。

## 本分支功能

- 支持本次验证的日版汉化镜像；导入流程沿用上游的 ISO 选择和资源提取。
- 保留美版静态重编译布局，适配游戏语言、中文字库和 534 处内嵌文字。
- 齿轮设置中的“肩键/扳机互换”：同时交换 LB↔LT、RB↔RT。
- “ABXY 布局”：微软默认布局，或任天堂布局（A↔B、X↔Y）。
- 两项手柄设置仅影响实体手柄，切换立即生效，退出并重新打开后恢复。
- 正式包默认 Vulkan 渲染，无需 ADB 属性；修复远处建筑转动视角时的深度闪烁。
- 支持外接/内置副屏常驻地图，显示玩家位置、地图标记、地名和当前目标；触摸旋转限制在 35°，支持缩放和双击复位。
- 副屏右侧显示已解锁角色头像，直接读取游戏资源；两列、每列最多显示五个，超出可上下滑动。
- 任务横条有动态雪花与平滑走马灯，区域标题左对齐、左亮右淡；地图整体左移，为角色栏留出空间。
- 修复后台返回黑屏及偶发上方黑边。正式包关闭性能叠层和 Android 调试属性入口；开发包仍可使用诊断功能。

## 支持范围与验证

当前版本为 `v0.6.6-cn.1`（Android versionCode 41），以本分支已验证的 0.6.6 代码为基础，包含 Vulkan、CPU 热点和帧节奏优化，以及双屏地图、实时换人和生命周期修复。完整变化见 [发布说明](docs/release-notes/0.6.6-cn.1.md)。历史版本保留在 Releases。

本版主要在 AYN Thor 上验证，优化过程中没有调整用户锁定的 CPU 频率。用户反馈多数场景可达 60 帧，具体表现取决于场景、驱动及设备。

用户已确认中文、手柄、宽屏、双屏地图及远景闪烁修复。实时换人仍可能极少出现异常腾空，暂保留现状；地图未知任务文本可能回退为游戏英文。以上验证不等同于完整通关或所有设备兼容。

目前仅支持下列已测量的 `default.xbe`。SHA256 是版本识别信息，仓库不包含对应文件：

| 输入 | SHA256 |
|---|---|
| 原版美版（构建用） | `bb2410618c35ccab1ab8ad989194bbd50619eeb648a03b21483efca57f36547d` |
| 日版汉化（构建和设备使用） | `9ff97d993c826ad47f94d540bb0cbe1205f390eb35e6184c370b8cd8cdfb97e9` |

其它日版、其它汉化修订版、修改过可执行文件的镜像没有验证，不会自动套用这一兼容配置。

## 构建与维护

参见 [构建及底层检查](tools/cn_compat/README.md) 和 [上游同步说明](docs/CN_MAINTENANCE.md)。构建者需要自己提供上述两个输入；手机玩家只需中文版镜像。

本分支新增 Git 内容仅包含源码、地址/哈希元数据、测试及文档。镜像派生的兼容配置和重编译代码仅在本地生成，不提交到 Git。Release 单独发布签名 APK 和 SHA256 校验文件。

上游原有文件和历史随标准 fork 保留。新增发布检查针对我们引入的 Git 内容，不将其描述为对上游全部历史的版权审核。

## 致谢与许可

- [JuMatt/JetSetRadioFuture-Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed)：上游游戏移植项目。
- 原项目作者及第三方组件：见 [LICENSE](LICENSE)、[NOTICE](NOTICE)、[LICENSES](LICENSES)。
- 中文资源由用户持有的汉化镜像提供；本分支不主张其翻译、字体或游戏资源的所有权，也不分发这些独立资源文件。
