"""Scripted button sequences for unattended runs (AC_INPUT_SCRIPT, runtime/hid_service.h).

  python -m astral_chain.input_scripts highway   -> prints the script

Masks: A 0x1, B 0x2, ZL 0x100, ZR 0x200, + 0x400, Left 0x1000, Down 0x8000, left stick left 0x10000.
Times are seconds after the first input sample. Steps are repeated where the game may not be ready yet; extra
presses land on screens that ignore them.
"""
import sys


def highway(offset: float = 0.0) -> str:
    """Title -> load the first save (character creation next) -> name/confirm -> opening highway chase, firing."""
    steps = []
    def press(t, mask, length=0.3):
        steps.append(f'{t + offset:g}-{t + offset + length:g}:{mask:x}')
    for t in (30, 34, 38):                      # title: ZL+ZR (hold 2 s)
        press(t, 0x300, 2)
    press(46, 0x1)                              # main menu: A
    press(56, 0x1)                              # LOAD DATA: A
    for t in range(62, 106, 4):                 # "play with the selected data?": Left, A
        press(t, 0x10000, 0.5)
        press(t + 2, 0x1)
    for t in range(106, 126, 6):                # skip the cutscene: + twice
        press(t, 0x400)
        press(t + 1, 0x400)
    for t in (130, 133, 136):                   # character select, then "Name" (software keyboard)
        press(t, 0x1)
    for t in (140, 141, 142):                   # down to "Confirm"
        press(t, 0x8000)
    press(144, 0x1)
    for t in range(147, 256, 3):                # confirmations, cutscenes
        press(t, 0x1)
    press(256, 0x200, 80)                       # highway chase: hold ZR (rapid fire)
    return ';'.join(steps)


def explore(duration: float = 900.0) -> str:
    """highway(), then repeated gameplay input until `duration` s: walk forward, attack, confirm, dodge, and a
    paired + every 12 s (skips cutscenes; in play it opens and closes the menu). Uses "@period/duty" repeats."""
    start = 336
    steps = [highway()[:-len(';256-336:200')] if highway().endswith(';256-336:200') else highway()]
    steps += [f'256-{duration:g}:200@1/0.6',          # ZR (attack / rapid fire) 60% of each second
              f'{start}-{duration:g}:20000@4/3',        # left stick up (walk forward) 3 of every 4 s
              f'{start}-{duration:g}:1@3/0.3',          # A (dialogue, prompts)
              f'{start}-{duration:g}:2@7/0.2',          # B (dodge)
              f'{start + 5}-{duration:g}:400@12/0.3',   # + ...
              f'{start + 6}-{duration:g}:400@12/0.3']   # ... + again 1 s later
    return ';'.join(steps)


def wander(duration: float = 1200.0) -> str:
    """Title -> load the latest save (the menu's default slot), then a 20 s movement cycle in the stick-direction
    bits (16 left, 17 up, 18 right, 19 down; right stick 20-23): forward 8 s, forward+right 3 s, forward 2 s,
    forward+left 3 s, back 2 s, camera turn; attacks, confirms, dodge-dash and paired + presses throughout."""
    steps = []
    def press(t, mask, length=0.3):
        steps.append(f'{t:g}-{t + length:g}:{mask:x}')
    for t in (30, 34, 38):
        press(t, 0x300, 2)
    press(46, 0x1)
    press(56, 0x1)
    for t in range(62, 100, 4):
        press(t, 0x10000, 0.5)
        press(t + 2, 0x1)
    s, d = 110, duration
    steps += [f'{s}-{d:g}:20000@20/8', f'{s + 8}-{d:g}:60000@20/3', f'{s + 11}-{d:g}:20000@20/2',
              f'{s + 13}-{d:g}:30000@20/3', f'{s + 16}-{d:g}:80000@20/2', f'{s + 18}-{d:g}:400000@20/1',
              f'{s}-{d:g}:200@1/0.5', f'{s}-{d:g}:1@2.5/0.3', f'{s}-{d:g}:2@6/1.5', f'{s}-{d:g}:100@9/0.3',
              f'{s + 5}-{d:g}:400@15/0.3', f'{s + 6}-{d:g}:400@15/0.3']
    return ';'.join(steps)


SCRIPTS = {'highway': highway, 'explore': explore, 'wander': wander}

if __name__ == '__main__':
    print(SCRIPTS[sys.argv[1]](*(float(a) for a in sys.argv[2:])))
