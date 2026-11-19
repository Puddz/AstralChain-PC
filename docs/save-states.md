# Save states (experimental)

A save state is the whole running game written to a file: you can go back to that exact moment at once, after the
game was started again, or on another PC. It is meant for showing a problem: save just before it happens and send
the file, instead of a long recording.

It is new and off by default. Tested so far in scripted runs and on two PCs with NVIDIA graphics cards; *not yet
confirmed* by players.

## Using it

1. In the launcher, tick **Save states (experimental)** under Troubleshooting and press Play. Or in the game:
   **F1**, tick **Save states, F6 and F7** in the **Game** box, then close the game and start it again (the
   option only takes effect at the start of the game).
2. **F6** saves. The game stops for about a second and "State saved" is shown.
3. **F7** goes back to it. This also works after the game was closed and started again: press F7 on the title
   screen.

The state is one file, `userdata\saves\01007300020fa000\savestates\state.acss`; each F6 replaces it. Rename or
copy the file to keep more than one (F7 loads the one named `state.acss`).

## Sending a state with a problem report

- Press F6 a few seconds before the problem shows, then check with F7 that it shows again.
- Zip `state.acss` (1 to 1.5 GB; about a third of that zipped) and add `userdata\launcher.ini`, your settings.
- **Send it privately, not on a public page.** The file holds the game's memory, which is game data. It holds no
  console keys and no Windows user name or file paths; it does hold the profile name you gave in the launcher.

## Limits

- A state only loads in **the same version of this port** (the same release) and at **the same render resolution**
  it was made at. The game says so in its log (`userdata\launcher.log`) and shows "State not loaded" otherwise.
- **Your save file.** After a state is loaded the game goes on from that state, and when it next saves it writes
  that state's progress over your save, as it would have at that moment. Loading a state of another player or of an
  older moment and playing on can so replace newer progress. The launcher's *Back up each save* keeps copies.
- F6 does nothing while the game is writing a save ("State not saved"); press it again a moment later.
- For some seconds after a state is loaded in a newly started game the frame rate is lower, while shaders and
  textures are made again.
- Rarely Windows has put something where the game's memory has to go. Then a load stops the game with a message in
  the log, or save states are off for that run; start the game again.
- Not tried yet: a state taken during a pre-rendered video cutscene.
- F6 and F7 are not used when a game key is set to them in the launcher's key settings.
