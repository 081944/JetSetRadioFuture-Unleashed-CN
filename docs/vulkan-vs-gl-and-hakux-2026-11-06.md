# 当前 Vulkan / GL 对照及 hakuX 架构差异（2026-11-06）

结论：当前 Vulkan 版本尚未证明优于 GL。这次同机短测反而低于 GL，不能把先前 Vulkan 自身改前/改后的收益说成迁移路线已成功。共享的 KickOff 阻塞等待也能改善 GL，必须同时应用于基线。

## 最新同一 APK 后端对照

同一 OnePlus 8，涉谷车站起点、固定镜头、1280×960 内部渲染，最新 APK（包含上屏矩形修复）。两组都启用 KickOff 阻塞等待；Vulkan 启用 uniform 去重。每组重启，预热 60 秒，然后 root simpleperf 1000 Hz 采样 15 秒。细粒度逐 draw 计时关闭，保留每约 2 秒的帧级统计。

帧指标使用末尾 8 个汇总窗口，加权统计约 16 秒，和 CPU 采样区间并非严格对齐。

| 指标 | GL | Vulkan |
| --- | ---: | ---: |
| 统计帧数 / 区间 | 806 / 16.115 s | 670 / 16.094 s |
| 平均帧率 | 50.02 FPS | 41.63 FPS |
| 平均帧间隔 | 19.99 ms | 24.02 ms |
| 渲染线程 CPU / 帧 | 12.73 ms | 12.33 ms |
| GPU 查询时间 / 帧 | 7.68 ms | 7.42 ms |
| CPU 样本 / 15 秒 | 23,607 | 22,786 |
| 全进程 CPU 核时间折算 | 1.574 核 | 1.519 核 |

本窗口 Vulkan 帧率约低 16.8%，帧间隔约长 20.1%。两者渲染线程 CPU 和 GPU 时间接近，不能靠这两个数字相加解释整帧，更不能把 CPU 占用下降自动解释为更高吞吐。需要补充 guest / 命令消费 / Vulkan 提交及等待的时间线，定位多出的关键路径时间。

这是一次短时对照，未锁频、未固定 NPC/车辆状态，不能把 16.8% 当作所有场景的稳定回归幅度。已经足以否定“现有 Vulkan 已经明确优于 GL”的说法。两次截图均为同一起点，并确认游戏有正常出帧。

原始资料：`build/profiles/20261106-final-backends-awake/`，包含 `gl.data / vulkan.data`、日志、截图、设备快照、对应 `libmain.so` 和 `summary.json`。较早的 `20261106-final-backends/` 测试遇到设备 Dozing / 熄屏，GL 仅 188 个 CPU 样本且无有效帧统计，已废弃，不得纳入结果。不能据此判断息屏是自动发生还是人为操作。临时改变的屏幕超时已恢复原值 2147483647。

## “ARM 原生”为什么不能保证比 JIT 快

本项目是 x86 二进制静态重编译到 C，再由 NDK 编译为 ARM64。它不是拿到游戏原始源码后重写成 Android 游戏逻辑。生成代码仍包含 Xbox 寄存器状态、guest 栈、内存映射、间接调用和中断/调度检查。

本地具体证据：

- `port/recomp/gen/recomp_types.h` 用 `__thread` 的 `g_eax/g_ecx/g_edx/g_esp` 等保存寄存器，`MEM32` 等访问保留 volatile 和 MMIO 处理。
- `port/recomp/gen/recomp_0008.c` 中 D3D KickOff 的原始实现逐步保留 x86 状态转移和轮询。仅提前编译，并不会自动消除跨线程握手。
- 最新优化前后采样均能看到 `tlsdesc_resolver_dynamic`；此前去重组约占全进程 6.42% 的 self 样本。这证明 TLS 解析有实际成本，但不是寄存器模拟的全部成本，也不是每次访问必然调用解析器。
- 构建为 `RelWithDebInfo`、`-O2 -DNDEBUG`，不是因为忘记开编译优化；调试符号用于采样，不能直接等同于未优化代码。

hakuX 使用 FEX fork 将 x86 热代码 JIT 为 ARM64。编译后的代码同样是 ARM64 本机执行；JIT 不是每条指令反复解释。其 `native/run/xboxrun.c` 明确记录 x86 GPR 在 JIT 内的 ARM 静态寄存器映射，例如 eax→x4、ecx→x7，`native/cpu/cpu.cpp` 的信号上下文恢复也使用 SRA 映射。相比把寄存器状态通过 C 的 TLS 全局变量暴露给各个函数，这提供了减少状态读写的机制。

这些是已确认的实现差异。没有同机 hakuX 分项采样，不能由此宣布 FEX 的纯游戏逻辑整体一定更快，或承诺移植某项就能获得多少 FPS。

## 图形架构的差距比 API 名称更重要

核对的 hakuX 提交：`a7cdb5a975e31730d488c47f6f10440b16c1e6f4`。

| 方面 | 本项目目前 | hakuX 已有机制 |
| --- | --- | --- |
| 前端与后端 | 共用 `nv2a_gl` 前端，ack 线程中解析并调用后端；独立上屏不等于命令解码和后端完全解耦 | `front/fe.c` 的 16 MiB 命令环，`vk/back.c` 的独立消费/录制线程 |
| 状态更新 | 大量共用方法派发，Vulkan 新增 uniform/描述符/资源管理 | 按变化的状态块发送，后端缓存状态，动态 UBO、条件支持的 bindless |
| 源内存变化 | 顶点内容哈希、纹理抽样和分批内容校验仍有显著成本 | `native/hle/dirty.c` 通过写保护与首次写入处理跟踪脏页，并对 host 写入单独报告 |
| guest 数据访问 | 已保留顶点缓存，但仍需要检查、转换和上传一些数据 | `vk/guest_ram.c` 在 AHardwareBuffer 导入成功时共享 guest RAM，并有复制回退 |
| 完成与可见性 | 历史 ack/GET 镜像语义和绘制耦合，提前清 flush 的尝试已在设备上失败 | 显式队列、timeline、内存可见性和资源退休机制 |

因此，目前实现的是“为现有前端接上 Vulkan 后端”，没有移植 hakuX 的整套执行和数据管理架构。把共享前端的大部分成本保留下来，再增加一套资源管理，完全可能比成熟 GL 路径慢。hakuX 的 GPU/内核实现也有原生代码，不应把它描述为整台 Xbox 的所有工作都在 JIT 内完成。

参考源码：

- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/native/run/xboxrun.c
- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/native/hle/dirty.c
- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/hw/xbox/hle/front/fe.c
- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/hw/xbox/hle/vk/back.c
- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/hw/xbox/hle/vk/guest_ram.c

## 后续判断标准

以 GL 同机同场景作为性能基线，所有共享优化同时应用到 GL。当前 Vulkan 应按实验路径评估，尚无依据把它作为默认性能升级。

优先量清当前关键路径中的线程/队列等待，并优化共用前端的派发、内容校验和重编译状态访问。若继续以 hakuX 为参考，应规划命令队列、写入跟踪和 guest 数据访问的接口，而不是继续只在 GL 形状的后端接口内堆 Vulkan 调用。该架构工作量明显大于 API 替换，收益需要逐项验证。

本轮没有修改渲染/重编译实现或重新构建 APK；只扩展并实际运行后端对照脚本、核对源码、记录结果。结束时清除自动按键/跳图与帧级诊断属性，保留原 Vulkan 后端选择。
