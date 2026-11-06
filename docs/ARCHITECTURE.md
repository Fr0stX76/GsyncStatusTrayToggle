# GsyncStatusTrayToggle architecture

How the code is organized, for anyone who maintains it. Comments in the code explain why, not what: Windows, driver and NVAPI traps. Names, tests and this page carry the rest.

## Files

| File | Contents |
| --- | --- |
| `GsyncLogic.h` | Pure logic, without Windows: states, command-line parsing, what a click, the menu or a command must do, the five setting values, exit codes. |
| `GsyncCore.h` | NVAPI and display work, shared by the tray and the command line: the machine-wide lock, reading and saving the NVIDIA global profile, waiting for the display, the redraw and the refresh-rate check. |
| `TrayMenu.h` | The owner-drawn tray menu: placement, per-monitor DPI, light and dark colors, high contrast, names for screen readers. |
| `GsyncStatusTrayToggle.cpp` | Entry point, tray icon, menu commands, settings, the watcher and command threads. |
| `tests/GsyncLogicTests.cpp` | Console test runner for `GsyncLogic.h`, no test framework. Exit code 0 when every check passes. |
| `nvapi/` | NVIDIA NVAPI headers and library, unchanged. |

## Threads

| Thread | Owns |
| --- | --- |
| UI thread | Window, tray icon, menu, settings, Start with Windows. It never calls NVAPI and never waits for the display. |
| Watcher thread | Watches the NVIDIA settings folder, reads the state through NVAPI and posts it to the UI thread. It holds one NVAPI reference for the life of the tray. |
| Command thread | Runs one G-SYNC change (`Execute`) and posts the result. |
| System thread pool | Monitor removal and arrival callbacks (`ReturnWatch`) during a change. |

Rules that keep this safe:

- The UI thread starts a command only when none is running, and reads the result after the thread has ended.
- NVAPI is called from one thread at a time (`g_nvapi`), because its thread safety is not documented.
- One G-SYNC change at a time across all processes: `Global\GsyncToggleMutex` is held for the whole change, by the tray and by `/off`, `/fs` and `/fsw`.
- Nothing that can block, such as NVAPI or a wait for the display, runs on the UI thread.

## From a click to G-SYNC

1. **Decide** (`Decide`): the action and the current Application Mode give the target. An unknown value is refused for a click or the menu; `/off`, `/fs` and `/fsw` still apply.
2. **Plan** (`MakePlan`): the five settings for the target. Switching between the two on modes changes only the mode settings.
3. **Save in three steps**, in the NVIDIA Control Panel's order:
   1. Application Mode and Application State. The driver disconnects and reconnects the display.
   2. After the display is back (`WaitForReturn`, from monitor notifications), Global Feature.
   3. After one redraw of the displays (`RedrawDisplays`), Preferred Refreshrate and Global Mode.
4. **Check**: if the display came back at a lower refresh rate, the best configuration is applied once more (`KeepRate`). The profile is then read back and compared with the plan.

A change takes about 3.5 seconds and the screen blinks once.

## Icon state

The icon shows Application Mode from the global profile. The watcher thread waits until NVIDIA rewrites `nvdrsdb0.bin`, `nvdrsdb1.bin` or `nvdrssel.bin` in `%ProgramData%\NVIDIA Corporation\Drs`, which every program does when it saves NVIDIA settings, then reads the profile 250 ms after the last write. It also reads it when the menu opens and after each command. No administrator rights are needed: the driver's registry key is not used.

## Settings

- `HKCU\Software\GsyncStatusTrayToggle`, value `ToggleMode`: `Fullscreen` or `Windowed`, the mode a click turns on. Missing or invalid means `Fullscreen`.
- `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `GsyncStatusTrayToggle`: the quoted path of the exe. Start with Windows is checked only when it points to this copy and Task Manager has not disabled it (`StartupApproved`).

## Command line

`/off`, `/fs` and `/fsw` run one change without the tray and return an exit code, listed in the README and in `GsyncLogic.h`. Any other argument returns 64. The command line shows no message, so scripts never wait on a dialog.

## Tests

`GsyncLogicTests.exe` checks the command-line parsing, the five values of each plan, every click, menu and command decision, the exit code values and the names of the watched files. The README commands and GitHub Actions build and run it before the app.
