# OnePlus 8：GL / Vulkan root 性能采样（2026-11-06）

> 本文是改动前的诊断记录。随后已完成阻塞等待、uniform 去重和上屏边界修复；实测结果、原始数据入口与限制见 [第一轮优化验证](android-vulkan-optimization-2026-11-06.md)。

当前主要证据指向 CPU 前端和提交同步，不能把低帧率主要归因于 Vulkan GPU 执行，也没有证据证明当前移植已带来帧率提升。本次只做诊断，没有更改渲染或同步实现。

## 条件和方法

- OnePlus 8 IN2010，Snapdragon 865 / Adreno 650；Magisk `su -c id` 验证 uid=0。
- 同一已安装 APK，切换 GL / Vulkan；本地带符号 `libmain.so` 与 APK 对应，采样期间未重编译。
- Shibuya Terminal `0x9006E`，相同起点、固定镜头、2 倍内部渲染分辨率（1280×960）；NPC、车辆和动画仍在运动，因此不是逐帧相同工作量。
- 关闭 `debug.jsrf.recomp_gl_profile` 后重启，消除细粒度墙钟计时；关闭开关必须设为空，字符串 `0` 仍会启用基于存在性的开关。保留性能浮窗。
- root simpleperf：`cpu-clock`，1000 Hz，DWARF 8192 字节调用栈，各 15 秒。Vulkan 28,337 样本，GL 27,909 样本，均 0 丢样。
- 同时以约 1 秒间隔读取 KGSL `gpubusy` 和 `devfreq/cur_freq`，各 15 次；未锁 CPU/GPU 频率或修改热控。
- 电池温度参考：Vulkan 前约 39.2°C，GL 后约 39.3°C。不是所有芯片热传感器的温度，不能排除瞬时调频影响。

原始资料保存在被 git 忽略的 `build/profiles/20261106-vulkan/`：

- `vulkan-clean.data` / `gl-clean.data`：原始 CPU 采样。
- `vulkan-clean-hotspots.txt` / `gl-clean-hotspots.txt`：全进程 self 占比。
- `vulkan-clean-guest.txt` / `vulkan-clean-render.txt`：按线程的 self / children。
- `vulkan-clean-copy-callers.txt`：复制和时钟读取的调用者。
- `vulkan-clean-gpu.txt` / `gl-clean-gpu.txt`：GPU busy / total、频率原始记录。
- `vulkan-clean.png` / `gl-clean.png`：现场截图。
- `cpu-comparison.html`：simpleperf 交互报告，含两次采样的调用图。
- `symbols/` / `binary_cache/`：本次采样匹配的符号。后续重编译后不得直接替换旧采样的符号。

## 主要 CPU 热点

以下百分比是全进程所有线程的 CPU 样本占比，不是帧时间比例，也不是单线程占比。Self 不重复计算子调用；children 和 self 不能相加。

| 函数 / 类别 | Vulkan self | GL self | 含义 |
| --- | ---: | ---: | --- |
| `sub_001912A0_gen` | 22.09% | 22.38% | D3D KickOff 中的 flush 位等待 |
| `tlsdesc_resolver_dynamic`（guest 线程） | 4.43% | 4.84% | 动态线程局部存储访问；部分与等待循环周围的重编译寄存器访问相关 |
| `xbox_GuestCpuPreempt` | 3.62% | 3.78% | 重编译循环回边的抢占 / 中断检查；不能直接删除 |
| `nv2a_gl_method` | 3.13% | 2.87% | 共用 NV2A 状态派发 |
| `nv2a_gl_method_run` | 2.69% | 2.39% | 共用批量状态派发 |
| `nv2a_hash_wide` | 3.27% | 2.95% | 缓存内容哈希 |
| `tex_sample` | 2.28% | 1.69% | 纹理缓存源数据抽样校验，并非 CPU 软件光栅化 |
| `__memcpy`（渲染线程） | 3.41% | 0.51% | Vulkan 路径存在额外复制，需继续按数据种类定量 |
| `nv2a_pb_scan` | 1.52% | 1.18% | 命令缓冲遍历自身，不含完整子调用 |
| `vg_pipeline` | 0.49% | — | 当前管线查找自身，优先级低于上述大头 |

Vulkan guest 主线程共 13,330 样本（约 13.33 CPU 秒 / 15 秒）；`sub_001912A0_gen` 占该线程 self 的 46.96%，含子调用 59.33%。渲染线程约 10.17 CPU 秒 / 15 秒。全进程约消耗 1.89 个 CPU 核的时间，GL 约 1.86；采样器也有扰动，不能把小差异当成明确回归。

### 提交等待已定位到具体指令

`sub_001912A0_gen` 的 6,260 个样本中，约 96% 落在四个地址：`0x2932f90 / 0x2932fa0 / 0x2932fa8 / 0x2932fac`。匹配符号和反汇编显示，它们位于读取并检测 `register_base + 0x100410` 的 bit 16 循环，guest 回边 `0x001912EA`。不是游戏角色、物理或 AI 算法。

源码位置为 `port/recomp/gen/recomp_0008.c:19810` 附近。手工包装在 `port/recomp_manual.c` 的 `sub_001912A0`，通过 `xbox_nv2a_hurry()` 唤醒 ack 线程，仍运行原始轮询。ack 清位和 pushbuffer 执行共用 `src/kernel/xbox_memory_layout.c` 的线程；该线程执行绘制时不能同时回到开头清下一个 flush 请求。

这是 CPU 用量的大头，但“去掉自旋就能增加相同比例帧率”不成立：若只是把等待改成睡眠，只能降低 CPU 和功耗，未必缩短关键路径。后续必须分别测量 flush 请求到应答、pushbuffer 执行和真正 GPU 完成的时间；只对不要求 GPU 完成的 flush 应答解耦，保留 DMA、软件方法和 GPU fence 的正确性。

### 共用前端仍然昂贵

Vulkan 渲染线程的 `vc_prepare` 含子调用约 12.01%，`upload_texture` 约 7.43%，`nv2a_gl_method` 约 12.86%，`nv2a_gl_method_run` self 约 7.50%。范围相互包含，不可累加。这些路径使用 `nv2a_gl` 名称，但也服务 Vulkan；换 API 后仍会执行。

优化候选是可靠的写入 / 脏状态跟踪、减少重复状态派发和内容检查。不能直接关闭缓存校验：游戏会在同一地址改写纹理，错误缓存会造成画面不更新。

### Vulkan 复制是明确候选，尚未细分完

`__memcpy` 的可展开调用链包含 `vg_draw_impl`、其内部驱动调用以及 `vg_uniform_f`；一部分栈展开不完整，不能把全部 memcpy 归给 uniform。源码中每个变化后的 uniform 快照复制约 5712 字节，另外还有常量属性、索引、流式顶点和驱动内部数据。

后续先计数每类字节和复制次数，再处理重复 uniform / 属性上传与索引重传；用相同场景验证 CPU 时间和画面。避免在尚未测量前继续扩大 bindless 或管线改造。

### 计时本身有明显开销

第一轮启用细粒度计时，`clock_gettime` 含子调用占 Vulkan 渲染线程约 19.50%；关闭后降至约 7.58%。剩余读取主要来自 ack / vblank 的 `GetTickCount` 和 `GetTickCount64`，少部分来自帧节奏。两个占比来自不同运行，不能据此精确推算节省的毫秒数。

后续日常性能比较应关闭逐 draw 计时，使用采样和少量帧级计数。不要把带大量计时的浮窗 CPU 数字当成发布性能。

## GPU 证据和限制

| 项目 | Vulkan | GL |
| --- | ---: | ---: |
| KGSL GPU busy 平均 | 31.57% | 37.14% |
| GPU busy 范围 | 29.99%～34.23% | 35.51%～40.66% |
| 15 次频率读数 | 均 305 MHz | 均 305 MHz |
| 现场浮窗 GPU | 7.08 ms | 7.94 ms |
| 现场浮窗 FPS / 帧间隔 | 34.3 / 29.19 ms | 34.7 / 28.80 ms |
| 现场浮窗 CPU frame | 19.73 ms | 13.43 ms |

截图是瞬时数据，不是采样区间平均帧率或稳定收益。两边没有明显帧率提升，CPU 前端与同步优先级高。驱动库所在 CPU 样本：Vulkan 渲染线程 Turnip 库约 3.06% 全进程，GL Adreno GLES 库约 10.37%；驱动 CPU 成本减少，仍未转化为帧率收益。

浮窗 `CPU frame` 实际只取调用 `perf_frame_tick()` 的当前线程 CPU 时间差，不是全进程各线程的 CPU 总和。GPU 是渲染查询区间，不包括完整的独立上屏 / 系统合成；不能把 CPU + GPU 直接加成一帧，也不能用 CPU 样本统计推断 GPU shader 成本。

本次尚未得到 GPU 各 render pass、纹理采样、片元、深度、拷贝的耗时细分。现有总量和 GPU 利用率支持先查 CPU 供给与同步，不足以宣布 GPU 内部没有优化空间。下一轮 GPU 分析应加入低开销 pass / copy / present timestamp，并核对 KGSL 提交到执行时间；不能直接声称是 LRZ、深度格式或 barrier 导致。

截图还显示 Vulkan 最终上屏的左右边界 / 裁切与 GL 有差异。内部渲染分辨率相同，但最终合成正确性仍需单独修复并复验，不能声称画面完全等价。

## 后续顺序和验收

1. 测清并解耦不必要的 KickOff flush 等待。验收分别记录占核、应答延迟、帧间隔；降低自旋不等于帧率提升。保留同步语义并覆盖启动、进图和连续游玩。
2. 细分并压缩 Vulkan 逐 draw 复制，尤其变化后的整块 uniform 快照和重复索引上传。
3. 优化共用命令派发 / 缓存校验；依据脏状态消除重复工作，保留动态纹理正确性。
4. 补 GPU pass / copy / present 时间分解，再决定深度、带宽、渲染目标切换等 GPU 优化。持续使用相同内部尺寸与正确上屏画面比较。

采样结束已停止 App，后端设置恢复 Vulkan，细粒度计时仍关闭。没有通过 root 锁频、改温控、改系统性能参数。本次没有重建 APK，也没有声称已修复低帧率。
