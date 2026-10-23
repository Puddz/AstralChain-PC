# Troubleshooting

## Setup

| Message | What to do |
| --- | --- |
| `Python: the download does not match its pinned SHA-256`, or the Python download fails | Check the internet connection (setup downloads Python from api.nuget.org) and run setup again. |
| `Setup needs about 35 GB free` | Free space on the drive that holds this folder, or move the folder to another drive. |
| `... does not match its pinned SHA-256` | The download was damaged or changed upstream. Run setup again. If it repeats, open an issue. |
| `nstool.exe failed` while reading a file | Check that `prod.keys` is from a console on a firmware at least as new as the game requires, and that the file is a complete dump. |
| `The base game file has no Astral Chain Program/Control NCA` | You picked the update, or another game, as the base. |
| `The update is not Astral Chain v1.0.1` | Only the v1.0.1 update (version 65536) is supported. |
| `... is for another game: its ... belongs to program ..., not Astral Chain` | The file you picked (base game or update, as the message says) is for a different game: an emulator accepts it as that game's update, so working there does not mean it is Astral Chain's. Pick the Astral Chain file (update: title ID 01007300020FA800, v1.0.1). Setup before 2026-10-23 kept the first file's extracted contents and reported the same error with any file picked after it; update, or delete `game\work`, and run setup again. |
| The build fails | Press **Install game** (or run `setup.cmd`) again (finished steps are skipped). If it fails again, open an issue with the last 30 lines. |

To start over completely, delete `game\`, `build\` and `tools\`. Keep `userdata\`: it holds your saves.

## Playing

- **The window closes straight away.** See `build\game\run.txt` and the end of `build\game\round-00\trace.log`.
  - `WATCHDOG`: the GPU driver reported an error and the game was closed on purpose. Update your GPU driver.
  - A `STOP ...` line: the game reached something the runtime does not implement yet. Please report the line.
- **Short pauses in new areas.** Shaders are compiled for your GPU before the game starts; check that the
  launcher's *Compile shaders* says *(Already compiled)* and that *Compile shaders for your GPU before playing* is on
  in Settings. Remaining pauses are mostly the game loading its data. For a report, use **Play with diagnostics**:
  it logs where the time of each slow frame went.
- **Out of video memory, or stutter on GPUs with 8 GB or less.** Set *Texture compression (BC7)* in Settings:
  *prepared at setup* (best, 12 minutes and 16 GB once) or *converted on the GPU* (nothing to prepare, a few more
  short pauses when areas load). See the README's *Texture compression* section. If textures then look wrong, set
  it back to *Off* and please report it.
- **A key does nothing.** Check **Controls** in the launcher; *Defaults* restores the original keys.
- **No sound.** The game uses the default Windows output device. If the device disappears during play, the game
  reopens it.
- **Which GPU is used.** The first lines of `build\game\round-00\stderr.log` list the Vulkan GPUs and the one in
  use (`[vk] using ...`).

## Reporting a problem

The launcher's **Play with diagnostics** and then **Create report** make a zip in `reports\` with everything below
(and a crash dump if the game crashed). Attach that. Otherwise include:

- `build\game\run.txt` and `build\game\round-00\stderr.log`;
- your GPU, driver version and Windows version;
- what you were doing.

Never attach game files, keys, `game\` or `build\` contents other than those two logs.
