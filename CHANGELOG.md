# Changelog

Releases are tagged `vX.Y.Z` and built from that tag: the Mac app, the Linux
build and the iPhone app say which version they are (Finder › Get Info,
`./jsrf-unleashed --version` on Linux, the ⚙ button on iPhone), and so does
the first line of every log. Downloads are on the
[Releases](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/releases)
page.

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
