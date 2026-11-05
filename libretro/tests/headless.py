#!/usr/bin/env python3
"""Headless regression runs of the libretro core in RetroArch (null video / audio, unthrottled).

Each set boots a fresh board (no .srm) and runs N frames with a scripted input (libretro.c RS12_INPUT:
"frame:p1_hex:sys_hex[:analog]", p1 bits 0-3 up/down/left/right, 4-9 buttons 1-6, 10 start, 12-13 buttons 7-8;
sys 1 coin 1; p1 fffff mashes from then on; analog -100..100 holds the lever / wheel; gun games shoot the screen
centre). The core dumps every n-th picture (RS12_DUMP_DIR) and the pictures are hashed. Hashes are compared with
tests/expected/<arch>/<set>.txt; --update rewrites them. A contact sheet of the pictures goes to the output dir
(out/<set>.png) when Pillow is installed: check that the last pictures show gameplay when writing a script.

  tests/headless.py                 # all sets, compare
  tests/headless.py tektagt pacapp  # some sets
  tests/headless.py --update        # record the current hashes as expected
Env: RS12_ROMS (default ~/system12), RS11_ROMS (System 11 sets, default ~/system11), RETROARCH (default retroarch), RS12_CORE (default ../resys12_libretro.so)
"""
import hashlib, os, platform, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROMS = os.environ.get("RS12_ROMS", os.path.expanduser("~/system12"))
ROMS11 = os.environ.get("RS11_ROMS", os.path.expanduser("~/system11"))
S11 = {"tekken2", "souledgea"}  # System 11 sets (RS11_ROMS, namcoc76.zip next to them)
CORE = os.environ.get("RS12_CORE", os.path.join(HERE, "..", "resys12_libretro.so"))
RETROARCH = os.environ.get("RETROARCH", "retroarch")
ARCH = platform.machine()
OUT = os.path.join(HERE, "out")


def keys(*ev, mash=None):
    """20-frame presses of (frame, p1 hex, sys hex) [, analog -100..100]; from mash on the core mashes the buttons."""
    out = ["%d:%s,%d:0:0" % (e[0], ":".join(e[1:]), e[0] + 20) for e in ev]
    return ",".join(out + (["%d:fffff:0" % mash] if mash else []))


COIN = ("0", "1")
START = ("400", "0")
B1, B2, B3 = ("10", "0"), ("20", "0"), ("40", "0")
# fresh EEPROM: most sets show FIRST POWER ON + notice for ~1500 frames before taking a coin
FIGHT = keys((1500,) + COIN, (1600,) + START, (1900,) + B1, (2000,) + B1, (2100,) + B1, mash=2200)  # pick a character, fight
PROBE = keys((1500,) + COIN, (1600,) + START, (1900,) + B1, mash=2100)
COIN_START = "600:0:1,620:0:0,700:400:0,720:0:0,1500:fffff:0"  # sets that take a coin right after the boot
# set: (frames, dump every, input script)
SETS = {
    "tektagt": (4200, 300, FIGHT),
    "tekken3": (4200, 300, FIGHT),
    "soulclbr": (4200, 300, FIGHT),
    "ehrgeiz": (3000, 300, COIN_START),
    "fgtlayerja": (3900, 300, PROBE),
    "toukon3": (3900, 300, PROBE),
    "lbgrande": (4200, 300, PROBE),
    # pacapp: the four buttons are the joystick directions (MAME: P1 UP = button 1); pressed in turn every second
    "pacapp": (4800, 300, "600:0:1,620:0:0,1000:400:0,1030:0:0,1300:400:0,1330:0:0," +
               ",".join("%d:%x:0,%d:0:0" % (f, 1 << (i & 3), f + 10) for i, f in enumerate(range(1600, 4800, 60)))),
    # pacapp2 / pacappsp: P1 START picks the mode and the song
    "pacapp2": (4800, 300, keys((1500,) + COIN, (1600,) + START, (2000,) + START, (2600,) + START, mash=2800)),
    "pacappsp": (4800, 300, keys((1500,) + COIN, (1600,) + START, (2000,) + START, (2600,) + START, mash=2800)),
    "aquarush": (3000, 300, PROBE),
    "tenkomor": (4200, 300, PROBE),
    "mdhorse": (3600, 300, PROBE),
    "kaiunqz": (4200, 300, PROBE),
    "mrdrillrja2": (4200, 300, keys((1600,) + COIN, (1700,) + START, (2000,) + START, (2500,) + START, mash=2600)),  # start skips the tutorial
    # gun games: the gun stays at the screen centre, button 1 (golgo13: 2) is the trigger
    "ptblank2": (4200, 300, COIN_START),
    "ghlpanic": (3900, 300, PROBE),
    # golgo13 / g13knd: ~50 s of ROM test first, 2 coins a game
    "golgo13": (7200, 600, keys((3700,) + COIN, (3760,) + COIN, (3900,) + START, (4100,) + B2, mash=4300)),
    "g13knd": (7200, 600, keys((3700,) + COIN, (3760,) + COIN, (3900,) + START, (4100,) + B2, mash=4300)),
    # driving: 2 coins, button 8 is the gas (truckk: the horn, button 3, starts the game)
    "technodr": (4800, 300, keys((1500,) + COIN, (1560,) + COIN, (1700,) + START, (1900,) + START, (2100,) + START) + ",2200:2000:0"),
    "kartduel": (6000, 300, keys((1500,) + COIN, (1560,) + COIN, (1700,) + START, (2400, "2000", "0"), (2700, "2000", "0"), (3000, "2000", "0")) + ",3300:2000:0"),  # the gas is the OK button
    "truckk": (7200, 600, keys((4000,) + COIN, (4060,) + COIN, (4200,) + B3, (4400,) + B3) + ",4500:2000:0"),
    # aplarail: the OK button is somewhere in the mash; the lever (analog) pushed forward starts the train
    "aplarail": (4200, 300, keys((1500,) + COIN, (1600,) + START) + ",1900:fffff:0,2300:0:0:100"),
    "ujlnow": (9000, 600, keys((2500,) + COIN, (2560,) + COIN, (2700,) + START, (4300,) + START, (5000,) + START, (5600,) + START, (6200,) + START, (6800,) + START, mash=6900)),
    # baseball: 1 player, team, stadium, then start skips to the first pitch
    "sws98": (4200, 300, PROBE),
    "sws99": (4200, 300, PROBE),
    "sws2000": (5400, 300, PROBE + "," + keys((3700,) + START, (4000,) + START, (4300,) + START, mash=4400)),
    "sws2001": (4200, 300, PROBE),
    # System 11
    "tekken2": (3600, 300, PROBE),
    "souledgea": (3600, 300, PROBE),
}


def run_set(name, frames, every, script):
    dump = os.path.join(OUT, name)
    shutil.rmtree(dump, ignore_errors=True)  # pictures and the EEPROM (.srm): every run boots a fresh board
    os.makedirs(dump)
    cfg = os.path.join(dump, "ra.cfg")
    with open(cfg, "w") as f:
        f.write('video_driver = "null"\naudio_driver = "null"\nvideo_vsync = "false"\naudio_sync = "false"\n'
                'input_driver = "null"\ninput_joypad_driver = "null"\nmidi_driver = "null"\n'
                'savefile_directory = "%s"\nsavestate_directory = "%s"\n' % (dump, dump))
    # fixed RTC4543 time: games show the date and seed RNGs from it
    env = dict(os.environ, RS12_INPUT=script, RS12_DUMP_DIR=dump, RS12_DUMP_EVERY=str(every), RS12_RTC="1762080000")
    log = os.path.join(OUT, name + ".log")
    with open(log, "w") as lf:
        rc = subprocess.call([RETROARCH, "--appendconfig", cfg, "-L", CORE, os.path.join(ROMS11 if name in S11 else ROMS, name + ".zip"),
                              "--max-frames=%d" % frames], env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=1800)
    files = sorted(f for f in os.listdir(dump) if f.endswith(".ppm"))
    hashes = ["%s %s" % (f[:-4], hashlib.md5(open(os.path.join(dump, f), "rb").read()).hexdigest()) for f in files]
    blank = files and all(len(set(open(os.path.join(dump, f), "rb").read()[20:])) <= 1 for f in files[-3:])
    sheet(dump, os.path.join(OUT, name + ".png"))
    return rc, hashes, blank


def sheet(dump, out, cols=5, tw=256):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    files = sorted(f for f in os.listdir(dump) if f.endswith(".ppm"))
    if not files:
        return
    th = tw * 3 // 4
    rows = (len(files) + cols - 1) // cols
    im = Image.new("RGB", (cols * tw, rows * (th + 12)), "black")
    dr = ImageDraw.Draw(im)
    for i, f in enumerate(files):
        x, y = (i % cols) * tw, (i // cols) * (th + 12)
        im.paste(Image.open(os.path.join(dump, f)).resize((tw, th)), (x, y + 12))
        dr.text((x + 2, y), f[:-4], fill="yellow")
    im.save(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    update = "--update" in sys.argv
    names = args or list(SETS)
    exp_dir = os.path.join(HERE, "expected", ARCH)
    os.makedirs(exp_dir, exist_ok=True)
    failed = 0
    for name in names:
        frames, every, script = SETS[name]
        rc, hashes, blank = run_set(name, frames, every, script)
        exp = os.path.join(exp_dir, name + ".txt")
        status = "ok"
        if rc != 0 or not hashes or blank:
            status = "FAIL (retroarch rc %d, %d pictures%s)" % (rc, len(hashes), ", blank" if blank else "")
        elif update:
            open(exp, "w").write("\n".join(hashes) + "\n")
            status = "recorded"
        elif os.path.exists(exp):
            want = open(exp).read().split("\n")
            diff = [h.split()[0] for h in hashes if h not in want]
            if diff:
                status = "CHANGED at frames %s" % " ".join(diff)
        else:
            status = "ok (no expected file, run with --update)"
        failed += status.startswith(("FAIL", "CHANGED"))
        print("%-10s %s" % (name, status), flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
