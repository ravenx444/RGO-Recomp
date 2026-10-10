# Display settings (RGO)

Video settings in the launcher frontend control how the game is shown on any
monitor, window size and aspect ratio. Everything is saved in the profile's
`video.ini` (the file the widescreen work introduced), which the game re-reads
every 250 ms, so most changes apply while playing.

| Setting | Values | Applies |
|---|---|---|
| Window mode | Windowed / Borderless / Exclusive | immediately (frontend) |
| Output resolution | the monitor's modes, or any typed width x height | immediately |
| Render resolution | Match output / Render scale 25-200 % / Custom size | immediately |
| Aspect ratio | Original 4:3 / Widescreen | immediately |
| Max aspect ratio | Unlimited / 32:9 / 21:9 / 16:9 / 16:10 | immediately |
| Anti-aliasing | Off / 2x / 4x / 8x MSAA | next game start |
| VSync | On / Off | immediately |
| FPS counter | Off / On | immediately ([what it shows](framerate.md#fps-counter)) |
| Frame rate | Original / True 60 / Display / Unlocked | immediately ([frame rate](framerate.md)) |
| Interpolate extra frames | On / Off | immediately (Display and Unlocked only) |
| Blend moving objects | On / Off | immediately (with interpolation) |
| Original N64 slowdown | Off / On | immediately, Original frame rate only ([details](framerate.md#the-fast-clock)) |

## How each setting works

**Window mode.** The frontend owns the game's window (the game renders into a
child window), so it handles the mode.
- *Windowed* is a normal resizable window.
- *Borderless* covers the monitor at the desktop resolution.
- *Exclusive* switches the monitor to the chosen output resolution with
  `ChangeDisplaySettingsEx(CDS_FULLSCREEN)`, using the highest refresh rate
  the monitor offers at that size, then covers it like Borderless.
  - The desktop mode comes back when you leave fullscreen, switch to another
    app, or close the game. Windows also restores it if the process ends.
  - If the monitor rejects the mode, the frontend stays in Borderless and says
    so in the status line.
- F11 switches between windowed and the chosen fullscreen kind.

**Output resolution.**
- In *Windowed* it sizes the game area (below the menu bar) to exactly the
  chosen size. You can still drag the window to any size afterwards.
- In *Exclusive* it is the display mode.
- *Borderless* always uses the desktop resolution.
- The list comes from `EnumDisplaySettings` for the window's monitor;
  *Custom...* accepts any width and height from 320 to 16384.

**Render resolution.** RT64 renders the 3D scene at a multiple of the game's
240-line framebuffer (`Resolution::Manual`, fractional multipliers allowed).
- *Match output* (the default) uses `picture height / 240`, so the scene is
  rendered at exactly the pixels the picture covers. For example 4.5x at
  1080p, 3.2x at 768p, 6x at 1440p.
- *Render scale* multiplies that by 25-200 % in 5 % steps.
- *Custom size* is fitted to the picture's shape, so pixels stay square. For
  example 1280x720 requested on a 21:9 picture renders 1280x536.
- The multiplier is clamped to 0.5-32x (120 to 7680 lines; 32x is RT64's
  limit).
- The settings page shows the size the current game area renders at.

**Aspect ratio and Max aspect ratio (Hor+).**
- RT64 keeps the game's vertical field of view and widens the picture
  horizontally to the window's shape (`AspectRatio::Manual`).
- The F3DDKR triangle rejection, `G_CULLDL` and the level-block frustum are
  widened by the same factor, so nothing is culled at the new edges.
- Windows narrower than 4:3 (5:4, portrait) show the original 4:3 picture
  letterboxed instead of narrowing the view.
- *Max aspect ratio* caps the widening; wider windows get side bars.
  *Unlimited* stops at 8:1 (72:9).
- HUD elements anchored by the widescreen HUD work stop at the edges of a
  centred 16:9 area on wider pictures, as in Zelda 64: Recompiled. On 16:10
  and other pictures narrower than 16:9 they sit at the screen edges.

**Anti-aliasing.**
- RT64's `UserConfiguration::antialiasing` (MSAA), set before the renderer
  starts.
- RT64 turns it off by itself if the GPU lacks that sample count.
- Changing it live would need `Application::updateMultisampling()`, which
  discards every framebuffer. That is unsafe with this port's CPU framebuffer
  writeback, so the setting applies at the next game start.

**VSync.**
- The RT64 swap chain's `setVsyncEnabled` (plume).
- With VSync off, presents may tear and use the display's tearing support.
- It does not change the game's speed: game logic follows the emulated VI
  clock, not the display.

**FPS counter.**
- The game reports its rates to the frontend twice a second (`WM_APP + 24`,
  `jfg/renderer/frame_stats.hpp`).
- The frontend shows them in the top-right corner while playing, for example
  `Game 60 | Render 60 | Display 144 | 6.9-7.0 ms`.
- *Game* is game logic updates per second, the rate the game plays at;
  *Render* is real frames drawn; *Display* is frames shown, blended ones
  included; the range is the shortest and longest gap between two shown
  frames. See [frame rate](framerate.md#fps-counter).

## File format

```
version=2
aspect=widescreen
max_aspect=unlimited
fullscreen=borderless
output=auto
render=match
render_scale=100
render_size=1920x1080
msaa=off
vsync=on
fps_counter=off
framerate=original
interpolate_frames=on
interpolate_objects=on
n64_slowdown=off
```

`framerate`, `interpolate_frames`, `interpolate_objects` and `n64_slowdown`
are described in [frame rate](framerate.md).

`jfg::parse_video_settings` (`include/jfg/renderer/video_settings.hpp`) reads
this format and the original `version=1` file. It rejects the whole file on:
- a malformed value;
- a duplicate key;
- a missing final line break.

In those cases the last good settings stay in effect, so a partial write
changes nothing. Missing keys keep their defaults. Unknown keys are ignored,
so an older game build can read a newer file.

Replays and captures started without the launcher keep the original renderer
setup: 4:3, RT64's own resolution choice, no MSAA.

## Tests

`tests/video_settings_tests.cpp` covers:
- the format;
- picture fitting;
- the render multiplier;
- square pixels;
- Max aspect ratio;
- render scale and custom size.

It runs them on 16:9, 16:10, 21:9 (2560x1080, 3440x1440, 3840x1600), 32:9
(5120x1440), triple 16:9, 4:3, 5:4, 1366x768, portrait (800x1280,
1080x1920) and an arbitrary dragged window size.
