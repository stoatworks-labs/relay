#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family. The rest of the repos get a
    rotating log, a crash report and a diagnostics bundle; an FFGL effect gets
    only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- an effect is a
      list of sliders in someone else's inspector.

    What it covers is the failure that actually happens: `InitGL` returning
    `FF_FAIL` because the shader would not compile. From the operator's side
    that looks like "the mixer does nothing", with no message anywhere. The GL
    vendor and version strings go in next to it, because a shader that
    compiles on one machine and not on another is a driver answer, not a
    source answer.
*/
namespace relay::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

/// The file this code is running out of: the bundle or DLL the host actually
/// loaded, not the place the build put it and not the place the installer was
/// told to use.
///
/// It is the only way to answer "which copy did Resolume load, from which folder?"
/// from inside the plugin, and it is written at LOAD time rather than at
/// instantiation -- a host that scans the folder, reads the plugin's name and
/// then never offers it to the operator leaves a log with this line and
/// nothing after it, which is a different answer from an empty folder.
std::string modulePath();

} // namespace relay::diag
