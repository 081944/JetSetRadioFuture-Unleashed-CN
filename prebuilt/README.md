# prebuilt: Vulkan 驱动与 libadrenotools 的构建产物

这个目录不放源码，只放**本机构建出来的二进制**，不进 Git（`.gitignore` 已忽略）。
缺了它们，引擎照常编译、照常以 GL 运行；只是 Turnip 那条路走不通（CMake 会打印
`AdrenoTools: absent`）。

## adrenotools/

`libadrenotools.a`、`liblinkernsbypass.a` 和 `include/adrenotools/*.h`，来自

    github.com/bylaws/libadrenotools  （pin 8fae8ce）

用**本项目的 NDK**编，避免 ABI 与 STL 不一致：

    cmake -S libadrenotools -B build-adrenotools -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
      -DCMAKE_BUILD_TYPE=Release -DGEN_INSTALL_TARGET=ON
    cmake --build build-adrenotools

取 `build-adrenotools/libadrenotools.a`、`lib/linkernsbypass/liblinkernsbypass.a`、
`include/adrenotools/`，以及 `src/hook/` 下四个 `.so`。

## arm64-v8a/

- `libmain_hook.so`、`libhook_impl.so`、`libgsl_alloc_hook.so`、`libfile_redirect_hook.so`
  ——上一步 `src/hook/` 的产物，随 APK 打包，装进 `nativeLibraryDir`；
- `libvulkan_freedreno.so`——Turnip 驱动本体。本项目测试用的是
  [TurnipZ 0.3.0](https://github.com/AlexSanz0902/turnip-sd865-z)（Mesa 27.0.0-devel，
  Adreno 650 是它的主要目标），解压 `TurnipZ-0.3.0.zip` 取里面的
  `libvulkan_freedreno.so` 放这里即可。adpkg 的 `meta.json` 运行时不需要。

## 运行

    setprop debug.jsrf.recomp_vk_driver_probe 1

启动后看 `[VKD]`：应当打印 native/temp 目录，然后是 `Turnip` 与
`Vulkan 1.3`（原厂 Adreno 650 只有 1.1.128，见 `docs/android-vulkan.md`）。
