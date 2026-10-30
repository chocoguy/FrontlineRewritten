# Phase 2 status

Hand-maintained. Phase 2 is the enhancements phase ([ROADMAP](../../ROADMAP.md#phase-2--enhancements-l)).

## Done

### Mouse and keyboard (both modes)

The keyboard and mouse drive controller 1 as **game actions**, not GameCube buttons. The old keyboard mapping pretended to be a controller, so what a key did depended on the controller layout picked in the game's options, and the mouse could only have faked a stick. Now:

- **The app** (`apps/macos/Sources/Input.swift`) maps keys and mouse buttons to actions (`NATIVE_ACTION_*` in `FrontlineDecompiled/include/native/hooks.h`) and measures the mouse: raw movement from `GCMouse` (no pointer acceleration), with the view's pointer events as a fallback for devices `GCMouse` doesn't report, and the wheel from scroll events. Sensitivity and inverted look are app settings (`mouseSensitivity`, `invertMouse`; the Settings window).
- **The platform** (`platform/src/pad.cpp`) keeps what's held, the presses since the game's last look (so taps shorter than a frame count), and the mouse movement and wheel notches since then.
- **The game** takes it once a frame in `CInputManager::Update` (`input.cpp`, native only) and sets input events on top of what the controller's keymap produced:
  - The configurable actions (move, jump, crouch, aim, fire, reload, next and previous weapon, melee, action) set the same events the controller config screen binds, so the rest of the game can't tell them apart from a controller. The edge-triggered events (`INPUTEVENT_PRESS`) get presses; the held ones get the held state.
  - Pause and the HQ hint act only while playing (in a level, not paused, past the intro screens). The menu actions (D-pad, A, B, X, Y, Z, L, R, START: the UI's events 0x79 and up) act only otherwise. So Esc pauses in a level and backs out of menus, and the arrow keys don't call HQ during play. Esc in the pause menu continues the game through the menu's own Continue (`GameMessage` 2, as the pause-screen cheat codes do); B there would move the cursor to Quit.
  - The wheel switches weapons one per frame, or, with a scope up (when the game locks the D-pad to the scope's zoom), zooms 5 frames per notch.
- **Mouse look** (`player.cpp`, native only) adds the frame's movement to the stick's look rate on foot, aiming and on mounted guns, so camera shake, recoil and the pitch limits treat both alike. It scales with the zoom (the view's width relative to the normal 35°), so the crosshair follows the mouse at any field of view. On a mounted gun or turret it's capped at the stick's top speed, so guns turn no faster than in the original.
- **Pointer capture** (`GameView.swift`): while playing in the key window, the view hides the pointer and holds it still, and lets go when the game pauses, a menu comes up, or the window or app loses focus. It starts only with the pointer over the view, and mouse buttons held when it starts are ignored until let go, so the click that focuses the window doesn't fire.
- **Testing:** `FRONTLINE_INPUT` takes `@` actions too (`@FIRE+1`, `@MENU_A`, `@LOOKRIGHT+1.5` turns the mouse at a radian a second, `@WHEELDOWN`), so headless runs can drive everything the keyboard and mouse do.

Controllers work as before and can be used at the same time.

### On-screen controls (both modes)

Prompts and menu button icons show the keyboard's keys while the keyboard and mouse were used last, and the original GameCube text and icons once a controller is used. They switch live. The platform tracks the device (`NativePromptDevice` in `native/hooks.h`: keys, mouse movement and the wheel count for the keyboard; buttons, or sticks and triggers past 30%, for a controller) and takes the keys' names from the app's bindings (`SetKeyLabel`), so they follow key remapping later.

- **Prompts** (`game/native/prompts.cpp`, rules in `platform/src/prompts.cpp`): all of them are in one table, `DATA/chars.bum`, loaded by `CUserMessageTable::Load` for the shell and for each level. After a load, the table moves to a new buffer where each rewritten message has room for both versions, and a device change copies the right one in. The game keeps pointers into the table (hints, objectives) and copies a prompt only when it shows it, so nothing holds stale text. 71 messages are rewritten:
  - the 48 "Press ACTION to ..." prompts, "Press CROUCH", "Press START" (Esc), "Use AIM" (Right Mouse Button), "Use +Control Pad to Zoom" (Mouse Wheel), "Press +Control Pad Right to call HQ" (H);
  - 18 memory card dialog lines ("press the A Button" becomes "press Return"; the sentences that break across lines are rewritten line by line, keeping their `%c`).
  - The controller configuration screen's labels (0x2E5-0x2F3, 0x2F7, 0x2F8, 0x2FA) are left alone: they name actions, and the screen copies them.
  - Limits: prompts at the top of the screen are cut at 49 characters and objectives over 49 aren't drawn. `tests/datacheck` checks every rewrite against the disc's table (length, `%` conversions, nothing still naming a button).
- **Menu icons** (`game/native/hint_icons.cpp`, drawing in `platform/src/key_icons.mm`): the hint bars' icons are small textures (`X_button.gsh` is the A button, `Tri_button.gsh` B, `Cir_button.gsh` X, `Squ_button.gsh` Y, `DPad_blank.gsh` and its lit `DPad_*HL.gsh` arrows, named after the PS2's buttons), in the shell and in every level's pause menu. When a screen loads one, it gets a keyboard twin: a key drawn with Core Graphics and Core Text at about 4x its size on screen (return, esc, X, Y; the four arrow keys in a plus for the D-pad, lit ones over it), made into a GX RGBA8 shape file. `TibPolyDraw` draws the twin while the keyboard is in use. The words next to the icons ("SELECT", "GO BACK") name actions and stay.
- **Left alone:** the controller configuration screens, which picture the GameCube controller (they set up the gamepad), and the loading and briefing screens (no controls on them).

### The setup screen and the mode switch

Each launch opens a setup screen in the game window (`apps/macos/Sources/SetupView.swift`):
- **Mode:** Classic or Rewritten.
- **Resolution**, **widescreen**, **frame rate**, **anti-aliasing** and **draw distance:** Rewritten only.
- **Disc folder.**

Play saves the choices (user defaults `gameMode`, `renderLines`, `widescreen`, `highFrameRate`, `msaa`, `drawDistance`) and starts the game; they hold for the whole run. The game relaunches the app between levels (`LoadElf`), and the app then skips the setup screen (`FRONTLINE_RESTART` is set) and starts with the saved settings. Headless runs use `FRONTLINE_MODE=rewritten`, `FRONTLINE_RESOLUTION=<lines>`, `FRONTLINE_WIDESCREEN=<w>:<h>`, `FRONTLINE_HFR=1`, `FRONTLINE_MSAA=4` and `FRONTLINE_DRAW_DISTANCE=<times>`.

- **Where the settings live:** the platform (`platform/src/settings.cpp`). In Classic it reports the original picture (scale 1, 640 wide) whatever the app set.
- **What the game reads:** `NativeGameMode()` and `NativeRenderWidth()` (`native/hooks.h`). This is the ROADMAP's run-time mode switch; Classic is the default path.

### Render resolution (Rewritten)

**Scale:** 720p, 1080p, 1440p and 4K render at 1.5, 2.25, 3 and 4.5 times the original, the lines on the 480-line screen.

**How it works:** the game draws everything (3D, HUD, text, fades, movies) in the console's 640 x 448 coordinates through GX viewports and orthographic projections. So all the scaling happens where GX meets Metal (`gx_metal.mm`):
- the EFB textures are the scale times 640 x 528;
- viewports and scissors scale (a scissor covers every pixel its rectangle touches);
- the frame copy's rectangle and texture scale too.

**Display:** frames above the original have mipmaps, made while the window shows the frame smaller than it was rendered, so it still looks smooth. The display shader samples with a mip filter, which a one-level texture ignores.

**Classic is the original path:** the same texture sizes, and the same viewport and scissor values. Movie frames rendered through GX came out byte-identical to the build before this change.

**Costs:**
- ~~Movies are nearest-sampled, so they upscale blocky.~~ Fixed: see [Picture quality](#picture-quality-rewritten).
- 4K is about 20 times the original's pixels. See [Performance](#performance-both-modes) for what that costs now.

### Widescreen (Rewritten, levels only)

**Width:** the picture is the display's shape on the 480-line screen, to a multiple of 16: 768 for 16:10, 848 for 16:9, up to 1136 for about 21:9.

**When it applies:**
- It starts after a level has loaded (`CScreen::NativeSetRenderWidth`, called from `Moh2.cpp`). The loading screen, the shell and the boot movies stay 4:3, and each level is a fresh process.
- Split screen stays 4:3.

**What changes** (factors `g_nativeWideScale` = width / 640 and `g_nativeWideMargin` = (width - 640) / 2):
- **3D view:** `CCamera::SetPerspective` widens `m_xScale` by the scale: the same vertical view, more to the sides ("Hor+"). The frustum and so all culling follow, at every zoom and on vehicles.
- **Weapon:** its projection and the muzzle flash's screen position widen the same way, so the viewmodel isn't stretched.
- **Sky:** all six faces are drawn, since its facing test drops faces at a wider view's sides.
- **2D:** the sprite and font orthographic projections put the 640 x 448 screen in the middle with the margins beyond it. The pause menu and other UI screens stay 4:3 in the middle, with the game around them.
- **Full-screen 2D reaches the edges:** the letterbox bars, the popup and subtitle backgrounds, and the scope and binocular overlays' sides (filled with the texture's corner, as the sniper scope's side bars are).
- **HUD:** the ammo and weapon name move out to the right edge, and the compass, health and hit markers to the left, no further out than at 16:9 (`g_nativeHudShift`, set around them in `UserInterface::Draw`).
- **Frame buffers:** allocated at the widest width. The platform's EFB is that wide, and the display takes the picture's shape from the frame.

**Capacity:** the render list (the frame's draw commands) was 192 KB, with no bounds check. Natively its entries are bigger (pointers): Omaha Beach already peaks at 161 KB in Classic, and widescreen adds almost nothing. It is now 768 KB natively (`GC_SIZE` in `Moh2.cpp`). Scene lights, shadows and compartment light links stayed at zero in Omaha and Into the Breach, at 4:3 and 21:9.

### Picture quality (Rewritten)

Done 2026-10-27. Anti-aliasing, better texture filtering, smooth movies and a longer draw distance. Classic is unchanged, apart from a texture-decoding fix that makes it closer to the console (below).

**Texture filtering** (always on in Rewritten; `gx_metal.mm` `Texture` and `Sampler`):
- **Made mipmaps.** The levels' own textures (`compartment.cpp`, `staticmesh.cpp`) have no mipmaps, so distant walls, ground and water shimmered at any resolution. Rewritten makes the whole chain on the CPU when a linearly filtered texture is first decoded.
  - Colors are averaged by their alpha, so the colors of see-through texels don't bleed into the edges.
  - Alpha-tested textures (alpha only near 0 or 255) keep, at every level, the share of texels that pass the game's test (alpha > 160), so wire, fences and leaves don't thin out and vanish in the distance (`KeepCoverage`).
  - Textures filtered nearest (the movie player's lookups, pixel-exact 2D) are left alone.
- **Trilinear and 16x anisotropic** for every linearly filtered texture with mipmaps, the game's own included. The console goes to 4x anisotropic, and the game asks for 2x at most.
- `FRONTLINE_GX_ORIGINAL_FILTERING=1` turns this off, for comparison.

**Anti-aliasing: 4x MSAA** (the setup screen's default in Rewritten; `FRONTLINE_MSAA=4`):
- The EFB is drawn multisampled and resolved into the single-sample EFB texture as each pass ends, so frame copies read it as before.
- A pass whose next pass clears the color keeps only the resolve, not the samples (`EndPass`), which saves most of the bandwidth.
- **Alpha to coverage:** alpha-tested draws that don't really blend (the levels' use ONE, ZERO) and test one threshold (a > ref or a >= ref) write the share of the pixel past the threshold as alpha instead of discarding, so their edges are smoothed too (`AlphaCoverage`; a function constant in the TEV shader, a uniform in the ubershader).
- High frame rate: records carry the sample count, and replays draw multisampled too.
- `FRONTLINE_PERF` counts the alpha-to-coverage draws.

**Movies** (Rewritten): the game draws a movie with GX from its decoder's Y, U and V planes, read in place through an indirect-texture swizzle that only works with point sampling (`bigswizzler.cpp`), so movies upscaled blocky. In Rewritten, `hwvideodisplaylist.cpp` marks the planes as rows of texels (`NativeLinearTexture`), filtered linearly, and drops the swizzle; the TEV's YUV-to-RGB stays as it was. The display's own YUV frames (the movie player writing a frame buffer) are filtered between pixels too, in both modes, as RGB frames already were.

**Draw distance** (setup screen: Original, Far 2x (the default), Farthest 4x; `FRONTLINE_DRAW_DISTANCE=<times>`):
- The game has no level-of-detail system: what it draws is limited by the camera's far plane and hidden by the level's fog. `NativeDrawDistance()` multiplies both, the far plane in `LoadLevelSettings` and the fog in `SetFog` (which scripts call too), so the fog keeps its look, only farther out.
- On Omaha Beach at 2x, the bluff's bunkers show with their textures instead of as flat fog-colored shapes, and ships appear on the horizon. The haze is thinner: Original keeps the level's own atmosphere.

**Texture decoding (both modes; Classic accuracy):** CMPR now blends its in-between colors at 3/8 and 5/8, as the console does (Dolphin's `DXTBlend`), not at thirds. 5-, 6- and 3-bit colors widen to 8 bits by repeating their bits, as the texture unit does, instead of scaling. Classic's CMPR texels were off by up to 10/255 and its RGB565/RGB5A3 ones by 1.

**Checks:**
- **Classic unchanged:** a build with all of the above reverted except the decoding fix and this one drew the same 1,298 distinct frame copies through the intro movies and menus (`FRONTLINE_GX_HASH`).
- **High frame rate with MSAA:** `FRONTLINE_HFR_VERIFY` replayed 8,400 frames (movies, menus, the landing and play at 1080p) byte-identical to the frames drawn.
- **Specialized against ubershader** (`FRONTLINE_GX_VERIFY`): both features make the two differ more, so check renderer changes with `FRONTLINE_MSAA=1 FRONTLINE_GX_ORIGINAL_FILTERING=1`. Run that way through the intro, menus, landing and play, no pixel differed by more than 1/255; the filtered movies differ by 1 in a few thousand pixels a frame (the two shaders' coordinates differ in the last bits, which bilinear weights now show).
  - With the made mipmaps, the ubershader's lookups in run-time branches pick slightly different levels (the known texture-LOD difference, now on most textures): about 50 pixels a frame, up to 35/255.
  - With MSAA, alpha to coverage turns the two shaders' tiny alpha differences into a sample more or less at alpha-tested edges: about a thousand pixels a frame, by up to about a quarter of the edge's contrast.
  - The ubershader only draws while a specialized pipeline compiles, for a few frames.
- **Cost:** D-Day at 1080p, GPU 2.2 ms a frame before, 3.2 ms with all of it on. 4K at 16:9 with 4x MSAA and 2x draw distance holds 60 fps, the GPU at 5–8 ms.

### Performance (both modes)

The Debug build ran the game at 30–45 fps on an M5. It drops no frames now, up to 4K at 21:9. None of this changes the picture, so it applies to Classic too.

**What was slow:**
- **Everything was built unoptimized.** The `ninja` and `xcode` presets are Debug builds (-O0): the game, the CPU vertex stage, EA's audio mixer. The CPU alone took 14–17 ms of the 16.7 ms frame.
- **The CPU and GPU took turns.** `GXDrawDone` waited for the GPU every frame, so a frame cost CPU time plus GPU time, and at 4K that came to more than two retraces.
- **The TEV ubershader cost a lot per pixel.** Every pixel ran a 16-stage loop with run-time switches.
- **Every draw re-hashed its textures.** Tiny textures were hashed byte by byte, and the cache lookups boxed their keys into heap `NSNumber`s.

**What changed:**
- **Optimized build:** the `release` preset (`RelWithDebInfo`, -O2) and `xcode-release` (README). The game's compiler flags (0004) keep its floats the same as at -O0.
  - **Race fixed:** -O2 found a race in the thread layer. A thread cancelled and created again in the same `OSThread` (the sound driver, at every level start) could leave its old host thread waiting for a turn forever, so the new one never started and the game hung. `OSCreateThread` now waits for the old host thread before setting the `OSThread` up again (`JoinLastHost`, with a test in `tests/ostest`).
  - **Better hang reports:** an interrupt queue overflow now crashes on the stuck game thread, so the backtrace shows where it is.
- **CPU and GPU overlap** (`DrawDone`): the GPU never reads game memory (each draw copies its vertices, indices and uniforms, and textures are decoded into Metal textures), and the game reads nothing back from the GPU. So `GXDrawDone` only waits for the frame before, and the GPU draws one frame while the game builds the next. `FRONTLINE_GX_SYNC=1` brings back the console's wait.
- **Cheaper draws:**
  - Textures are hashed at most once per frame copy, in 64-bit words. The movie player's in-place rewrites are still caught every frame; `FRONTLINE_GX_TEXCHECK=1` hashes at every draw and reports any texture that changes within a frame (none did, from boot through a level).
  - The caches are plain arrays and an open-addressed table instead of `NSDictionary`.
  - A draw only sets the encoder state and uniforms that changed.
  - Indices are 16-bit.
  - Vertices carry only the texture coordinates that are generated: 60 bytes for one texgen, down from 192.
- **Specialized TEV shaders** (`gx_shader.cpp`): the same shader source, given the draw's TEV setup ("shape": stages, inputs, operations, alpha compare, fog type, indirect setup, texgen count) as Metal function constants. That unrolls the stages, folds the switches and drops unused interpolants.
  - Constant colors, fog parameters and matrices stay uniforms, so setups that differ only in values share a pipeline.
  - Each shape compiles on a background thread the first time it's drawn (about 15 ms), and the ubershader draws it until then.
  - `FRONTLINE_GX_UBER=1` uses only the ubershader, and `FRONTLINE_GX_SYNC_SHADERS=1` compiles on the spot.
  - **Startup race fixed:** the shader library compiles in the background from the moment the app asks for the device (its window opening), which can take several seconds with a cold shader cache. The game thread's first GX call used to find the device set while the main thread was still compiling, go on, and abort in Metal on the nil ubershader (seen in the app when the intro movies were skipped). Now it waits for the compile, delivering interrupts between 2 ms waits so their queue doesn't overflow (`Init` in `gx_metal.mm`).
- **Less memory traffic at high resolutions:**
  - Frame textures are GPU-private.
  - A pass whose depth the next pass clears doesn't store it.
  - Mipmaps for the display are made only while the window shows the frame smaller than it was rendered.

**Measuring:** `FRONTLINE_PERF=1` prints a line a second:
- frame rate and frame time;
- the CPU's share, and the GPU's busy time (overlapping frames counted once);
- the waits for the GPU and for the retrace;
- draws, triangles and vertices;
- texture hashes and decodes;
- draws still on the ubershader, and pipelines compiling.

`FRONTLINE_GX_HASH=1` logs a hash of every frame copy.

**Results:** headless, on an M5. The route is mission 1's first level (`route.py 0 0`), unpaused and walked down the trench, averaged over 60 s of play.

| Build | Classic | 1080p | 4K 16:9 |
|---|---|---|---|
| Debug, before | 43.8 fps (dips to 30); CPU 14.2 ms, GPU 4.7 | 43.0 fps (27); CPU 13.0, GPU 9.6 | 37.9 fps (20); CPU 14.8, GPU 18.2 |
| Optimized, overlapping, cheaper draws | 59.9 fps; CPU 4.7 | 59.9 fps; CPU 4.7 | 59.9 fps; CPU 3.8 |
| All of it | 59.9 fps; CPU 4.6, GPU 1.5 | 59.9 fps; CPU 5.0, GPU 2.4 | 59.9 fps; CPU 5.2, GPU 5.2 |

- **Before specialization, 4K had no headroom.** The same optimized build with `FRONTLINE_GX_SYNC=1` ran at 41.5 fps, with 20.5 ms of GPU work a frame. The overlap is what kept it at 60.
- **Specialization halves the GPU's time at 4K.** The final build with `FRONTLINE_GX_UBER=1` takes 10.7 ms; specialized, it takes 5.2 ms. The shape lookup adds about 1 ms of CPU.
- **1440p and 21:9:** 1440p runs at 59.9 fps (GPU 3.1 ms). 4K at 21:9 (5112 x 2376) runs at 59.9 fps (GPU 5.7 ms).

**Checks:**
- **Specialized against ubershader:** `FRONTLINE_GX_VERIFY=1` draws every draw both ways, into the EFB and into a shadow EFB, and compares each frame copy.
  - At 4K: 8,100 frame copies through movies, menus and play, all identical.
  - At Classic: 98% of 18,900 copies are identical. The rest differ by 1 (of 255) in 1–8 pixels.
  - That small difference isn't fused multiply-adds, fast math or the dropped interpolants: it stays with each of those turned off. It's most likely texture filtering, which the ubershader does inside run-time branches.
- **Settled screens:** byte-identical across the Debug build before, the ubershader and the specialized build.

### High frame rate (Rewritten; experimental, on the back burner)

**Status (2026-10-27):** tried on a 120 Hz monitor and not working as expected. Left in as an opt-in setting marked experimental; further work is on hold. Frame rate 60 stays the default.

The game still runs at 59.94 frames a second. On a screen that refreshes faster, the display shows up to 120: the setup screen's **Frame rate: Up to 120**, or `FRONTLINE_HFR=1` headless.

**Why the frames in between are replays.** Neither obvious way works:
- **A faster simulation** is out. Game time is whole retraces (`CScreen::Wait`), scripts and AI count frames, and several consumers truncate dt to an integer.
- **Running the game's drawing twice** is out too. `g_scene.Draw` moves the game on:
  - bones (hit volumes) are computed only in `CAnimObject::Draw`;
  - soldiers drop their weapons in `CSoldierObject::Draw`;
  - muzzle flashes and tanks take numbers from the gameplay random number generator;
  - thrown objects spin, and pop-up and HUD timers advance.

So the GX layer records each frame's draws, and the display draws them again with the camera moved on.

**Presentation (both modes).** The app presents from a `CAMetalDisplayLink` on its own thread (`apps/macos/Sources/Renderer.swift`) at the screen's rate, from 60 up to 120. Input stays on the main thread at the same rate.
- In Classic, each present shows the latest latched frame, so on a 120 Hz screen a new frame is shown up to 8 ms sooner. Nothing else changes.
- Frame latency is 1 refresh. At 2, the 59.94 Hz frames land unevenly on a 60 Hz screen, some shown twice and some never. `FRONTLINE_FRAME_LATENCY` tries other values.
- `vi.cpp` gives the displayed frame its retrace's exact due time (`latchTime`), read in one atomic with its frame buffer.

**Recording** (`platform/src/gx/gx_record.{h,mm}`, `gx_metal.mm`):
- **What a draw keeps.** It is recorded as the backend made it:
  - clip-space vertices and indices, in 16 MB chunks shared by the records that use them;
  - the pipeline, depth state, cull, viewport and scissor;
  - the fragment uniforms (only when they change) and textures (retained);
  - the projection, and a draw class.
- **Passes.** These carry the pending clears, so the weapon's mid-frame depth clear and the frame's own clear replay as on the console.
- **Frames.** A frame copy tags its frame buffer with the frame being recorded. The frame ends at `GXDrawDone` right after a copy (the split-screen viewports' mid-frame `GXDrawDone` comes without one). Its record is then finished and becomes what that frame buffer shows (`XfbRecord`).
- **Nothing is drawn on the game thread.** No EFB textures are made there; the display has its own. The game thread's GPU time is gone, and with it its Metal encoding.
- **Checks that still draw here.** `FRONTLINE_GX_VERIFY`, `FRONTLINE_GX_HASH`, `FRONTLINE_GX_SNAP` and `FRONTLINE_HFR_VERIFY` draw on the game thread as well as recording.

**Replaying** (`ReplayFrame`, called from `display.mm`):
- Each refresh draws the displayed frame's record into the display's own EFB and copies the frame's rectangle out, with mipmaps when the window shows it smaller.
- The vertex shader's mode 1 (`gx_shader.cpp`) moves a recorded position:
  - back to eye space through the projection it was made with (exact: the 6 terms are invertible);
  - through a 3x4 move;
  - and through the projection again, with its scales zoomed.

  Mode 0 is today's pass-through, so Classic's output is untouched.
- **Draw classes** (`NativeDrawClass` in `native/hooks.h`):
  - **world:** the default;
  - **sky:** the sky bin, turned but not moved;
  - **viewmodel:** the weapon bin, after its depth clear; it isn't moved;
  - **screen:** orthographic draws (HUD, text, fades, movies); they aren't moved.

**Moving the camera** (`platform/src/gx/frame_motion.{h,cpp}`, plain math with its own test):
- **The cameras.** A record keeps its camera (`NativeFrameView`, from `CScene::Draw`) and the frame before's.
- **Extrapolated, not interpolated.** For a refresh `ticks` retraces after the frame was latched (at most 1.5), the camera's position and its turn about the world are carried on at the last frame's speed, and so are the projection's scales (zoom). Interpolating between the last two frames would add a whole retrace of delay; `FRONTLINE_HFR_INTERPOLATE=1` does it, for comparison.
- **The mouse.** While the mouse turns the view (`NATIVE_VIEW_LOOK`), the turn comes from the mouse movement the game hasn't drawn yet instead of being carried on. That movement is the input takes since the frame's camera, plus what hasn't been taken (`PendingLook` in `pad.cpp`).
  - It is scaled as `AddMouseLook` will scale it, and kept within the game's ±1.45 pitch.
  - The game still takes all of it at its tick, so shots go where the screen pointed.
  - `FRONTLINE_HFR_TRACE=1` confirmed the signs: the game's yaw is the heading, and its pitch is minus the elevation.
- **Left as the game drew them:**
  - views carried by something (`m_attached`: the landing craft, cutscenes), except for the mouse;
  - mounted guns, which turn with the view;
  - split screen (more than one camera);
  - a jump of more than 1.5 m or a turn of more than 0.6 rad in a retrace.
- **Culling.** Frames are culled with the game's camera, so a turned camera would show gaps at the edge. With the high frame rate on:
  - the culling frustum is widened by 2.5 times the last retrace's turn plus 1.7° (`NativeCullTan` in `CFrustum::Set`);
  - every sky face is drawn.

  **Side effect:** objects just off-screen count as drawn and get their bones updated, a small change to the game in Rewritten.

**Game hooks** (shared source, all under `FRONTLINE_NATIVE`, GameCube build unchanged):
- `scene.cpp`: `NativeFrameView`.
- `mainloop.cpp`: `NativePlayerView`, single player only: attached, on a gun, mouse look, the mouse's zoom scale and the pitch.
- `renderlist.cpp`, `skybox.cpp`, `dmesh.cpp`: `NativeDrawClass`.
- `camera.cpp`: `NativeCullTan`.
- `skybox.cpp`: all faces drawn.

**Checks** (headless, release build, M5):
- **Replays match.** `FRONTLINE_HFR_VERIFY=1` draws each frame on the game thread too and compares it with its unmoved replay. 12,600 frames were all byte-identical: intro movies, menus, the D-Day landing and play at 1080p 16:9.
- **Math.** `tests/hfrtest` covers the math: walking, turning, slow frames, cuts, interpolation, the mouse with its pitch limit, and zoom.
- **Pictures.** `FRONTLINE_HFR_DUMP=<folder>` writes each second's frame at 0, ¼, ½ and ¾ of a retrace on. On the beach while turning, the world turns a little further each quarter, while the HUD, crosshair and rifle stay put, with no gaps at the edges.
- **The app** runs with replays on its real display path. On this 60 Hz screen that is 60 presents a second, about 0.1 ms to encode a menu frame.

**Cost** (headless, `FRONTLINE_HFR_RATE=120`, D-Day while walking and turning):

| | Game thread | Display: per present |
|---|---|---|
| 1080p 16:9 | 59.9 fps, CPU 4.2–4.9 ms, no GPU | 120 presents/s; encode 0.8–1.4 ms, GPU 1.6–3.7 ms (about 4,500–5,200 draws) |
| 4K 21:9 | 59.7–60.1 fps, CPU 5.5 ms, no GPU | 120 presents/s; encode 0.6–0.7 ms, GPU 6.1–7.5 ms of the 8.3 a refresh allows (about 5,200 draws) |

- **4K 21:9 at 120 Hz is close to the M5's limit.** The headless display waits for each present's GPU work, so its sampling of the retraces drifts: some game frames are shown once and others three times, where 1080p shows nearly all twice. The camera stays smooth (each present places it for its own time), but anything that moves only with the game's frames steps unevenly.
- At 1080p, the irregular "retraces shown" counts come once a second, from the debug dumps (`FRONTLINE_HFR_DUMP` and the headless frame dump), which stall the offscreen display.

**Fixes after the first test on a 120 Hz monitor (2026-10-27: minor stutters, and unrendered edges on fast turns):**
- **Frame timing (`vi.cpp`).**
  - The VI now keeps the retrace that latched the frame shown. Before, a missed retrace restarted the extrapolation and the camera jumped back.
  - The display shows a flushed frame as soon as its retrace is due. Before, it waited for the interrupt and extrapolated the old frame a retrace further.
  - Both are read in one snapshot, under a sequence lock.
- **Steps.** The change between frames is divided by the game's own step (`NativePlayerView` counts retraces).
- **What's carried on.** A turn or move is carried on only while it keeps the direction of the frame before. Turns slower than 0.004 rad a retrace aren't carried on, so camera shake and head bob aren't thrown further.
- **Mouse.** Mouse movement reaches the platform as it arrives (`FrontlineAddMouseLook`), not once per main-thread tick.
- **Culling.** The margin comes from the mouse movement not yet drawn, plus 3 times its recent top rate (fading over about half a second), plus a 0.06 rad base. `NativeCullTan` now takes the axis.
- **Measures.** `FRONTLINE_PERF` reports, each second:
  - frames shown once, twice or more;
  - uneven and backward camera steps, for turning and moving;
  - presents turned past the culling, and by how much.

  `FRONTLINE_HFR_TRACE=2` logs every present.
- **Results** (headless 1080p, D-Day):
  - nearly every frame is shown exactly twice;
  - backward moving steps went from about 15 a second to 0–4;
  - the remaining turning back-steps were the game's own camera shake;
  - nothing is drawn past the culling in steady turns, walking or 6 rad/s flicks. Only 8 rad/s left-right flicks from rest overran it, 0–3 times a second, by up to 5°.

**Limits:**
- Characters, props, particles and tracers still move in 60 Hz steps; only the camera moves in between. Next: per-object motion, matched by an ID the game passes down (`&m_tm` for static objects, `m_cluster` for characters).
- A fixed-rate 144 or 165 Hz monitor capped at 120 may get 72 Hz or uneven pacing. Set it to 120 Hz, or use variable refresh.
- A record that starts without clearing color starts from the last replay's picture: on the console the EFB keeps the last frame, and the sky covers it in levels.
- Not yet tried on a 120 Hz screen: pacing, smoothness and how the mouse feels need the user's monitor.

### Enemy AI (Rewritten; done, not yet playtested)

**How the original works.** The soldiers' decisions are scripts, not C++.
- Every German runs `SoldierA_Roamer.cbs`: 187 KB, the same in 19 levels, three threads of 79, 998 and 18 states (Alert, Attack, AdvanceToCoverPoint, AttackFromCoverPoint, ChargeTarget, the mounted-gun states, ...). `SoldierB_`/`SoldierC_Roamer`, `Defender`, `Guard`, `Platform`, `StayOnPath` and the `_NoDodge` variants override a few of its states; snipers and machine-gun nests have their own scripts.
- Most decisions are random rolls and timers: the Roamer calls `GetRandomInteger` 300 times and `RegisterTimerEvent` 116 times.
- The C++ is the services those scripts call through built-ins (`bsbifunc.cpp`): sight and hearing (`CAIObject`), aim springs, A* over the level's path nodes, cover search (`CAIFilterGlobal`, `CCoverPoint`) and firing (`CAISoldierDoodad`).
- **Difficulty never touches the AI code.** It picks data: `weapbul_{easy,medium,hard}.dat` (on Hard the Germans' guns are tighter and hit harder: the NPC Kar98 does 3, 5 and 12 damage), the hit-box files, and the health the player carries into the next stage. Accuracy is a byte per placed soldier (`m_aiSkill`: inaccuracy 1.25, 0.75 or 0.333).

**What Rewritten changes.** The scripts stay as they are; their services change.
- **Where:**
  - hooks in `FrontlineDecompiled/include/native/ai.h`, each call inside `#ifdef FRONTLINE_NATIVE` and gated by `NativeAIOn()`;
  - the code in `game/native/ai/`: `state.cpp` (profiles, the per-object table, its own random numbers, tracing), `aim.cpp`, `cover.cpp`, `grenades.cpp`, `director.cpp`, and `ai_math.h` (geometry, tested without the game).
- **When:** Rewritten, single player. `NativeAILevelStart` (`mainloop.cpp`) picks the profile from the difficulty when the level loads.
- **Its own random numbers.** It draws from its own generator, seeded per level, so the scripts' rolls use the game's sequence as before.

**Profiles** (`kProfiles` in `state.cpp`, starting values to tune by playing):

| | Easy | Medium | Hard |
|---|---|---|---|
| Enemy inaccuracy / aim-time cap | x1.0 / original | x0.9 / 1.5 s | x0.7 / 1.0 s |
| Lead on a moving target | none | 40% | 80% |
| Reaction delays (target search, sounds) | x1.0 | x0.75 | x0.4 |
| "Player right there" radius (5 originally) | 5 | 6 | 8 |
| Accurate shooters at once (attack tokens) | 1 | 2 | 3 |
| Callout radius | 10 | 20 | 30 |
| Flankers in a fight of 3 or more | 0 | 1 | 2 |
| Advance in cover scoring; forward cover (type 0) | off | half; on | full; on |
| Enemy grenade cooldown; warning after a throw | 12 s; 0.8 s | 9 s; 0.5 s | 7 s; 0.3 s |

**The changes:**
- **Sight.** The original sets `m_targetVisible` when a target is picked, and only some script checks clear it, so aim followed the player through walls. Each soldier now checks every 0.2 s and aims where it last saw a hidden target.
- **Aim** (enemies of the player):
  - the profile's accuracy and aim time;
  - leading a moving target by part of its travel during the bullet's flight (the original computes the bullet speed but never uses it);
  - the aim error restarts when the target comes back into sight after a second or more, so the first shots at a player breaking cover miss and then close in;
  - soldiers without an attack token aim off the player by up to their initial aim error. They still shoot, which reads as suppression, but only a few are dangerous at once.
- **Friendly fire.** Soldiers hold fire while a squadmate stands in the line of a bullet (not of a grenade).
- **Perception.** Reaction delays scale. The proximity check's radius grows. The perception schedules poll more registrations at once, so checks don't fall behind when there are many soldiers.
- **Cover:**
  - the original's rules still pick the candidates; a score picks the winner: the walk there, whether the threat can see the stand spot (vision lines at crouching heights, kept 0.5 s), whether the peek spot can shoot back, crowding, the player's view, ground gained, and for flankers the angle round the player's side;
  - cover type 0 ("forward far"), which can never match in the original, means forward cover at least halfway to the threat (Medium and Hard);
  - the peek side claimed is one that sees the target, and the peek position is that side's (the original always used the first side the cover allows);
  - a soldier the target has been able to see at its cover for 1.5 s is told the cover is lost (event 0x3E), so it moves.
- **Running away** (`AISetRunAwayMovepoint`): a breadth-first walk over the path nodes for the nearest spot far enough from the danger, preferring one the player can't see. The original's greedy walk is the fallback.
- **Grenades:**
  - each grenade's landing spot is predicted while it flies, and after the profile's delay every soldier near it gets the original's events (0xAC/0xAD for the closest, 0xAE for the others), including ones who walk up later; running away uses the predicted spot;
  - throws try 30, 45 and 20 degrees along 8 segments each, and the grenade leaves at the slope that was clear (the original tries one arc with two line tests, and ignores its angle argument);
  - one enemy grenade at a time, with the profile's cooldown.
- **The squad director** (once a frame, after the AI updates):
  - callouts: squadmates near a soldier who sees the player, and not fighting yet, hear a noise at the player's position after a moment;
  - attack tokens go to the closest soldiers who see the player and pass on every 4-5 s;
  - in a fight of three or more, the Roamers furthest off to the side flank, and the rest hold cover that sees the player;
  - near misses: a soldier the player's bullet passes close to hears where it came from.
- **Original bugs fixed in Rewritten:** `UpdateReactionTime`'s reversed test; tank aim raised for a crouching target; the proximity check's uninitialised field of view; `ChooseTarget` keeping a dead or long-hidden target over a visible one.
- **Left alone:** event 0x7C ("soldier fired", never raised) and the cover "high" events, whose script paths never ran.

**Testing:**
- `FRONTLINE_AI=off|easy|medium|hard` (off, or a profile whatever the difficulty), `FRONTLINE_AI_TUNE=lead=1,tokens=4` (profile settings by name), `FRONTLINE_AI_TRACE=1` (a line per decision on stderr, and the line tests a second).
- `tests/aitest` checks the geometry: the original grenade solver against the game's own integration, the 45- and 20-degree throws landing on the same spot, the arc, segment distance and the tune parsing.
- **Checked so far (2026-10-26):**
  - The GameCube build: `regress.py` reports 0 dropped and 0 missing.
  - Headless, optimized build, Hard:
    - D-Day's Your Finest Hour for 330 s, landing and beach. No Roamers there yet; the beach's shooters are scripted guns.
    - A Storm in the Port's Seaside Stowaway for 330 s, walking and shooting among SoldierB_Roamers and Allied soldiers. The trace shows 114 scored cover picks (46 for flankers), token hand-offs, flank roles, callouts, aim restarts after lost sight, friendly-fire holds, exposed-cover moves, and two enemy grenades 17 s apart. At most 51 added line tests a second. No crash.
  - Classic on the same route: no AI lines, no crash.
  - Not measured yet: hits on the player and other numbers against Classic. That needs a fixed fight to compare.

**The scripts' own decisions** (`game/native/ai/script_bias.cpp`):
- **The disassembler.** `tools/bsdis.py` reads a class's `.sin` and `.cbs` from a level archive, read-only. It's also groundwork for Phase 4's script tools.
  - It prints states (level, parent, entry, event and message handlers), code with built-in names, strings and labels, the callers of a function, and a summary per state: its debug strings, the built-ins it calls, the states it moves to, and its rolls and timers.
  - `--check-sites` checks the bias table against a level; all 20 sites match in all 27 level folders.
- **What the Roamer's Logic thread decides** (79 states):
  - Attack (state 39) enters 40, which charges (59) on a 30/50/80% roll by the soldier's aggression byte (trigger 0x77), if it's allowed to charge.
  - Otherwise it takes cover by type: aggressive soldiers try forward cover first (type 0, then 1), cautious ones cover behind them (5, 4).
  - AttackFromCoverPoint (55), when the target has been out of sight for 3–6 s, charges on the same roll or goes back to searching.
  - A grenade close by picks one of throwing it back or warning, diving on it, ducking, or running, weighted by aggression. One by a squadmate makes the soldier run unless a 10/25/40% roll says stay.
- **What the Anim thread decides** (998 states): at cover, each pop-out fires 1–5 cycles of an aiming animation, and each wait behind cover lasts 2–10 cycles of the crouched idle.
- **How it's biased.** `GetRandomInteger` and `RegisterTimerEvent` ask `NativeAIScriptRandom` and `NativeAIScriptTimer`.
  - The call site comes from the interpreter's instruction (`NativeBSInstruction`) and the running thread's code segment. The segment says which class's code it is, along the base classes, so subclasses running the Roamer's code are covered too.
  - A listed site is adjusted for enemies of the player, and only when its arguments are the listed ones.
  - The other 400-odd rolls (mostly animation variants) keep their value. No new random numbers are drawn.
- **The sites, by profile (easy / medium / hard):**

| Site | Effect | Factor |
|---|---|---|
| Charge chance (t0 0x30B) | roll pulled low: Hard turns 30/50/80% into about 43/71/100% | 1 / 0.85 / 0.7 |
| Stay put by a squadmate's grenade (t0 0x328) | roll pushed high: more running on every difficulty | 1.25 / 1.5 / 1.75 |
| Target out of sight at cover before charging or searching (t0 0x220F) | duration | 1 / 0.8 / 0.6 |
| Off cover after one it couldn't reach, 15 s (t0 0x238A) | timer | 1 / 0.8 / 0.6 |
| Off charging after failing to reach the target, 10 s (t0 0x24C3, 0x25C7) | timer | 1 / 0.75 / 0.5 |
| Fire bursts from cover (9 sites in t1) | loop count | 1 / 1.25 / 1.5 |
| Waits behind cover (5 sites in t1) | loop count | 1 / 0.8 / 0.6 |

- `FRONTLINE_AI_TUNE=scriptbias=0` leaves the scripts' decisions alone.

**Still to do:**
- Playtesting on each difficulty, and tuning the profiles and the bias factors.
- The close-grenade reaction is a weighted roll with "dive on it" between "throw back" and "duck", so shifting the roll can't remove it. Making soldiers smarter there would mean changing the weights the script passes, not the roll.

### Enemy drops at death (Rewritten)

The ammo or health an enemy drops now appears the moment it dies. In the original it appeared only once the death animation ended.

- **How the original does it.** The soldier scripts mark the soldier dead (`AISetStatusVar(13, 15)`) as they enter a death state. The drop is `CreateDumpObject`, called once the animation is over.
  - In SoldierA_Roamer (and the subclasses that run its code): death states 26, 67 and 70, which drop in 28, 68 and 72.
  - In SoldierA_MachineGunNest: states 94, 104 and 115.
- **What changes** (`game/native/ai/drops.cpp`). At those call sites, for enemies (trigger team 2), the drop is made right away, at the soldier's position. The script's later `CreateDumpObject` for that soldier is skipped.
- **Left as they were:** deaths that never drop (a scripted kill by message goes to state 73, which removes the body without a drop), and everything else that drops things (crates, barrels, bottles).
- **Mode:** Rewritten, single player. It doesn't depend on `FRONTLINE_AI`.
- **Trace:** `FRONTLINE_AI_TRACE=1` logs each early drop and how long after it the animation's drop was skipped.

### Bodies stay (Rewritten)

The dead no longer fade away after a few seconds; their bodies stay where they fell for a minute and a half, then fade out.

- **How the original does it.** A dying soldier's script calls `FadeOut` (6–8 s in SoldierA_Roamer's state 76), then `DestroyObject` once the fade is over. `BSGO_Soldier::Destroy` gives back the soldier's slots: the live-soldier count that spawners wait on (`commondata.crp`'s limit), its animated object (the factory has 26 slots in single player), its script, AI, weapons and volumes. The D-Day beach GIs and many other scripts fade the same way.
- **What changes** (`native/corpses.h`, `game/native/corpses.cpp`).
  - `FadeOut` still stops the body, but it doesn't fade yet (`NativeCorpseFadeOut`).
  - At `Destroy`, a soldier that was fading is kept as a body. The count, script, AI, weapons, hit volumes and the helmet object are freed as before (the helmet's mesh stays on the head as a bare mesh). Only the animated object is kept, with copies of its two bounding boxes. It is drawn after the scene's nodes, frozen on the death animation's last frame, with no collision.
  - 90 s after `FadeOut`, the body fades over 4 s, using the original's fade in `CSoldierObject::Draw` (run by `NativeCorpsesUpdate` from `CScene::Update`, so it pauses with the game). Then its slot is released.
    - The first version kept bodies until the cap, and the user found it lagged badly as they built up (2026-10-30), so the 90 s hold was added.
  - The factory gets 64 extra slots for bodies, so the living get as many as ever. When they run out, the oldest body out of view is released.
  - Soldiers removed alive (out of sight, to make room under the soldier limit) and scripted removals (state 73) still vanish.
- **Animation matrix caches** (both modes, natively). The skinned and morph caches had 16 slots, and the GPU reads a slot's matrices only when the render list is drawn. With more characters in a frame than that, earlier ones draw with later ones' bones. With 20 or more bodies in view, the rifle vanished. Natively both caches now have 128 slots.
- **Mode:** Rewritten, single player. `FRONTLINE_CORPSES=off` turns it off.
- **Checked** headless on D-Day (Easy, 270 s): 87 bodies kept, the 64 cap reached and recycled, up to 30 drawn in one frame. The rifle stays visible, and bodies lie in their death poses. GameCube build unchanged (`regress.py`: 0 dropped).
  - With the 90 s hold (D-Day, 300 s): 45 bodies faded and were released. The count still levels off at about 60 there, since that many GIs die on the beach within 90 s; other levels should keep far fewer.
- **Cost:** each body in view is re-skinned and redrawn every frame, like a live soldier, though it never moves. Caching a body's skinned pose would make bodies much cheaper if they still cost too much.

### Pause in the background (both modes)

The game pauses itself when the app goes to the background or its window to the Dock. Before, it kept running with every key let go, so a player who switched apps mid-fight came back dead.

- The app (`GameView.swift`) sends one press of `NATIVE_ACTION_PAUSE` (`FrontlinePressAction`, `frontline::PressAction` in `pad.cpp`) on `NSApplication.didResignActiveNotification` and `NSWindow.didMiniaturizeNotification`, only while the player is playing.
- The game takes it like Esc: `input.cpp` maps the pause action only while `Playing()`, so it does nothing in menus, during the D-Day intro's states or the death fade. The pause menu pauses the music and effects as always.
- Not on losing the key window, so the Settings window can be used while playing (the field of view changes live).
- Settings: "Pause when Frontline is in the background" (`pauseInBackground`, on by default).

### Display stays awake (both modes)

A game controller's input doesn't reset the system's idle timer, and long stretches play without any input (D-Day's landing), so the display dimmed and slept. `Power.swift` (`DisplayAwake`) holds an `IOPMAssertion` (`PreventUserIdleDisplaySleep`) while the game runs and the app is active, and lets it go in the background. `pmset -g assertions` shows it.

### Soft limiter (Rewritten)

Rewritten's mix goes through a soft limiter instead of the DSP's hard clipping (D-Day's beach overloads the output when explosions land on music and dialogue). Classic keeps the clipping.

- **Where:** the main bus's final clamp to 16 bits in `audio.cpp` (`RunFrame`). `audio_out.cpp` only receives samples already clipped, so the limiter can't go there.
- **How** (`platform/src/audio_limiter.{h,cpp}`): a stereo-linked peak limiter with 1.5 ms of lookahead, a -0.5 dBFS ceiling and an 80 ms release.
  - Each frame's need is ceiling / peak; the gain takes the smallest need over the lookahead, recovers exponentially, and is averaged over the lookahead, so it has reached each peak's need when that peak comes out.
  - While nothing nears the ceiling the gain is exactly 1: samples come out unchanged, 47 frames (1.5 ms) late. The release runs in double precision; in float its last small steps stalled short of 1.
- `FRONTLINE_LIMITER=off` keeps the clipping for comparison; `FRONTLINE_AUDIO_TRACE=1` also counts the frames turned down.
- **Tests:** `tests/limitertest` checks quiet input comes out bit-identical (only delayed), loud input never passes the ceiling or clips, the channels share one gain, the gain is down before a peak and back up over about 80 ms.

### Field of view (Rewritten, live)

The view's width can be set from 70° (the original, across at 4:3) to 110° in the Settings window, while playing. Classic stays at 70°.

- **The game:** `NORMAL_FOV` (`include/game/player.h`) is the normal view's half-angle: 35 on the GameCube, `NativeBaseFov()` natively (35 in Classic). The player's view returns to it after zooming, it starts there, and the mouse's zoom scaling and the high frame rate's look scale measure against it (`player.cpp`, `mainloop.cpp`), so mouse sensitivity at the hip doesn't change with it.
  - Zooms keep their own angles: scopes (5–35), iron sights (the weapon's own), binoculars.
  - The weapon in hand is drawn at the original's angle at most (`player_weapon_object.cpp`), so a wider view doesn't shrink the gun; zooms narrower than that still apply.
  - Culling follows, through `CCamera::SetPerspective`. Widescreen widens it further (Hor+): 90° at 4:3 is about 106° across at 16:9.
- **The platform:** an atomic in `settings.cpp` (`SetFieldOfView`), set by the app each frame (`Renderer.step`), so moving the slider changes the view at once (the game eases to it at its zoom rate). `FRONTLINE_FOV=<degrees>` sets it headless.
- GameCube build unchanged (`regress.py`: 0 dropped).

### Key remapping (both modes)

The Settings window's Keyboard and mouse section rebinds the playing actions: up to two keys or mouse buttons each (the side buttons too). Click a slot, then press the key or button; Esc cancels, Delete clears. A key taken from another action says so. Reset to Defaults goes back to the original layout.

- **Actions:** move (4), walk, jump, crouch, action, fire, aim, reload, melee, next and previous weapon, center view, hint, frag counts and pause. Previous weapon is new on E; center view starts unbound. The menu keys (arrows, Return, Esc/Delete, Space, X Y Z L R) and the wheel stay fixed.
- **Where:** `Bindings.swift` (the actions, defaults, saving to the `keyBindings` default, names), `BindingsView.swift` (the Settings section). Keys are captured with `GCKeyboard`'s handler, so they're the same codes the game's input reads; Command and Delete can't be bound.
- **Applies at once:** `Input` rebuilds its tables when the saved bindings change, and hands the game the new names (`publishKeyLabels`). Prompts and menu icons are made as a screen or level loads, so they name the new keys from the next one. `NativeKeyLabel` hands out a copy, since the names now change while the game runs.
- **Prompt length:** a prompt shows 49 characters, and the longest one with an action in it ("Press F to Wear Uniform and Conceal Weapons") has room for a name of 7. Longer names ("Right Mouse Button", "Keypad 5", "Caps Lock") have short forms ("RMB", "KP 5", "Caps") that `prompts.cpp` uses when the full one won't fit. `tests/datacheck` checks every rewritten prompt with each long name.

### Changing levels in place (both modes)

The game no longer relaunches the app between levels. The window, fullscreen and focus stay, and so do the Metal device with its compiled pipelines and the sound output; a level change is a moment of black screen, as on the console.

**Why it relaunched.** The game leaves a level by booting a stub program (`LoadElf::RunModule`) that boots the game again: everything starts over except the block LoadElf keeps. Natively that was an `execv` of the whole process.

**The game image.** The game (the decomp's units and `game/native/`) is now its own Mach-O bundle, `FrontlineGame.so` (`game/CMakeLists.txt`):
- The app loads it from `Contents/Frameworks` and headless from the build tree (`frontline::LoadGame`; `FRONTLINE_GAME_IMAGE=<path>` overrides). Its static initializers run there, on the main thread, as before main() used to.
- It calls the platform layer in the executable, which links all of it and exports it (`frontline_game_host`). The platform calls only two functions in the image, `FrontlineGameEntry` and `FrontlineGameFree` (`game/native/entry.cpp`).
- Its `operator new` and `delete` stay its own: the platform layer and the app use the system allocator everywhere now.
- The 32-bit address map covers both images: the game's at `0x80000000`, then the executable's (the game keeps pointers to platform statics such as the default thread's `OSThread`). See ADR 0001.
- The Xcode build's hardened runtime needs `com.apple.security.cs.disable-library-validation` (`FrontlineMac.entitlements`) to load an ad-hoc signed image.

**The reboot** (`platform/src/os/reboot.cpp`, ADR 0002):
- **Snapshot:** `StartGame` first saves the game image's writable segments (2 MB), the arena's non-zero pages (1.4 MB), the OS heaps and arena bounds, and the default thread's `OSThread`: the state the game's static initializers left.
- **Reboot:** `RunModule`'s thread leaves without handing the baton on. A host thread ends every game thread, drops the work in flight (DVD reads finish first; alarms, VI and PAD callbacks, AX voices, ARAM allocations and the decoded textures go), clears the interrupts, maps fresh arena pages and copies the snapshot back into them and into the image, and starts `main()` on a new default thread. The LoadElf block stays where it is. It takes about 5 ms.
- Host threads (retrace, alarms, audio frames, disc) keep running; the display shows black meanwhile, as the game set it.
- `FRONTLINE_RESTART_EXEC=1` relaunches as before; `FRONTLINE_REBOOT_TRACE=1` prints the snapshot's size and each reboot's time.

**Autorelease pools.** Game threads have no run loop, so Metal's autoreleased command buffers, encoders and pass descriptors were never released. Within one process per level that was bounded; rebooting in place it wouldn't be. `DrawDone` now drains a pool each frame. `ReadDisplayedFrame` (the frame dumps) has its own: each readback had kept its 1 MB pixel buffer.

**Checks** (headless, release build, D-Day on Easy):
- Abort Mission from the pause menu reboots in place (about 4.5 ms) to the post-level screen. Replay Level loads the level again and plays it. Three reboots in a row ran in one process.
- That screen matches the exec restart's pixel for pixel, except the menu icons: after the in-place reboot they follow the device used last (a controller here), where a new process started on the keyboard's.
- Memory stays flat: 160-170 MB in each level, about 120 MB at the shell after each reboot. Before the pool fixes, with frame dumps on, it grew 1.1 MB a second.
- The app boots from the image in its Frameworks folder.
- The GameCube build is unchanged (`regress.py`: 0 dropped).

## Next

1. Playtest with mouse and keyboard and list the game behavior issues left from Phase 1.
2. ~~The mode switch~~: done (the setup screen).
3. ~~Key remapping in Settings~~: done.
