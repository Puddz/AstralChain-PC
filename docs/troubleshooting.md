# Troubleshooting

## Setup

| Message | What to do |
| --- | --- |
| `Python 3.12 or newer is required` | Install Python from python.org with the default options (the `py` launcher), then run `setup.cmd` again. |
| `Setup needs about 35 GB free` | Free space on the drive that holds this folder, or move the folder to another drive. |
| `... does not match its pinned SHA-256` | The download was damaged or changed upstream. Run setup again. If it repeats, open an issue. |
| `nstool.exe failed` while reading a file | Check that `prod.keys` is from a console on a firmware at least as new as the game requires, and that the file is a complete dump. |
| `The base game file has no Astral Chain Program/Control NCA` | You picked the update, or another game, as the base. |
| `The update is not Astral Chain v1.0.1` | Only the v1.0.1 update (version 65536) is supported. |
| `belongs to program ..., not Astral Chain` | The file is a different game. |
| The build fails | Run `setup.cmd` again (finished steps are skipped). If it fails again, open an issue with the last 30 lines. |

To start over completely, delete `game\`, `build\` and `tools\`. Keep `userdata\`: it holds your saves.

## Playing

- **The window closes straight away.** See `build\game\run.txt` and the end of `build\game\round-00\trace.log`.
  - `WATCHDOG`: the GPU driver reported an error and the game was closed on purpose. Update your GPU driver.
  - A `STOP ...` line: the game reached something the runtime does not implement yet. Please report the line.
- **Short pauses in new areas.** Shaders are compiled the first time they are seen. Later sessions use the cache.
- **No sound.** The game uses the default Windows output device. If the device disappears during play, the game
  reopens it.
- **Which GPU is used.** The first lines of `build\game\round-00\stderr.log` list the Vulkan GPUs and the one in
  use (`[vk] using ...`).

## Reporting a problem

Include:

- `build\game\run.txt` and `build\game\round-00\stderr.log`;
- your GPU, driver version and Windows version;
- what you were doing.

Never attach game files, keys, `game\` or `build\` contents other than those two logs.
