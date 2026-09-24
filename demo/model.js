/**
 * The relay as arithmetic, ported to JavaScript for the browser demo.
 *
 * This file is a HAND PORT of the plugin's CPU half and nothing checks it but
 * a reader: source/Controls.cpp (every `...FromParam`), source/Raster.cpp
 * (the PAL/NTSC timing and the time <-> (line, pixel) mapping),
 * source/Model.cpp (the coil, the bounce schedule, the PLL step response and
 * the crosstalk filter) and the constants Relay.cpp keeps in its anonymous
 * namespace. Everything is in doubles, as the C++ is; the plugin's parameter
 * store is float, so `f32` is applied by the caller before a value reaches a
 * conversion, exactly where `static_cast< double >( params[ ... ] )` promotes
 * a float in the plugin.
 *
 * The order and the names follow the C++ so a reader can hold the two side by
 * side. When Controls.cpp, Raster.cpp or Model.cpp change, this must change
 * with them; `demo/tools/check_shaders.py` guards only the GLSL.
 */

export const f32 = (v) => Math.fround(v);
export const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
export const clamp01 = (v) => Math.min(1.0, Math.max(0.0, v));

//===========================================================================
// Controls.h / Controls.cpp
//===========================================================================
export const kPAL = 0;
export const kNTSC = 1;
export const kStandardCount = 2;

export const kAnywhere = 0;
export const kVerticalInterval = 1;
export const kSwitchPointCount = 2;

export const kOperateMaxSeconds = 0.040;
export const kBounceMaxSeconds = 0.004;
export const kRestitutionMax = 0.9;
export const kLockTimeMinSeconds = 0.05;
export const kLockTimeMaxSeconds = 2.0;
export const kDampingMin = 0.1;
export const kDampingMax = 2.0;
export const kCornerMinHz = 0.25e6;
export const kCornerMaxHz = 4.0e6;
export const kCrossTauMaxPixels = 16.0;
export const kCrossTapsMax = 80;
export const kCrossTapsPerTau = 5.0;

const kTwoPi = 6.283185307179586;

/// lo * ( hi / lo )^t: geometric between two ends.
function geometric(t, lo, hi) {
  return lo * Math.pow(hi / lo, clamp01(t));
}

function geometricParam(value, lo, hi) {
  if (!(value > 0.0)) return 0.0;
  return clamp01(Math.log(value / lo) / Math.log(hi / lo));
}

export function operateSecondsFromParam(value) {
  return clamp01(value) * kOperateMaxSeconds;
}

export function bounceSecondsFromParam(value) {
  return clamp01(value) * kBounceMaxSeconds;
}

export function restitutionFromParam(value) {
  return clamp01(value) * kRestitutionMax;
}

export function lockTimeSecondsFromParam(value) {
  return geometric(value, kLockTimeMinSeconds, kLockTimeMaxSeconds);
}

export function naturalFrequencyFromParam(value) {
  return kTwoPi / lockTimeSecondsFromParam(value);
}

export function dampingFromParam(value) {
  return geometric(value, kDampingMin, kDampingMax);
}

export function cornerHzFromParam(value) {
  return geometric(value, kCornerMinHz, kCornerMaxHz);
}

// The inverse conversions, which the plugin's constructor uses for its
// defaults (ParamForOperateSeconds( 0.008 ) and so on). `f32` because the C++
// returns float.
export function paramForOperateSeconds(seconds) {
  return f32(clamp01(seconds / kOperateMaxSeconds));
}

export function paramForBounceSeconds(seconds) {
  return f32(clamp01(seconds / kBounceMaxSeconds));
}

export function paramForRestitution(e) {
  return f32(clamp01(e / kRestitutionMax));
}

export function paramForLockTimeSeconds(seconds) {
  return f32(geometricParam(seconds, kLockTimeMinSeconds, kLockTimeMaxSeconds));
}

export function paramForDamping(zeta) {
  return f32(geometricParam(zeta, kDampingMin, kDampingMax));
}

export function paramForCornerHz(hz) {
  return f32(geometricParam(hz, kCornerMinHz, kCornerMaxHz));
}

//===========================================================================
// Raster.h / Raster.cpp
//===========================================================================

/// A television system's line and field timing, in seconds.
class Standard {
  constructor(name, line, frontPorch, sync, backPorch, activeLines, fieldLines) {
    this.name = name;
    this.line = line;
    this.frontPorch = frontPorch;
    this.sync = sync;
    this.backPorch = backPorch;
    this.activeLines = activeLines;
    this.fieldLines = fieldLines;
  }

  /// The active part of a line: what the blanking leaves.
  active() {
    return this.line - this.frontPorch - this.sync - this.backPorch;
  }

  /// How long the active lines take to scan.
  scan() {
    return this.activeLines * this.line;
  }

  /// The vertical interval: the rest of the field.
  blanking() {
    return (this.fieldLines - this.activeLines) * this.line;
  }

  /// One field.
  field() {
    return this.fieldLines * this.line;
  }
}

//The numbers clamp carries: 625/50 and 525/59.94, line period, front
//porch, sync, back porch, active lines per field, line periods per field.
const kStandards = [
  new Standard('PAL', 64.0e-6, 1.65e-6, 4.7e-6, 5.7e-6, 288, 312.5),
  new Standard('NTSC', 1001.0 / 15750000.0, 1.5e-6, 4.7e-6, 4.7e-6, 240, 262.5),
];

export function standardOf(index) {
  return kStandards[clamp(index, 0, 1)];
}

/// The cut for an event `seconds` after the frame's scan start: the line,
/// and how far along the line's active part.
export function cutAt(seconds, s) {
  const lines = Math.floor(seconds / s.line);
  return {
    line: Math.trunc(lines),
    xfrac: (seconds - lines * s.line - s.sync - s.backPorch) / s.active(),
  };
}

/// The active line row `row` (from the top) of an H-row frame lands on.
/// Integer division, as the C++ and the shader do it.
export function lineOfRow(row, rows, s) {
  return Math.trunc((row * s.activeLines) / rows);
}

/// The moment pixel ( x, row ) of a W x H frame is scanned, in seconds after
/// the frame's scan start. The harness's independent formulation.
export function scanTime(x, row, width, rows, s) {
  return lineOfRow(row, rows, s) * s.line + s.sync + s.backPorch + ((x + 0.5) / width) * s.active();
}

//===========================================================================
// Shaders.h: the shader-side limits the CPU has to respect.
//===========================================================================
export const kMaxCuts = 104;

//===========================================================================
// Model.h / Model.cpp
//===========================================================================

/// What the contact is touching. These are the shader's state codes too.
export const kContactA = 0;
export const kContactOpen = 1;
export const kContactB = 2;

/// The closed part of each bounce interval.
export const kDwellFraction = 0.5;
/// Most bounce cycles in a schedule, whatever e says.
export const kMaxBounceCycles = 48;

/// One switch, from the break to the last make.
export class Schedule {
  constructor() {
    this.from = kContactA;
    this.to = kContactB;
    this.breakTime = 0.0;
    this.makeTime = 0.0;
    this.lastTime = 0.0;
    this.cycles = 0;
    this.events = []; // { time, state }, in time order, the break first
  }

  /// The contact state at `time`; `from` before the break.
  stateAt(time) {
    let state = this.from;
    for (const e of this.events) {
      if (e.time <= time) state = e.state;
      else break;
    }
    return state;
  }
}

/// The bounce schedule for a switch from `from` to `to` breaking at
/// `breakTime`, with the first bounce interval `t1`, restitution `e`, and the
/// raster's line period as the interval below which bouncing stops.
export function bounceSchedule(breakTime, from, to, t1, e, lineSeconds) {
  const s = new Schedule();
  s.from = from;
  s.to = to;
  s.breakTime = breakTime;
  s.events.push({ time: breakTime, state: kContactOpen });

  //The approach flight: as long as the first bounce.
  let hit = breakTime + t1;
  s.makeTime = hit;
  s.events.push({ time: hit, state: to });

  //Each cycle: closed for the dwell, open for the flight, then the next
  //hit. Stops when the next interval would be shorter than a line -- a
  //bounce the raster could not show -- or at the cycle cap.
  let interval = t1;
  let cycles = 0;
  while (interval >= lineSeconds && interval > 0.0 && cycles < kMaxBounceCycles) {
    s.events.push({ time: hit + kDwellFraction * interval, state: kContactOpen });
    hit += interval;
    s.events.push({ time: hit, state: to });
    interval *= e;
    ++cycles;
  }
  s.cycles = cycles;
  s.lastTime = s.events[s.events.length - 1].time;
  return s;
}

/// The closed form the schedule's intervals sum to: t1 / ( 1 - e ).
export function totalBounceTime(t1, e) {
  return e < 1.0 ? t1 / (1.0 - e) : 0.0;
}

/// The unit step error of a second-order loop, g( t ): 1 at t = 0, settling
/// to 0. For zeta < 1 it rings at omega_n sqrt( 1 - zeta^2 ); at 1 it is
/// critically damped; above 1 it is the sum of two decays. g( t < 0 ) = 1.
export function relockResponse(t, omegaN, zeta) {
  if (t <= 0.0) return 1.0;
  const wn = Math.max(omegaN, 1e-9);
  if (zeta < 1.0) {
    const root = Math.sqrt(1.0 - zeta * zeta);
    const wd = wn * root;
    return Math.exp(-zeta * wn * t) * (Math.cos(wd * t) + (zeta / root) * Math.sin(wd * t));
  }
  if (zeta === 1.0) return Math.exp(-wn * t) * (1.0 + wn * t);
  //Overdamped: two real poles r1, r2 = -wn ( zeta -/+ sqrt( zeta^2 - 1 ) ).
  const root = Math.sqrt(zeta * zeta - 1.0);
  const r1 = -wn * (zeta - root); //the slow one
  const r2 = -wn * (zeta + root);
  return (r2 * Math.exp(r1 * t) - r1 * Math.exp(r2 * t)) / (r2 - r1);
}

/// The slowest decay rate in the response, per second.
export function relockSlowestRate(omegaN, zeta) {
  if (zeta < 1.0) return zeta * omegaN;
  return omegaN * (zeta - Math.sqrt(zeta * zeta - 1.0));
}

/// The crosstalk filter: see Model.h. An empty one (gain 0) is what the
/// shader reads as "skip the whole thing".
export function makeCrossFilter(crosstalk, cornerHz, standard, outputWidth) {
  const f = { cornerCpw: 0.0, tauPx: 0.0, a: 0.0, taps: 0, norm: 0.0, gain: 0.0 };
  if (!(crosstalk > 0.0) || outputWidth <= 0) return f;
  const twoPi = 6.283185307179586;
  let cpw = cornerHz * standard.active();
  let tauPx = outputWidth / (twoPi * cpw);
  if (tauPx > kCrossTauMaxPixels) {
    tauPx = kCrossTauMaxPixels;
    cpw = outputWidth / (twoPi * tauPx);
  }
  f.cornerCpw = cpw;
  f.tauPx = tauPx;
  f.a = Math.exp(-1.0 / tauPx);
  f.taps = clamp(Math.trunc(Math.ceil(kCrossTapsPerTau * tauPx)), 1, kCrossTapsMax);
  f.norm = (1.0 - f.a) / (1.0 - Math.pow(f.a, f.taps));
  const atCorner = crossResponse(1.0 / tauPx, f);
  f.gain = atCorner > 0.0 ? crosstalk / atCorner : 0.0;
  return f;
}

/// |H( omega )| of that filter, omega in radians per output pixel.
export function crossResponse(omega, f) {
  //1 - norm * sum a^i e^{-j omega i}, summed as a complex geometric series.
  let re = 0.0;
  let im = 0.0;
  let w = f.norm;
  for (let i = 0; i < f.taps; ++i) {
    re += w * Math.cos(omega * i);
    im -= w * Math.sin(omega * i);
    w *= f.a;
  }
  const hre = 1.0 - re;
  const him = -im;
  return Math.sqrt(hre * hre + him * him);
}

/// The coil, with hysteresis. Primed by its first reading.
export class Coil {
  constructor() {
    this.primed = false;
    this.on = false;
  }

  /// Returns whether the coil is energised after this reading. The
  /// effective drop-out is never above the pull-in.
  update(voltage, pullIn, dropOut) {
    const drop = Math.min(dropOut, pullIn);
    if (!this.primed) {
      this.primed = true;
      this.on = voltage >= pullIn;
      return this.on;
    }
    if (!this.on && voltage >= pullIn) this.on = true;
    else if (this.on && voltage <= drop) this.on = false;
    return this.on;
  }
}

//===========================================================================
// Relay.cpp's anonymous-namespace constants, which ProcessOpenGL reads.
//===========================================================================

/// The re-lock is over when its envelope is below this fraction of the
/// picture. The roll is then EXACTLY zero.
export const kRollSettled = 1e-7;

/// The line PLL's tear: its throw as a fraction of the width at a full
/// roll, and how many lines it takes to catch up. A look, not a model.
export const kTearFraction = 0.03;
export const kTearLines = 12.0;
export const kTearRollFull = 0.125;

export function optionIndex(value, count) {
  return clamp(Math.round(value), 0, count - 1);
}
