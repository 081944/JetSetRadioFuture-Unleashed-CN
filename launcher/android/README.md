# Android

The Android app: the engine as `libmain.so` (`port/`, built with the NDK for
arm64, Android 10 and later), SDL2 for the window, the input and the sound,
and OpenGL ES 3 for the picture (`NV2A_GLES` in `src/nv2a_gl`: the same
renderer as the Windows and Linux builds, in its OpenGL ES dialect).

| | |
|---|---|
| `java/.../SetupActivity.java` | the first screen: finds the game in the app's folder, or unpacks a disc image there (`port/ios/xiso.c`) |
| `java/.../JsrfActivity.java` | the game: SDL's activity, which calls the engine's `SDL_main` |
| `../../port/android/jsrf_android.c` | the engine's side: the log, the environment, the folders, the unpacker's JNI |
| `AndroidManifest.xml`, `res/` | the app |
| `build_apk.sh` | builds it all with the SDK's own tools (no Gradle) |

Building needs the Android SDK with an NDK, build-tools and a platform, a JDK
17 or later, CMake, SDL 2.32's source (its Java side is part of the app; 2.30.0 does
not build with a recent NDK) and
the generated code (`port/recomp/gen`, see the top-level README):

```sh
JAVA_HOME=/path/to/jdk launcher/android/build_apk.sh ~/Library/Android/sdk \
    /path/to/SDL2-2.32.10 port/recomp/gen 0.4.0 out
```

The game goes in the app's folder, `Android/data/com.jumatt.jsrfunleashed/files/game`
(the extracted folder, or unpacked there from a disc image by the first
screen); its saves are kept there, under `UDATA`, and Logs/last-run.log beside
it.
