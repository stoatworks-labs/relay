# relay — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **mixer** for Resolume Arena/Avenue that cuts
between this layer and the layer below the way a relay-switched router did:
with hysteresis on the coil, bounce on the contacts, a monitor that has to
re-lock, and a capacitor's worth of crosstalk. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT. Intended home
`github.com/stoatworks-labs/relay`, released v0.1.0 2026-09-24. Never loaded
into Resolume on macOS; probed by hand in Arena 7.27.1 on Windows the same day
(see "What Relay showed in Arena"). The fleet's third mixer, after genlock
and wipe.

`CLAUDE.md` is the command reference. This file is the *why*: the idea, every
number in the harness and where it comes from, the traps this build actually
hit, what is verified and what is assumed, and the decisions taken without
asking.

For how an FFGL mixer behaves at the ABI — the type being one argument, the
input count being a separate declaration, which base class and why, what
Resolume does and does not do — read genlock's `AGENTS.md`
(`~/Projects/resolume/genlock/AGENTS.md`). Nothing here contradicts it; what
this build learned *beyond* it is under "The traps".

---

## The one idea

**A video frame is scanned in time, and a relay's events happen in
milliseconds.** About fifteen PAL lines each. So each part of the relay lands
somewhere in the picture:

    the coil        hysteresis on the layer's opacity fader: up past Pull-in
                    selects B, down past Drop-out selects A, nothing in between
    the armature    the break comes Operate Time after the coil says so, and
                    the scan is wherever it is: that line, that pixel
    the contacts    open (black) for the approach flight t1; hit; closed for
                    d t1, open for ( 1 - d ) t1; hit; ... t1 e^k ... until an
                    interval is under one line. Bands of A, black and B.
    the monitor     the new source's field is Phase Offset away; the vertical
                    PLL pulls it in as a second-order step response, in double,
                    once per frame; the picture rolls and settles
    the capacitor   the unselected input, high-passed along the line, added

The CPU does the relay (`Model.cpp`, `Relay.cpp`) in double, in seconds of real
elapsed time from `SetTime`. The shader (`Shaders.cpp`) does one thing: for
each pixel, which line and how far along it, and therefore which contact the
relay was on. Everything else is a fetch.

---

## Every number in the harness

Every check runs at **640×360 and 320×180** (CI's raster), on this Mac's GPU
**and** on Apple's software renderer (`RLTEST_RENDERER=software`, what a
GPU-less runner gets), and each tolerance is derived from something physical,
never fitted. Where a measurement is sub-pixel it goes through a **float
framebuffer**.

| Check | The number | Where it comes from |
|---|---|---|
| `--mixer` guards | none | Five `FFResult` comparisons. No raster, no rasteriser. |
| `--mixer` sentinel | **135** in summed channel difference | genlock's: the nearest card colour is 302 (A) and 315 (B) from the magenta padding, asserted, and half of that is unreachable by any blend of two card colours. The third render is a switching frame — Operate 9 ms puts the break mid-frame — so both inputs are fetched with their own MaxUV in one picture. |
| `--mixer` quadrant means | **1 of 255** | One 8-bit code. Bilinear interpolation of a constant region is the constant on any rasteriser; the interior is inset by `ceil( out / used ) + 1` pixels. |
| `--mixer` marker | **one source texel**, each axis | The quantum the marker's edges are drawn on. |
| `--ends` | **zero bytes**, all four channels | At rest the state is a uniform and the fetch is at the pixel's own texel centre with MaxUV 1. The cards' alpha is 96 down one strip and 128 in a disc so a wrong alpha would show. After a switch, the roll is set to EXACTLY 0.0 once the envelope is under 1e-7 of the picture (`kRollSettled`; 113 frames at the defaults), so `RolledSource` is −1 and the fetch is the plain one. Negative controls: a frame in the bounce is neither card; three frames after the switch the picture is still rolling. |
| `--hysteresis` | **zero bytes**, 41 frames × 2 settings × 2 rasters | Operate 0 and Bounce 0 make every frame one card. Thresholds sit half way between the twentieths the fader steps by (0.725, 0.275), so no frame lands on one and the float comparison has a margin of 0.025. With Drop-out 0.8 above Pull-in 0.425 the drop is at the pull-in. Negative control: `kFaultNoHysteresis` (drop-out at the pull-in) fails the down sweep. Plus Select, Take, a held Take and a second press, bitwise. |
| `--bounce` pixels | **zero pixels wrong** away from a cut | The prediction is written a second way: the harness computes each pixel's scan time in SECONDS (`raster::ScanTime`) and compares against the schedule's event times; the shader compares an integer line and `uv.x` against (line, fraction). They agree exactly except within one pixel's scan time of a cut on the cut's own line, where the software renderer's 1e-5 on `uv.x` can move the comparison; those pixels are counted separately and not asserted. Measured 0 wrong at every raster and standard; 0–42 within a pixel. |
| `--bounce` events | **one line's blanking + one pixel** at 360 rows (12.1 µs PAL, 11.0 NTSC); **+ one line** at 180 rows (76.2 / 74.6 µs) | The events are read off the picture line by line (see the trap) as the scan time of the first pixel of the new state. An event in a line's blanking shows at the next active pixel, up to `line − active` later; when H < activeLines a line no row samples adds a whole line (never more than one: `2H ≥ activeLines` is asserted). Measured worst 10.4 µs and 74.4 µs. The count of events must equal the schedule's: 16. |
| `--bounce` law | **e within 2 × tol / flight**, at least 2 comparisons; **the tail** | Out of the picture: successive open flights shrink by e, each ratio's tolerance from two events each within tol, compared while that is under 0.35 (2 flights at 180 rows, 5 at 360). The bounce lasts t₁ / ( 1 − e ) minus the tail the raster cannot show, which is under `line / ( 1 − e )` = 0.16 ms: measured 4.860 ms of 5.000. |
| `--bounce` negative | `kFaultBounceInFrames` | Every contact event snapped to the start of the frame it falls in — the bounce timed in frames, the spec's negative control. Fails 2 assertions (the pixel map and the event count). |
| `--vi` | frame index exact; **no A pixel**; **zero bytes** with no bounce | The switch lands on the first frame whose scan starts after the operate time is up: `2 + ceil( operate × fps )`. Its break is in the blanking before that frame, so no pixel of A can be in it, whatever the operate time (0, 3, 9.7, 16 ms). Negative control: Anywhere with 9.7 ms leaves A across the top. |
| `--relock` trajectory | **0.02 rows**, bound 0.0036 (360) / 0.0018 (180) | The band's centre is the weighted circular mean of the rows' brightness: bilinear interpolation of a translated band is a convolution with a symmetric tent, so the centroid translates exactly, wrap included. What is left is the GL's 1e-5 on the interpolated coordinate the fetch is made at, plus 2⁻²⁴ for the float the roll travels in, times H; asserted ≤ tol / 3. Measured worst 0.0020 rows over 150 frames, ζ 0.447 and ζ 2. |
| `--relock` signature | **one frame** on the zero crossing; **log decrement within 2( 1 − cos( ωd / 120 ) ) + 2 tol/H/peak + 0.02**; **one frame** on the half period | Read off the picture, not the formula: the first sign change at (π − atan( √(1−ζ²)/ζ )) / ωd; the log of the first two extremes' ratio at π ζ / √(1−ζ²) = 1.571 (measured 1.565, 1.564), the tolerance being how far a 60 Hz sample can miss a peak; the extremes half a damped period apart. Negative control: `kFaultRelockFirstOrder` (a plain exponential) fails 4 assertions. |
| `--crosstalk` filter | **1e-4 relative**, bound 1.3e-6 / 7e-7 | The taps are `texelFetch` at `floor( uv × W )`, half a texel from any boundary, so the GL's 1e-5 on uv cannot move one; the sum is at most 81 float32 terms of at most 1, 81 × 2⁻²⁴. The input is the exact bytes the harness uploaded, projected the same way, so 8-bit quantisation is not in the comparison. Measured 1.6e-7 and 2.5e-7. |
| `--crosstalk` octave | **2 within 0.05** | The physics, out of the picture: 2→4→8 cycles per width, well under the 52-cycle corner, doubles per octave. The stated filter's own departure from 2 there is printed beside it (0.0119, 0.0112); measured 0.0106 and 0.0127. Negative controls: 32 cycles leaks more than 4× 2 cycles; `kFaultFlatCrosstalk` (the other input itself, not its high-pass) fails 3 assertions. |
| `--crosstalk` corner | **1e-12** | `gain × |H( corner )| = Crosstalk` on the CPU: at the corner the leak equals the setting, by construction. |
| `--mutation` | **fails** | One character of the shipped GLSL — `/ OutSize.y` → `/ OutSize.x` in the line mapping — fails 3 `--bounce` assertions; the shipped text through the same hook passes and renders the default path's bytes. |
| `--bench` | not asserted | No threshold is worth asserting on somebody else's GPU. |

**Negative controls live in the shipping class.** `Relay::SetFaultForTest`
takes a bitmask of `relay::Fault`; the shipped plugin carries 0 and nothing
but the harness sets it. Each fault builds the wrong answer into the real
`ProcessOpenGL` or the real shader (`Fault` uniform), so the check is shown to
reject the plugin's own wrong version rather than an imitation.

**The mutation test ships** (`--mutation`) and `verify.sh` runs it. By hand,
once, on the committed tree (2026-09-24): `uv.x >= c.y` → `uv.x <= c.y` in
the cut comparison failed **12** `--bounce` assertions (10,084 pixels wrong at
640×360 PAL, 36 events for 16) and, correctly, nothing in `--vi`, `--ends` or
`--hysteresis`, none of which has a mid-line cut. Reverted from a copy,
`touch`ed against the same-second make trap.

### Would this hold on another rasteriser, at another raster?

- `--mixer`: yes — constant regions, one source texel, as genlock argued.
- `--ends`: yes — a uniform branch and a texel-centre fetch; the only
  rasteriser dependence is whether GL_LINEAR at an exact texel centre returns
  the texel, which the 8-bit rounding forgives.
- `--hysteresis`, `--vi`: yes — whole frames of one card; no sub-pixel claim.
- `--bounce`: yes — the line is integer arithmetic on both sides; the only
  float comparison is `uv.x` against a fraction, and pixels within one pixel
  of a cut on its line are counted, not asserted. At 180 rows the tolerance
  gains a line because rows skip lines; at 360 rows several rows share a line
  and the event list is read per line. Neither depends on the rasteriser.
- `--relock`: yes — the centroid is a linear functional of a translated band;
  the bound is the spec's 1e-5 × H, asserted ≤ tol / 3.
- `--crosstalk`: yes — `texelFetch` has no interpolation in it; the bound is
  float32 summation.
- Two rasterisers have run all of it: this Mac's GPU and Apple's software
  renderer, at both rasters. llvmpipe and any other GPU have not.

---

## The traps

Ordered by how much time they cost.

**The shader's state codes and the roll's source code were different
codes.** States are 0 A, 1 open, 2 B; the first version set `RolledSource` to
0 or 1 and the shader compared it with the state — so B never rolled, and the
only thing that noticed was `--ends`' negative control ("three frames after
the switch the picture is still rolling"), which is why that negative control
exists. `rolledSource` is now the contact code.

**Rows that share a line are not scan order.** At 360 rows and 288 lines, 1.25
rows sit on each line, and a cut mid-line is on both of them at the same
pixel. Reading events off the picture row by row counted every such cut
twice (36 events for 16) and a state change at column 0 that never happened.
The event list is read per LINE, from the line's first row, and the other
rows of the line are asserted identical.

**A skipped line costs a whole line of tolerance, plus the blanking.** At 180
rows the row→line map `( r × 288 ) / 180` skips every fifth line, and an
event in a skipped line shows at the next row's first pixel: up to one line
plus the sync and back porch later. The first tolerance had the line and
forgot the blanking, and failed by 10 µs.

**A `%s` through a double-only `fmt`.** The harness's `fmt` takes doubles;
two labels passed a `const char*` to `%s` and printed `(null)`. Cosmetic,
but a check's label is its documentation.

**The fleet's cue sheet holds the first key before its frame.** A cue file
with `2 Select 1` and nothing at frame 0 has Select ON from frame 0 — that is
the documented rule, not a bug, and it looked like the pipe ignoring Opacity
for a quarter of an hour. `verify.sh`'s pipe step keys every track at 0.

**The Bash tool here is zsh.** `${PIPESTATUS[1]}` is empty; `bash -c` or
`pipestatus`. `verify.sh` is bash and uses `PIPESTATUS`.

**Whether a switch lands in THIS frame is not "ready ≤ now".** The scan of a
PAL frame runs 18.4 ms past its SetTime, so an operate time of 8 ms ends
inside the frame that saw the coil move. The rule is `ready < now + scan`
for Anywhere, and `ready ≤ now` (then the blanking before now) for Vertical
Interval. An event that falls between two frames' scans is folded into the
later frame's starting state.

Inherited from genlock, wipe and tinsel, and all bitten again on cue: the
scoped bindings clearing rather than restoring; `StoatworksAboutParams.h`
needing the SDK first; the OBJECT library; the 0..1 clamp on STANDARD
defaults; the synthetic clock; the same-second make.

---

## Shape of the code

    source/Shaders.cpp      the pass. One vertex, one fragment: which contact
                            was the relay on when this pixel was scanned;
                            the roll; the crosstalk taps.
    source/Relay.*          the plugin: type, parameters, the two inputs, and
                            the state machine that turns the coil into a
                            schedule and the schedule into this frame's cuts.
    source/Model.*          the relay as arithmetic: the coil, the bounce
                            schedule, the PLL step response, the crosstalk
                            filter. No GL.
    source/Raster.*         PAL and NTSC timing (clamp's numbers) and the
                            time <-> (line, pixel) mapping, both ways.
    source/Controls.*       0..1 host parameters to ms, e, seconds, MHz.
    source/Timing.*         the host clock, the epoch, real elapsed time.
    source/Diag.*           a log file, for the shader that will not compile.
    tools/rltest/           the offline harness. Two inputs. Eight checks.
    tools/sweep.py          no control is silently dead.
    tools/verify.sh         all of it.

---

## Decisions taken without asking

**`Opacity` defaults to 1.** A mixer dropped on a layer at full opacity shows
this layer, which is what a layer at full opacity does. In Resolume the
layer's fader overrides it from the first frame anyway. The first frame takes
the relay to wherever the coil already says, with no event.

**`Select` and `Take` invert the energised coil's choice.** The spec asked for
both "for hosts without the fader binding". XOR with the coil is the one
definition that works with the fader too: at layer opacity 1, Select swaps
the two, and Take is a momentary Select. A change of either is a switch, with
its bounce.

**The dwell fraction is 0.5 and the approach flight is t₁.** The spec says
"between hits the output is open", which read literally means B is never
visible during the bounce and the bands are all black. A real contact dwells
in contact after each hit (compliance), so each interval is half closed, half
open, and the shrinking bands of A/black/B the spec describes appear. The
approach — break to first make — is one bounce time long. Both are model
constants in `Model.h`, not measurements of a relay.

**The schedule stops at one line.** A bounce shorter than a line period could
not cut a picture, and the truncated tail is under `line / ( 1 − e )`, which
the `--bounce` total asserts.

**Each host frame is one field starting at its own SetTime, and the vertical
interval is the blanking before it.** The alternative — a free-running field
clock the host frames sample — makes "the frame the switch lands on" depend
on the beat between two rates and puts the VI somewhere in the middle of a
host frame. A frame that starts at its SetTime has its VI just before it,
which is where a VI switch has to land for "every cut at the top" to be true.
The overlap at 60 fps / PAL (an 18.4 ms scan every 16.7 ms) is stated, not
hidden.

**A VI switch with Operate 0 breaks 1.6 ms before the frame that saw the
coil.** Acausal by a blanking, and invisible: nothing in that frame was
scanned before its own start.

**The re-lock is one number per frame.** `Phase Offset × g( t )` at the
frame's time, in double, on the CPU. Within a frame it is a translation, which
is what makes it checkable to 0.002 rows; a per-pixel evolution would change
the roll by a part in 10⁴ across a frame and be worth nothing.

**The roll is set to exactly 0.0 when the envelope is under 1e-7 of the
picture.** Otherwise the selected input never comes back bitwise. At 4K that
is 0.0004 of a row.

**The tear is a look.** Rows within about twelve lines after the rolled
source's own vertical interval are thrown sideways by up to 3% of the width,
scaled by the roll. Nothing measures it and no real monitor was consulted for
the shape; `--relock`'s band is uniform along the line so the tear cannot
move its centroid.

**Output alpha is the selected input's.** Resolume's demo clips are DXV with
alpha, so alpha is passed through untouched at rest (`--ends` asserts all four
channels bitwise on cards whose alpha is not 255 everywhere). An open contact
is opaque black — it is a signal of black, not the absence of a layer — and
crosstalk is added to RGB only. Switching between an opaque and a transparent
clip therefore cuts the transparency too, which is what a router would do.

**The crosstalk corner is in the signal's MHz, and clamped.** The capacitor's
time constant is a time, so its corner is a frequency of the video signal,
converted to cycles per picture width through the standard's 52 µs active
line (1 MHz = 52 cycles/width at PAL). The shader realises the one-pole
low-pass as a truncated exponential of up to 80 `texelFetch` taps, so the time
constant is clamped to 16 output pixels — a floor of 0.73 MHz at 4K, 0.37 at
1080p, nothing below 1440 wide. Stated in `Controls.h`.

**The leak at the corner equals the setting.** `Crosstalk 0.3` leaks 30% of
the unselected picture's amplitude at the corner, less below it (6 dB an
octave), up to √2 × that well above. A gain normalised to the far asymptote
would have made the setting a number nobody sees.

**Crosstalk of a rolled source is unrolled.** The leak reads the other input
at the pixel's own row. Documented, not fixed: a ghost of a rolling picture
inside a rolling picture is not something a check could tell from a bug.

**PAL by default, at index 0.** Arena hides a mixer's first parameter, so in
Resolume the raster is PAL for ever. Both standards are measured; NTSC is
reachable in every other host and in the harness.

**No presets.** Seventeen controls in five groups did not need one, and the
preset machinery is the fleet's largest source of host-behaviour assumptions.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple M4 Max, macOS 26.4.1),
2026-09-24, at 640×360 and 320×180, on the GPU and on Apple's software
renderer:** everything in the table above, and the README's Status table
restates it with the numbers. `tools/verify.sh` runs all of it on a fresh
universal build: 2 shaders compile through glslc, 9 suites twice, 17 controls
swept, the pipe's three assertions, `plugMain` exported, `x86_64 arm64`,
plist, ad-hoc codesign, `oxbow probe` reading SW Relay / RL01 / mixer /
inputs 2..2 / Standard at 0.

**The render cost**, `rltest --bench`, 120 frames after a 20-frame warm-up,
`glFinish` both sides, worst of two runs:

| | at rest, ms/frame | Crosstalk 1, ms/frame |
| --- | --- | --- |
| 1280×720 | 0.026 | 0.026 |
| 1920×1080 | 0.039 | 0.047 |
| 2560×1440 | 0.051 | 0.086 |
| 3840×2160 | 0.109 | 0.127 |

The crosstalk path is 80 texel fetches a pixel and still under a millisecond
at 4K. As genlock found, a tenth of a millisecond is close to what a
`glFinish` round trip costs to observe; take the ceiling and do not read a
20% change as a regression.

**Assumed, or not yet done:**

- **Never loaded into Resolume on macOS.** On Windows it was probed by hand in
  Arena 7.27.1 (below). Padded inputs and `SetTime` in ms are still inherited
  from genlock and wipe, not re-measured on Relay.
- **CI runs both platforms**; the Windows DLL compiled first time on MSVC
  (`kPi`, `<cmath>`, no `near`/`far`, no GLSL 4.10 reserved word as an
  identifier — all four checked before the first push).
- **Two rasterisers, not all.** Nothing has run on llvmpipe or another GPU.
- **The dwell fraction, the approach flight, the tear and the settle
  threshold are model constants**, argued above and measured against
  themselves. No relay's bounce and no monitor's PLL were characterised.
- **The 60 fps / PAL scan overlap** is a model choice: a frame's 18.4 ms scan
  runs 1.7 ms past the next frame's start, and an event in that overlap
  appears at the bottom of one frame and is the next frame's starting state.
- **A switch during a switch** keeps the old schedule's events before the new
  break and replaces the rest. Not measured by a check; the sweep never does
  it.
- **The hero image** is the harness's render, not Resolume's.
- The About block is generated now (`sync-about.py`), with the User guide
  button: 22 parameters, 17 swept. No presets, no OpenFX port. The browser demo
  exists and is a port, not the plugin; see *The browser demo* below.

---

## What Relay showed in Arena

A CI build of v0.1.0 in **Resolume Arena 7.27.1** (build 15990) on win-lab —
Windows x64, Mesa llvmpipe, no GPU — on 2026-09-24. The fleet's Arena gate
cannot gate a mixer, so it was probed by hand over Arena's REST API (wipe's
recipe: a still on layer 2, SW Relay as layer 3's Blend Mode) and read back from
the plugin's own diag log, which since bba55b5 logs one line per switch.

- **Loads from Extra Effects.** Arena's log shows `Created Relay mixer`; the
  diag log shows `initialised` on `Mesa llvmpipe ... 4.5 (Core Profile)` and
  `host=Resolume Arena version=7.27.1 15990 loaded from C:\Users\lab\Documents\Resolume Arena\Extra Effects\Relay.dll`.
  No error lines in either log.
- **Offered in the Blend Mode list and in the transition list**, both under
  `SW Relay`.
- **Index 0 is hidden, and it is Standard.** The mixer panel (REST) exposes 21
  of the 22 declared parameters; the missing one is Standard. Third mixer to
  show Arena hiding a mixer's first parameter, after genlock and wipe.
- **`Opacity` is the layer's opacity fader — answered, with the plugin's own
  log.** With a clip connected on the layer, setting the layer's opacity to
  0.2, 0.85, 0.2 and 1.0 read back as the mixer's `Opacity`, and the diag log
  recorded `switch to A (coil off, Opacity 0.200000)`, `switch to B (coil on,
  Opacity 0.850000)`, A at 0.2, B at 1.0. A REST write of 0.15 to the mixer's
  own `Opacity` was overridden (read back 1.0, the layer's).
- **No instance exists until the layer has a clip.** With no clip connected on
  layer 3, the mixer's `Opacity` did not follow the layer fader over REST and
  accepted a write of 0.2 — nothing was rendering, so nothing was bound. A
  first probe read that as "not bound" until the clip was added. Any mixer
  probe must connect a clip on the mixer's own layer first.
- **`Take` is shown as a button** (`ParamEvent`, like the About buttons). A
  REST press logged `switch to A (coil on, Opacity 1.000000, Take latched)`;
  a second press 1.5 s later logged nothing, and later switches still said
  `Take latched`, so over REST Arena sent the press and no release (a held
  Take counts once, by design). Whether a click in Arena's panel sends the
  release is not established. `Select` on and off switched B then A, logged
  with `Select on`.
- **A layer transition drives `Opacity` — answered, open since genlock.** With
  SW Relay as layer 3's *transition* blend mode and duration 2 s, connecting a
  second clip produced `switch to B (coil on, Opacity 0.711614)`, `switch to A
  (coil off, Opacity 0.050988)` and `switch to B (coil on, Opacity 0.709958)`:
  the transition ramps the mixer's `Opacity` through the relay, one cut at
  Pull-in per fade. The transition's own instances are separate from the
  layer's (two `Created Relay mixer` lines). The autopilot was not tried, but it
  triggers clips, which is the transition case.
- **Not established:** no frame of the mixer's output was captured (Arena's
  REST does not serve a mixer's picture), so a correct render in Resolume is
  not claimed. Padded inputs and `SetTime` in milliseconds are still inherited
  from genlock and wipe.

## Open questions

1. ~~Does the layer's transition or autopilot move `Opacity`?~~ **The
   transition does — answered on Relay 2026-09-24** (above): a 2 s transition
   produced one cut at Pull-in on the way up and one at Drop-out on the way
   down. The autopilot triggers clips, which is that case; not separately tried.
2. **Should the release time differ from the operate time?** Real relays
   release faster than they operate. One control was chosen; a second is a
   parameter away.
3. **Should the bounce dwell be a control?** It is the difference between
   bands of B during the bounce and none. A constant for now.
4. **NTSC is unreachable in Arena** because it is at index 0 — confirmed on
   Relay itself (Standard is the one hidden parameter). If Arena's hiding of
   the first parameter is ever explained (or a dummy first parameter proven to
   work), Standard could move.
5. ~~What does Arena do with an event parameter on a mixer?~~ **Shown as a
   button (`ParamEvent`), and a REST press reaches the plugin as a press**
   (above). Still open: whether a click in the panel sends the release, since
   over REST a second press 1.5 s later was not counted.

---

## The browser demo

**<https://relay-demo.stoatworks-labs.com>**, served from `demo/` by this repo's
own Worker (`wrangler.toml`). Built 2026-09-24 on the shared kit
(`stoatworks-backend/resolume-demo`, vendored into `demo/vendor/`), wipe's
two-input arrangement, the suite's second mixer with a demo.

**What runs for real.** The plugin's two shaders, `kVertexShader` and
`kRelayShader`, spliced into `demo/plugin.js` from `source/Shaders.cpp` by
script, unedited, and compiled by the kit's `port()` (the version line and
precision qualifiers, nothing else) into WebGL2. Every uniform `ProcessOpenGL`
sets is set by the page, `SizeA`/`SizeB`/`OutSize` through `glUniform2i` and
`Cuts` through `glUniform3fv` as the plugin does. `demo/tools/check_shaders.py`
compares the copies with the C++ character for character and `tools/verify.sh`
runs it.

**What is a port, checked by nobody but a reader.** `demo/model.js` is
Controls.cpp (every `...FromParam`, applied to float32-rounded values as the
plugin's floats are), Raster.cpp (both standards, `CutAt`, `LineOfRow`,
`ScanTime`), Model.cpp (`Coil`, `BounceSchedule` with the half-closed dwell,
the one-line floor and the 48-cycle cap, `RelockResponse` in all three damping
regimes, `RelockSlowestRate`, `MakeCrossFilter`, `CrossResponse`) and the
constants Relay.cpp keeps in its anonymous namespace (the settle threshold, the
tear). `demo/plugin.js` carries `ProcessOpenGL`'s frame logic — the coil, the
operate time, the Anywhere / Vertical Interval firing rule, a switch during a
switch, the schedule turned into this frame's starting state and cuts, the roll
and the tear, the crosstalk filter — and `SetFloatParameter`'s Take latch. A
run of the port in Node against the numbers the README's harness table
records: the 2 ms / e 0.6 PAL schedule has **16 events** and lasts **4.860 ms**
after the make against the closed form's 5.000; the default loop's first zero
crossing is at **0.1145 s** and its log decrement **1.571**; the 4 K corner
floor is **0.735 MHz**; the coil pulls in at 0.75 and drops at 0.25 with
Pull-in 0.725 / Drop-out 0.275. That is the port agreeing with the harness's
published results, not a C++-vs-JS comparison of the same frame: none exists.

**Decisions taken without asking:**

- **Two inputs from one kit**, as wipe did. A (the layer below,
  `inputTextures[0]`) is the kit's clip, relabelled `Clip A` and the only one
  "Use my own…" replaces. B (this layer) is a second `SourceRenderer` from the
  kit's own `sources.js`, at the same raster and on the same clock, picked by
  the kit's one extra transport dropdown (`demo.variants`, labelled `Clip B`).
  B is transport, not a parameter the plugin declares, so it is not in the
  inspector. Defaults: A colour bars, B the geometry card.
- **Opacity is a slider**, in the Coil group where the plugin declares it, with
  the plugin's default of 1. In Arena it is the layer's opacity fader and the
  mixer's own control is overridden; the banner and the disclosure say so.
- **Standard is shown**, although Arena hides a mixer's parameter 0: a browser
  does not, and hiding it would be inventing a host behaviour. NTSC is
  therefore reachable on the page and not in Arena; the disclosure says so.
- **Take is a toggle the renderer releases.** FF_TYPE_EVENT; the kit has no
  event type (toolpath's and flyback's answer). The toggle going to 1 is the
  press; the renderer flips the latch once and sets it back to 0, which is the
  host's release. The latch logic is `SetFloatParameter`'s, ported.
- **"Hold switching frame" is a transport checkbox, not a parameter.** When
  on, the page pauses its own clock (the kit's `state.playing`, the same thing
  the Pause button does) on the first frame a bounce schedule cuts into, so a
  frame the plugin shows for one host frame can be looked at. Step walks on
  from there; Play resumes; the next switch holds again. The plugin has no
  such thing and the disclosure says so.
- **The clock is the page's**, in seconds, handed to the frame logic as
  `now`. The plugin's unit voting and epoch are host plumbing and are not
  exercised. Restart puts the page's clock back to zero, which a host never
  does; the port then resets itself as a freshly created instance (a clock
  going backwards has no other honest reading). Disclosed.
- **Both MaxUVs are 1**: the page's textures are unpadded, so the per-input
  MaxUV the `--mixer` check exists for has nothing to correct here. Disclosed.
- **Alpha is not shown.** Output alpha is the selected input's, as the plugin
  decides; the page's canvas draws over black, so a cut between the
  transparent clip and an opaque one is not visible as a cut of alpha.
  Disclosed rather than adding a backdrop the kit only offers to effects.
- **A statistics line under the picture** reports what the port decided for
  the frame: the coil, a pending operate time, the schedule (cycles, its
  length against the closed form), how many cuts fall in this frame and where
  the first lands, the roll, the crosstalk filter's taps. The plugin draws no
  such thing.
- **Presets are the page's.** The plugin declares none (a decision above);
  each is only a combination of the plugin's own parameters.
- **Absent:** the About block; the harness's `Fault` uniform is 0 as shipped.
  Relay has no audio path, so nothing is missing for want of one.
- **The host is a Worker route, not a custom domain.** stoatworks-labs.com
  reached Cloudflare's 100 Workers custom domains on 2026-09-24, so
  `relay-demo` is a proxied AAAA `100::` record made through the API plus a
  `[[routes]]` entry in `wrangler.toml`, as slowscan's and teletext's are.
  Deleting the record takes the page dark while deploys stay green.

**Verified 2026-09-24** headlessly (Chrome + SwiftShader through
`stoatworks-backend/release/cdpshot.py`'s Chrome class) on the local server
and again on the live host: no console errors or exceptions (SwiftShader's
"GPU stall due to ReadPixels" performance notes come from the screenshot
capture); the 17 parameters in the plugin's order and five groups; Opacity
1 → 0.1 with the hold on pauses on a frame that starts on B with **10 cuts
from line 125 of 288**, reads 14 horizontal band edges and 47 near-black rows
in the canvas, and differs from the settled frame by a mean 47 levels; Take at
Bounce 4 ms / e 0.81 holds a frame with 5 cuts (20 cycles over 24.74 ms) that
differs from the 1 ms frame by 92 levels; Step shows the next frame with 38
cuts from line 14; the Take toggle reads Off again; Crosstalk 0.5 changes the
settled picture; embed mode drops the banner and keeps the hidden statement.
Deploy with `cf-run npx wrangler deploy` from the repo root, or push to main:
`.github/workflows/deploy.yml` (wipe's, renamed) deploys `demo/` and checks the
live `<head>` is this build.

---

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). The mixer
section of genlock's `AGENTS.md` is the part that belongs there; this repo
adds the raster-timing and negative-control shape.
