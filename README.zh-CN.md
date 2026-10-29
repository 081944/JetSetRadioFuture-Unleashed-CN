# JSRF Unleashed CN

基于 [JuMatt/JetSetRadioFuture-Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed) 的中文 Android 分支，由 081944 维护。本项目保留上游代码、许可证及第三方署名；新增工作是运行时适配与手柄设置，不包含自行制作的游戏汉化资源。

## 下载与使用

从 [本分支 Releases](https://github.com/081944/JetSetRadioFuture-Unleashed-CN/releases) 下载 `JSRF-Unleashed-android-arm64-CN.apk`。需要 Android 10 或以上、ARM64 和 OpenGL ES 3.0。

1. 安装 APK，把自己持有的受支持中文版 ISO 复制到设备。
2. 启动应用，选择中文版 ISO，等待资源提取完成。
3. 后续直接启动游戏，无需再提供美版镜像。更新 APK 可保留已提取资源和存档。

仓库及 Release 不提供游戏镜像、提取资源、字库、汉化补丁或镜像下载地址。

## 本分支功能

- 支持本次验证的日版汉化镜像；导入流程沿用上游的 ISO 选择和资源提取。
- 保留美版静态重编译布局，适配游戏语言、中文字库和 409 处内嵌文字。
- 齿轮设置中的“肩键/扳机互换”：同时交换 LB↔LT、RB↔RT。
- “ABXY 布局”：微软默认布局，或任天堂布局（A↔B、X↔Y）。
- 两项手柄设置仅影响实体手柄，切换立即生效，退出并重新打开后恢复。

## 支持范围与验证

首个版本 `v0.5.9-cn.1` 基于上游 `414b1037a4503a167c5440a258ae2ff904d408f5`（0.5.9）。用户已在 AYN Thor 上确认游戏正常运行且显示中文，也已进行手柄设置测试；这不等同于完整流程通关验证。

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
