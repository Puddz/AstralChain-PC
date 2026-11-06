# Texture mods

> [!NOTE]
> Only on the experimental [`texture-dump`](https://github.com/Puddz/AstralChain-PC/tree/texture-dump) branch
> (issue #14). *Not yet confirmed in play.*

The port can write out every texture the game loads (**dumping**) and load edited versions in their place
(**replacing**), including higher-resolution ones. The game's files are not changed: replacements are applied as the
textures are uploaded to the graphics card, and turning the setting off brings the originals back.

- [Making a texture mod, step by step](#making-a-texture-mod-step-by-step)
- [Dumping](#dumping)
- [Replacing](#replacing)
- [If a replacement does not show](#if-a-replacement-does-not-show)
- [How it was tested](#how-it-was-tested)
- [Environment variables](#environment-variables)

## Making a texture mod, step by step

1. **Dump.** In the launcher, **Settings > Mods > Dump textures**. Optionally pick a **Texture dump folder** (default
   `userdata\textures\dump`). Press **Play** and go through the areas, menus and cutscenes whose textures you want.
   Each texture is written the first time it appears.
2. **Find the texture.** Open the dump folder in a viewer that reads DDS (Paint.NET, GIMP, or Windows Explorer with a DDS
   thumbnail extension). `textures.csv` lists every file with its size and format, which helps narrow it down: a
   character's main texture is usually 1024x1024 or 2048x2048, UI pieces are small.
3. **Edit.** Open the DDS, edit it (or paint a new one at a larger size, keeping the shape: a 1024x512 texture as
   2048x1024).
4. **Save as DDS.** BC7 with a full mip chain is the best choice for colour textures; BC1 is smaller for textures
   without transparency. With Microsoft's [texconv](https://github.com/microsoft/DirectXTex/wiki/Texconv), from a PNG:
   `texconv -f BC7_UNORM_SRGB -m 0 -y jacket.png` (`-m 0`: all mip levels). Paint.NET saves BC7, BC3 and BC1 DDS files
   with mip maps; GIMP saves BC1 / BC3 (DXT1 / DXT5) with mip maps.
5. **Name it.** The file name must **start with the 16-character hash** from the dumped file's name. Anything may
   follow: `9f3943bf7e8129bc_2048x2048.dds`, `9f3943bf7e8129bc jacket HD.dds`.
6. **Put it in the replacement folder.** **Settings > Mods > Replacement textures folder** (default
   `userdata\textures\replace`). Subfolders are searched, so a mod can be organised however you like
   (`replace\MyMod\characters\...`).
7. **Turn it on.** **Settings > Mods > Use replacement textures**, then **Play**. The folder is read when the game
   starts: restart the game after adding or changing files.

To share a mod, share the files (and the folder layout you chose); players copy them into their replacement folder.

## Dumping

**Settings > Mods > Dump textures** (or `AC_TEXTURE_DUMP=1`) writes each texture the game loads, once, as a DDS file.
**Texture dump folder** (or `AC_TEXTURE_DUMP_DIR`) picks the folder; empty means `userdata\textures\dump`.

- **Names:** `<hash>_<width>x<height>.dds`. The hash is of the texture's data as the game has it, so the same texture
  gets the same name in every run and on every PC with the same game version. A file that is already there is kept,
  so dumping again only adds new textures.
- **`textures.csv`** in the same folder lists each file: hash, width, height, mip levels, layers, type (2d, array,
  cube), the game's format and the DDS format. The game gives its textures no names, so they cannot be sorted into
  folders by what they show.
- **Formats:** most of the game's textures are BC1 or BC3 and are written as they are, with all their mip levels, as
  are BC6H, RGBA8 and the other common formats. ASTC textures (a Switch format PCs do not use) are decoded to RGBA8.
  Cube maps and texture arrays are written as DDS cube maps and arrays. The files use the DX10 DDS header.
- **Not dumped:** what the game draws into while playing (render targets, shadow and reflection maps), video frames
  (the opening lab scene and other movies), textures that change while playing, depth textures, 3D textures, and a few
  16-bit formats DDS has no match for.
- **Size and speed:** the highway save's first two minutes wrote about 2,200 textures (3.8 GB). Files are written on
  a background thread; the frame rate was unchanged in testing.

## Replacing

**Settings > Mods > Use replacement textures** (or `AC_TEXTURE_REPLACE=1`) loads DDS files from **Replacement textures
folder** (or `AC_TEXTURE_REPLACE_DIR`; empty means `userdata\textures\replace`) in place of the game's textures.

- **Matching:** a file replaces the texture whose hash its name starts with (16 hexadecimal digits, then anything that
  is not one). If two files start with the same hash, the first one found is used.
- **Size:** any. Larger than the original works (HD textures). Keep the original's shape, or the texture is stretched.
- **Mip levels:** as many as the file has. A full chain is best: without one, textures in the distance look grainy
  and shimmer.
- **Formats:** BC1, BC2, BC3, BC4, BC5, BC6H, BC7, RGBA8 and BGRA8, also R8, RG8, RGBA16F and RGBA32F; with the DX10
  DDS header or the older one (DXT1, DXT3, DXT5, ATI1, ATI2, 32-bit RGBA). BC7 and BC1 use a quarter (BC1: an eighth)
  of RGBA8's video memory.
- **Colour space:** whether the colours are sRGB follows the game's texture, whatever the file says. Editors often
  save the wrong flag, so you do not need to get it right.
- **Cube maps and texture arrays** need the same number of faces / layers as the game's texture.
- **Normal maps and other data textures** (the dump's `UNORM` BC5, BC4 and non-sRGB BC1/BC3 files) hold numbers, not
  colours: edit them with care and keep their format.
- **Video memory:** larger textures need more. In testing, replacing every texture of the highway chase with versions
  twice as wide and high added about 1 GB.

## If a replacement does not show

- **Check the name.** It must start with the full 16-character hash of the texture as dumped. Re-dump to check the
  texture still has the same name (it should).
- **Restart the game** after adding files: the folder is read at start.
- **Look in the game's log.** Files that cannot be used are listed with the reason (unknown format, wrong number of
  layers or faces, file cut short, the GPU cannot use the format), and the game's own texture is shown instead. The log
  is `build\game\round-00\stderr.log` (lines starting with `[texreplace]`); it also lists the first 100 replacements
  made and a count every 500.
- **Movies cannot be changed this way.** The opening lab scene and other pre-rendered cutscenes are videos.
- **Some things are drawn by the game while playing** (shadows, reflections, effects rendered each frame): they are not
  textures from the game's files and cannot be replaced.

## How it was tested

On the highway save (about 7 minutes from the title screen through the motorbike chase), RTX 4090, with generated test
patterns for all 2,165 2D textures dumped there:

| Replacement set | Result |
| --- | --- |
| RGBA8 checkerboards, same size up to 512 (larger originals smaller) | Every surface and the UI showed them |
| DXT5 stripes at twice the original size, up to 2048 (older DDS header) | Every surface showed them |
| RGBA8 stripes at twice the size, up to 1024, full mip chain (each level its own colour) | Shown, mip levels in use |

The runs averaged 59.7-59.9 fps (59.9 without replacements). The worst frame was the same loading spike as without
(103-112 ms against 109), except one 221 ms frame in the first run. Video memory grew by about 1 GB with the
double-size sets. Every file of a test dump was opened and decoded with the Pillow library; a second dump over the same
route added no files (the names are stable).

Not tested yet: BC7 replacement files, cube map and array replacements, real (hand-made) mods.

## Environment variables

For running the game without the launcher (`run_guarded.ps1 -Env`, or **Extra environment variables**):

| Variable | Meaning |
| --- | --- |
| `AC_TEXTURE_DUMP=1` | Dump textures |
| `AC_TEXTURE_DUMP_DIR=<folder>` | Where to dump them (default `userdata\textures\dump`) |
| `AC_TEXTURE_REPLACE=1` | Use replacement textures |
| `AC_TEXTURE_REPLACE_DIR=<folder>` | Where they are (default `userdata\textures\replace`) |

The code: `astral_chain/runtime/las/nvn/vulkan/texdump.c` (dumping), `texreplace.c` (finding and reading
replacement files) and `replace_upload` in `textures.c` (putting them on the GPU).
