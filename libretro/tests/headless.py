#!/usr/bin/env python3
"""Headless regression runs of the libretro core in RetroArch (null video / audio, unthrottled).

Each set runs N frames with a scripted input (libretro.c RS12_INPUT), the core dumps every n-th picture (RS12_DUMP_DIR)
and the pictures are hashed. Hashes are compared with tests/expected/<arch>/<set>.txt; --update rewrites them.
A contact sheet of the pictures goes to the output dir when Pillow is installed.

  tests/headless.py                 # all sets, compare
  tests/headless.py tektagt pacapp  # some sets
  tests/headless.py --update        # record the current hashes as expected
Env: RS12_ROMS (default ~/system12), RETROARCH (default retroarch), RS12_CORE (default ../resys12_libretro.so)
"""
import hashlib, os, platform, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROMS = os.environ.get("RS12_ROMS", os.path.expanduser("~/system12"))
CORE = os.environ.get("RS12_CORE", os.path.join(HERE, "..", "resys12_libretro.so"))
RETROARCH = os.environ.get("RETROARCH", "retroarch")
ARCH = platform.machine()
OUT = os.path.join(HERE, "out")
COIN_START = "600:0:1,620:0:0,700:400:0,720:0:0,1500:fffff:0"  # coin, start, then mash buttons (frame:p1:sys)

# set: (frames, dump every, input script)
SETS = {
    "tektagt": (3000, 300, COIN_START),
    "tekken3": (3000, 300, COIN_START),
    "soulclbr": (3000, 300, COIN_START),
    "ehrgeiz": (3000, 300, COIN_START),
    # pacapp: the four buttons are the joystick directions (MAME: P1 UP = button 1); pressed in turn every second
    "pacapp": (4800, 300, "600:0:1,620:0:0,1000:400:0,1030:0:0,1300:400:0,1330:0:0," +
               ",".join("%d:%x:0,%d:0:0" % (f, 1 << (i & 3), f + 10) for i, f in enumerate(range(1600, 4800, 60)))),
    "ptblank2": (3000, 300, COIN_START),
    "golgo13": (6000, 600, ""),
    "technodr": (3000, 300, COIN_START),
    "mrdrillrja2": (3000, 300, COIN_START),
}


def run_set(name, frames, every, script):
    dump = os.path.join(OUT, name)
    os.makedirs(dump, exist_ok=True)
    for f in os.listdir(dump):
        os.remove(os.path.join(dump, f))
    cfg = os.path.join(OUT, "ra.cfg")
    with open(cfg, "w") as f:
        f.write('video_driver = "null"\naudio_driver = "null"\nvideo_vsync = "false"\naudio_sync = "false"\n'
                'input_driver = "null"\ninput_joypad_driver = "null"\nmidi_driver = "null"\n'
                'savefile_directory = "%s"\nsavestate_directory = "%s"\n' % (OUT, OUT))
    # fixed RTC4543 time: games show the date and seed RNGs from it
    env = dict(os.environ, RS12_INPUT=script, RS12_DUMP_DIR=dump, RS12_DUMP_EVERY=str(every), RS12_RTC="1762080000")
    log = os.path.join(OUT, name + ".log")
    with open(log, "w") as lf:
        rc = subprocess.call([RETROARCH, "--appendconfig", cfg, "-L", CORE, os.path.join(ROMS, name + ".zip"),
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
