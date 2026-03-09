# CSP S Pen Eraser

Hold the Galaxy Book S Pen side button to use it as an **eraser** in Clip Studio Paint — simulating a Wacom-style tail switch.

## Problem

The Galaxy Book 360 S Pen has no physical eraser (tail switch). To erase in Clip Studio Paint, you have to manually switch tools in the palette, or press the side button and lift the pen off the screen before touching again. Both break the drawing flow.

This program emulates eraser mode at the OS level while the side button is held. Clip Studio Paint sees it as a real tail switch and automatically switches to the eraser tool.

## Demo

<!-- TODO: demo video -->

## How it works

1. **pen_right_click.exe** — Detects the pen barrel switch state via Raw Input API and writes it to shared memory
2. **pen_eraser_hook.dll** — Injected into the CSP process via `SetWindowsHookEx`, IAT-hooks `GetPointerPenInfo` and related Windows Pointer APIs
3. While the side button is held, converts `PEN_FLAG_BARREL` → `PEN_FLAG_INVERTED | PEN_FLAG_ERASER` so CSP interprets the input as eraser

## Install

Download `csp-s-pen-eraser.zip` from the [Releases](../../releases) page, extract both files into the same folder, and run `pen_right_click.exe`.

Right-click the tray icon → **Enable auto-start** to launch automatically with Windows.

### Build from source

Requires Visual Studio (MSVC). In a Developer Command Prompt:

```
rc pen_right_click.rc
cl /O2 /W4 /LD pen_eraser_hook.c user32.lib /Fe:pen_eraser_hook.dll
cl /O2 /W4 pen_right_click.c pen_right_click.res user32.lib shell32.lib advapi32.lib
```

## Performance

| Metric | Value |
|--------|-------|
| Memory (resident) | ~1.5 MB |
| CPU usage | ~0% (event-driven, no polling) |
| Background timer | CSP connection check every 3s (single `FindWindowW` call) |

Zero CPU usage when the pen is idle. During pen input, the overhead is negligible — just Raw Input message handling and a single `InterlockedExchange` write to shared memory.
