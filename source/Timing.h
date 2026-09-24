#pragma once

/**
    The host's clock, and the one thing Relay takes from it: real elapsed
    time, in seconds, in double, frame-relative.

    A relay's events are milliseconds apart and the whole plugin is "where
    was the scan when the contact moved", so the time since the switch has
    to be real elapsed time -- not a frame count -- and it has to survive
    the two clock lessons the fleet has already paid for:

    - **Resolume hands over milliseconds, not seconds.** The FFGL header never
      says which, and hosts disagree. `Clock` settles the unit by watching the
      host's clock against a real one for a few frames -- the ratio names the
      unit -- exactly as tinsel, genlock and wipe do. Measured on genlock and
      wipe in Arena 7.27.1: a MIXER gets SetTime every frame, in milliseconds.
    - **A float cannot hold the host's clock.** Resolume counts milliseconds
      from the start of the session, and past about 4.99e8 ms a 32-bit float
      can no longer represent consecutive milliseconds. So the first settled
      reading becomes the epoch, everything downstream is `now - epoch` in
      double, and what the shader is handed is never an absolute time: the
      contact events are converted to (line, fraction of a line) relative to
      the frame's own scan start, and the roll to a fraction of the picture.
*/
namespace relay::timing
{

/// The host's clock, in seconds, with its unit worked out by observation.
class Clock
{
public:
	/// Called from the plugin's SetTime with whatever the host said.
	void Observe( double hostTime );

	/// Advance to this frame. Must be called once per ProcessOpenGL, after
	/// Observe. Returns seconds since the epoch.
	double Tick();

	/// Seconds since the epoch, as of the last Tick.
	double Elapsed() const
	{
		return elapsed;
	}

	/// The multiplier taking the host's unit to seconds: 1.0 for seconds,
	/// 0.001 for milliseconds, 0 while undecided. Declared rather than
	/// inferred by the offline harness, which knows what it sends.
	void SetScaleForTest( double scale )
	{
		scale_ = scale;
	}

	double Scale() const
	{
		return scale_;
	}

	bool Observed() const
	{
		return raw_ >= 0.0;
	}

	double Raw() const
	{
		return raw_;
	}

	void Votes( int& seconds, int& millis ) const
	{
		seconds = secondsVotes_;
		millis  = millisVotes_;
	}

private:
	double raw_        = -1.0;///< the host's last reading, in the host's unit
	double lastRaw_    = -1.0;
	double lastWall_   = -1.0;
	double wallStart_  = -1.0;
	double scale_      = 0.0;
	double epoch_      = 0.0;
	bool epochSet_     = false;
	int secondsVotes_  = 0;
	int millisVotes_   = 0;
	double elapsed     = 0.0;
};

} // namespace relay::timing
