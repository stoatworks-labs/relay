# relay — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **mixer** for Resolume Arena/Avenue that cuts
between this layer and the layer below the way a relay-switched router did:
with hysteresis on the coil, bounce on the contacts, a monitor that has to
re-lock, and a capacitor's worth of crosstalk. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT. Intended home
`github.com/stoatworks-labs/relay`, released v0.1.0 2026-09-24; v0.2.0 adds
the OpenFX build. Never loaded into Resolume on macOS; probed by hand in Arena
7.27.1 on Windows the same day (see "What Relay showed in Arena"). The fleet's third mixer, after genlock
and wipe. It also builds as an **OpenFX transition** for Resolve and Vegas
(`source/ofx/RelayOFX.cpp`, CPU render): see "The OpenFX build" below. Its
first build ran in DaVinci Resolve 21.1 on the Edit page (2026-10-03); the
current one, with Pull-in 0.5 and Ends, ran in DaVinci Resolve Studio 21.1 on
macOS on the Edit page (2026-10-04), switching at the midpoint and ending
exactly on SourceTo.

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
| `--transition` curve | **1e-9 of a frame** | `transition::CoilHistory` against closed forms: a 0→1 ramp over 50 frames pulls in at 36.25 (Pull-in 0.725 × 50); up to 1 at 30 and back to 0 at 60 pulls in at 21.75 and drops at Drop-out's 51.75; reversed 1→0 starts energised and drops at 36.25; flat 0.5 never switches; a step at frame 7 switches at 7 **exactly**; NaN outside the transition (a refusing host) with the read-back starting a duration early changes nothing. The bound is the bisection's: 32 halvings of a one-frame bracket, 2.3e-10. Negative control: Drop-out at Pull-in drops at 38.25. |
| `--transition` in-scan | **the line** | A ramp crossing Pull-in 0.3 frames (5 ms at 60 fps) after frame 10 starts, Operate 0: frame 10's first cut is on PAL line floor( 5 ms / 64 µs ) = 78, and frame 9 has none. The FFGL plugin cannot show this (it reads its fader at frame starts); see "The OpenFX build". |
| `--transition` end | **exact** at the ends of the ramp, 1e-6 at its middle | `transition::RelayStrength` against its definition: 1 under Cut at any progress; under Fade 1 up to 1 - End Length (0.85), exactly 0 from 1, 0.5 at 0.925, monotone, slope zero at both ends of the ramp (a 1e-4 step moves it 1.3e-6 = 3 ( 1e-4 / 0.15 )^2), End Length 0 a cut at 1, End Length clamped to 0.5, NaN shows the relay. A k/24 transition's last frame (23/24) judged a frame on is exactly SourceTo; judged at its own progress it would keep 0.19 of the relay (the negative control). `FadeToRows` at 0 copies SourceTo exactly. |
| `--transition` plan | **exact**, every field | `transition::PlanFromCurve` on the cue sheet as a step curve against the plan the FFGL plugin handed its shader (`StateForTest().plan`), compared with `!=` on doubles: starting contact, every cut's line, xfrac and state, open level, rolled source, roll, tear, crosstalk gain / decay / taps / norm. 50 frames × 2 rasters over five scenarios (below). Both sides are the same double arithmetic on the same floats, so there is no tolerance to derive. The first run differed by 3e-7 in one xfrac -- the harness's step curve had a 1e-9 epsilon in its floor, which moved the step 1e-9 frames early; a step must be AT its frame. Negative control: the curve a frame late is not the plan (10 cuts vs 0). |
| `--transition` pixels | **one 8-bit code** away from a cut; within one pixel of a cut on its line, counted | `pass::Render` on the plan against the GPU's RGBA8 frame. Away from a cut both passes fetch the same texels with the same weights in float; the GPU's bilinear weights are fixed-point and its float sums may fuse, which moves a channel by under one code before rounding. Measured: worst 1/255, 75,891 of 14.4 M pixels differ (8,848 on the software renderer), none by more than one code, none at a cut. Negative control: twice the bounce, 7,765 pixels off by more than one code away from any cut. |
| `--bench` | not asserted | No threshold is worth asserting on somebody else's GPU. The C++ pass is timed beside it, on every core and on one. |

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
    source/Pass.*           the same pass in C++, for the OpenFX build: every
                            function marked `//= mirrored`. Change both.
    source/Frame.*          frame::Plan -- one frame as the pass is handed it
                            (the uniforms) -- and the settle and tear constants.
    source/Transition.*     the OpenFX build's relay: the coil over a
                            Transition curve, and the frame planned from it as
                            a pure function of time.
    source/ofx/RelayOFX.cpp the OpenFX plugin: the Transition context, the
                            parameters, marshalling. No relay logic of its own.
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
    tools/rltest/           the offline harness. Two inputs. Eight checks, and
                            --transition for the OpenFX build.
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
  button: 22 parameters, 17 swept. No presets. The OpenFX build is a
  transition whose only real host so far is Resolve on macOS; see *The OpenFX
  build*. The browser demo exists and is a port, not the plugin; see *The
  browser demo* below.

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

## The OpenFX build

**What it is.** The FFGL mixer's OpenFX equivalent is the **Transition
context**: `SourceFrom` is A (what the relay rests on), `SourceTo` is B, and the
mandated `Transition` parameter -- 0 at the start of the transition, 1 at its
end, driven by the host -- is the coil voltage, the FFGL build's Opacity. CPU
render over the host's buffer, rows sliced across the host's thread suite.
Identity `com.stoatworks.relay`, label Relay, group Stoatworks, bundle
`com.stoatworks.relay.ofx`.

**What is shared, and how much.** Everything but the marshalling. `relay_dsp`
(an OBJECT library with no GL in it) holds Controls, Raster, Model, Frame, Pass
and Transition; the FFGL plugin and rltest link it beside `relay_core`, and the
OpenFX plugin links it alone. `Relay::ProcessOpenGL` now fills a `frame::Plan`
and sets every uniform from it, and the OpenFX build hands `pass::Render` the
same struct, so the two passes are handed the same thing by construction.
`HostValues` (Controls.h) is the one table of defaults both builds declare
from.

**The reformulation.** The FFGL plugin integrates: a coil primed by its first
frame, a pending switch, a schedule carried forward and merged, a roll that
switches itself off. None of that survives a host that renders frames out of
order, alone, on several threads. What survives is the curve: OpenFX lets a
plugin read a parameter at any time. So every render reads `Transition` from
the start of the transition to **the end of this frame's scan**, runs
model::Coil over the readings, bisects each change of state to the instant the
threshold was met, and replays the FFGL plugin's armature logic over those
events in closed form (`transition::PlanAt`): break at operate time (or the
blanking before the first frame starting after it), BounceSchedule, merge into
the previous schedule at the break, roll from the last make. `--transition`
shows that on frame-step curves the result is the FFGL plugin's plan exactly.

**Decisions taken without asking:**

- **Transition context only.** General would cost a user-animated Transition of
  the plugin's own, over a span as long as the clip, read back on every render,
  and in General there is no host-driven ramp to say where a transition starts.
  Resolve's own sample declares both; this declares one, and describeInContext
  refuses any other.
- **The relay starts where the coil says at the first reading**, as the FFGL
  plugin's first frame does: no switch at the start. A reversed transition
  (1 → 0) therefore starts on SourceTo and drops at Drop-out. Pull-in 0 is a
  transition that is SourceTo throughout.
- **The read-back starts the effect's duration before this frame**
  (`getEffectDuration`), which reaches the transition's start wherever the host
  puts time zero; the output clip's frame range if no duration; no history if
  neither. Capped at 3600 frames. A host that refuses a time (the parameter
  call throws) gives no reading there, and the coil skips it. Readings are
  every whole frame plus the span's ends, so an excursion narrower than a frame
  is not seen.
- **The curve is read to the end of the frame's scan, not its start.** A
  crossing while the frame is scanned therefore cuts that frame at the line
  the scan had reached. The FFGL plugin reads its fader at frame starts and
  shows the same cut a frame later. It also means a frame can see a coil change
  that the FFGL plugin would only learn of at the next frame -- at 60 fps PAL
  (an 18.4 ms scan every 16.7 ms) a break in the last 1.8 ms of the scan. The
  `--transition` scenarios keep their breaks out of that overlap (Operate 2 ms,
  not 0, in "switch during a switch"); the in-scan check exercises it on
  purpose.
- **A coil change before the armature moves replaces the pending switch**,
  judged by the BREAK time. The FFGL plugin judges by the frame the switch
  fires on, and the two differ only when the coil flips again within a frame
  of a break that fell between two scans. **A switch to where the armature
  already is does nothing**; the FFGL plugin would bounce A → A (break and
  remake) when the coil flips twice inside one operate time. Neither case is
  in the harness.
- **Every setting is read at the frame being rendered** and stands for the
  whole history: keyframing Operate Time moves the cut as if the relay had
  always been that slow. **Standard, Switch Point, Select and Genlocked do not
  animate** (`setAnimates( false )`): with them static, the roll's "genlocked
  kills it for good" and the schedule's line period are the FFGL plugin's.
- **Select and Take are one fixed choice**, `Select`: *From, then To* (rest on
  SourceFrom, the FFGL default) or *To, then From* (Select on). Take is a
  momentary latch, which has no timeline meaning.
- **Inputs are read at the output's pixel positions**, premultiplied float, a
  clip smaller than the frame transparent where it has no pixels; the pass then
  sees two pictures the output's size. The shader stretches each texture over
  the output instead; with equal sizes -- what Resolve hands a transition -- the
  two are the same. The pass itself supports two sizes, and `--transition`'s
  padded scenario checks that against the GPU.
- **Option labels are spelt out** (`Vertical Interval`, not `Vert Interval`):
  FFGL's 16-character limit does not apply. The parameter names and 0..1 ranges
  are the FFGL build's.
- **No Fault uniform.** The negative controls are GPU-side, to show the GPU
  checks can fail; the C++ pass does not mirror them.
- **Pull-in defaults to 0.5 here, 0.7 in FFGL** (`transition::kPullInDefault`;
  every other default is `HostValues`'). In Resolume Pull-in is a fader
  position. On a timeline it is where in the transition the cut lands, and at
  0.7 a one-second transition in Resolve 21.1 was still rolling on its last
  frame (6.4% of the picture not yet SourceTo) and popped on the next. 0.5 is
  the edit point of a centred transition, with half of it left to settle. The
  lead's call, 2026-10-03.
- **Ends: Fade (default) or Cut, End Length 0..0.5, default 0.15** -- the names,
  options and defaults of lenticular's and pilot's transitions, both static.
  Half a transition is still not enough (see the settling table below), so
  under Fade the last End Length crossfades the relay's picture to exactly
  SourceTo with a smoothstep in premultiplied colour; Cut is the first OpenFX
  build bit for bit. **Only the end** -- the start is the relay at rest on
  SourceFrom already, and fading it too would fade a low Pull-in's switch.
  The fade goes to SourceTo even under Select "To, then From", because
  SourceTo is what the host shows next.
- **The end is judged one frame on** (`transition::EndProgress`): the progress
  plus its rise over the last frame, while it is rising. Resolve's 24-frame
  transition switched on frame 17 at Pull-in 0.7, which fits a progress of k/24
  or (k + 0.5)/24 and not k/23 -- its last frame never reaches 1, and judged at
  its own progress the fade would leave up to 0.19 of the relay there and pop on
  the next frame. Judged a frame on, the last frame is SourceTo bitwise whether
  the host's progress reaches 1 on it or after it. Where lenticular and pilot
  judge at the frame's own progress, this differs by that one frame.
- **The frame rate falls back to 24**, read from the output clip, each input,
  then the effect, each in its own try (FUSION-FIX, 2026-10-03: Resolve's
  Fusion page reports the frame rate on the effect but on no clip, and an
  unguarded clip read escapes render as kOfxStatErrMissingHostFeature; in
  Fusion the effect's rate, the timeline's, is what is read). The
  premultiplication reads are guarded the same way, and a frame range of
  [0, 0] counts as unknown. Fusion cannot host a transition, so for Relay this
  is defence, not a feature.

**Verified (2026-10-03, M4 Max):** `rltest --transition` as tabled above, on
the GPU and on Apple's software renderer. The bundle itself in a CPU OpenFX
test host -- resolume-ofx-bridge's `ofxprobe` at 0208a04, extended in scratch
with a Transition context, keyed parameters, `--time` and `--batch` --
against `rltest --pipe` on the GPU, opaque cards, Transition keyed to step
1e-9 frames before each cue: 30 frames of four scenarios (defaults; drop-out
NTSC with a 4 ms bounce, Open Level and crosstalk; Vertical Interval, Select
inverted, genlocked, up and down; Crosstalk 1 with a ringing re-lock), worst
1/255, none off by more than one code; twice the bounce is 32,491 pixels off.
On the host's own curves at 60 fps: a 0 → 1 ramp over 24 frames leaves frames 15
and 16 SourceFrom bitwise and cuts frame 17 from row 90 of 360 (the break, 8 ms
after the crossing at 16.8, is line 72 of frame 17's scan); 0 → 1 → 0 over 48
frames opens the contact at frames 17 and 41 and not 31, and with Drop-out 0.7 at
31 and not 41; 1 → 0 is SourceTo bitwise until the drop. Frames rendered alone,
after their predecessors in one instance, and out of order hash the same.
8-bit and float renders agree bitwise. 1920×1080 in that host, 8 threads,
marshalling included: 3.3 ms at rest, switching or rolling; 9.8 ms with
Crosstalk 1. Those were measured at Pull-in 0.7 and no Ends, and re-run
unchanged with the two pinned (`ends` Cut, `pullIn` 0.7) once both existed.

**Settling, at the defaults** (Pull-in 0.5; a one-second transition, progress
k/N; 1080p; `PlanFromCurve` frame by frame, 2026-10-04). The bounce is over
about 2 ms after the break, inside the switching frame. The roll is what lasts:

| fps | switch frame | roll on the last frame | under half a row from | exactly zero from |
| --- | --- | --- | --- | --- |
| 24 | 12 | 1.8 rows, tear 0.8 px | 0.667 s after the crossing | 1.750 s |
| 25 | 13 | 1.6 rows, tear 0.7 px | 0.660 s | 1.780 s |
| 30 | 15 | 0.9 rows, tear 0.4 px | 0.633 s | 1.767 s |

At Pull-in 0.7 the last frame is rolling by 22, 21 and 17 rows. Half a
one-second transition does not hold the re-lock at any of these rates, hence
Ends. In the test host at 1080p with the cards: the frame before the switch is
SourceFrom bitwise; the last frame is SourceTo bitwise under Fade and not under
Cut (1.3%, 1.3%, 0.7% of pixels still rolling at Pull-in 0.5; 14.0%, 13.4%,
11.0% at 0.7 -- the lead's stills in Resolve gave 6.4% at 0.7 and 24 fps);
the frame before the last is mid-fade (4.9%, 4.3%, 3.1%, worst 40 codes).

**In a real host: DaVinci Resolve 21.1, Edit page, 2026-10-03** (the lead,
with the first build: Pull-in 0.7, no Ends). A 24-frame centred transition
between two stills at 24 fps: frames exactly SourceFrom until the switch on
frame 17 of 24; the bounce and the roll then played -- so Resolve answers
`Transition` at other times and the crossing search works, and the read-back
from the effect's duration reaches far enough; frames after the transition
exactly SourceTo; the roll still running on the last frame (6.4% of pixels
not yet SourceTo), then a pop. That pop is why Pull-in and Ends changed.

**In a real host: DaVinci Resolve Studio 21.1, macOS, Edit page, 2026-10-04**
(the lead, with the current build at its defaults: Pull-in 0.5, Ends Fade). A
24-frame centred transition at 24 fps: frames exactly SourceFrom until the
switch at the midpoint, frame 12 of 24 -- the settling table's switch frame;
the bounce and the roll then played; the transition's last frame exactly
SourceTo, so no pop. That last frame is the case `EndProgress` exists for
(Resolve's last frame does not reach progress 1), and it held in the host.

**No frame rate (`--quirks fusion`, 2026-10-04).** The test host's Fusion
mode is stricter than Fusion, which reports the effect's rate -- no FrameRate
on the effect or any clip, frame ranges [0, 0], the Unmapped pair and the
render-status props absent. It renders this build at frames 12, 13, 17 and 23
of a 24-frame ramp byte-identical to the normal host at 24 fps, and frames 13
and 17 differ at 25 fps, so the fallback is what was used. The previous head
(eb863fc) did NOT fail under the quirk either: its frame-rate read was already
guarded, falling back to 25, and its quirks frames equal its own 25 fps
frames. `verify.sh` runs the check when `OFXHOST` names the extended host.

**Not verified:** any real host but Resolve on macOS. Never loaded into Vegas,
Nuke or Natron. The Windows and Linux builds have only compiled (and, on
Linux, `dlopen`ed on Rocky 8 in CI); 16-bit and RGB-only clips have not been
rendered by any host.

### Declare the output frame-varying, or Fusion repeats a generator's first frame

`getClipPreferences` calls `setOutputFrameVarying( true )`. The relay works in
seconds: the coil's bounces depend on when the Transition crossed, not only on its
value now. Without that declaration a host may treat the output as fixed while the
inputs and parameters hold still. Measured 2026-10-04 in Resolve Studio 21.1's
Fusion page: every fleet generator rendered frames 20-22 byte-identical, none having
declared it, and with the declaration they animate.

A tool fed by a MediaIn is re-rendered every frame either way, so in Fusion this
changes nothing visible.

**Relay in Fusion matches the test host** (2026-10-04, Resolve Studio 21.1, a
debug build logging the plan). With Transition keyed 0→0.6 over frame 0→1 and
held, Fusion answers the curve at past times exactly (a raw-API probe read 0, 0.3
and 0.6 at frames 0, 0.5 and 1 from a render at frame 20). It reports an effect
duration of 121, so the history starts well before the crossing. The relay
computes the same roll as `ofxprobe` at every frame measured: 0.190932 / 0.190929
at frame 2, then 0.0755 and -0.0141, and 0.000135 by frame 20. The pixels show the
roll at frames 2 and 3 and a settled SourceTo by frame 20. An earlier note here
called Fusion's plain SourceTo at frames 20-22 an open question; it was only the
settled relay, and the test host's frames there differ by sub-row residue alone.

The flag changes no pixels: `ofxprobe` renders byte-identical with and without it,
on a moving sequence, on a still and under `--quirks fusion`.

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
