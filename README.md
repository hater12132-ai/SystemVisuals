# SystemVisuals

**Visual-only** native pack for **stock LeviLauncher** (`.levipack`), not a custom client.

## Features (Mod Menu)

| Module | Role |
|--------|------|
| **TargetHUD** | Crosshair target card (name / HP) — native GLES HUD |
| **Item ESP** | Dropped item **names** in FOV only, depth-tested (no wallhack) — hooks TBD |
| Hand chams / outlines | Visual-only render tint (from prior Bactro work) |
| Performance | Optional unlock FPS / fullbright (visual comfort) |

No combat, movement, or server-side cheats.

## ProtoHax note

`ProtoHax-2.2.0` is a **packet relay** (Kotlin). It does **not** ship TargetHUD or Item ESP sources. Those UIs in prod clients are either closed-source or different modules. This pack ports the **native** TargetHUD path from the HandOutline/Bactro work and defines Item ESP the same way Levi mods work (`System.load` before game start).

## Install

1. Build → get `SystemVisuals.levipack`
2. Open **LeviLauncher** → Mods → import the `.levipack`
3. Enable **SystemVisuals** → Launch Minecraft

## Build (PC / CI)

```bash
# Android NDK + xmake
xmake f -p android -a arm64-v8a -m release
xmake
# output: build/.../SystemVisuals.levipack
```

Or use GitHub Actions workflow `build-levipack.yml`.

## Layout

```text
manifest.json (inside .levipack)
libSystemVisuals.so
icon.png
```

Target game builds: 1.26.51.x – 1.26.52.x (signatures in `Signatures.cpp`; refresh when Mojang updates).
