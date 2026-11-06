# Vulkan 第一轮性能优化与验证（2026-11-06）

> 后续已补充同一 APK 的 GL / Vulkan 对照：该短测 GL 50.02 FPS，Vulkan 41.63 FPS，尚未证明 Vulkan 优于 GL。本文三组结果只描述 Vulkan 内部改前/改后的变化。见 [最新后端对照及 hakuX 架构分析](vulkan-vs-gl-and-hakux-2026-11-06.md)。

已实现并安装两项优化：将 KickOff 自旋改为保持原应答顺序的阻塞等待，减少重复 uniform 更新和快照上传。CPU 自旋热点已消失，短时同场景对照有帧时间收益；尚未达到稳定 60 FPS。

## 改动

### KickOff 阻塞等待

`port/recomp_manual.c` 保持 Xbox 函数的寄存器、栈、PUT 记账、可选 idle wait 和软件设备回退。普通硬件设备路径通过 `xbox_nv2a_flush_wait()` 请求 flush，释放 guest 锁后等待条件变量，收到应答后重新取得 guest 锁。

`src/kernel/xbox_memory_layout.c` 的 ack 线程仍在原来的位置清除 flush 位，在互斥锁保护下通知等待者。请求位、检查位和清位使用原子操作；条件变量检查放在循环内，避免提前通知或伪唤醒。未提前移动 DMA_GET、软件方法或真正的 GPU fence。

`debug.jsrf.recomp_nv2a_flush_wait=0` 可切回原始生成函数的自旋，空值默认启用阻塞等待。其他寄存器映射和软件设备分支仍回退原始函数。

最初尝试在生产线程内立即完成 coherent flush，主机寄存器/内存对照虽通过，设备却在加载阶段卡在 `sub_00191440` 的空间等待。该尝试已撤除，没有交付。它表明现有命令 walker 依赖原握手的背压；后续真正解耦生产与消费必须先修正队列消费和可见性模型，不能只清忙位。

### Vulkan uniform 复制与失效

- 连续布局的 vec4 数组使用整段复制，替代逐个 vec4 的小 memcpy。
- 按原始字节比较更新，数据相同则不写入、不增加 program revision，从而复用同 slot 中已有的 GPU uniform 快照。
- 部分常量更新和 int 更新同样只在字节变化时增加 revision。
- 非连续 std140 类型保留 stride 和 mat2 的列间填充。按位比较可正确区分 NaN payload、正负零，不以浮点近似相等判断。
- 不覆盖已经提交 draw 引用的 GPU 快照，不更改资源退休/fence 顺序。

`debug.jsrf.recomp_vk_uniform_dedup=0` 关闭去重，空值默认启用。连续数组的批量复制在两种模式均启用，因此下面的对照没有单独量出批量复制相对最早版本的收益。

新增低开销诊断：`debug.jsrf.recomp_frame_stats=1` 每约 2 秒汇总一次既有帧间隔、渲染线程 CPU 时间与 GPU 查询结果，没有增加逐 draw 时钟读取；uniform 计数随既有 10 秒报告输出。

### 上屏矩形

缩小后的 oversized fullscreen triangle 并不能构成有明确边界的 4:3 矩形，会露出斜边并采样超出 1 的 UV，产生右侧边缘拉伸。最终 presenter 改用两个三角形的矩形，再进行宽高比缩放和预旋转。最终设备截图显示左右黑边正常，原有斜边和右侧拉伸消失。内部渲染分辨率保持 1280×960。

## 同一 APK 三组对照

OnePlus 8 / Adreno 650 / Vulkan，同一自动启动脚本、Shibuya Terminal 起点、固定镜头、1280×960 内部渲染。每组重启并预热 60 秒，然后 root simpleperf 采样 15 秒（1000 Hz，DWARF），均无丢样。帧指标取各组末尾 8 个帧级汇总窗口，加权计算约 16 秒的平均；与 CPU 采样时间不完全重合。

该对照使用修正上屏矩形之前的同一个 APK，因此两项 CPU 优化的对照未混入最终上屏修复。场景动画、车辆、NPC、调度和频率没有锁定，不能视为确定性基准。电池温度依次 35.1°C、36.2°C、36.2°C。

| 指标 | 自旋 / 不去重 | 阻塞 / 不去重 | 阻塞 / 去重 |
| --- | ---: | ---: | ---: |
| CPU 样本数 / 15 秒 | 28,453 | 22,763 | 23,434 |
| 全进程 CPU 核时间折算 | 1.897 核 | 1.518 核 | 1.562 核 |
| 统计窗口 FPS | 36.48 | 43.19 | 51.50 |
| 平均帧间隔 | 27.41 ms | 23.15 ms | 19.42 ms |
| 渲染线程 CPU / 帧 | 17.27 ms | 12.43 ms | 10.41 ms |
| GPU 查询区间 / 帧 | 7.41 ms | 7.48 ms | 7.24 ms |
| 最近 10 秒窗口 draws / 帧 | 613.2 | 610.1 | 611.7 |
| uniform 上传 / 帧 | 3,502,736 B | 3,484,668 B | 1,735,731 B |
| uniform snapshots / draw | 1.000 | 1.000 | 0.497 |

去重组约 94.9% 的 setter 调用内容未变，uniform 快照上传/帧约减少一半。全进程 CPU 样本下降不能单独解释 FPS，但与热点消失、复制减少和每帧绘制量相近共同支持这轮优化有效。

重要限制：51.50 FPS 是这轮短窗口的结果，不能宣称稳定 50 或 60 FPS。最终 APK 重启复核后，在同一起点也记录到约 39～46 FPS 的区间，另有 50～60 FPS 的区间；最终截图为 41.9 FPS。应继续重复测量、测试移动镜头/其他地图，并补充 CPU 频率与 GPU 分阶段记录。浮窗 CPU 仍仅是渲染线程，不是全进程 CPU。

原始资料位于 `build/profiles/20261106-vulkan-ab/`：三组 `.data`、日志、截图、温度/频率快照、匹配符号、`summary.json`、`optimization-comparison.html`。最终 APK 复核资料位于 `build/profiles/20261106-vulkan-final/`。不要使用最后一次构建的符号去解析前一 APK 的数据；各目录已保留对应 `libmain.so`。

## 已运行验证

- `python tools/cn_compat/test_kickoff_flush.py`：实际生成函数与手写包装函数的 8 种 flags / idle 分支比较，两种开关模式均通过；寄存器、栈、fixture 内存和 idle 调用次数一致。
- 同一脚本对实际条件变量等待/应答函数执行 20,000 次跨线程测试，通过；没有丢唤醒，guest 锁释放/重取成对。
- `python tools/cn_compat/test_vk_uniforms.py`：所有 uniform descriptor 的写入、std140 填充、NaN/负零字节、局部更新和 revision 失效检查通过。
- Android arm64 编译、链接和 APK 打包、签名、zipalign 检查通过；最终 APK 安装成功。
- 最终 APK 设备像素测试：RGBA 64/128/191/255；CCW 可见、CW 剔除；静态/流式顶点和 16/32 位索引，均得到预期像素。
- 多次启动、进入地图、累计百万级 draw 和持续出帧已验证。最终上屏 4:3 边界通过截图检查。
- `git diff --check` 没有空白错误；有既存文件的 LF/CRLF 提示。编译仍有现有 warning，不声称零 warning。

## 已知未完成项

验证中复现 Android `surfaceDestroyed` / `surfaceCreated` 后旧 swapchain 的 acquire 返回 `VK_ERROR_SURFACE_LOST_KHR`。目前窗口失败路径停止出帧，需要重启 App 才恢复；自动重建 surface/swapchain 尚未实现。重启后已恢复游戏画面。没有把该现有生命周期缺口掩盖为性能问题，也没有为绕过失败继续使用无效 Vulkan 资源。

共用前端的命令派发、顶点/纹理哈希仍是热点；动态 TLS 访问也明显。GPU pass / copy / present 分阶段计时、长时间游玩和全地图回归未完成。本轮只交付前两项性能优化及上屏边界修复，不声称整个 Vulkan 后端已完成。

最终产物：`build/dist/vulkan-profile-opt-20261106/JSRF-Unleashed-android-arm64-CN.apk`，SHA256 `06649e2a146cbcd92b1820afc1bf6403c44a311fe2c6cdd7ce2d88535deb6014`。已安装手机；修改尚未提交。
