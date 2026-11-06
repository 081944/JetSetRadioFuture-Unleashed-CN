# 按 root 函数采样优化寄存器 TLS 与索引包（2026-11-07）

用户确认先前管线查找等微优化未带来明显整场景改善，继续保持 CPU 锁频。
本轮先对安装的 versionCode 22 执行 root simpleperf 的 CPU/等待联合采样，
再针对主要热点改动；没有调整频率、性能模式或温控。

## 实际热点

15 秒窗口，500 Hz CPU-clock，另外记录调度离开 CPU 的调用栈，0 丢样。
必须将 on-CPU 与 off-CPU 分开统计；不能把所有睡眠线程的等待时间当成 CPU 热点。

| 线程 | 函数 | 线程 on-CPU self |
| --- | --- | ---: |
| 游戏线程 29437 | `tlsdesc_resolver_dynamic` | 16.62% |
| 渲染线程 29438 | `nv2a_gl_method` | 12.94% |
| 渲染线程 29438 | `nv2a_gl_method_run` | 6.28% |
| 渲染线程 29438 | `nv2a_hash_wide` | 5.37% |
| 渲染线程 29438 | `tex_sample` | 4.49% |

`pthread_cond_broadcast` 的含子调用占渲染线程 on-CPU 约 8.16%。游戏线程
在 `xbox_nv2a_flush_wait` 累计 off-CPU 约 3.87 秒；渲染线程在 ack 等待处
累计约 4.46 秒。这些等待与实际绘制、生产/消费握手相关，不能直接删除应答
或提前移动 DMA/flush 位。该类提前应答的既往设备失败记录仍然有效。

原始数据与匹配符号：`build/logs/20261107-hotpaths/profile22/`。
`analyze.py` 用 NDK ReportLib 分别选择 on-CPU/off-CPU，再按线程聚合 self、
inclusive 和调用栈中首个引擎函数。部分 vendor 子驱动符号不可用。

## 改动

### 通用寄存器共享一个动态 TLS 对象

Android AArch64 下将 eax/ecx/edx/esp/ebx/esi/edi/ebp 放入同一个 32 字节
`__thread RecompGpr`，生成代码和手写包装函数通过这个对象访问寄存器。
原有八个 ELF TLS 符号分别别名到对应字段，供原生内核桥、回调、上下文保存
等继续使用。每个线程仍有独立状态，浮点、标志寄存器等保持原布局。

构建目录生成 `recomp_gpr_types.h` 副本，在原寄存器声明之后、helper 函数
定义之前接入字段别名；不修改镜像派生的原始生成文件。声明 hook 不唯一时
拒绝构建。`JSRF_GROUPED_GPR_TLS=OFF` 可编译回原布局。

仍使用动态 ELF TLS，不要求 initial-exec TLS、预留静态 TLS 或 loader 改动。
参考：[Android ELF TLS](https://android.googlesource.com/platform/bionic/+/b48087f8054372437748f21f86a63e9c077d4ede/docs/elf-tls.md)。

### 索引包使用局部写游标

16 位索引批处理不再每写一个索引就更新 `S.idx_count`，在局部指针中连续
展开每个 dword 的两个索引，批次结束时一次写回计数。容量计算、奇数剩余
空间、溢出截断、状态变化拆分和诊断回退保持原语义。

## 已完成验证

- `test_gpr_tls.ps1`：设备上通过 dlopen 加载实际 TLS 定义，八个旧符号的
  地址与新字段一致，跨编译单元读写及双向调用可见，八个线程并发和反复 yield
  后状态隔离正确。测试使用实际生成类型头与构建副本。
- 生成 chunk 0000 的编译目标中，旧八寄存器的 TLSDESC 重定位为 0；最终 ELF
  `g_recomp_gpr` 与八个旧符号的偏移分别为 0、4、8、12、16、20、24、28。
- `test_geometry_packets.py`：提取实际新旧函数，10,000 组覆盖索引/inline 包、
  奇数/满容量、拆分、标志以及禁用/失败路径，主机和 AYN Thor 均一致。
- 设备独立索引测试（400,000 个 32-word 包）：原函数 23.345 ms，局部游标
  3.595 ms。该数字不代表游戏帧率收益。
- 管线/纹理哈希、uniform 和手柄脚本回归通过；NDK 29、`-O2 -g1`、两路并行
  完整原生库重编译通过。
- versionCode 23 APK 签名及 ZIP 对齐检查通过，覆盖安装成功；实际启动与游戏
  运行通过，设备像素、剔除和静态/流式 16/32 位索引自检通过。

产物：`build/dist/0.6.6-cn.1-gpr-tls/JSRF-Unleashed-android-arm64-CN.apk`。
SHA256：`670f1592abfacd1daf0a1037bf38738bfdc43c645073e76dd72d865f1cf902a5`。

## 更新后的设备反馈与采样

用户反馈：简单测试所有场景，基本都能 60 FPS 运行。这是快速实机反馈，
不能等同于长时间、全流程回归。

随后在白天地图执行 root CPU-clock 500 Hz、15 秒采样：10,343 个样本，
0 丢样。末尾约 16 秒帧统计为 60.01 FPS、渲染线程 CPU 8.307 ms/帧、
GPU 查询 2.560 ms/帧。该场景与前一轮夜间广场不同，不作为同视角 A/B。

剩余主要热点：渲染线程 self 中 `__kernel_clock_gettime` 15.12%、
`nv2a_hash_wide` 10.35%、`nv2a_gl_method` 9.33%、`tex_sample` 4.64%、
`memcpy_opt` 4.18%；游戏线程 TLS 动态解析 self 10.17%。

时钟热点的 vDSO 调用栈未能完整展开，不能把全部时间精确归给同一调用者。
源码中 Android 限帧仍固定保留每帧末尾 1.2 ms 自旋，与该热点规模吻合；
因此下一步可优先验证自适应短自旋/绝对时间休眠，目标是降低 CPU 和功耗并
保留帧间隔稳定性。随后再考虑减少顶点/纹理重复内容哈希、批量状态派发和
剩余 TLS 访问。没有在本轮追加这些改动。

数据：`build/logs/gpr-tls-test/profile23/`。诊断属性已清空，保留 Vulkan 后端。
CPU 锁频保持用户设置，未修改。
