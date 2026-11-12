# What's new

Fixes marked *not yet confirmed* were tested in scripted runs, but not yet confirmed in play.

**2026-11-12**

- **Render resolutions, AMD FSR 1 and ultrawide are now in the main version:** the `upscaling` branch was merged
  into `main`. Everything it added is off unless you choose it (the default is still 1600x900): render resolutions
  from 854x480 to 4K, AMD FSR 1, 21:9 to 48:9 with the HUD at the screen's edges and the HUD layout page (**F2**).
  See [Experimental options](playing.md#experimental-options). To update an `upscaling` install, unzip the `main`
  zip over it (or `git switch main`); saves and settings are kept. In a replay of one recording at the default
  settings, the picture and the frame rate were the same before and after the merge.
- **Frame rate display: choose its corner** (issue #30; *not yet confirmed in play*): **Settings > Display > Frame
  rate: corner** in the launcher, and in the in-game menu (**F1**, and on the HUD layout page, **F2**): top left (as before), top
  right, bottom left or bottom right, for the plain counter and the detailed panel. It moves at once.
- **Ultrawide: Legion wheel no longer doubled** (issue #29; confirmed in play): with **Ultrawide: HUD
  at the screen's edges** on, holding Y showed a second copy of parts of the Legion wheel (the ring that follows the
  stick, at times the cube in the middle) at the right of the screen. Those parts were taken for the HUD's side
  panels and moved to the edge; they now stay with the wheel. Seen fixed in a replay of the report's recording at
  3840x1080.
- **Ultrawide: Legion skill cooldown rings move with the skill panel** (issue #32; *not yet confirmed in play*): with
  **Ultrawide: HUD at the screen's edges** on, the ring that counts down a Legion skill's cooldown stayed where the
  HUD is at 16:9 while the rest of the bottom right panel was at the screen's edge. The rings are turned like the
  panel IRIS shows for a scanned person, which stays in place, and were taken for it; they are now told apart by
  where they are. In a scripted run at 3840x1080 (ZL held, X) the ring was at the edge with its panel, and IRIS's
  panel was still next to the person scanned.

**2026-11-11**

- **Saves and settings made safer, and several stops and freezes addressed** (from a review of the code; *not yet
  confirmed in play*). Nothing moves: saves, settings and game data stay where they are, and the next **Play**
  rebuilds the game once as after any update.
  - **Save backups never hold a half-written save:** the automatic backup next to the saves (`...-snapshots`) is now
    taken only while the game is not writing, and thrown away and taken again if the game changed its save during
    the copy. Before, a backup taken in the middle of the next save could be kept as complete.
  - **`userdata\launcher.ini` is not lost or overwritten:** the in-game menu no longer writes a new file when it
    cannot read the old one (which dropped key bindings and the other settings); the launcher keeps what the
    in-game menu saved while it was open, and writes the file in one step. The in-game sliders save when let go, not
    at every step.
  - **A folder that was copied or moved is built again** on the next **Play**. Before, a copy kept reading from, and
    **saving into**, the folder it was copied from, and a moved folder did not start (exit code 31).
  - **Folder names with letters outside English** (for example `C:\Users\Jörg\...`): the game no longer closes at
    every start with exit code 31.
  - **A rare freeze or stop in the game's thread locks** addressed: two places changed a lock's state in two steps
    while the game changed it too.
  - **A fast mouse can no longer stop the game** (`host-message-budget`) after a long stall.
  - **Setup:** a folder path with `[` or `]` is reported at the start instead of failing later; the GPU warm-up's
    time limit now works when it hangs; a failed **BC7 textures** preparation no longer blocks **Play**; a build
    cancelled at the wrong moment no longer fails every later build.
  - **Reports from a downloaded zip say which version it is** (a `VERSION` file filled in when the zip is made).
- **Detailed frame rate panel** (issue #28; *not yet confirmed in play*): **Settings > Display > Show the frame rate**
  (also in the in-game menu) is now **Off / Frame rate / Detailed**. Detailed shows a frame time graph, the 1% and
  0.1% lows, GPU and CPU load, video and system memory, the GPU's temperature, power and clock, the GPU's busy time
  and the waits per frame, and the window, picture and 3D render sizes. Temperature, power and clock come from the
  graphics driver's own library (NVIDIA, AMD; nothing extra is downloaded); Intel GPUs show the rest. Tested on NVIDIA
  only.
- **Quit game** in the in-game menu, and **Swap A and B** for controllers (PR #23, by red-Geck0; confirmed in play):
  see *In-game menu* and *Controllers* under [Playing](playing.md).
- **Sharp picture with Windows display scaling** (by red-Geck0, from issue #25; *not yet confirmed here*): with
  scaling above 100% (common on laptops and handhelds), Windows gave the game a smaller screen and stretched the
  result, so the picture was blurry. The game now uses the screen's real pixels. In a window it opens at 1600x900
  real pixels, so with scaling the window looks smaller than before.
- **Launcher background** no longer tiles when Windows enlarges the launcher after it opens (PR #24, by red-Geck0).
- **Ultrawide: HUD at the screen's edges** (issue #27; experimental, confirmed in play):
  **Settings > Display**, or the in-game menu (changes at once). With a render resolution wider than 16:9, the HUD's
  side panels (health, minimap, objectives, tutorials) move to the left and right edges instead of staying in the
  middle 16:9 area; markers over characters, subtitles and menus stay where they are. Off by default.
- **HUD layout (F2)** (issue #27; confirmed in play): in the game, **F2** (or the button under
  the option in the F1 menu) opens a window to place the HUD on a screen wider than 16:9: for each corner's panels
  (top left, bottom left, top right, bottom right) how far they move toward the edge (0-100%) and up or down, with
  the game drawn behind it so each change shows at once. Saved with the other settings. The first time the HUD shows
  on a wide screen, a line at the top says F2 is there (until you have opened it once).
- **Ultrawide fixes** (confirmed in play on a 32:9 recording): the light blocks above (issue #20); markers
  on characters and targets stay on them instead of drifting toward the middle (issue #21); menus, subtitles, the
  opening credits, the title screen and videos are shown in a centred 16:9 area instead of stretched, while
  full-screen fades and backdrops still cover the whole width (issue #22).

**2026-11-10**

- **Lights missing in blocks of the screen fixed** (issue #20; confirmed in play): dark, stair-stepped blocks, for
  example on the side of the truck on the street after the highway chase, at any screen shape.
- **Input recorder** for reporting problems: **Settings > Troubleshooting > Record controller input** records your
  buttons and sticks from launch with a copy of your save, into a folder next to your save. Zip it and attach it to an
  issue, and the problem can be played again on another PC.
- **Frame rate counter**: **Settings > Display > Show the frame rate** (see 2026-11-11 for the detailed panel).

**2026-11-09**

- **Much less processor use** (*not yet confirmed in play*): three of the game's worker threads each kept a core busy
  doing nothing; they now really sleep. Highway chase on the test PC: 4.6 busy cores down to 0.8, frame rate the same
  or slightly better. Mostly matters on laptops (less heat and power).
- **For GPUs with 4 GB of video memory** (*experimental, not yet tested on a 4 GB GPU*): **Settings > Shaders and
  performance > Keep the game's memory copy in system RAM** (issue #17).

**2026-11-08**

- **Title screen** (*not yet confirmed in play*): **"Port by Puddz"** above the version number (on by default;
  **Settings > Display**, or the in-game menu, turns it off), and an option for the twin on the roof: the game shows
  the twin you did *not* choose; **Settings > Display > Title screen shows the twin you play as** (off by default)
  shows yours. See *Title screen* under [Playing](playing.md).
- **All the settings explained** in one place: [docs/settings.md](settings.md).
- **The game window comes to the front when it opens** (issue #15; *not yet confirmed*): the game reads keys and
  controllers only while its window is the active one, and it used to open without asking to be (a controller doing
  nothing until a click; in fullscreen, no start prompt).
- **Installing works when Python cannot check a download's certificate** (issue #16; *not yet confirmed*): setup
  then downloads its tools with Windows' own downloader (each still checked against its SHA-256).
- **Saves are flushed to disk** when the game closes a save file and when it commits, and **save backups are kept only
  when complete** (*not yet confirmed*).

**2026-11-06**

- **Smaller render sizes for slow GPUs** (issue #4): 1280x720, 960x540 and 854x480 in **Render
  resolution**. The 3D view is drawn at that size and the game scales it up, as its own dynamic resolution does; the
  HUD and menus stay sharp. It only lightens the graphics card's work: a slow processor is not helped. Highway save,
  RTX 4090, GPU time per frame compared with 1600x900: 720p -6%, 540p -20%, 480p -25%; a weaker GPU should gain
  more, as more of its time goes on pixels. Handheld mode keeps its 1280x720 and ignores the smaller sizes. *Not
  yet confirmed in play.*

**2026-11-04**

- **Controllers other than Xbox pads** (issue #10): Switch Pro Controller, DualSense, DualShock 4, 8BitDo and most
  pads SDL knows now work, through SDL3 (setup downloads it). Buttons follow their position, as on the Switch. See
  *Controllers* under [Playing](playing.md). Switch 2 controllers are not supported yet. *Not yet confirmed with a real
  non-Xbox controller* (tested with a virtual SDL pad: every button, trigger and stick direction arrived).
- **Every connected Xbox-style pad is read** (PR #13, by lcd1232), not only the first one found: Steam Input's
  virtual pad, or a pad plugged in after the start, used to be ignored until a restart.
- **Prepare graphics on a separate thread: hangs and crashes fixed**, seen at high frame rates (with the
  uncapped frame rate, the title screens run at 200+ fps): the list of GPU results waiting to be copied back was
  changed by two threads at once, and a fence wait could end before its work reached the GPU. In tests with both
  settings on, 25 of 25 runs through the title screens were clean (about a third hung or crashed before). *Not yet
  confirmed in play.*
- **Title screen flicker fixed** (reported in play: about every 10 s at 60 fps, nearly all the time uncapped): for
  a frame the background movie was drawn squashed into a band between black bars. The game had rewritten the
  vertices of its menu and movie quads by the time the GPU read them; small draws now take a copy of their
  vertices when the port handles them. Uncapped title screen, 25 s captured per run: 1-12 squashed frames per
  run before, none in 5 runs after, same frame rate (~186 fps); street walk unchanged (about 70 fps). Confirmed
  in play (2026-11-06).
- **Faster atomic memory operations**: each one the game makes checked its memory against every mapping table; the
  last region that passed is now kept until a mapping changes. Street after the opening helicopter scene, uncapped
  (RTX 4090 / Ryzen 7 7800X3D, 3 runs each): 67.0 -> 71.4 fps; at the street's start 55.6 -> 58.5 fps, slowest
  2 s 41.8 -> 44.4. *Not yet confirmed in play.*
- **Uncapped frame rate (experimental)**: **Settings > Game** in the launcher, or the in-game menu. Gameplay is no
  longer held to 60 fps; cutscenes stay at 60 (their cuts are timed for it), and it stops at 450 fps, where the 60 fps
  patch's game timing ends. Most places are limited by the CPU: on the street after the opening helicopter scene
  64-66 fps instead of 60 (RTX 4090 / Ryzen 7 7800X3D). The 60 fps patch scales game speed to the measured frame
  time up to 480 fps. Reported in play: it does not feel right above 60 fps, for about 10 fps more; being looked
  into.
- **In-game settings menu**: **F1**, or **Minus and Plus** together on a controller, opens it over the game, which
  pauses as under the Switch's HOME menu. Button icons, rumble strength, the mouse settings, fullscreen, upscaling
  and FSR 1 sharpening, 30 fps cutscenes and keeping the game running while minimized change at once; render
  resolution, handheld mode, presentation mode and the separate graphics thread from the next start. Changes are saved for the launcher too. Navigate with the mouse, the
  arrow keys or a controller (A selects, B goes back); Esc, F1 or Minus and Plus close it. Confirmed in play: F1
  opens it, the game pauses, navigation works, changes apply and the launcher shows them.
- **Xbox and PlayStation button icons**, and **automatic icons** (the new default): the game's button prompts
  follow what you last used, keys and mouse buttons for the keyboard, the buttons of an Xbox, PlayStation or Switch
  controller for a controller (by position: the right face button is Xbox B and PlayStation circle). The style can
  also be fixed in the menu or the launcher. *Not yet confirmed in play.*
- **Rumble**: controllers rumble when the game asks for it (it used to be dropped), through XInput for Xbox pads
  and SDL3 for the others. **Settings > Controller > Rumble strength** (0 turns it off). The game asks for weak
  rumble (made for the Switch's actuators); weak values are raised for PC pads' motors, as yuzu does. Stops when
  the window loses focus or the game closes. Confirmed in play (rumble works; an 8BitDo controller's buttons work).
- **Occasional stop at startup** (`host-shared-placeholder-reserve`), seen about once in 20 starts after the
  controller change: Windows had placed one of the program's own thread stacks where the game then put one of its
  thread stacks. The game now places its memory around such allocations. Forced in a test (a large block reserved
  there on purpose): the game stopped without the fix and played with it.
- **Mouse support, keyboard and mouse button icons, two bindings per action** (on main since 2026-10-25). Click in the game window to give it the mouse: it turns the camera, left click attacks (ZR),
  right click calls the Legion (ZL), the wheel changes weapons. Hold Alt to get the cursor back. Sensitivity,
  invert and the button icons are in **Settings > Mouse and keyboard**. New default keys: Shift sprints, Space
  dodges, Tab is IRIS, Esc the pause menu (the old keys stay as alternatives). Keys you had changed in Controls
  stay on their actions; a new default that would clash with one of them is left empty.

**2026-10-27**

- **In-game camera**: the camera never locked on to anyone, so no name label appeared and photo quests (such as
  Lappy's) never completed. The game measures how much of a subject is visible with the GPU, and reads that count
  a frame later without waiting; the count always read 0. Photos are also read back from the GPU now. Confirmed in
  play (the owner found the last part of this fix).

**2026-10-26**

- **Frame rate with the Legion out on GPUs behind a slower link** (such as an eGPU over OCuLink): the GPU copied
  about 47.5 MB of results back to system memory every frame, though only a few pages changed; on an RTX 5070 Ti
  over OCuLink that took 8 of 10 ms a frame, 19 of 24 ms with the Legion out. Only the changed pages are copied now.
  Reported in play on that PC: 42 → 53-54 fps with the Legion out (2560x1440, FSR 1).
- **Street after the opening helicopter scene**: it was held at 33-53 fps on a fast PC (RTX 4090 / Ryzen 7 7800X3D,
  CPU-bound) by thousands of small memory-tracking queries a second. They are now grouped: average 55.6 → 58.7 fps
  there at 2560x1440. *Not yet confirmed in play.*
- **Settings > Prepare graphics on a separate thread (experimental)**, off by default: the game's render thread no
  longer translates and submits each frame itself. On the same street: average 59.9 fps, the slowest 2 s about 57.
  *Not yet confirmed in play*; sound may run slightly later relative to the picture with it on.
- **Brightness pumping in a dialogue after the opening street fight**: the picture brightened over a few frames
  and snapped back. A compute shader that averages the scene's brightness read stale data (threads of a group passed
  a barrier early); the measured average is now steady in every frame. *Not yet confirmed in play.*

**2026-10-24**

- **Flickering light in the opening helicopter cutscene**: the whole picture jumped between two brightnesses. The
  game asks for depth clamping, which was ignored, so an effect card it places right in front of the camera was
  cut away in some frames and drawn in others. Depth clamping is now on. On a test save, that stretch of the
  cutscene went from 150-200 brightness jumps per 500 frames to 1-2 (camera cuts). The cutscene now stays at the
  darker of the two brightnesses. *Not yet confirmed in play.*
- **Setup after picking the wrong file**: once a wrong base game or update had been picked, every retry failed with
  the same "belongs to program" message. Setup now extracts again for any other file, and says which file to pick.
- **Ultrawide render sizes** (issue #4): 2560x1080 and 3440x1440 (21:9), 3840x1080 and 5120x1440
  (32:9), 5760x1080 and 7680x1440 (48:9, three screens; issue #19). See [Experimental options](playing.md#experimental-options). *Untested on a real ultrawide monitor.*

**On the `upscaling` branch since 2026-10-23 (in `main` since 2026-11-12)**

- **Render resolution** (issue #4): 1920x1080, 2560x1440 and 3840x2160 besides the default 1600x900.
- **AMD FSR 1 upscaling** option.

The options are under [Experimental options](playing.md#experimental-options).

**2026-10-23**

- **Shorter wait before the game after a play session.** The game records the shader combinations it meets in
  play; the next Play then redid all of "Preparing shaders" and "Compiling for your GPU". Now only what was added
  since the last time is done. On the test PC, with about 15% new records, the wait went from about 3 s to under
  1 s (the saving is larger on slower PCs). A new version still prepares everything once.
- **Fullscreen** (issue #5): **F11** or **Alt+Enter** switches between a window and borderless fullscreen on the
  window's monitor (the display mode is not changed). **Settings > Display > Start in fullscreen** starts in it.

**2026-10-22**

- **Python no longer needs to be installed** (issue #3). Setup downloads its own Python 3.14 into `tools\python`
  (pinned, checked by SHA-256), so the version installed on your PC, if any, does not matter. An existing `.venv`
  that works is kept.
- **Setup from a ZIP download** stopped at "unreviewed Maxwell translator identity" (issue #2): a file check
  depended on line endings. Fixed; confirmed by the reporter.
- **Green flashes in the title movie** (and noise or green in the tutorial clips): video frames were sometimes
  drawn before the decoded picture reached the GPU, for up to 8 frames. Textures written by the CPU are now
  checked every frame. In a recorded start, the title movie had about 50 green frames before and none after.
  *Not yet confirmed in play; the tutorial clips are untested.*
- **Handheld mode** (Settings > Display, off by default; issue #4): the game runs as on a Switch out of its dock
  and renders at 1280x720 instead of 1600x900 (36% fewer pixels), for GPUs that cannot hold 60 fps. The picture
  is scaled to the window as before. *Its frame rate gain was not measured: the test PC's GPU is not the limit.*

**2026-10-21**

- **Keyboard controls can be changed**: **Controls** in the launcher (click a key, press the new one).
- **Optional BC7 texture compression** (Settings, off by default), in two versions: converted on the GPU while
  playing, or prepared once at setup. Both save about 2.5 GB of video memory; see
  [Texture compression](playing.md#texture-compression-bc7) for what each costs. *Not yet confirmed in play.*
- **Launcher.** `Astral Chain Launcher.exe`: install, compile shaders, play without console windows, play with
  diagnostics and create a bug report, and settings for every option, over original key art (six backgrounds to
  pick from with the arrows at the top right). See [Playing](playing.md).
- **Shaders compiled for your GPU before the game starts** ("Compiling for your GPU", about 10 s the first time).
  In-game, pipelines are linked from those compiled shaders instead of being compiled whole, and the recorded
  ones are built before their first draw. Most shader pauses in new areas are gone (see Status). *Not yet
  confirmed.*
- **ASTC textures are decoded on the GPU** instead of the CPU: no more 60-240 ms pauses when large textures load.
  Checked byte for byte against the CPU decoder.
- **Crash at the File 01 boss** (when it throws a container): the game stopped on a rumble value out of range. It
  now ignores such values. *Not yet confirmed.*
- **NSP base games** stopped setup at the RomFS step (issue #1). Setup now gives NSTool both the base game's and
  the update's title keys. *Not yet confirmed by the reporter.*
- A failed rebuild no longer loses the shader cache.
- Crash dumps in diagnostics mode; the watchdog script now records the game's exit code.

**2026-10-20**

- All of the game's shaders are translated before the first launch ("Preparing shaders").
- NVN's alpha test is emulated: defeated enemies no longer leave a dark tentacle mass. *Not yet confirmed.*
- Cutscene cuts keep their time at 60 fps, and sound skips gaps after a stall, so sound stays with the picture.
  *Not yet confirmed.* `AC_CUTSCENE_FPS=30` plays cutscenes at 30 fps as on the Switch.
- Dragging the window by its title bar no longer freezes the game; closing hides the window at once.
- Each save is also copied to a backup folder (the newest 30 are kept).
- Textures are uploaded ahead of first use on loading screens; the pipeline cache is kept between sessions.
- Fixed wrong textures on faces and armour, and a rare crash in the GPU write-back handler.
