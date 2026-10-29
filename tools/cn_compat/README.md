# 日版汉化资源适配：本地构建与检查

目标：手机只提供 APK 和现有 `JSRF-CN.iso`；沿用安装、选 ISO、提取资源、启动的流程。

用户已在 AYN Thor 上确认游戏运行正常且显示中文，并已进行新增手柄设置的测试；合并上游 0.5.10 后也已确认正常运行。底层检查和单台设备反馈不等于完整通关或所有设备兼容验证。

发布构建输出 `build-cn-apk/JSRF-Unleashed-android-arm64-CN.apk`，支持 Android 10 及以上的 ARM64 设备。之前的测试包已通过签名、ZIP 完整性、16 KiB ZIP 对齐、安装信息和原生 JNI 入口检查。

手机只需复制这个 APK 和原有 `JSRF-CN.iso`。安装后选择该 ISO，沿用原应用的资源提取流程；不需要把美版镜像复制到手机，也不需要修改中文版 ISO。

## 已实现

- 电脑端从用户的两份 XBE（或 ISO）生成兼容数据。只接受已测量的两份 SHA256，原始 ISO 只读。
- 保留美版代码/全局数据地址布局，游戏资源语言 `0x1FC724` 设为 0。
- 移植汉化字表、字体页参数、`.cnfont` 映射和五处字体补丁。
- 按经过审核的地址白名单移植 534 处内嵌文字，保留原始自定义编码和美版字符串地址，不覆盖日版指针表。
- 中文字节按移植字库的实际映射校验；不能用标准 CP932 解码筛选，否则会漏掉存读档提示等 121 处已有汉化。
- 另有四处涂鸦名称的汉化字节存在，但日版表不再引用其地址；为美版仍使用的条目保留四个明确审核的引用例外，仍检查槽位边界、长度和字库映射。
- 适配只移植原镜像已有汉化。原镜像未翻译的记忆卡名称、空间不足提示等仍需另外翻译，不能据移植数量声称全部文本已汉化。
- 把两处资源管理器指针从日版地址修正为美版地址。
- 修正字体取用补丁跨函数传递比较标志的问题：在同一辅助函数完成比较和取用，便于静态转译。不是修改系统语言。
- 生成约 218 KiB 的转换字节及对应位置表。生成物包含游戏派生数据，只放在 git 忽略目录。
- 实验引擎加载中文版 XBE 后，在内存中转换并校验，不修改提取出的文件。
- 安卓原生 ISO/文件夹预检查自动使用对应版本识别，不需要改 Java 文件选择/提取流程。
- CMake 校验兼容数据与生成代码的版本标记；普通构建不启用中文模式。
- 安卓齿轮设置中加入实体手柄 LB↔LT / RB↔RT 互换，以及微软/任天堂 ABXY 布局。立即生效，使用应用私有 SharedPreferences 保存并在下次启动恢复。

## 文件

- `prepare.py`：构造局部移植后的重编译输入和内存转换表。
- `generate.py`：执行 XBE 解析、反汇编、函数识别、ABI 分析和 C 生成；不生成 APK。
- `build_native.ps1`：通过 PowerShell 进行 Android ARM64 原生库编译；不打包 APK。
- `build_apk.ps1`：把匹配当前兼容数据的原生库打成签名测试 APK，直接调用 SDK 的 Java 入口，避免执行 `.bat`。测试签名保存在本地输出目录，供后续更新使用。
- `test_local.py`：使用 Unicorn 执行实际 x86 补丁，检查映射、绘制选择、资源生命周期和栈。
- `verify_codegen.py`：把字体查找/取用实际转译成 C，使用运行时头文件和 GCC 编译执行，再对比原汉化 x86。
- `test_loader.c`：测试真实 ISO 预检查、完整内存转换、拒绝损坏/不匹配输入及关闭功能后的行为。
- `test_extract.c`：调用安卓 JNI 共用的原生 ISO 提取器，测试完整资源提取。
- `port/cn_compat.c`：运行时转换器；没有编译本地 profile 时不启用。

## 生成与验证

从仓库根目录，在 PowerShell 7 执行。需要 Python、capstone、Unicorn、GCC；原生编译还需要完整 Android NDK、CMake、Ninja、SDL2 源码。

```powershell
$ErrorActionPreference = 'Stop'
python tools/cn_compat/prepare.py --us '../Jet Set Radio Future.iso' --cn '../JSRF-CN.iso' --out game_files/cn_compat
if ($LASTEXITCODE -ne 0) { throw 'Preparation failed' }
# 两个 XBE 由构建者自行从自己的镜像提取，放在本地忽略目录：
$env:JSRF_US_XBE = "$PWD/game_files/input/us.xbe"
$env:JSRF_CN_XBE = "$PWD/game_files/input/cn.xbe"
python -m unittest tools.cn_compat.test_local -v
if ($LASTEXITCODE -ne 0) { throw 'x86 tests failed' }
python -m tools.cn_compat.verify_codegen --profile game_files/cn_compat
if ($LASTEXITCODE -ne 0) { throw 'Generated C tests failed' }
python tools/cn_compat/generate.py --profile game_files/cn_compat --gen port/recomp/gen
if ($LASTEXITCODE -ne 0) { throw 'Code generation failed' }
./tools/cn_compat/build_native.ps1
# 已明确要求生成 APK 时再执行：
./tools/cn_compat/build_apk.ps1
```

`test_local.py` 及 `verify_codegen.py` 通过 `JSRF_US_XBE`、`JSRF_CN_XBE` 读取构建者提供的输入。未设置环境变量时保留早期工作区 `../analysis/us.xbe` 与 `../analysis/cn.xbe` 的兼容路径。这些文件不属于仓库源码；`prepare.py` 可直接读取 ISO。

原生构建必须同时设置：

```text
JSRF_GEN_DIR = 对应实验 XBE 生成的 recomp/gen
JSRF_CN_PROFILE_DIR = game_files/cn_compat
```

不要把这个 profile 与上游美版生成代码混合使用。生成的 `default.xbe` 是本地重编译输入，不是可在原 Xbox 上运行的发行版程序。

## 已通过的底层检查

- 字表 1,158 项与扩展映射 387 项，共 1,545 个不同编码，移植后的 x86 查找结果与原汉化版一致。
- 转译后的原生 C 检查全部 65,536 个输入、未命中返回值和栈清理；又直接抽取完整流水线生成的实际 C 函数，复核了字体取用和资源加载/释放。
- 字体取用索引 0–7、未就绪状态、新增资源加载/释放。
- 绘制路径对 387 个扩展字符选择正确的字体页。
- 实际中文版 ISO 的识别、原生内存转换与预期重编译输入逐字节一致。
- 原生完整提取 3,221 个文件，共 2,499,778,844 字节，逐个 SHA256 与原 ISO 内的文件一致。
- 534 处文字逐字节匹配原汉化字节；其余 `.rdata` 保持美版内容。
- 完整性错误/错误版本拒绝；关闭 profile 后恢复普通美版检查。
- 转换器通过 Android ARM64 编译器检查；SDL2 和完整 Android ARM64 引擎均已编译链接成功。
- 已核对 `build-cn-native/engine/libmain.so` 为 ELF64 / AArch64，并导出 `SDL_main`、安卓 ISO 检查与提取 JNI 接口。

完整生成统计：9,343 个函数，9 个由项目手写实现，9,334 个转译成功，0 个转译失败，约 115 万行 C。生成器还报告了 20 个未解析调用占位及一些未实现指令诊断；本次字体补丁的入口不在未解析调用占位中。编译通过不证明这些路径在实际运行中都安全，需要启动日志验证。

手柄映射的独立检查可用 `gcc -O2 -Isrc tools/cn_compat/test_controller_mapping.c -o game_files/cn_compat/test_controller_mapping.exe` 编译后执行，覆盖四种组合和全部 0–255 模拟压力值。它检查实际输入层调用的映射函数。

## 验证边界

仍需更多设备和长时间游戏测试，关注生成器诊断、切场景、存档读档、手柄设置重启恢复等路径。用户反馈已验证的内容与尚未完整覆盖的路径应分开记录。

此目录仅提交脚本、测试和地址元数据。运行 `prepare.py` 生成的配置包含镜像派生数据，必须保留在忽略目录中，不可提交 Git。发布与同步规则见 [维护说明](../../docs/CN_MAINTENANCE.md)。
