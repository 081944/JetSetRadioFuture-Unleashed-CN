# Vulkan 性能路线评估（2026-11-06）

> 以下保留最初基础链路阶段的评估和验证记录，文中的 stub / 未验证完整游戏等描述属于当时状态。当前已经实现游戏渲染；最新同机 GL / Vulkan root 采样、CPU 热点和 GPU 限制见 [root 性能采样报告](android-root-profile-2026-11-06.md)。目前没有实测帧率收益，优化优先级已收敛到提交同步、共用前端和 Vulkan 数据复制。

结论：值得继续做可运行游戏场景的最小 Vulkan 后端，并以同机同场景帧时间决定后续投入；现有证据不支持承诺稳定 60 fps。单纯替换 GL API 不足以复现 hakuX 的整套收益。

## 本地证据及其限制

`android-vulkan.md` 的历史 OnePlus 8 记录为 37.5 fps、26.7 ms/帧：整条 draw 路径 9.4 ms，其中 glDraw 调用区间 3.2 ms；状态派发约 4.9 ms；present 约 1.4 ms。这些范围有包含关系，不能把 glDraw 再加到整条 draw 上。

`src/nv2a_gl/nv2a_gl.c` 的细分计时使用 CLOCK_MONOTONIC，是调用区间的墙钟耗时，可能包含驱动等待，不能直接当作 CPU 采样占比。pushbuffer 外的剩余时间也不能严格归因于游戏逻辑。需要线程 CPU 时间、GPU timestamp 和等待时间一起验证关键路径。

从 26.7 ms 到 60 fps 的 16.7 ms，需要减少约 10 ms。即使把 3.2 ms 的 glDraw 区间全部消除，串行预算模型也只有约 42.6 fps；实际 Vulkan draw 仍有成本。节省 5/7/10 ms 分别对应约 46.1/50.8/59.9 fps。这些是敏感性计算，不是收益预测，异步 CPU/GPU 重叠会改变结果。

此前“静态重编译必然比 FEX JIT 更快”和“GLES 固定成本已无法优化”的结论都过强。没有同机同场景 CPU 采样证明前者；已失败的几个优化只排除了对应尝试，不能排除全部 GL 优化。

## hakuX 对照

检查的提交：`a7cdb5a975e31730d488c47f6f10440b16c1e6f4`（master，2026-10-31）。只读参考 checkout 在 `build/tmp/hakux-reference`，没有移入本项目源代码。

- `hw/xbox/hle/front/fe.c` 和 `vk/back.c`：前后端命令环、独立渲染线程、按 fence/swap 等边界提交；不是每个 draw 同步等待 GPU。
- `vk/draw.c`：缓存管线与录制状态，bindless 描述符、动态 uniform buffer 和上传 arena；减少逐 draw 的资源处理。
- `native/core/vm.c` 和 `vk/guest_ram.c`：通过 Android Hardware Buffer 在条件满足时共享 guest RAM，顶点等可直接使用，另有复制回退。我们现在的 `Nv2aGfx.vertices` 接口接收已经展开的 float 顶点，单换后端不会自动消掉前端展开成本。
- `vk/batch.c`：相同状态下的 strip/fan 合并，注释中的主要案例是 Blinx。本地 JSRF 的相邻状态复用记录很低，不能把该案例收益套过来；仍需区分相同缓冲和相同渲染状态，前者不同不必然禁止 Vulkan 合并。
- `vk/pipeasync.c`、`vk/pcache.c`：异步构建和管线缓存，主要控制编译卡顿；不能直接等同于稳定场景平均帧率提升。

源码入口：

- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/hw/xbox/hle/vk/draw.c
- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/hw/xbox/hle/vk/back.c
- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/hw/xbox/hle/vk/guest_ram.c
- https://github.com/rfandango/hakuX/blob/a7cdb5a975e31730d488c47f6f10440b16c1e6f4/hw/xbox/hle/vk/batch.c

## 建议推进与验收

1. 固定 OnePlus 8、地图、镜头、分辨率、画质和热状态，暖缓存采样 GL 基线；区分 guest、pushbuffer、draw 录制、提交、等待、GPU 时间。hakuX 若作对照，也必须同设备同场景同画质；本次没有跑 hakuX 性能测试。
2. 复用现有前端和着色器生成器，实现一个正确的游戏场景：真实 render targets、纹理、uniform blocks/描述符、顶点、深度/模板/混合、draw 和 present。当前 stub 仍不画游戏。
3. 从首版就使用可复用上传缓冲、管线/状态缓存和少量批次提交。避免逐 draw 建 pipeline、分配内存、创建描述符或等待 GPU。需要同步时遵守 guest 可见性；不能任意删 fence/读回。
4. 建议以正确画面下关键 CPU 绘制路径至少减少约 30%，或端到端帧时间减少至少 20%，作为继续扩大 Vulkan 实现的收益门槛；这是项目决策标准，不是实测结果。
5. 若后端变快后前端派发/顶点转换成为主因，再评估原始顶点路径、dirty tracking、命令快照和前后端并行。不要一开始就搬入整个模拟器核心。

## 本次基础链路修复与实测

修复了缺少 vkEndCommandBuffer、重复录制前未 reset pool、动态 viewport/scissor 未声明、等待阶段不覆盖 color attachment、局部 wait-stage 指针生命周期、present 信号量复用、队列锁错误路径与 present 并发、swapchain format 硬编码、image layout 未记录、退出线程/资源释放问题。

同时修复 SPIR-V 按指针大小复制造成的越界读取，以及内存类型筛选误把 memoryTypeBits 当 propertyFlags、读回 pool/command/fence 从未建立、初始读回图像未初始化的问题。

OnePlus 8 / Adreno 650 / Turnip 1.3.363：

- 原 APK 复现：Vulkan 黑屏，仅触摸控件可见，循环计数无法证明画面正确。
- 修改后 Android arm64 编译、链接、APK 签名和 zipalign 检查通过，已安装。
- 最终 APK 的三角形路径和纯清屏路径分别记录至少 900 帧 present 成功；截图分别为预期橙色和深蓝色。
- `debug.jsrf.recomp_vk_spirv_test=1`：顶点 275 words、片元 90 words，成功。
- 新增 `debug.jsrf.recomp_vk_readback_test=1`：16×16 target、非零偏移 3×2 子区域、RGBA/BGRA 两种通道顺序、四次重复提交通过，像素为预期清屏值。
- 同一 APK 切回 GL，确认显示汉化开场画面。测试结束恢复 `recomp_backend=gl`，清除本次测试开关并停止 App。

未验证：完整 Vulkan 游戏画面与性能、设备丢失后的恢复、swapchain 尺寸变化自动重建。当前发生不可恢复的出帧错误会停止重用资源并记录需要重启，不会误入无 GL context 的路径。不要把测试三角形的 60 fps 当作游戏性能数据。

本地产物：`build/dist/vulkan-fix-20261106/JSRF-Unleashed-android-arm64-CN.apk`。原版本发布目录未覆盖，修改尚未提交。
