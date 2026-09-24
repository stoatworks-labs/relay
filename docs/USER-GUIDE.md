# Relay user guide

Relay is **an A/B cut made by a relay, bounce and all**, as an FFGL **mixer** for
[Resolume](https://resolume.com) Arena and Avenue. It cuts between this layer and the
layer below the way a relay-switched router did: the layer's opacity fader is the
coil, with hysteresis; the contacts bounce, so the frame the switch lands on is
cut into bands of the old picture, black and the new one; the monitor has to
re-lock to the new source, so the picture rolls and settles; and the open contact
is a capacitor, so the other picture's edges leak through. Nothing dissolves.

![The switching frame: bands of A, black and B where each contact bounce landed](hero.png)

*The repo's two test cards through the plugin on the frame the switch lands on:
Operate Time 6 ms, Bounce Time 2 ms, Restitution 0.63. Rendered by the offline
harness, not captured from Resolume. The top was scanned while the relay was still
on A; the black bands are the open contact in flight; below each is B, arriving,
and rolled, because the monitor has not pulled B's field phase in yet.*

> **Before you rely on this:** released at **v0.1.0**, and honestly early. The relay
> is measured, not just asserted. An offline harness drives the real plugin with two
> inputs at two different sizes, on the GPU and on Apple's software renderer. The
> switching frame's bands sit where the bounce schedule puts them, with **every
> pixel** away from a cut exactly right and all **16 contact events** seen within one
> line's blanking of when they happened. The coil switches at Pull-in on the way up
> and at Drop-out on the way down, **41 of 41 frames** bitwise. The re-lock follows
> the second-order step response to **0.002 rows** over 150 frames. The crosstalk
> doubles per octave to **0.013**. Every check carries a negative control that fails.
> All 17 controls the harness can sweep change the picture.
> It has **never been loaded into Resolume on macOS**.
> On Windows, a build of v0.1.0 loads in Resolume Arena 7.27.1, is offered as a layer's Blend Mode and as a transition, is driven by the layer's opacity fader and by a layer transition, and hides only Standard, as designed — on software rendering, and no picture of it inside Resolume has been captured, so a correct render there is not yet shown.
> **Try it on a spare layer first**, and please report anything that misbehaves.
>
> This codebase was created with AI assistance, directed and reviewed by a human
> author.

---

## Installing

Download the build for your platform. For macOS there is a universal `.dmg` or
`.zip` (Apple silicon and Intel), **Developer ID-signed and notarised** so the bundle
simply loads (the signing happens on the maintainer's Mac shortly after each release
is published, so a download made in the first minutes may need **Open** from the
context menu once), and for Windows an x64 installer or `.zip`. Every download
carries one mixer, **SW Relay**. Put it in Resolume's FFGL folder, then restart
Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout in its own folder. It really is **Extra Effects**,
even though this is a mixer: Resolume has one FFGL plugin folder, and sources,
effects and mixers all load from it. There is no `Extra Mixers`.

A mixer does not appear in the effects browser. In Resolume it appears in a
layer's **Blend Mode** list, beside Resolume's own blend modes (the same list
Resolume uses for transitions). Choose **SW Relay** as the blend mode of the upper
layer.

The Windows builds are not code-signed. Plugin files are not gated the way `.exe`
files are, so Resolume loads them as normal; only the installer trips SmartScreen,
once: **More info** → **Run anyway**.

---

## It is a mixer, not an effect

An effect gets one picture. A mixer gets two, and Relay needs both:

| Input | In the code | In Resolume | What it is here |
|---|---|---|---|
| **A** | `inputTextures[0]` | the layer **below** | The de-energised contact. Shown at Opacity 0. |
| **B** | `inputTextures[1]` | **this** layer, the one whose Blend Mode is SW Relay | The contact the energised coil makes. Shown at Opacity 1. |

To patch it, put one clip on a layer and the clip you want to cut *to* on the layer
**above** it. Then set the upper layer's **Blend Mode** to SW Relay. Handed only
one picture, the plugin declines to draw.

The two layers do not have to be the same size. Each input is read at its own
resolution.

---

## Start here

**The layer's opacity fader is the coil.** Relay's fader is a parameter called
**Opacity**, and Resolume drives a mixer parameter of that name from the layer's
own opacity fader. So once SW Relay is the upper layer's blend mode, that layer's
opacity is the coil voltage: push it up past **Pull-in** (0.7 by default) and the
relay cuts to this layer; pull it down past **Drop-out** (0.3) and it drops back to
the layer below. In between, nothing happens: there is no half-way picture, and a
fader parked at 0.5 stays wherever the relay last was.

**When you first choose SW Relay, you see this layer.** Opacity defaults to 1
(the coil energised), and in Resolume the layer's fader is usually at 1 anyway, so a
mixer dropped on a layer at full opacity shows that layer, which is what a layer at
full opacity does. Pull the fader down past 0.3 to see the layer below.

Then, in this order:

1. **Cut.** Move the fader from 0 to 1 and back, slowly. The cut lands at 0.7 on
   the way up and 0.3 on the way down, and each cut is followed by a roll: the
   picture is not genlocked by default, so the monitor pulls the new source's
   field in over about half a second.
2. **Bounce Time** and **Restitution**, high. Cut again and watch the frame the cut
   lands on: bands of the old picture, black and the new one, across the top part
   of the frame. It is one frame, so it is brief; Resolume's freeze on a clip will
   not hold it, but a screen recording will.
3. **Genlocked** on, and **Switch Point** set to Vert Interval. Cut again: nothing
   rolls, and the bounce lands in the blanking, so the cut is clean. That is the
   good router.
4. **Crosstalk** up. The picture you are *not* looking at ghosts through, edges
   first.

---

## Raster

**Standard**: PAL (625 lines, 50 fields) or NTSC (525 lines, 59.94 fields). It sets
the line period and the number of active lines the host frame is scanned as, which
is what turns milliseconds of bounce into lines of picture. **Resolume Arena does
not show a mixer's first parameter**, and Standard is first on purpose, so in
Resolume it is always PAL; the difference is the height of the bands (a 1 ms
flight is 15.6 lines at PAL and 15.7 at NTSC). NTSC is reachable in other hosts
and in the offline harness.

**Switch Point**: **Anywhere** (the default) lets the contact break wherever the
scan happens to be when the operate time is up, so the bands land somewhere down
the frame, cut mid-line at the pixel where the contact moved. **Vert Interval**
waits for the blanking before the next field, so the break is at the top of the
frame and the bounce runs down from line 1. With a bounce shorter than the blanking,
the whole switch happens where nothing is drawn, and the cut is clean.

---

## Coil

**Opacity**: the coil voltage. In Resolume this is the **layer's opacity fader**,
and the mixer's own Opacity control is overridden by it; see *Start here*.

**Pull-in** (0.7) and **Drop-out** (0.3): the two thresholds. Up past Pull-in the
coil energises and the relay goes to B; down past Drop-out it de-energises and goes
to A. Set Drop-out above Pull-in and the drop happens at the pull-in instead: a
relay without hysteresis.

**Operate Time** (0 to 40 ms; 8 ms by default): from the coil crossing a threshold
to the contact leaving its rest. A small telecom relay operates in 5 to 15 ms. With
Switch Point Anywhere this is what decides where in the frame the break lands: the
scan has moved on by that much since the frame began. The same time is used for
release, which real relays do faster.

**Select**: a switch that inverts what an energised coil selects, so at a fader of
1 it swaps the two pictures. **Take**: a button that does the same once per press.
Both are for a host that does not drive Opacity; in Resolume they still work, on top
of the fader. A change of either is a switch, with its own bounce and roll.

---

## Contacts

**Bounce Time** (0 to 4 ms; 1 ms by default): the first bounce interval, and the
flight from the broken contact to the made one, which is taken to be the same
length. A PAL line is 64 µs, so 1 ms is about fifteen lines.

**Restitution** (0 to 0.9; 0.45 by default): each bounce interval is the previous
one times this. Each interval is half closed (B shows) and half open (black), so
the bands shrink down the frame by this ratio until one is shorter than a line,
where the schedule stops. The whole bounce lasts the first interval divided by one
minus this: 4 ms and 0.9 is 40 ms, most of a frame; 1 ms and 0.45 is under 2 ms.

**Open Level** (0 by default): what an open contact shows. 0 is black, a signal of
black rather than transparency; higher is grey. The open contact is opaque whatever
the two pictures' alpha.

---

## Sync

**Genlocked** (off by default): on, the two sources share field timing and the new
picture arrives locked. Off, the new source's field is **Phase Offset** away (0 to 1
of a field; 0.25 by default), and the monitor's vertical PLL has to pull it in: the
picture appears rolled by that much and settles as a second-order step response.

**Lock Time** (0.05 to 2 s, geometric; 0.32 s by default): the loop's natural
period. The roll takes about four periods divided by the damping to settle.

**Damping** (0.1 to 2, geometric; 0.45 by default): below 1 the picture overshoots
and rings; at 1 it is critically damped; above 1 it creeps in. At the defaults it
overshoots once and settles in about half a second; at Lock Time 2 s and Damping
0.1 it swings up and down the picture for ten seconds.

The line PLL is a look, not a model: the first lines after the rolled source's own
vertical interval are thrown sideways by up to 3% of the width, scaled by the roll,
which is the tear a real monitor showed at the seam.

Crosstalk of a rolled source is *not* rolled: the leak reads the other picture at
the pixel's own row.

---

## Crosstalk

**Crosstalk** (0 by default): how much of the unselected picture leaks through the
open contact's stray capacitance. It is a high-pass, so edges leak and flat areas do
not, and the leak rises 6 dB an octave up to the corner. The value is the leak *at
the corner*: 0.3 means 30% of the other picture's amplitude there, less below,
up to 1.4 times that well above.

**Corner** (0.25 to 4 MHz, geometric; 1 MHz by default): the capacitor's corner, in
the video signal's own frequency, converted to cycles across the picture through the
standard's 52 µs active line (1 MHz is 52 cycles per picture width at PAL). Low, the
whole other picture ghosts through; high, only its finest edges. The filter is
realised as up to 80 taps along the line, so the time constant is clamped to 16
output pixels: at 1080p the corner cannot go below 0.37 MHz, at 4K 0.73 MHz, and
below 1440 wide the whole range is reachable.

---

## How it works: a raster scanned in time

A video frame is not a picture that appears; it is scanned, a line at a time, 64
µs a line at PAL, 18.4 ms for the active field. A relay's events happen in
milliseconds. So each event lands somewhere *in* the picture: the contact breaks at a
line and a pixel along it, flies for a bounce time (fifteen lines of black), makes,
dwells, breaks again, and so on, each flight shorter than the last, until a flight
is under a line and could not cut a picture. The plugin builds that schedule on the
CPU, in double precision, in seconds of real elapsed time from the host's clock,
and the shader asks one question per pixel: which line, how far along it, and so
which contact the relay was on when that pixel was scanned. Everything else is a
fetch from one of the two inputs.

The roll is one number per frame: the new source's phase offset times the
closed-form step response of a second-order loop at the frame's time. Once the
envelope is under a ten-millionth of the picture the roll is set to exactly zero, so
the selected picture comes back bit for bit.

Each host frame is treated as one field starting at its own time, with the vertical
interval just before it. The host's frame rate and the standard's field rate are not
tied together: at 60 fps a PAL scan runs 1.7 ms past the next frame's start, and an
event in that overlap shows at the bottom of one frame and is the next frame's
starting state.

---

## Performance

It is one pass with two texture reads, plus up to 80 reads a pixel when Crosstalk
is on. The worst figures from the offline harness on an Apple M4 Max:

| | at rest, ms/frame | Crosstalk 1, ms/frame | Share of a 60 fps frame |
|---|---|---|---|
| 1280×720 | 0.026 | 0.026 | 0.2% |
| 1920×1080 | 0.039 | 0.047 | 0.3% |
| 2560×1440 | 0.051 | 0.086 | 0.5% |
| 3840×2160 | 0.109 | 0.127 | 0.8% |

At this size the measurement itself costs about as much as the work, so take the
ceiling, not the average. The relay's schedule is built on the CPU once per switch
and costs nothing worth measuring.

No timing has been taken inside Resolume.

---

## If it looks wrong

**Only one of the two pictures shows, whatever I do.** The fader is on one side of
the hysteresis. In Resolume the fader is the layer's opacity, not the Opacity slider
in the mixer's panel; and it has to go *past* 0.7 or 0.3, not to them.

**Moving the Opacity slider does nothing.** That is Resolume overriding it with
the layer's opacity fader. Use the layer's fader.

**The cut looks like a plain cut.** At the defaults it nearly is: a 1 ms bounce with
Restitution 0.45 is a couple of black bands fifteen lines high on one frame, and a
roll of a quarter of the picture that settles in half a second. Raise Bounce Time
and Restitution for the bands, lower Damping and raise Lock Time for the roll, and
raise Crosstalk for the ghost.

**The bands are always in the same place.** With Switch Point Anywhere they land
Operate Time into the scan, from the top; change Operate Time to move them. With
Vert Interval they always start at the top.

**I want NTSC.** Not in Resolume Arena: Standard is the mixer's first parameter and
Arena does not show it. The only visible difference is band height.

**The mixer does nothing at all.** A shader that fails to compile looks exactly
like that. The plugin writes a small log:

```
macOS    ~/Library/Logs/relay/relay.YYYY-MM-DD.log
Windows  %LOCALAPPDATA%\relay\logs\relay.YYYY-MM-DD.log
```

It records when an instance is created, the host's name and version and the file
it was loaded from, the GL vendor, renderer and version when it is put on a layer,
one line per switch saying what drove the coil, and an error line if the shader
failed to compile.

---

## Known limits

- **Standard is hidden in Resolume.** Arena does not show a mixer's first
  parameter, so Relay puts Standard there on purpose. In Resolume it is always
  PAL. Checked with Relay in Arena 7.27.1 on Windows: the one hidden control is Standard.
- **The fader is the layer's opacity.** In Resolume the mixer's own Opacity
  slider is overridden by the layer's opacity fader, so it cannot be set or
  automated separately. A layer **transition** does drive it (checked in Arena 7.27.1: with SW Relay as the transition blend mode, a 2 s transition is one cut at Pull-in on the way up and one at Drop-out on the way down). The autopilot triggers clips, which is that case.
- **Never loaded into Resolume on macOS.** In Resolume on Windows it has run only
  on software rendering, and its picture there has not been captured. No graphics card but the
  Mac it was built on has run it.
- **The switching frame is one frame.** Nothing in the plugin holds it; the video
  and the browser demo hold it for you, the plugin does not.
- **The dwell, the approach flight, the tear and the settle threshold are model
  constants**, argued in the repository's AGENTS.md; no relay's bounce and no
  monitor's PLL were characterised.
- **A switch during a switch** keeps the old schedule's events before the new
  break and replaces the rest.
- **Output alpha is the selected picture's**, so cutting between an opaque clip and
  a transparent one cuts the transparency too, as a router would. The open contact
  is opaque.
- **No presets** and no OpenFX version.
- **There is a browser demo** at [relay-demo.stoatworks-labs.com](https://relay-demo.stoatworks-labs.com).
  It is a port to a web page, not the plugin: the shaders run in WebGL2 and the
  relay's CPU half is rewritten in JavaScript. The page lists what it does not
  reproduce.

---

## About

The last group, **About**, carries a credit line (name, version, licence and
maker) and buttons that open this user guide, the project page, the source on
GitHub and the support page in your browser.

## Reporting something

[github.com/stoatworks-labs/relay/issues](https://github.com/stoatworks-labs/relay/issues).
A screenshot, your Resolume version, the settings, the composition's resolution
and frame rate, and the day's log are usually enough.
