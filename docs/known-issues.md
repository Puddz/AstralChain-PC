# Known issues

- Dark, shadow-like patches on scenery when fighting close to it were reported. Under investigation: a screenshot
  helps.
- Some short pauses remain when entering new areas: the game loading its own data, not shader compiling.
- Defeated enemies left a large dark tentacle mass for about two seconds. Cause found: the chimera blood effect
  dissolves through NVN's alpha test, which was not emulated. It now is (test runs of the highway chase show no
  mass), not yet confirmed in play.
- In-game cutscenes were reported with the sound behind the picture after camera cuts (the 60 fps mod's known limit:
  each cut ended up to a frame early). Cut ends now keep their time: the cutscene after the highway chase lasts
  35.192 s, as at the original 30 fps (35.193 s; 35.077 s before). Sound gaps after stalls are skipped to stay in
  sync. Not yet confirmed fixed.
- Rarely the game closes right at startup (`STOP host-shared-placeholder-reserve` in `trace.log`). Start it again.
- Save-slot thumbnails are not captured (`nvnTextureReadTexels` is not implemented).

Reports are welcome. The easiest way: play with **Play with diagnostics** in the launcher until the problem
happens, then attach the zip that **Create report** makes. Otherwise include `build\game\round-00\stderr.log` and
`trace.log` and your GPU and driver version, but no game files.
