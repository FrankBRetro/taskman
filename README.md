# TaskMan and FLoad

GEM system monitors for the Atari ST, STE, TT and Falcon running
**MultiTOS** or **FreeMiNT**.

## TaskMan

A task manager in the style of a modern activity monitor.

- **Task Manager window** with two tabs:
  - **CPU**: process name, PID, PPID, nice level, state, % CPU, CPU time and memory
  - **Memory**: memory, % of RAM, text/data/BSS sizes, ST or TT RAM, and memory protection mode
- Click a column header to sort, and click it again to reverse the order. The Sort menu offers the same choices.
- **Stop** (SIGTERM), **Kill** (SIGKILL) and **Nice...** for the selected process. Double-click a row to set its nice level.
- **CPU** and **Memory** meter windows, and scrolling **CPU History** (kernel and user time) and **Memory History** (used and free) graphs.
- Meters and graphs can be iconified. The icon keeps showing live data, and its title shows the current figure, e.g. "CPU 42%".
- **View > Hide AES** leaves the AES out of the list and the totals, so you can see what everything except the GUI is using.
- **Light and dark themes.** The startup theme follows the system palette. In 2- to 256-colour modes TaskMan uses the system palette as it is and never changes it. Each colour is the nearest existing one that still contrasts with its background. True-colour modes get exact colours.
- Sample rate from 0.5 to 20 seconds (Rate menu). Any window can be hidden or shown from the View menu.

All process information comes from `u:\proc`, so no `u:\kern` is needed. Kernel and user time come from each process's own accounting in the MiNT kernel.

Keys: `Ctrl+Q` quit, `Up`/`Down` select, `Delete` stop, `Tab` switch tab.

## FLoad

A GEM take on X11 `xload`: a resizable window with a scrolling CPU graph and the load average drawn as a horizontal line.

- On FreeMiNT it reads `u:\kern\stat`, which gives a user/system split, and `Suptime()` for the load average.
- On MultiTOS, which has no `u:\kern`, it measures CPU use with a low-priority idle-meter thread. The line then shows 1, 5 and 15 minute averages of CPU busy time.
- The title bar shows which data sources were found.

Usage: `fload.app [-i seconds] [-n]`. `-n` turns off the idle meter.

## Download

Ready-built 68000 programs are attached to the
[latest release](../../releases/latest).

## Building

You need the `m68k-atari-mint` cross toolchain (gcc, MiNTLib and gemlib).

```sh
make                    # both programs, 68000 code
make taskman            # TaskMan only
make fload              # FLoad only (built in FLOAD/)
make CPU=-m68020-60     # 68020-68060
make CPU=-mcpu=5475     # FireBee / ColdFire
```

## Licence

MIT. See [LICENSE](LICENSE). The built programs also contain MiNTLib and gemlib, which are under their own licences.
