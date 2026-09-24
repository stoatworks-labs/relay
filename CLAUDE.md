# relay

An A/B cut made by a relay — bounce and all — as an FFGL **mixer** for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT. Public, released v0.1.0; never loaded into Resolume on
macOS, probed by hand in Arena 7.27.1 on Windows (the fleet's third mixer,
after genlock and wipe).

Read `AGENTS.md` before changing the schedule, the raster mapping, the PLL or
any tolerance in the harness. Read `~/Projects/resolume/genlock/AGENTS.md`
(the fleet's account of how an FFGL mixer behaves, with the Arena
measurements) before touching `ProcessOpenGL`.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Effects`
  (yes, Extra Effects, although this is a mixer: Arena has one FFGL folder and
  no `Extra Mixers` — measured on genlock and wipe.)
- Render a frame offline: `./build/rltest --out /tmp/f.png --size 1920x1080`
- Choose the two inputs: `--input-a video --input-b graphic`
  (a is **A**, the layer below, the de-energised contact; b is **B**, this
  layer, the contact the energised coil makes. Also `quads-a`, `quads-b`,
  `black`, `white`, `flat`, `card-a`, `card-b`, `grating`, `band`.)
- Set anything by name: `--set "Bounce Time=0.5" --set "Genlocked=1"`
  (options by index: Standard 0 PAL, 1 NTSC; Switch Point 0 Anywhere,
  1 Vert Interval)
- Force a switch: `--cue "2 Opacity=1" --frames 3` sets Opacity before frame 2
  and reads back frame 2, the switching frame. Repeatable; this is how the
  sweep exercises controls that only act during a switch.
- List parameters: `./build/rltest --list`
- Film through it (the fleet's `--pipe` format, with a second input):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/rltest --pipe --size WxH
  [--pipe-src FILE_OR_FIFO] [--src-size WxH] [--fps N] [--script cues.txt] |
  ffmpeg -f rawvideo -pix_fmt rgba -s WxH -i - out.mov`. stdin is A (the layer
  below), `--pipe-src` is B (this layer; without it `--input-b` is held).
  Cues are `frame Name value`, value in the parameter's host units (0..1 or
  the option index). **Standard parameters ramp between keys; options,
  booleans and events step** — a relay's whole point is a switching event.
  The first key holds before it. Unknown names and the About block are
  refused. The clock is SetTime in **ms**, frame-relative, as Arena sends it.
  A partial frame at EOF ends the run. SIGPIPE is ignored: a closed stdout is
  exit 1.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~2 min)
- No name over 16 characters or duplicated, parameter 0 is Standard, the coil
  input is Opacity: `./build/rltest --names`
- Two inputs, two sizes, two MaxUVs, and the guards: `./build/rltest --mixer`
- At rest A and B come back bitwise, alpha included, even after a roll has
  settled: `./build/rltest --ends`
- Pull-in and Drop-out, Select and Take: `./build/rltest --hysteresis`
- The switching frame's bands against the bounce schedule, PAL and NTSC:
  `./build/rltest --bounce`
- Vertical Interval puts every cut at the top: `./build/rltest --vi`
- The roll is the second-order step response: `./build/rltest --relock`
- The leak doubles per octave below the corner: `./build/rltest --crosstalk`
- One character of the shipped GLSL fails --bounce: `./build/rltest --mutation`
- ms/frame, 720p through 4K, at rest and with Crosstalk on: `./build/rltest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Any check on Apple's software renderer, as the GPU-less CI runner gets it:
  `RLTEST_RENDERER=software ./build/rltest --bounce` (verify.sh runs them all)
- The harness writes the plugin's log; point `RELAY_LOG_DIR` elsewhere before
  running it around an Arena session (verify.sh does).

Every check runs at 640x360 and 320x180 and carries its own negative control.

## Notes
- **This is an `FF_MIXER`.** The type is the eighth argument of
  `CFFGLPluginInfo`; `SetMinInputs`/`SetMaxInputs` are a **separate**
  declaration. `verify.sh` asserts both through `oxbow probe`.
- **`inputTextures[0]` is A (the layer below), `[1]` is B (this layer).** Each
  has its own `MaxUV`, half-texel inset and texel size, applied once in its
  own fetch. The vertex shader passes UV through unscaled.
- **The scoped bindings are declared interleaved** — activate(0), bind(0),
  activate(1), bind(1) — because each clears to 0 on exit rather than restoring.
- **The CPU builds the switch, the shader only cuts.** `Model.cpp` makes the
  bounce schedule in double, in seconds of real elapsed time; `Relay.cpp` turns
  the part inside this frame's scan into a starting state and a list of cuts —
  (line, fraction of the line, new state) — and the shader compares each
  pixel's line (`( row * ActiveLines ) / H`, integer) and `uv.x` against them.
- **Each host frame is scanned as one field starting at its SetTime.** The
  vertical interval is the blanking *before* the frame's first line. The
  host's frame rate and the standard's field rate are not tied together.
- **The coil input is `Opacity`, and the name is load-bearing.** Resolume binds
  a mixer parameter named `Opacity` to the layer's opacity fader (measured on
  genlock and wipe). Up past Pull-in: B. Down past Drop-out: A. `Select` and
  `Take` invert what an energised coil selects, for hosts without the binding.
- **The roll is one number per frame**, `Phase Offset` times the closed-form
  step response at the frame's time, in double; it is set to exactly 0 once the
  envelope is under 1e-7 of the picture, which is what makes the selected
  input bitwise again (`--ends`).
- **Output alpha is the selected input's**, untouched; the open contact is
  opaque black (a signal of black, not transparency); crosstalk touches RGB
  only. See AGENTS.md, "Decisions".
- **The crosstalk filter is `texelFetch` taps**, so no interpolation is in it;
  its corner is in the signal's own MHz, converted through the standard's
  active line, and clamped to a 16-pixel time constant (80 taps).
- **The clock is frame-relative and in double** before anything reaches the
  shader. Resolume counts milliseconds and a float stops resolving them at ~5e8.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every ranged parameter
  is 0..1 and the conversions live in `Controls.cpp`.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `relay_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- **Parameter 0 is sacrificial.** Resolume Arena does not expose a mixer's
  first parameter (measured on genlock and wipe). So index 0 is `Standard`,
  whose default (PAL) is right if it can never be reached. Never put a control
  a mixer needs there. `--names` and `verify.sh`'s oxbow step assert it.
- FFGL id is `RL01`. Display name `SW Relay`.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by the backend's syncs
  (`sync-about.py`, `sync-attributions.py`); edit the source tables there.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured
  offline through the real plugin class. On Windows: probed by hand in Arena
  7.27.1 on software rendering, see AGENTS.md.
- No presets, no OpenFX port. There is a user guide (`docs/USER-GUIDE.md`).

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/relay/relay.YYYY-MM-DD.log
