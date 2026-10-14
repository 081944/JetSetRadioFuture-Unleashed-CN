# Changelog

Releases are tagged `vX.Y.Z` and built from that tag: the Mac app, the Windows
and Linux builds and the iPhone app say which version they are (Finder › Get
Info, `JSRF Unleashed.exe --version` on Windows, `./jsrf-unleashed --version`
on Linux, the ⚙ button on iPhone), and so does the first line of every log. A
download with nothing new for its platform is carried over unchanged and keeps
its version. Downloads are on the
[Releases](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/releases)
page.

## 0.3.1 — 14 October 2026

- Linux: the F9 resolution window's text is no longer cut off at the right
  edge (SDL's own dialog on X11 does not wrap long lines).
- The Windows download is 0.3.0's, and the Mac and iPhone downloads are
  0.2.0's, unchanged.

## 0.3.0 — 14 October 2026

- **Windows** (new, experimental): `JSRF-Unleashed-windows-x86_64.zip`, for
  Windows 10 and 11, 64-bit. Unzip it and run `JSRF Unleashed.exe`: it asks
  for the game folder, or a disc image, which it unpacks once. The same engine
  and OpenGL renderer as the Linux build, controllers through SDL, the
  keyboard. Played under CrossOver on a Mac and under Wine on Linux (boot,
  title, loading a save, the garage); not yet on a Windows PC.
- On Windows and Linux, **F9** sets the resolution — 1× to 4× the Xbox's
  640×480, up to 2560×1920 — and restarts the game to apply it; **F10**
  switches widescreen and **F11** full screen. The resolution and widescreen
  are remembered (on Windows, widescreen was forgotten at every start).
- **Linux**: rebuilt from the 0.3.0 tag for F9; nothing else changes for it.
  (For the first hour after the release, the Linux download was still 0.2.0's
  build.)
- The Mac and iPhone downloads are 0.2.0's, unchanged (they call themselves
  0.2.0); AltStore keeps offering 0.2.0.
- Docs: saves are kept in the game folder, in `UDATA`, on every platform — not
  in Application Support (Mac), `~/.local/share` (Linux) or `Saves` (iPhone),
  as the README said.
- Building: `launcher/windows/` has the Windows launcher, its packaging, and
  the toolchain file for cross-compiling with llvm-mingw.

## 0.2.0 — 12 October 2026

- **iPhone and iPad** (new, experimental): `JSRF-Unleashed-iOS.ipa`, for
  sideloading (AltStore, SideStore, Sideloadly). The game goes into the app's
  folder in the Files app — the extracted folder, or the disc image, which the
  app unpacks itself. Controllers, on-screen controls (a stick and the pad's
  buttons), a hardware keyboard. Played in the iOS Simulator; not yet tried
  on a real device.
- **Mac: Metal is the renderer now.** Each frame goes straight to the window,
  every one of them reaches the screen, and on a 120 Hz display (a MacBook
  Pro's) nearly every frame stays up for exactly a sixtieth of a second:
  smooth 60 fps. Less CPU, too. OpenGL is still there, under Video › Renderer.
- **Fixed: the picture could freeze for good** now and then during play (all
  platforms). The game's graphics driver sometimes waits for an interrupt the
  GPU raises when it reaches a marker in the command stream; nothing raised
  it.
- Widescreen: on the character select the characters stay inside their
  panels; the boot logos fill the 16:9 screen.
- Fixed: a 1-pixel dark line along the right and bottom edges.
- Logs are a small fraction of the size (most of a log was one line per GPU
  submission).
- Building on a Mac needs nothing from Homebrew.

## 0.1.2 — 2 October 2026

No change to the game.

- Both downloads are built from the tagged source and carry its version. (The
  0.1.0 and 0.1.1 Mac apps call themselves "0.1".)
- The repository builds the game on its own: `port/CMakeLists.txt`, given the
  generated code in `port/recomp/gen`.
- `launcher/package_mac.sh` and `launcher/linux/package.sh` make the release
  downloads.

## 0.1.1 — 2 October 2026

- Linux: New Game no longer stays on "Now Loading" for good. The game asks for
  its files in a different case from the one on the disc
  (`TypeA.bin` / `typea.bin`); file names are now found whatever their case.
- The Mac app is the same as in 0.1.0.

## 0.1.0 — 2 October 2026

First public release.

- macOS app for Apple Silicon, and a first Linux (x86_64) build — whose New
  Game hangs on "Now Loading"; use 0.1.1 or later.
- Playable from New Game through the tutorial, the DJ K scene and Dogenzaka
  Hill: skating, grinding, tricks, graffiti, the pause map, music, sound
  effects and voices, saving and loading.
- Controllers and the keyboard.
- Widescreen 16:9 with a wider view, not a stretch; the HUD keeps its shape,
  the title screen stays 4:3.
