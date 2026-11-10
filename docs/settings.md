# Settings

Every setting of the launcher's **Settings** window, in the window's order. Settings marked **F1** are also in the
in-game menu (**F1**, or **Minus and Plus** together on a controller), which saves to the same file
(`userdata\launcher.ini`). In the launcher, a setting applies the next time you press **Play**. In the in-game menu
most apply at once; the ones that cannot say *Applies at the next start*.

Running the game without the launcher (`astral_chain\run_guarded.ps1 -Env`, or the launcher's **Extra environment
variables**), each setting is the environment variable in its last column.

The keyboard keys are in the launcher's **Controls** window, not here.

- [Game](#game)
- [Sound](#sound)
- [Display](#display)
- [Mouse and keyboard](#mouse-and-keyboard)
- [Controller](#controller)
- [Shaders and performance](#shaders-and-performance)
- [Troubleshooting](#troubleshooting)
- [Only on the upscaling and texture-dump branches](#only-on-the-upscaling-and-texture-dump-branches)
- [Only on the texture-dump branch: Mods](#only-on-the-texture-dump-branch-mods)

## Game

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Uncapped frame rate** (experimental) **F1** | Off | Gameplay is no longer held to 60 fps; cutscenes stay at 60. With Vsync (*Presentation mode*) the display's refresh rate is the limit. Gains are small where the processor is the limit. Reported in play: it does not feel right above 60 fps yet | `AC_UNCAPPED=1` |
| **Cutscenes at 30 fps, as on the Switch** **F1** | Off | In-game cutscenes run at 30 fps as in the original. Off: at 60 fps like the rest of the game. In the menu, from the next cutscene on | `AC_CUTSCENE_FPS=30` |
| **Keep running while the game window is minimized** **F1** | Off | Off: the game pauses while minimized, as under the Switch's HOME menu. Switching to another window never pauses it; it only stops keyboard input | `AC_IGNORE_FOCUS=1` |
| **Back up each save** | On | Each time the game saves, a copy goes next to the save in `userdata\saves` (`<save>-snapshots`, the newest 30 are kept). A copy is kept only when every file was copied | `AC_SAVE_SNAPSHOTS=0` turns it off |

## Sound

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Skip sound gaps after a stall to stay in sync** | On | After a hitch, up to 0.3 s of sound is skipped so it does not fall behind the picture | `AC_AUDIO_RESYNC=0` turns it off |
| **Keep cutscene cuts on time at 60 fps** | On | Off: cuts end as in the original code, each up to a frame early at 60 fps (the sound drifts ahead) | `AC_CUT_CARRY=0` turns it off |

## Display

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Start in fullscreen** **F1** | Off | The window covers the whole screen without borders (the display mode is not changed). **F11** or **Alt+Enter** switches at any time; the game starts the way you leave it | `AC_FULLSCREEN=1` |
| **Show the frame rate** **F1** | Off | Frames per second and the average frame time (counted over half a second) in the top left corner of the window. It is drawn on the window, so it is not in screenshots of the game picture | `AC_FPS_COUNTER=1` |
| **Show "Port by Puddz" on the title screen** **F1** | On | A small credit above the version number while the title screen shows | `AC_TITLE_CREDIT=0` turns it off |
| **Title screen shows the twin you play as** **F1** | Off | The title screen's scene (a video) shows one of the twins on a roof, in the outfit of your newest save. The game picks the twin you did **not** choose (your sibling). On: the one you play as. Your save is not changed. In the menu, from the next time the title screen appears | `AC_TITLE_OWN_TWIN=1` |
| **Presentation mode** **F1** | Mailbox | **Mailbox**: no tearing, the game paces itself. **Vsync (FIFO)**: waits for the display; can slow the game when the desktop is busy. **Immediate**: tearing, lowest latency | `LARECOMP_VK_PRESENT=fifo` or `immediate` |
| **Handheld mode: render at 1280x720** **F1** | Off | The game runs as on a Switch out of its dock: it draws at 1280x720 instead of 1600x900, for GPUs that cannot hold 60 fps. The picture is scaled to the window as before | `AC_HANDHELD=1` |

## Mouse and keyboard

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Mouse moves the camera** **F1** | On | Click in the game window to give it the mouse: the cursor hides and the mouse turns the camera (and steers the Legion while ZL is held). Hold **Alt**, or switch to another window, to get the cursor back. Off: the mouse buttons still work, the camera stays on the keys | `AC_MOUSE_LOOK=0` turns it off |
| **Mouse camera sensitivity (%)** **F1** | 100 | How fast the camera turns for a given mouse movement (10-500) | `AC_MOUSE_SENS=<n>` |
| **Invert mouse up and down** **F1** | Off | Moving the mouse forward looks down | `AC_MOUSE_INVERT=1` |
| **Button icons in the game** **F1** | Automatic | **Automatic**: the game's button prompts follow what you last used (keyboard and mouse, an Xbox, PlayStation or Switch controller). The other choices fix one style | `AC_PROMPTS=keyboard`, `xbox`, `playstation` or `switch` |

## Controller

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Rumble strength (%)** **F1** | 100 | How strongly the controller rumbles (Xbox pads, and the controllers that go through SDL3). 0 turns rumble off, 200 is the strongest | `AC_RUMBLE=<n>` |

## Shaders and performance

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Compile shaders for your GPU before playing** | On | About 10 s the first time and after driver updates; removes most shader pauses in play | `AC_GPU_WARM=0` turns it off (setup) |
| **Prepare graphics on a separate thread** (experimental) **F1** | Off | The game's frames are turned into Vulkan work on a thread of their own while the game goes on with the next frame. Helps where many things are drawn (the street after the opening helicopter). Turn it off if you see graphics problems | `AC_ASYNC_SUBMIT=1` |
| **Texture compression (BC7)** | Off | Saves about 2.5 GB of video memory (7.2 GB to 4.7 GB on the highway chase), for GPUs with 8 GB or less. **On the GPU**: nothing to prepare, but about 5 extra short pauses (60-100 ms) while an area loads and slightly less detail on some surfaces. **Prepared at setup**: the best detail and about as smooth as off, but about 12 minutes once (at the next Play) and 16 GB of disk space. See *Texture compression* in the README | `LARECOMP_BC7=1` |
| **Keep the game's memory copy in system RAM** (4 GB GPUs, experimental) | Off | For GPUs with 4 GB of video memory, which the game can fill (a crash on an RTX 3050 Ti Laptop, [issue #17](https://github.com/Puddz/AstralChain-PC/issues/17)). The game keeps a 2.8 GB copy of the Switch's memory on the GPU; this keeps it in system RAM instead. Some GPU work gets slower (it reads across PCIe), and it needs about 3 GB of free RAM. Use it together with *Texture compression*. Not tested on a 4 GB GPU yet | `AC_HEAP_IN_RAM=1` |

## Troubleshooting

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Decode ASTC textures on the GPU** | On | Off: textures are decoded on the processor (slower loading). Try it if textures look wrong | `LARECOMP_GPU_ASTC=0` turns it off |
| **Shader libraries (fast pipeline linking)** | On | Off: each pipeline is compiled whole when first drawn (longer pauses). Try it if graphics break | `LARECOMP_GPL=0` turns it off |
| **Build recorded pipelines ahead of their first draw** | On | Off: pipelines are only built when first drawn | `LARECOMP_NO_PIPE_WARM=1` turns it off |
| **GPU hang stop (milliseconds)** | 1200 | The game stops itself if the GPU stays busy this long, before Windows would reset the driver (1000-1800) | `AC_GPU_HANG_MS=<n>` |
| **Record controller input** | Off | Records your buttons and sticks from launch, with a copy of your save as it was at launch, so a problem you meet can be played again on another PC. Each session goes to a new folder in `userdata\saves\01007300020fa000\<user>-recordings`; zip the folder to send it. Replay: [development notes](development.md) | `AC_INPUT_RECORD=1` |
| **Show console windows** | Off | Shows the game's console and the watchdog's window | (launcher only) |
| **Extra environment variables** | Empty | `NAME=value;NAME=value`, passed to the game; see [development notes](development.md) | (these are the variables) |

## Only on the upscaling and texture-dump branches

These are in the **Display** group (and **F1**) of the `upscaling` and `texture-dump` downloads.

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Render resolution** (experimental) | 1600x900 | The size the game draws its picture at. 1600x900 is the Switch's docked size, with the game's own dynamic resolution. Smaller sizes (down to 854x480) are for slow GPUs: the 3D view is drawn at that size and scaled up (softer; the HUD stays sharp); a slow processor is not helped. Larger sizes (up to 3840x2160) are sharper and need a faster GPU. The 21:9 and 32:9 sizes are for ultrawide screens, the 48:9 ones (5760x1080, 7680x1440) for three screens side by side (untested on real ultrawide or triple screens). Past 8.6 million pixels a size is drawn smaller at the same shape: 7680x1440 at 6788x1272. Handheld mode keeps 1280x720 and ignores the sizes below it. Applies at the next start | `AC_RESOLUTION=<width>x<height>` |
| **Upscaling** (experimental) | Smooth | How the picture is enlarged to a bigger window or screen. **Smooth**: plain scaling. **AMD FSR 1**: edge-aware upscaling and sharpening, crisper on 1440p and 4K screens; works on any GPU. No effect when the window is not larger than the picture | `AC_UPSCALER=fsr1` |
| **FSR 1 sharpening** | Normal | How much FSR 1 sharpens: Strong, Normal, Light or Very light. Only used with AMD FSR 1 | `AC_FSR_SHARPNESS=0`, `0.2`, `0.5` or `1` |

## Only on the texture-dump branch: Mods

The **Mods** group of the `texture-dump` download (launcher only). The [texture mods guide](https://github.com/Puddz/AstralChain-PC/blob/texture-dump/docs/texture-mods.md) explains
how to make a mod.

| Setting | Default | What it does | Variable |
| --- | --- | --- | --- |
| **Dump textures** (experimental) | Off | Writes every texture the game loads as a DDS file, once, named by a hash of its data, with `textures.csv` listing them. Each new texture is written the first time it appears (several GB for the first chapters) | `AC_TEXTURE_DUMP=1` |
| **Texture dump folder** | Empty | Where the dumped textures go. Empty: `userdata\textures\dump` | `AC_TEXTURE_DUMP_DIR=<folder>` |
| **Use replacement textures** (experimental) | Off | Loads your edited textures in place of the game's: DDS files whose names start with the hash of the texture they replace. They can be larger than the original (HD textures), in BC1-BC7 or RGBA8, and in subfolders | `AC_TEXTURE_REPLACE=1` |
| **Replacement textures folder** | Empty | Where your replacement textures are. Empty: `userdata\textures\replace` | `AC_TEXTURE_REPLACE_DIR=<folder>` |
