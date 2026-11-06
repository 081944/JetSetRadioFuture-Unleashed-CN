# Android Vulkan 宽屏与手柄修复（2026-11-07）

基于 `cn` 的 `a68db81`，设备为 AYN Thor。修复包沿用本地测试版本
`0.6.6-cn.1`，versionCode 从 20 增至 21；通过构建脚本的版本覆盖参数记录，
没有修改发布版本元数据，也没有合并上游 0.6.8。

## 原因与修复

- Vulkan 的 presenter 按 1280×960 纹理尺寸计算显示比例，忽略了共用前端已经
  扩展的场景。现在生产线程在每帧交给 presenter 前保存该帧的显示比例，
  presenter 使用这份快照。启动标志和标题仍可保留 4:3，游戏场景遵循宽屏设置。
- Vulkan 在共用 `present()` 中提前返回，跳过 SDL 的事件处理与设置请求。
  现在共用前端使用 Vulkan 已创建的 SDL 窗口，每次 flip 消费 SDL 事件和
  待应用的设置，不调用 OpenGL 的上屏函数，也不创建第二个窗口。
- `pad_read()` 一旦加载过自动按键脚本，就始终返回脚本状态；脚本结束后
  依然不读取实体手柄，清空 Android 属性也不会清除当前进程缓存。
  现在先读取实体手柄，再叠加当时生效的自动输入；无脚本按键时保留实体状态。

## 验证

- `test_pad_script_merge.py` 提取实际 `pad_read()` 编译执行，验证脚本前、期间、
  结束后的实体输入、其他端口、自动摇杆覆盖及无实体手柄的脚本输入。
- `test_vk_uniforms.py` 通过；`test_controller_mapping.c` 的四种布局及完整
  0–255 压力范围通过；`git diff --check` 通过。
- NDK 29、两路并行、`-O2 -g1` 增量编译与链接通过。
- APK 签名和 16 KiB ZIP 对齐检查通过；同一签名覆盖安装成功。
- AYN Thor 上 Vulkan 游戏画面截图已铺满 1920×1080，设备日志记录
  `Odin Controller in slot 0` 和 `controller 0 connected`，短时帧统计约 60 FPS。
- 用户已在 AYN Thor 上确认实体手柄和宽屏切换两项均正常。
  没有进行长时间或全地图回归。

产物：`build/dist/0.6.6-cn.1-vulkan-input-wide-fix/JSRF-Unleashed-android-arm64-CN.apk`

SHA256：`02116a650c55426d31d576e787dd025d5d77fed8ece06333a79227cb85b209cd`

日志、截图：`build/logs/cn-latest-device/fixed-*`。
本次自动按键和帧统计属性已清空，Vulkan 后端属性保留供继续试玩。
