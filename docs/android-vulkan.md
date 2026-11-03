# Android 的 Vulkan 后端：现状、约束与方案

这份文档记录"把 GLES 换成 Vulkan"这件事在本项目里的实测结论、硬约束和已经定下的方案。
数字都来自 OnePlus 8（Adreno 650 / 骁龙 865）上跑的构建。

## 为什么考虑换

关卡内实测（37.5 fps，26.7 ms 一帧）：

| 项 | 占比 | 绝对值 |
|---|---|---|
| 全部 draw 提交 | 35.4% | 9.4 ms（443 draw × 21 µs） |
| 　其中驱动自己的 `glDraw` | 11.9% | 3.2 ms（7.2 µs/draw） |
| 状态派发 | ~18% | ~4.9 ms |
| present | 5.2% | 1.4 ms |
| 重编译的游戏代码 | ~32% | 8.6 ms |

在这台机器上试过、并且**实测无效**的优化（都已提交，保留为负结果）：

- 合并相邻 draw：563,873 次 draw 里，与上一条共用顶点缓冲的有 **0** 次，状态完全没变的 **0.16%**。
  这个游戏的每次 draw 都是新几何 + 新状态，相邻合并没有对象。
- 状态派发路由表（四个热块前置）：逐字路径耗时 525→552 ms/s，等于没变。瓶颈在访存不在分支。
- 已生效的小项：arm64 NEON 内容哈希（3.3%）、VAO 绑定缓存、`S` 之外的零碎。

结论：GLES 每个 draw 的固定成本是驱动的，榨不出来。hakuX 用 Vulkan 而不是因为它的 CPU 更快
（它是 FEX JIT，我们是静态重编译，CPU 侧我们更快）。

## 实测到的两条硬约束

### 1. 安卓上 SDL 只有一个窗口

试过"先开一个自己的 Vulkan 窗口、present 30 帧、销毁、再让 GL 建窗口"：App 崩在 GL 初始化后的第一帧
（`[CRASH] signal 11 ... fault addr=0x0`）。把窗口交还之后 GL 上下文没有 surface。

**所以 Vulkan 后端必须接管 `gl_init` 要建的那个窗口**：按 `RECOMP_BACKEND` 在建窗时选
`SDL_WINDOW_VULKAN` 而不是 `SDL_WINDOW_OPENGL`。它不能是"另开的窗口"，也不能是"和渲染器并行的探针"。

### 2. 原厂 Adreno 驱动停在 Vulkan 1.1

```
[VK] device: Adreno (TM) 650 (type 1, Vulkan 1.1.128, driver 0x801F5000)
[VK] cleared 640x480; first pixel 64 128 191 255, wanted 64 128 191 255
[VK] 0 of 307200 pixels differ from the clear colour
```

（`port/vk_probe.c`，`RECOMP_VK_PROBE=1`，走系统 loader：实例/设备/队列/图像/命令缓冲/屏障/清屏/回读全通。）

所以 dynamic rendering、synchronization2、push descriptor 这些 1.3 特性在原厂驱动上都没有。
同样的结论在 NGPort 项目里也有记录：Adreno 6xx 的原厂驱动缺这些，渲染器需要它们。

## Turnip：已验证可用（2026-11-03）

在 OnePlus 8（Adreno 650）上跑通，走 libadrenotools + TurnipZ 0.3.0
（`debug.jsrf.recomp_vk_driver_probe 1`，`port/vk_driver_probe.c`）：

```
[VKD] native dir /data/app/.../lib/arm64/
[VKD] temp dir   /data/user/0/com.jumatt.jsrfunleashed/files
[VKD] gipa(NULL): create 0x77747e4014, instance extensions 0x77747e5470, instance version 0x77747e6030
[VKD] device 0: Turnip Adreno (TM) 650 (Vulkan 1.3.363, driver 0x06863063)
[VKD] OK: Turnip loads and reports Vulkan 1.3 or newer
```

两条踩过的坑，写下来免得再犯：

- **`libmain.so` 不能硬依赖 `libvulkan.so`**。只要按名字调用 `vk*` 函数，链接就会写进
  `DT_NEEDED`，系统 loader 在 libadrenotools 打补丁之前就被加载，补丁失效——现象是
  `gipa(NULL, "vkCreateInstance")` 拿不到入口。所以 `port/vk_probe.c`（那个走系统 loader 的）
  现在只在定义 `NV2A_VK_SYSTEM_LOADER` 时才编译。
- **实例级函数要用 `gipa(instance, …)` 取**，`gipa(NULL, …)` 只给 `vkCreateInstance`、
  `vkEnumerateInstanceExtensionProperties` 这类全局入口。第一版探针在这上面误判过一次。

## Turnip：方案（NGPort 已验证的配方）

NGPort（`E:\HH\NGPort\zh-work`）已经把这套跑通了，可以照搬：

- **libadrenotools**（`github.com/bylaws/libadrenotools`，pin `8fae8ce`）：让**系统的 Vulkan loader**
  只为本 App 加载另一个驱动，不需要 root、不需要改系统设置；
- **Turnip**：Mesа 26.2.6 源码编译出的 `libvulkan_freedreno.so`，和 adrenotools 的 hook 一起打进
  APK 的 `jniLibs/arm64-v8a/`；
- 驱动选择：`NGB_GPU_DRIVER=system|turnip|package`，默认 Auto（先看系统驱动，缺特性才用 Turnip）；
- 入口：xboxrecomp 的 `instance.c` → `nv2a_vk_custom_loader`；App 侧见 NGPort `src/android_gpu.c`。

本机现状：`zh-work` 里只有 adrenotools 的 hook（`libmain_hook.so`、`libhook_impl.so`、
`liblinkernsbypass.a`），**没有 `libvulkan_freedreno.so`**（`build/android-deps` 是空的）；
手机上也没有。WSL 未安装，本地按 `packaging/android/build.sh` 起 Mesа 构建不可行（需要 meson/venv/bison/flex）。

所以要么从别处取一个 Turnip 的 `.so` 放进 `jniLibs`，要么先在原厂驱动（1.1，render pass）上把骨架做完。

## SPIR-V

我们的着色器是按状态在运行时拼出来的 GLSL 字符串；Vulkan 要 SPIR-V。两条路：

1. **把 glslang 静态链进引擎**（NGPort 的构建脚本同样编译了 glslang），运行时编译 + 缓存 blob；
2. 预编译管线变体（把状态空间收敛到有限集合）。

第 1 条与现有"运行时生成 + shader cache"的结构最贴合，代价是二进制增大和首次编译开销。

## 下一步

1. 窗口接管：`RECOMP_BACKEND=vulkan` → `SDL_WINDOW_VULKAN` + instance + surface + swapchain（默认仍走 GL）；
2. 一个 stub 的 `Nv2aGfx`（40 个函数）让引擎跑起来、每帧清屏 + present；
3. 管线与 SPIR-V（决定 glslang 还是预编译变体）；
4. 接 pushbuffer 的 draw；
5. Turnip 二进制到位后，把驱动选择和 1.3 特性（dynamic rendering 等）接上。