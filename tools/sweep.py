"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at two positions against the two input cards, and
report any that made no difference at all.

    python3 tools/sweep.py [--size WxH] [--jobs N] [--binary PATH]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**This is a MIXER, so every render needs two inputs.** The harness feeds them
itself -- `--input-a` is A, the layer below, and `--input-b` is B, this layer.

**Most controls only act during a switch.** A relay at rest is a pure fetch of
one input, so Standard, Switch Point, Operate Time, Bounce Time, Restitution
and Open Level are swept on the SWITCHING frame: the harness's `--cue` sets
Opacity 0 before frame 0 and 1 before frame 2, and the frame read back is
frame 2 (`_cues` and `_frames` below). Genlocked, Phase Offset, Lock Time and
Damping act on the frames AFTER the switch, while the monitor rolls, so they
are read a few frames later still.

**The default Operate Time is 8 ms**, which is 125 PAL lines: a switch cued at
frame 2 lands mid-frame, so both inputs are in the picture.

**Opacity is swept end to end at rest**, 0 (A) against 1 (B): the fader
Resolume drives from the layer's opacity. Pull-in and Drop-out are swept
against a fader position they straddle: Pull-in 0 vs 1 with Opacity going to
0.6 pulls in or not; Drop-out with Opacity dropping to 0.4 drops out or not.

**Select and Take swap the two inputs at rest.** Take is an event: `--set
"Take=1"` before frame 0 is one press, and one press is one toggle.

**Phase Offset 1 is Phase Offset 0** -- a whole field is no offset -- so it is
swept to 0.5.

**Corner needs Crosstalk on.** At Crosstalk 0 the filter is out of circuit.

**Every name must be unique.** `--set` finds a parameter by name and takes the
first match. `rltest --names` asserts it.

**A dropdown holds its element INDEX.** `rltest --list` prints an option's real
range for exactly this reason: the SDK's own range reads back 0..1.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "rltest")
SCRATCH = tempfile.mkdtemp(prefix="rlsweep")

WIDTH, HEIGHT = 480, 270
FRAMES = 1

# Parameters that cannot or must not be swept, with the reason.
SKIP = {}

# A switch cued at frame 2, the switching frame read back.
SWITCH = {"Opacity": 0, "_cues": ["2 Opacity=1"], "_frames": 3, "Genlocked": 1}
# The same switch, read back while the monitor is still rolling.
ROLLING = {"Opacity": 0, "_cues": ["2 Opacity=1"], "_frames": 6, "Genlocked": 0,
           "Bounce Time": 0, "Operate Time": 0}

CONTEXT = {
    "Standard": SWITCH,
    "Switch Point": SWITCH,
    "Operate Time": SWITCH,
    "Bounce Time": SWITCH,
    "Restitution": dict(SWITCH, **{"Bounce Time": 0.5}),
    "Open Level": dict(SWITCH, **{"Bounce Time": 0.5}),
    "Pull-in": {"Opacity": 0, "_cues": ["2 Opacity=0.6"], "_frames": 4, "Genlocked": 1,
                "Bounce Time": 0, "Operate Time": 0},
    "Drop-out": {"Opacity": 1, "_cues": ["2 Opacity=0.4"], "_frames": 4, "Genlocked": 1,
                 "Bounce Time": 0, "Operate Time": 0},
    "Genlocked": ROLLING,
    "Phase Offset": dict(ROLLING, _high=0.5),
    "Lock Time": dict(ROLLING, _frames=12),
    "Damping": dict(ROLLING, _frames=12),
    "Crosstalk": {"Opacity": 1, "Genlocked": 1},
    "Corner": {"Opacity": 1, "Genlocked": 1, "Crosstalk": 0.5},
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
        else:
            m = re.match(r"\s*(\d+)\s+(.+?)\s{2,}(about|text|buffer)\s", line)
            if m:
                found.append((int(m.group(1)), m.group(2).strip(), m.group(3), 0.0, 0.0))
    return found


def render(path, overrides, frames):
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    for cue in overrides.get("_cues", []):
        args += ["--cue", cue]
    merged = {k: v for k, v in overrides.items() if not k.startswith("_")}
    for name, value in merged.items():
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, low, high, context = job
    frames = context.get("_frames", FRAMES)

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo, frames)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi, frames)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run that is cut off by a CI
    # timeout still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT, BIN

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--binary", default=BIN)
    args = ap.parse_args()
    BIN = args.binary
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("text", "buffer") or name in SKIP:
            skipped.append((name, SKIP.get(name, kind)))
            continue
        context = CONTEXT.get(name, {})
        work.append((pid, name, low, high, context))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
