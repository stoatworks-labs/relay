/**
 * Relay (RL01) — the browser demo.
 *
 * Relay is an FFGL **mixer**: Resolume hands it two textures, the layer below
 * (A, `inputTextures[0]`, the de-energised contact) and this layer (B,
 * `inputTextures[1]`, the contact the energised coil makes), and the layer's
 * opacity fader as the coil voltage. It is the suite's second mixer with a
 * demo, and the arrangement is wipe's: the kit hands a demo one input, so A
 * is the kit's clip (its "Clip" dropdown, relabelled "Clip A", and the only
 * one "Use my own…" replaces) and B is a second generated clip from the kit's
 * own `sources.js`, picked from the transport's one extra dropdown. Both are
 * functions of (uv, time) and neither is footage.
 *
 * What runs for real, and what is a port:
 *
 *   - **The shader is the plugin's**: `kVertexShader` and `kRelayShader` from
 *     source/Shaders.cpp, copied across unedited by script.
 *     `demo/tools/check_shaders.py` compares them character for character and
 *     `tools/verify.sh` runs it. Every uniform the plugin sets is set here.
 *   - **The CPU half is a hand port**: model.js holds Controls.cpp (every
 *     `...FromParam`), Raster.cpp (PAL/NTSC timing and the time → (line,
 *     pixel) mapping), Model.cpp (the coil's hysteresis, the bounce schedule,
 *     the PLL step response, the crosstalk filter) and Relay.cpp's constants;
 *     this file holds the frame logic of `Relay::ProcessOpenGL` — the coil,
 *     the operate time, the Anywhere / Vertical Interval firing rule, a
 *     switch during a switch, the schedule turned into this frame's cuts,
 *     the roll and the tear, and the Take latch of `SetFloatParameter`.
 *     Nothing checks that port but a reader.
 *   - **Everything else is not the plugin**: no Resolume, no layer stack, no
 *     FFGL, GLSL ES 3.00 rather than 4.1 core, no padded textures (both
 *     MaxUVs are 1), and the page's clock in place of SetTime.
 *
 * Opacity is a slider on this page, in the Coil group where the plugin
 * declares it. In Resolume Arena it is not: a mixer parameter named Opacity
 * is bound to the LAYER's opacity fader (measured on genlock and wipe in
 * Arena 7.27.1), so the fader is the coil voltage and the mixer's own control
 * is overridden. Standard (index 0) is shown here; Arena hides a mixer's
 * first parameter. Take is FF_TYPE_EVENT; the kit has no event type, so it is
 * a toggle the renderer releases on the frame it acts, and each press flips
 * the plugin's latch exactly as SetFloatParameter does. The page says all of
 * this in its disclosure.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';
import { SOURCES, SourceRenderer } from './vendor/sources.js';
import {
  f32, clamp, clamp01,
  kPAL, kStandardCount, kVerticalInterval, kSwitchPointCount,
  operateSecondsFromParam, bounceSecondsFromParam, restitutionFromParam,
  lockTimeSecondsFromParam, naturalFrequencyFromParam, dampingFromParam, cornerHzFromParam,
  paramForOperateSeconds, paramForBounceSeconds, paramForRestitution, paramForCornerHz,
  standardOf, cutAt, kMaxCuts,
  kContactA, kContactB, Schedule, bounceSchedule, totalBounceTime,
  relockResponse, relockSlowestRate, makeCrossFilter, Coil,
  kRollSettled, kTearFraction, kTearLines, kTearRollFull, optionIndex,
} from './model.js';

//---------------------------------------------------------------------------
// The plugin's GLSL, from source/Shaders.cpp. DO NOT EDIT HERE: change the
// C++ and copy it across; demo/tools/check_shaders.py fails verify otherwise.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const RELAY = `#version 410 core

//inputTextures[0] -- Dest, the layer BELOW: A, the de-energised contact.
uniform sampler2D TextureA;
//inputTextures[1] -- Src, THIS layer: B, the contact the energised coil makes.
uniform sampler2D TextureB;

//Each input has its own. They are not the same number and there is no
//circumstance in which using one for the other is safe.
uniform vec2 MaxUVA;
uniform vec2 MaxUVB;
uniform vec2 HalfTexelA;
uniform vec2 HalfTexelB;
uniform ivec2 SizeA;   //used texels, for the crosstalk's texelFetch taps
uniform ivec2 SizeB;
uniform ivec2 OutSize; //the host frame, W x H

//The raster: how many active lines the H rows are scanned as.
uniform int ActiveLines;

//The switching frame: the state at the scan's start and the cuts, each
//( line, fraction of the line's active part, new state ), in scan order.
//State codes: 0 A, 1 open, 2 B.
uniform int State0;
uniform int CutCount;
uniform vec3 Cuts[ 104 ];

//What an open contact shows: a flat level, black by default.
uniform float OpenLevel;

//The re-lock. RolledSource is the input whose field phase the monitor is
//still pulling in (-1: none, 0 A, 2 B); Roll is its vertical offset as a
//fraction of the picture, positive down the scan. TearAmp is the line PLL's
//throw at the source's first line, TearLines its decay in lines.
uniform int RolledSource;
uniform float Roll;
uniform float TearAmp;
uniform float TearLines;

//The crosstalk: the unselected input high-passed along the line and added.
//A truncated one-pole low-pass of CrossTaps taps with per-pixel decay
//CrossDecay, weights CrossNorm * CrossDecay^i summing to 1; the leak is the
//input minus that, times CrossGain. Gain 0 skips the whole thing.
uniform float CrossGain;
uniform float CrossDecay;
uniform int CrossTaps;
uniform float CrossNorm;

//The harness's negative controls. 0 in the shipped plugin.
uniform int Fault;

in vec2 uv;
out vec4 fragColor;

const int kStateA    = 0;
const int kStateOpen = 1;
const int kStateB    = 2;
const int kFaultSharedMaxUV   = 1;
const int kFaultFlatCrosstalk = 2;

//Clamped half a texel inside the used area: GL_LINEAR at the boundary takes
//half its weight from the texture's undrawn padding.
vec4 fetchA( vec2 p )
{
	vec2 q = clamp( p, HalfTexelA, vec2( 1.0 ) - HalfTexelA );
	return texture( TextureA, q * MaxUVA );
}

vec4 fetchB( vec2 p )
{
	vec2 q = clamp( p, HalfTexelB, vec2( 1.0 ) - HalfTexelB );
	vec2 m = ( ( Fault & kFaultSharedMaxUV ) != 0 ) ? MaxUVA : MaxUVB;
	return texture( TextureB, q * m );
}

//The monitor has not pulled the new source's field in yet: its lines are
//Roll of a picture away, wrapping as a roll, and the first lines after its
//own vertical interval -- the seam -- are thrown sideways while the line PLL
//catches up.
vec2 rolled( vec2 p )
{
	float vt = 1.0 - p.y;            //from the top, the way the scan runs
	float vs = fract( vt + Roll );   //the source's own line
	float tear = TearAmp * exp( -vs * float( ActiveLines ) / TearLines );
	return vec2( p.x + tear, 1.0 - vs );
}

vec4 fetchSource( int src, vec2 p )
{
	if( src == RolledSource )
		p = rolled( p );
	return src == kStateA ? fetchA( p ) : fetchB( p );
}

vec3 texelOf( int src, int tx, int ty )
{
	if( src == kStateA )
		return texelFetch( TextureA, clamp( ivec2( tx, ty ), ivec2( 0 ), SizeA - 1 ), 0 ).rgb;
	return texelFetch( TextureB, clamp( ivec2( tx, ty ), ivec2( 0 ), SizeB - 1 ), 0 ).rgb;
}

//The open contact's capacitance: the unselected input, high-passed along
//the line. Taps step back one OUTPUT pixel at a time in picture space and
//land on the input's nearest texel, so at matched rasters they are exact.
vec3 leak( int src, vec2 p )
{
	ivec2 size = src == kStateA ? SizeA : SizeB;
	int ty = int( floor( p.y * float( size.y ) ) );
	vec3 here = texelOf( src, int( floor( p.x * float( size.x ) ) ), ty );
	if( ( Fault & kFaultFlatCrosstalk ) != 0 )
		return here;
	vec3 low = vec3( 0.0 );
	float w = CrossNorm;
	for( int i = 0; i < CrossTaps; ++i )
	{
		float u = p.x - float( i ) / float( OutSize.x );
		low += w * texelOf( src, int( floor( u * float( size.x ) ) ), ty );
		w *= CrossDecay;
	}
	return here - low;
}

void main()
{
	//Where the scan was when this pixel was drawn: its line, by integer
	//arithmetic, and its fraction of the line, which is uv.x.
	int row  = clamp( int( floor( ( 1.0 - uv.y ) * float( OutSize.y ) ) ), 0, OutSize.y - 1 );
	int line = ( row * ActiveLines ) / OutSize.y;

	int state = State0;
	for( int i = 0; i < CutCount; ++i )
	{
		vec3 c = Cuts[ i ];
		int cutLine = int( c.x );
		if( line > cutLine || ( line == cutLine && uv.x >= c.y ) )
			state = int( c.z );
	}

	if( state == kStateOpen )
	{
		fragColor = vec4( vec3( OpenLevel ), 1.0 );
		return;
	}

	vec4 colour = fetchSource( state, uv );
	if( CrossGain > 0.0 )
		colour.rgb = clamp( colour.rgb + CrossGain * leak( kStateA + kStateB - state, uv ), 0.0, 1.0 );
	fragColor = colour;
}
`;
//===========================================================================
// The parameters, in Relay.h's ParamID order, with Relay::Relay()'s names,
// groups, types, elements and defaults. The defaults that the constructor
// computes (ParamForOperateSeconds( 0.008 ) and so on) are computed here
// with the same ported functions, float-rounded as the plugin's are.
//===========================================================================
const STANDARD_NAMES = ['PAL', 'NTSC'];
const SWITCH_POINT_NAMES = ['Anywhere', 'Vert Interval'];

const ms = (seconds) => `${(seconds * 1000).toFixed(2)} ms`;

const PARAMS = [
  // Index 0, which Resolume Arena does not show for a mixer: PAL, for ever.
  { id: 'standard', name: 'Standard', type: 'option', elements: STANDARD_NAMES, default: kPAL, group: 'Raster',
    hint: 'The line period and the active line count the host frame is scanned as: PAL 64 µs × 288, NTSC 63.6 µs × 240. Index 0: Resolume Arena does not show a mixer’s first parameter, so in Arena this is PAL and stays PAL. A browser does not hide it.' },
  { id: 'switchPoint', name: 'Switch Point', type: 'option', elements: SWITCH_POINT_NAMES, default: 0, group: 'Raster',
    hint: 'Anywhere: the break lands wherever the scan is when the operate time is up. Vert Interval: it waits for the blanking before the next frame, so every cut is at the top.' },

  { id: 'opacity', name: 'Opacity', type: 'standard', default: 1.0, group: 'Coil',
    display: (v) => `${clamp01(v).toFixed(3)} of coil`,
    hint: 'The coil voltage. Up past Pull-in the relay cuts to B, this layer; down past Drop-out it drops back to A, the layer below. In Resolume this is the LAYER’s opacity fader and the mixer’s own control is overridden; here it is a slider.' },
  { id: 'pullIn', name: 'Pull-in', type: 'standard', default: 0.7, group: 'Coil',
    display: (v) => clamp01(v).toFixed(3),
    hint: 'The voltage at which the coil pulls the armature in.' },
  { id: 'dropOut', name: 'Drop-out', type: 'standard', default: 0.3, group: 'Coil',
    display: (v) => clamp01(v).toFixed(3),
    hint: 'The voltage at which it lets go. Never effectively above Pull-in.' },
  { id: 'operate', name: 'Operate Time', type: 'standard', default: paramForOperateSeconds(0.008), group: 'Coil',
    display: (v) => ms(operateSecondsFromParam(v)),
    hint: '0 to 40 ms from the coil crossing a threshold to the contact leaving its rest. The same time is used for release.' },
  { id: 'select', name: 'Select', type: 'boolean', default: 0, group: 'Coil',
    hint: 'Inverts what an energised coil selects, for a host that does not drive Opacity. A change is a switch, with its bounce.' },
  { id: 'take', name: 'Take', type: 'boolean', default: 0, group: 'Coil',
    hint: 'FF_TYPE_EVENT in the plugin — a button in Resolume. Each press flips a latch that inverts the selection, like a momentary Select. The kit has no event type, so this is a toggle the page releases on the frame it acts, which is why it blinks.' },

  { id: 'bounceTime', name: 'Bounce Time', type: 'standard', default: paramForBounceSeconds(0.001), group: 'Contacts',
    display: (v) => ms(bounceSecondsFromParam(v)),
    hint: 'The first bounce interval, 0 to 4 ms, and the approach flight from the broken contact to the made one. A PAL line is 64 µs, so 1 ms is about fifteen lines.' },
  { id: 'restitution', name: 'Restitution', type: 'standard', default: paramForRestitution(0.45), group: 'Contacts',
    display: (v) => `e = ${restitutionFromParam(v).toFixed(3)}`,
    hint: 'Each bounce interval is the previous one times this, 0 to 0.9. Bouncing stops when an interval is shorter than a line.' },
  { id: 'openLevel', name: 'Open Level', type: 'standard', default: 0, group: 'Contacts',
    display: (v) => clamp01(v).toFixed(3),
    hint: 'What an open contact shows: a flat level, black by default. Opaque — a signal of black, not the absence of a layer.' },

  { id: 'genlocked', name: 'Genlocked', type: 'boolean', default: 0, group: 'Sync',
    hint: 'On: the two sources share field timing and there is no roll. Off: the new source is Phase Offset fields away and the monitor pulls it in.' },
  { id: 'phaseOffset', name: 'Phase Offset', type: 'standard', default: 0.25, group: 'Sync',
    display: (v) => `${clamp01(v).toFixed(3)} field`,
    hint: 'How far the new source’s field timing is from the old one’s, as a fraction of a field: the roll starts here.' },
  { id: 'lockTime', name: 'Lock Time', type: 'standard', default: 0.5, group: 'Sync',
    display: (v) => `${lockTimeSecondsFromParam(v).toFixed(3)} s`,
    hint: 'The vertical PLL’s natural period, 0.05 to 2 s, geometric. Settling takes about 4 / (ζ ωn).' },
  { id: 'damping', name: 'Damping', type: 'standard', default: 0.5, group: 'Sync',
    display: (v) => `ζ = ${dampingFromParam(v).toFixed(3)}`,
    hint: '0.1 to 2, geometric. Below 1 the picture overshoots and rings; at 1 it is critically damped; above, it creeps in.' },

  { id: 'crosstalk', name: 'Crosstalk', type: 'standard', default: 0, group: 'Crosstalk',
    display: (v) => `${(clamp01(v) * 100).toFixed(1)} % leak`,
    hint: 'The open contact’s stray capacitance: the unselected input, high-passed along the line, added to the picture. This is the leak at the corner; below it the leak falls 6 dB an octave.' },
  { id: 'corner', name: 'Corner', type: 'standard', default: paramForCornerHz(1.0e6), group: 'Crosstalk',
    display: (v) => `${(cornerHzFromParam(v) / 1e6).toFixed(3)} MHz`,
    hint: '0.25 to 4 MHz in the video signal’s own frequency, geometric, converted to cycles per picture width through the standard’s active line. Clamped upward where a 16-pixel time constant would be exceeded (0.37 MHz at 1080p).' },
];

//===========================================================================
// B, the second input. The kit renders A; this renders B from the same
// generated clips, at the same raster and the same clock.
//===========================================================================
const B_CLIPS = ['grid', 'scene', 'bars', 'ramp', 'spot', 'detail', 'alpha'];
const B_DEFAULT = 'grid';

//===========================================================================
// Relay::ProcessOpenGL, as far as a browser has it.
//===========================================================================
const stats = { text: '' };

// The page's "Hold switching frame" convenience (transport, not a plugin
// control): when on, the page's clock is paused on the first frame a bounce
// schedule cuts into, so the bands can be looked at. `hold` is read by the
// renderer; `pause` is filled in once the page has mounted.
const holdState = { on: false, held: false };
let pause = () => {};

function createRenderer(gl, quad) {
  const program = new Program(gl, VERTEX, RELAY, 'relay');
  const clipB = new SourceRenderer(gl, quad);

  // The plugin's per-instance state, in Relay.h's order and names.
  let coil = new Coil();
  let takeLatch = false; // flipped by each press of Take
  let started = false;
  let lastNow = 0.0;
  let target = kContactA; // where the coil says the armature should be
  let contact = kContactA; // the settled contact
  let pending = false; // a switch waiting for its operate time / vertical interval
  let readyTime = 0.0;
  let haveSchedule = false;
  let schedule = new Schedule();

  //The re-lock.
  let rollActive = false;
  let rollStart = 0.0;
  let rollSign = 1.0;
  let rolledSource = -1;

  // Not the plugin: the page's Restart button puts the clock back to zero,
  // which a host never does to a running instance. The page answers as if
  // the instance had been created afresh -- the only honest reading of a
  // clock that went backwards.
  let switches = 0;
  function fresh() {
    coil = new Coil();
    takeLatch = false;
    started = false;
    lastNow = 0.0;
    target = contact = kContactA;
    pending = false;
    readyTime = 0.0;
    haveSchedule = false;
    schedule = new Schedule();
    rollActive = false;
    rollStart = 0.0;
    rollSign = 1.0;
    rolledSource = -1;
    switches = 0;
    holdState.held = false;
  }

  // The uniforms the plugin sets with glUniform2i / glUniform3fv directly
  // rather than through FFGLShader::Set.
  const loc = (name) => program.location(name);

  return {
    render({ input, params, width, height, time, variant }) {
      const P = (id) => f32(params.get(id));

      if (started && time < lastNow) fresh();
      const now = time;

      // Take is FF_TYPE_EVENT: 1.0 on press, 0.0 on release, and each press
      // flips the latch once (Relay::SetFloatParameter). The kit's toggle is
      // the press; releasing it here is the host's 0.0.
      if (P('take') >= 0.5) {
        takeLatch = !takeLatch;
        params.set('take', 0);
      }

      const outW = width;
      const outH = height;

      const standardIndex = optionIndex(P('standard'), kStandardCount);
      const standard = standardOf(standardIndex);
      const verticalInterval = optionIndex(P('switchPoint'), kSwitchPointCount) === kVerticalInterval;
      const genlocked = P('genlocked') > 0.5;

      //-----------------------------------------------------------------
      // The coil. Opacity is the voltage; the hysteresis is the relay's own.
      //-----------------------------------------------------------------
      const pullIn = clamp(P('pullIn'), 0.0, 1.0);
      const dropOut = clamp(P('dropOut'), 0.0, 1.0);
      const coilOn = coil.update(clamp(P('opacity'), 0.0, 1.0), pullIn, dropOut);
      const energised = ((coilOn !== (P('select') > 0.5)) !== takeLatch);
      const wanted = energised ? kContactB : kContactA;

      const operate = operateSecondsFromParam(P('operate'));
      const t1 = bounceSecondsFromParam(P('bounceTime'));
      const e = restitutionFromParam(P('restitution'));

      if (!started) {
        //The first frame: the relay is already where the coil says. No
        //event, no bounce, no roll.
        started = true;
        target = contact = wanted;
        lastNow = now;
      } else if (wanted !== target) {
        //The coil crossed a threshold this frame. The armature moves after
        //the operate time, from the frame's own time.
        target = wanted;
        pending = true;
        readyTime = now + operate;
        switches += 1;
      }

      //-----------------------------------------------------------------
      // The armature: Anywhere lands the break wherever the scan is when
      // the operate time is up; Vertical Interval waits for the blanking
      // before the first frame whose scan starts after it is up.
      //-----------------------------------------------------------------
      if (pending) {
        let breakTime = 0.0;
        let fire = false;
        if (verticalInterval) {
          if (readyTime <= now) {
            breakTime = now - standard.blanking();
            fire = true;
          }
        } else if (readyTime < now + standard.scan()) {
          breakTime = readyTime;
          fire = true;
        }

        if (fire) {
          //A switch during a switch: whatever the old schedule had done
          //up to the new break stands, and the rest is replaced.
          const next = bounceSchedule(breakTime, contact, target, t1, e, standard.line);
          if (haveSchedule) {
            const merged = [];
            for (const ev of schedule.events) if (ev.time < breakTime) merged.push(ev);
            merged.push(...next.events);
            next.events = merged;
            next.from = schedule.from;
          }
          schedule = next;
          haveSchedule = true;
          pending = false;
          contact = target;

          //The monitor starts pulling the new source in at the first make.
          rollActive = !genlocked;
          rollStart = schedule.makeTime;
          rollSign = target === kContactB ? 1.0 : -1.0;
          rolledSource = target; //the shader's state code: 0 A, 2 B
        }
      }

      //A schedule whose last event is behind even the vertical interval is
      //over: the contact is settled.
      if (haveSchedule && schedule.lastTime < now - standard.blanking()) haveSchedule = false;

      //-----------------------------------------------------------------
      // The switching frame: the state at the scan's start, and every cut
      // inside the scan as (line, fraction of the line, state).
      //-----------------------------------------------------------------
      const cuts = [];
      const cutStates = [];
      let state0 = contact;
      if (haveSchedule) {
        state0 = schedule.stateAt(now);
        for (const ev of schedule.events) {
          if (ev.time <= now) continue;
          const cut = cutAt(ev.time - now, standard);
          if (cut.line >= standard.activeLines) break;
          if (cuts.length >= kMaxCuts) break;
          cuts.push(cut);
          cutStates.push(ev.state);
        }
      }

      //-----------------------------------------------------------------
      // The re-lock: the roll at this frame's time, in double, from the
      // closed form; exactly zero once settled or genlocked.
      //-----------------------------------------------------------------
      let roll = 0.0;
      if (genlocked) rollActive = false;
      if (rollActive) {
        const phi = clamp(P('phaseOffset'), 0.0, 1.0);
        const wn = naturalFrequencyFromParam(P('lockTime'));
        const zeta = dampingFromParam(P('damping'));
        const t = now - rollStart;
        const rate = relockSlowestRate(wn, zeta);
        const settleAfter = Math.log(Math.max(2.0 * phi, kRollSettled) / kRollSettled) / Math.max(rate, 1e-9);
        if (t > settleAfter || phi <= 0.0) rollActive = false;
        else roll = rollSign * phi * relockResponse(t, wn, zeta);
      }
      const tearAmp = kTearFraction * clamp(roll / kTearRollFull, -1.0, 1.0);

      //-----------------------------------------------------------------
      // The crosstalk filter for this raster.
      //-----------------------------------------------------------------
      const cross = makeCrossFilter(clamp(P('crosstalk'), 0.0, 1.0), cornerHzFromParam(P('corner')), standard, outW);

      const shownRolled = rollActive ? rolledSource : -1;
      lastNow = now;

      //-----------------------------------------------------------------
      // The page's hold: the first frame a schedule cuts into stops the
      // clock. Not the plugin. Step walks on from here; Play resumes.
      //-----------------------------------------------------------------
      if (holdState.on && cuts.length > 0 && !holdState.held) {
        holdState.held = true;
        pause();
      }
      if (cuts.length === 0) holdState.held = false;

      //-----------------------------------------------------------------
      // The line under the picture. Not the plugin: it reads the state the
      // port chose for this frame.
      //-----------------------------------------------------------------
      const name = (c) => (c === kContactA ? 'A' : c === kContactB ? 'B' : 'open');
      const parts = [];
      parts.push(`coil ${coilOn ? 'on' : 'off'}${P('select') > 0.5 ? ', Select' : ''}${takeLatch ? ', Take latched' : ''} → ${name(wanted)}`);
      if (pending) parts.push(`operating, breaks in ${((readyTime - now) * 1000).toFixed(1)} ms${verticalInterval ? ' then waits for the vertical interval' : ''}`);
      if (haveSchedule) {
        const total = schedule.lastTime - schedule.breakTime;
        parts.push(`switching ${name(schedule.from)} → ${name(schedule.to)}: ${schedule.cycles} bounce cycles over ${(total * 1000).toFixed(2)} ms (closed form ${(totalBounceTime(t1, e) * 1000).toFixed(2)} + the approach ${ms(t1)})`);
        parts.push(cuts.length > 0
          ? `this frame starts on ${name(state0)} with ${cuts.length} cuts, the first at line ${cuts[0].line} of ${standard.activeLines}${holdState.held ? ' · HELD' : ''}`
          : `this frame is ${name(state0)} throughout`);
      } else {
        parts.push(`settled on ${name(contact)}`);
      }
      if (rollActive) parts.push(`rolling ${name(rolledSource)} by ${(roll * 100).toFixed(2)} % of the picture, ${((now - rollStart)).toFixed(2)} s in`);
      if (cross.gain > 0) parts.push(`crosstalk ${cross.taps} taps, τ ${cross.tauPx.toFixed(2)} px, corner ${cross.cornerCpw.toFixed(1)} cycles/width`);
      parts.push(`${standard.name} · switches ${switches} · t ${now.toFixed(3)} s`);
      stats.text = parts.join(' · ');

      //-----------------------------------------------------------------
      // The inputs and the draw. Every uniform Relay::ProcessOpenGL sets.
      //-----------------------------------------------------------------
      const bClip = SOURCES.find((s) => s.id === (variant ?? B_DEFAULT)) ?? SOURCES[0];
      const b = clipB.render(bClip, width, height, time);

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      program.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, input.texture);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, b.texture);
      program.setSampler('TextureA', 0);
      program.setSampler('TextureB', 1);

      // Unpadded textures in a browser: both MaxUVs are exactly 1.
      program.set('MaxUVA', 1.0, 1.0);
      program.set('MaxUVB', 1.0, 1.0);
      program.set('HalfTexelA', f32(0.5 / input.width), f32(0.5 / input.height));
      program.set('HalfTexelB', f32(0.5 / b.width), f32(0.5 / b.height));
      gl.uniform2i(loc('SizeA'), input.width, input.height);
      gl.uniform2i(loc('SizeB'), b.width, b.height);
      gl.uniform2i(loc('OutSize'), outW, outH);

      program.setInt('ActiveLines', standard.activeLines);
      program.setInt('State0', state0);
      program.setInt('CutCount', cuts.length);
      if (cuts.length > 0) {
        const packed = new Float32Array(cuts.length * 3);
        for (let i = 0; i < cuts.length; ++i) {
          packed[i * 3 + 0] = cuts[i].line;
          packed[i * 3 + 1] = cuts[i].xfrac;
          packed[i * 3 + 2] = cutStates[i];
        }
        program.setArray('Cuts', packed, 3);
      }
      program.set('OpenLevel', clamp(P('openLevel'), 0.0, 1.0));

      program.setInt('RolledSource', shownRolled);
      program.set('Roll', f32(roll));
      program.set('TearAmp', f32(tearAmp));
      program.set('TearLines', f32(kTearLines));

      program.set('CrossGain', f32(cross.gain));
      program.set('CrossDecay', f32(cross.a));
      program.setInt('CrossTaps', cross.taps);
      program.set('CrossNorm', f32(cross.norm));

      // The harness's negative controls: 0 in the shipped plugin, 0 here.
      program.setInt('Fault', 0);

      quad.draw();

      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, null);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, null);
    },
  };
}

//===========================================================================
// The page.
//===========================================================================
const mounted = mountDemo({
  name: 'Relay',
  // The FFGL type the plugin registers (PluginInfo), for the kit banner's
  // closing sentence.
  kind: 'mixer',
  pluginId: 'RL01',
  tagline:
    'An A/B cut made by a relay, bounce and all, as an FFGL mixer. The coil has hysteresis on the layer’s opacity; the contacts bounce, so the switching frame is cut into bands of A, black and B at the scanlines where each bounce landed; the monitor re-locks to the new source with a second-order roll; and the open contact leaks the other input’s edges through its stray capacitance.',
  repo: 'https://github.com/stoatworks-labs/relay',
  page: 'https://stoatworks-labs.com/software/relay/',

  blurb:
    'It is Relay’s own GLSL, ported from the repository to WebGL2, with the plugin’s CPU half — the coil, the bounce schedule, the raster mapping, the re-lock and the crosstalk filter — ported to JavaScript. Relay is a mixer, so it needs two pictures: A (the layer below) is the Clip A dropdown and B (this layer) is Clip B, both generated in this page. In Resolume the coil voltage is the layer’s opacity fader; here it is the Opacity slider.',

  sources: ['bars', 'scene', 'grid', 'ramp', 'spot', 'detail', 'alpha'],

  // The kit's one extra transport dropdown. B is transport, not a parameter
  // the plugin declares, which is exactly why it must not be in the
  // inspector.
  variants: {
    label: 'Clip B',
    default: B_DEFAULT,
    options: B_CLIPS.map((id) => {
      const s = SOURCES.find((x) => x.id === id);
      return { id, name: s.name, hint: `B, this layer: ${s.hint}` };
    }),
  },

  params: PARAMS,

  // The plugin declares no presets (a decision recorded in its AGENTS.md).
  // These are the page's: combinations of the plugin's own parameters and
  // nothing else, to put a visitor in front of each part of the relay.
  presets: {
    'Long bounce, slow relay': { bounceTime: 1.0, restitution: 0.85, operate: 0.5 },
    'Vertical Interval, no bounce': { switchPoint: 1, bounceTime: 0 },
    'Ringing re-lock': { phaseOffset: 0.5, lockTime: 0.7, damping: 0.15 },
    'Overdamped re-lock': { phaseOffset: 0.4, lockTime: 0.6, damping: 0.95 },
    'Grey open contact': { openLevel: 0.5, bounceTime: 0.75, restitution: 0.7 },
    'Crosstalk 30 %': { crosstalk: 0.3, corner: 0.5, genlocked: 1 },
    'NTSC, genlocked': { standard: 1, genlocked: 1, bounceTime: 0.6 },
  },

  differences: [
    'Two inputs, both generated here. The kit this page is built on hands a demo one input; Relay is a mixer, so B is rendered by a second copy of the kit’s own clip generator at the same raster and on the same clock (wipe’s arrangement). “Use my own…” replaces A only.',
    'Opacity is a slider. In Resolume Arena a mixer parameter named Opacity is bound to the layer’s opacity fader (measured on genlock and wipe in Arena 7.27.1): the fader is the coil voltage and writes to the mixer’s own Opacity are overridden. Arena also hides a mixer’s first parameter, which is why Standard is first and defaults to PAL — here it is shown and NTSC is reachable, because a browser does not hide it.',
    'Take is FF_TYPE_EVENT in the plugin — a button that sends 1 on press and 0 on release, each press flipping a latch. The kit has no event type, so it is a toggle here that the renderer releases on the frame it acts, which is why it blinks; the latch it flips is the plugin’s own logic, ported. Whether Arena shows a mixer’s event parameter as a button is itself an open question in the repository.',
    'The CPU half is a port, and nothing checks it but a reader. Controls.cpp, Raster.cpp, Model.cpp (the coil’s hysteresis, the bounce schedule with its half-closed dwell and one-line floor, the second-order step response, the crosstalk filter) and ProcessOpenGL’s frame logic (operate time, Anywhere and Vertical Interval firing, a switch during a switch, the schedule turned into this frame’s cuts, the roll and its settle threshold, the tear) are translated to JavaScript by hand in demo/model.js and demo/plugin.js. The shaders are checked: demo/tools/check_shaders.py fails the repository’s verify script if a character drifts from source/Shaders.cpp.',
    'The clock is the page’s, in seconds, handed straight to the frame logic as the plugin’s SetTime-derived `now` is. The plugin works out its host’s clock unit by watching it (Resolume sends milliseconds) and keeps time frame-relative so a long session does not lose millisecond resolution in a float; none of that is exercised here. Each host frame is one field scanned from its own time, so at the page’s 60 fps a PAL scan (18.4 ms) overlaps the next frame as it does in the plugin. Restart puts the clock back to zero, which a host never does; the page then behaves as a freshly created instance.',
    '“Hold switching frame” is the page’s convenience, not a plugin control: it pauses the page’s clock on the first frame a bounce schedule cuts into, so bands that the plugin shows for one frame can be looked at. Step walks on from there, one 60 fps frame at a time; Play resumes. Nothing in the plugin can hold a frame.',
    'Both MaxUVs are 1. Resolume hands a mixer two textures that are padded and usually of different sizes, and the shader applies each input’s own MaxUV — the arrangement the plugin’s --mixer check exists for. The page’s textures are unpadded, so that half of the shader runs with nothing to correct. A file of your own does arrive at its own size, so the two half-texel clamps and the crosstalk’s texel sizes differ then.',
    'Output alpha is the selected input’s, untouched, as the plugin decides; the open contact is opaque and crosstalk touches RGB only. The canvas here is drawn over black and does not show alpha, so switching between the “Shape on transparency” clip and an opaque one cuts the transparency in the plugin’s output without the page making that visible.',
    'The line under the picture is not the plugin. It reports what the port decided for this frame — the coil, the pending operate time, the schedule and how many of its cuts fall in this frame’s scan, the roll, the crosstalk filter’s taps. The plugin draws no such thing; its own record of the same state is the harness’s StateForTest and its log.',
    'The presets are the page’s. The plugin declares none; each is only a combination of the plugin’s own parameters.',
    'The About block — a text line and link buttons for the host’s panel — is absent: a web page has links of its own. The harness’s negative controls (the Fault uniform) are 0 here as in the shipped plugin. Relay has no audio path, so nothing here is missing for want of one.',
    'Nothing here is measured. The plugin’s numerical proof — every pixel of the switching frame against the schedule on PAL and NTSC, sixteen contact events placed within a line’s blanking, the roll following the closed-form step response to 0.002 rows, the leak doubling per octave, both inputs bitwise at rest — is tools/rltest in the repository, and that harness, not this page, is the reason to believe the maths.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The kit labels its clip picker "Clip". With two inputs that is ambiguous,
// so it is renamed for what it feeds, A, the layer below, and moved in front
// of Clip B: A, then B, the way the inputs are numbered. Wipe's arrangement,
// done in the page rather than the kit.
//---------------------------------------------------------------------------
for (const label of document.querySelectorAll('.transport__label')) {
  if (label.textContent === 'Clip') {
    label.textContent = 'Clip A';
    label.title = 'A, the layer below: the de-energised contact.';
    const fieldA = label.closest('.transport__field');
    const fieldB = [...document.querySelectorAll('.transport__label')]
      .find((l) => l.textContent === 'Clip B')?.closest('.transport__field');
    if (fieldA && fieldB) fieldB.before(fieldA);
  }
}

//---------------------------------------------------------------------------
// "Hold switching frame": transport, never the inspector, because it is the
// page's and not a parameter the plugin declares. It pauses the kit's clock
// the way the Pause button does, on the frame the renderer says has cuts.
//---------------------------------------------------------------------------
const transport = document.querySelector('.transport');
const playButton = transport?.querySelector('.btn--play');
if (transport && mounted?.state) {
  const holdLabel = document.createElement('label');
  holdLabel.className = 'transport__field';
  holdLabel.title = 'The page’s convenience, not a plugin control: pause the clock on the first frame a bounce schedule cuts into, so the bands can be looked at. Step walks on; Play resumes.';
  const hold = document.createElement('input');
  hold.type = 'checkbox';
  hold.id = 'hold-switching-frame';
  hold.addEventListener('change', () => { holdState.on = hold.checked; holdState.held = false; });
  const text = document.createElement('span');
  text.className = 'transport__label';
  text.textContent = 'Hold switching frame';
  holdLabel.append(hold, text);
  transport.append(holdLabel);

  pause = () => {
    if (!mounted.state.playing) return;
    mounted.state.playing = false;
    mounted.state.lastFrame = 0;
    if (playButton) playButton.textContent = 'Play';
  };
}

// The statistics line, under the transport.
const statLine = document.createElement('p');
statLine.className = 'stage__status';
statLine.setAttribute('aria-live', 'off');
statLine.dataset.role = 'relay-stats';
document.querySelector('.stage')?.append(statLine);
if (statLine.isConnected) {
  const tick = () => {
    if (statLine.textContent !== stats.text) statLine.textContent = stats.text;
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

export { mounted };
