# What's new

Fixes marked *not yet confirmed* were tested in scripted runs, but not yet confirmed in play.

**2026-11-22**

- **HUD at the edges: the button guide's sweep left behind** (issue #53; *not yet confirmed in play*): with *HUD at
  the screen's edges* on, a translucent strip that sweeps across the button guide every few seconds stayed where the
  panel is at 16:9, so it slid across the picture beside the moved panel. It is drawn far from its own origin, which
  kept it from being matched with its panel; it now goes with the panel. Checked at 5760x1080 on the reporter's save
  state (File 1 chapter 2): the strip no longer shows beside the panel; on a recording with the Legion wheel and the
  Police Gear menu nothing else moves differently.

- **HDR: bands in bright glows** (issues #54 and #52; *not yet confirmed in play*): with HDR output the bright parts
  of the picture (lights, the bloom around them, the bike's headlight on the wet tunnel road) showed steps instead of
  smooth gradients. The game keeps its picture in a format with 5-6 bits of precision per colour, enough below the
  SDR white but not for the brightness HDR shows above it (steps of 3-7%). With HDR on, those pictures are now kept
  at 16-bit float. Measured on the tunnel frame of the #52 console screenshot: in the parts above the SDR white,
  neighbouring pixels with exactly the same value (flat steps) went from 56% to 28%; a ceiling light that stepped
  558, 558, 533, 533 nits now falls off 558, 553, 548, 541, 529; same frame rate (59.8 fps on the route, with and
  without FSR 1). SDR is unchanged. The bright patch beside the bike in the tunnel is the bike's headlight on the
  wet road: the console shows it too (compared on the same moment), so that part is the game's look.

- **Ultrawide: enemy death flashes and shot flares drawn away from the enemy** (issue #51; *not yet confirmed in
  play*): wider than 16:9, the flash when an enemy dies and the flares on shots and hits landed further out the further
  the enemy was from the middle of the screen (3 times as far at 48:9). They take their place from the same screen
  position calculation as the HUD's markers, which since issue #21 works in the HUD's centred 16:9 layout; those
  effects now get the whole picture's instead, and the markers keep theirs. Checked at 5760x1080 on the reporter's save
  state of the File 1 truck fight: a flare drawn 400 px right of the middle for an enemy 133 px right of it before;
  after, the flares sit on the dying enemies and the markers stay on theirs.

- **Save states: loading after a restart stopped the game at ultrawide** (*not yet confirmed fixed*): loading a state
  in a newly started game at 3840x1080 with DLSS stopped it (`shared-map-overlap`). The game puts its input and clock
  memory at random addresses, and a state needs them at the same addresses again; Windows had put one of its own
  allocations there in the new run (larger graphics buffers make that more likely). The port now reserves the game's
  free address range at startup, before the graphics and sound libraries load, so Windows' allocations go elsewhere,
  and releases it piece by piece where the game maps memory. This reserves addresses only, no memory. Measured at
  3840x1080: Windows' and the driver's allocations inside the game's range went from 3.3 GB to none; the same frame
  rate (59.9 fps on the same route either way); states saved and loaded in the same run and in three new runs. The
  rare startup stop `host-shared-placeholder-reserve` has the same cause.
- **The opening video after a new game: 16:9 on ultrawide, and in step with its music** (issue #50; *not yet
  confirmed in play*): the lab scene that opens a new game is a video (`movie/ev0010.bk2`, 1600x900), drawn with its
  own shader rather than the UI's, so wider than 16:9 it was stretched to the full width. It is now shown in the
  16:9 area in the middle with black sides, as the title screen's videos are. The game's videos have no sound of
  their own (the music and voices are the game's), and the video player restarted its clock whenever the picture
  was more than two frames late, so every hitch over ~70 ms left the picture further behind the music for good.
  Long videos (60 s or more) now catch up after a hitch of up to 2 s instead. Measured at 5120x1440 with a test
  stall of 300 ms every 2 s: the picture fell 4.9 s behind real time over 35 s before, 0.0 s after; without stalls
  30 video frames a second at 60 fps either way. Below 30 fps the video still runs slower than real time (the game
  shows at most one video frame per frame).

**2026-11-21**

- **New: HDR output** (issue #49; experimental; reported working by the issue's author on their HDR display; off
  by default): **Settings > Display > HDR output** in the launcher, or **HDR** in the in-game menu (from the next
  start). On a display with Windows' **Use HDR** on, the window becomes HDR10 and the picture keeps the highlights
  the game's tonemap clips (lights, fire, neon), up to the display's peak, with the game's colour grade; the HUD,
  the game's menus and this port's menu are shown at their own brightness. Settings for the peak, the game's white,
  the HUD's white and the highlights' hue, applied at once. Ported from captainj-sparrow's port (which follows
  RenoDX's approach). Measured from the encoded picture of a gameplay frame (fire, street lights): below the game's
  white it matches the SDR picture (mean difference 0.004 of white), 1.4% of the pixels went above white, up to 983
  of the 1000 nits set; same frame rate (60 fps) as without it. With FSR 1 the upscaled HDR picture matched the SDR
  one below white.
- **5.1 surround: bass to the subwoofer, and its volume** (issue #48; reported working by the issue's author): **Bass to the
  subwoofer** (off by default) also sends the bass of the other five speakers to the subwoofer (LFE), which the game
  leaves silent: their sum through a 120 Hz low-pass, 10 dB down (receivers play the LFE 10 dB hot), as
  captainj-sparrow's port does. A **Subwoofer** volume slider joins the others. The other speakers keep their bass.
  Launcher and in-game menu (at once). Checked from the sound the port sends out, with the subwoofer at -6 dB: below
  60 Hz the subwoofer carries the other speakers' sum at -16.1 dB (-10 and -6), falling off above 120 Hz (-22 dB at
  120-240 Hz, -59 dB at 1-4 kHz).
- **5.1 surround: a volume per speaker, and what goes out** (issue #48; reported working by the issue's author): under
  **5.1 surround sound**, sliders for front left, front right, centre, surround left and surround right, -30 to
  +10 dB (launcher, and the in-game menu, where they apply at once). The menu also says what the sound output sends
  now: stereo, 5.1 with six channels, or the 5.1 mix folded down to stereo, with the sound device's channel count.
  Checked from the sound the port sends out: -30 dB on front right gave 0.033 of front left's level, -6 dB on
  surround left about half of surround right's.
- **New: 5.1 surround sound** (issue #48; experimental; reported working by the issue's author on their surround
  systems; off by default):
  **Settings > Sound > 5.1 surround sound** in the launcher, or **Sound** in the in-game menu (**F1**, from the next
  start). The port now tells the game the sound output has six channels, and the game's sound engine then mixes
  5.1 itself, as captainj-sparrow found in [their port](https://github.com/captainj-sparrow/astral-chain-pc-recomp).
  Where the sound device has fewer than six channels, the mix is folded back to stereo. Measured without speakers,
  from the sound the port sends out, on the save after the highway chase: in gameplay the side speakers carry their
  own sound (correlation with the fronts 0.1); the cutscene stays in the front pair, the centre and LFE get almost
  nothing. Folded back to stereo, the level is within 3% of the stereo mix.

**2026-11-20**

- **New: ambient occlusion** (issue #46; *not yet confirmed in play*; off by default): **Settings > Display >
  Ambient occlusion** in the launcher, or **Picture** in the in-game menu (**F1**, applies at once): Off, Low or
  High. It darkens creases, corners and the ground under characters and objects, which the game leaves as lit as
  open ground. Ported from [captainj-sparrow's port](https://github.com/captainj-sparrow/astral-chain-pc-recomp)
  (MIT): its shaders as they are, its hookup rewritten for this renderer; its "high + bounce light" level is not
  ported. Tested on save states in File 3: the darkening lands in the right places (wall corners, under railings,
  around rubble, at the character's feet), it is subtle by design; about 0.35 ms of GPU time per frame at
  2560x1440 on an RTX 4090, no change to the frame rate there (CPU-bound, about 75 fps uncapped either way).

**2026-11-19**

- **Fire light flickering on things near a fire** (issue #44; confirmed in play): in File 3, next to the
  burning barrel by the stairs, the fire's light on the chairs, crates and Akira went out for single frames, a few
  times a second at times. One of the fire's lights was lost on those frames: after the GPU's light pass, the port
  copied the buffers it had used back into the game's memory whole, read-only ones too, and so put old light data
  back over what the game had just written for the next frames. Now only what the GPU changed is copied back. On a
  save state at that spot (standing still, 90 to 150 frames recorded at full speed): single-frame drops of the
  picture's orange light of 5 to 10% in about 9 of 10 runs before, none in 13 runs with the change; same frame
  rate.
  `AC_WB_MERGE=0` turns it off, for comparing.

**2026-11-18**

- **Save states can be turned on from the in-game menu** (issue #44; *not yet confirmed in play*): **F1**, the
  **Game** box, **Save states, F6 and F7 (experimental)**. As with the launcher's option it takes effect at the
  next start of the game; the line of keys at the top of the menu says so until then.
- **Settings in boxes, in the launcher too; boxes close and open; menu size** (issue #43; *not yet confirmed in
  play*): the launcher's **Settings** window now has the same titled boxes by topic as the in-game menu, with the
  first sentence of each explanation under the name and the rest as a tooltip on the **(?)**, and a line at the
  top listing the keys used in the game (save states included). In both, a click on a box's title closes or opens
  it, and that is remembered. The HUD layout page (**F2**) has the same look (Size of the panels, Place on a wide
  screen, Frame rate display). The in-game menu's last box, **This menu**, has a **Size** slider (60% to 160%) for
  its text and window. Looked at in screenshots of the three windows; not tried with a controller.
- **Frame rate: size** (*not yet confirmed in play*): a slider under **Frame rate display** in the in-game menu
  (**F1**) makes the frame rate counter and the detailed panel smaller or larger, 50% to 200%. Applies at once.
- **In-game settings (F1): a tidier page** (issue #43; *not yet confirmed in play*): the settings are in titled
  boxes by topic (Controller, Mouse, Picture, Frame rate display, Ultrawide and HUD, Title screen, Game), with a
  line between settings and the ones that depend on another set in under it. Only the first sentence of each
  explanation shows; the rest opens when you point at the **(?)** after the name or move to the setting with a
  controller (that pop-up was not tried in the test runs). A strip at the top lists the keys: F1, F2, F3, F6 / F7
  (save states), F11. The launcher's settings window is unchanged.
- **Detailed frame rate panel: choose what it shows** (issue #43; *not yet confirmed in play*): with **Show the
  frame rate** on **Detailed**, the in-game menu (**F1**) lists the panel's eight parts under it, each with a box:
  GPU and video memory, CPU and memory, frame rate and frame time, the graph, renderer timings (the three orange
  lines), render sizes, frame limit and presentation, GPU and CPU names. Applies at once and is kept.
- **Size of the HUD's panels** (issue #43; *not yet confirmed in play*): the HUD layout page (**F2**) has a new
  slider, 50% to 150%, at any render resolution. It makes the HUD's corner panels (health, Legion, minimap,
  objectives, items, tutorials) smaller or larger, each from its own corner. Markers over characters, text in the
  middle of the screen, subtitles and the menus keep their size. Looked at in a replay at 3440x1440 and 2560x1440,
  at 70%, 80% and 130%; only that one place in the game was looked at.
- The in-game menu (**F1**) now says at the top that F6 and F7 are the save states, and where they are turned on.
- **Save states (experimental, off by default; confirmed in play on two PCs)**: with the launcher's new **Save
  states** option (Troubleshooting) on, **F6** writes the whole running game to a file and **F7** goes back to it:
  at once, after the game was started again, or on another PC. It is meant for showing a problem from the moment
  before it happens, in place of a long recording. The file is
  `userdata\saves\01007300020fa000\savestates\state.acss` (1 to 1.5 GB); it holds game data, so send it
  privately. A state only loads in the same release and at the same render resolution, and after loading one the
  game saves that state's progress over your save as usual: read [save states](save-states.md) first. Tested in
  scripted runs (20 loads in a row, a loading screen, an in-engine cutscene, FSR and DLSS on) and by loading a
  state made on another PC with another graphics card. With the option off nothing changes.
- **Frame rate: two more places** (issue #43; *not yet confirmed in play*): the frame rate counter and the detailed
  panel can also sit half way down the left or the right side of the window (**Middle left**, **Middle right**),
  which the game's own HUD leaves free. The setting is now called "Frame rate: position"; it applies at once.
- **Shadow resolution** (issue #40; *not yet confirmed in play*): a new setting in the launcher and the F1 menu,
  under Display. The game draws the shadows of characters and objects into one 2048x2048 shadow map, as on the
  Switch; from close up or at a high resolution their edges show as blocks. **2x**, **4x** and **8x** make that map
  finer each way, so the blocks get that much smaller. It takes video memory: about 250 MB at 4x, 1 GB at 8x (16x
  was tried and dropped: 4 GB for a difference hard to see). Off by default; applies at the next start. In the police headquarters at 2560x1440 the blocks along a shadow's edge shrank with each
  step. The line where the game switches to its coarser shadows further away is still there.
- **Ultrawide, HUD at the edges: the duty points and the case icon** (issues #41, #42; *not yet confirmed in play*):
  with "HUD at the screen's edges" on, the pop-up that counts duty points came apart (its arrow stayed in the middle
  of the screen, the bars behind the numbers ran too wide), and the red case icon after the objective's text stayed
  at its 16:9 place while the objective moved to the edge. Those parts are turned within their panel, which is why
  they were left behind; they now go with the panel they belong to. In a replay of File 02 at 3840x1080 the pop-up
  and the icon looked as at 16:9, and the Legion wheel, IRIS's profile panel and the Police Gear menu were as before.

**2026-11-17**

- **Ultrawide: boxes and a video in cutscenes** (issue #39; *not yet confirmed in play*): wider than 16:9, some
  cutscenes showed a darker or hazier box over the middle of the picture, of a different width from shot to shot. It
  is a flat card the game puts in front of the camera to tint the picture, sized to fill a 16:9 view; such a card is
  now widened with the picture. A video cut into a cutscene (Olive and Yoseph in File 02) showed the scene from
  before it at both sides of its 16:9 picture; those sides are now black. In a replay of the end of File 02 at
  3840x1080 none of the three boxed stretches showed a box any more, and the video had black sides.

**2026-11-16**

- **Ultrawide: a case's results screen** (issue #38; *not yet confirmed in play*): wider than 16:9, the list of
  bonuses on the results of a case was cut off on its left side, the rank badge (Lappy) that lights up beside the
  total showed stretched at the far right of the screen, and the photo behind the list was pressed into the left
  half of its frame. The list's clipping, the badge and the photo's frame now follow the panel into the 16:9 area.
  In replays of a recorded case at 3840x1080 and 2560x1080 the screen matched the 16:9 one, and the Police Gear
  menu's pages were unchanged. Still open: the photo itself is the wide scene pressed into its 16:9 frame.

**2026-11-13**

- **F3 hides the HUD** (issue #35; *not yet confirmed in play*): in the game, **F3** hides everything the game draws
  over the scene (HUD, markers, subtitles, button prompts, its menus) for screenshots, and **F3** again shows it. A
  line at the top says which for a moment. It is not saved: the HUD is back the next time you start. In scripted runs
  the HUD, the Legion wheel and the title screen's text were gone and the title's video stayed.
- **Anisotropic filtering** (issue #33; *not yet confirmed in play*): **Settings > Display > Anisotropic filtering**
  (and the F1 menu; applies at the next start): 4x, 8x or 16x for textures seen at a slant, such as the ground in the
  distance. The default is the game's own (2x or 4x on some surfaces). It is applied to the textures the game repeats
  across surfaces, not to every texture: with all of them at 16x, red speckles followed the edges in the scene. In the
  one dark scene tested here the picture was clean and the gain was small.
- **Clearer names for the scaling options** (issue #34): **Upscaling** is now **Window scaling** (how the finished
  picture is enlarged to the window; on the `dlss-fsr` branch it is separate from DLSS and FSR 3.1 / 4), and its
  sharpening is **Window scaling: FSR 1 sharpening**. Options that do nothing with the current choice (a sharpening
  whose upscaler is not picked, the frame rate's corner with the frame rate off) are greyed out in the launcher and in
  the F1 menu. **Ultrawide: HUD at the screen's edges** is no longer marked experimental.

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
