#include "Diag.h"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>

#if defined( _WIN32 )
	#include <windows.h>
#else
	#include <dlfcn.h>
	#include <sys/stat.h>
#endif

namespace relay::diag
{
namespace
{
constexpr const char* kAppName = "relay";

std::mutex g_mutex;
std::string g_path;
bool g_ready = false;

std::string environmentVariable( const char* name )
{
	const char* value = std::getenv( name );
	return value ? std::string( value ) : std::string();
}

std::string homeDirectory()
{
#if defined( _WIN32 )
	std::string local = environmentVariable( "LOCALAPPDATA" );
	if( !local.empty() )
		return local;
	return environmentVariable( "USERPROFILE" );
#else
	return environmentVariable( "HOME" );
#endif
}

/// Same locations the rest of the fleet uses, so one folder holds everything.
std::string logDirectory()
{
	const std::string override_ = environmentVariable( "RELAY_LOG_DIR" );
	if( !override_.empty() )
		return override_;

	const std::string home = homeDirectory();
#if defined( _WIN32 )
	return home + "\\" + kAppName + "\\logs";
#elif defined( __APPLE__ )
	return home + "/Library/Logs/" + kAppName;
#else
	const std::string state = environmentVariable( "XDG_STATE_HOME" );
	return ( state.empty() ? home + "/.local/state" : state ) + "/" + kAppName + "/logs";
#endif
}

/// Make the log directory, WITHOUT a shell.
///
/// This used to be `std::system( "mkdir -p ..." )`, which was harmless while
/// it only ran when a plugin instance was created. It is now also reached
/// from a file-scope constructor -- i.e. from inside dlopen, on whichever of
/// the host's threads is scanning the plugin folder -- and fork() from there
/// can inherit a malloc lock held by another thread and hang the child before
/// it ever reaches exec. mkdir() is a syscall and has no such problem. It is
/// also the end of quoting a path into a shell command.
void createDirectories( const std::string& path )
{
#if defined( _WIN32 )
	std::string partial;
	for( char c : path )
	{
		partial += c;
		if( c == '\\' || c == '/' )
			CreateDirectoryA( partial.c_str(), nullptr );
	}
	CreateDirectoryA( path.c_str(), nullptr );
#else
	std::string partial;
	for( size_t i = 0; i < path.size(); ++i )
	{
		partial += path[ i ];
		if( path[ i ] == '/' && partial.size() > 1 )
			(void)::mkdir( partial.c_str(), 0755 );
	}
	(void)::mkdir( path.c_str(), 0755 );
#endif
}

std::string timestamp( const char* format )
{
	const std::time_t now = std::time( nullptr );
	std::tm local {};
#if defined( _WIN32 )
	localtime_s( &local, &now );
#else
	localtime_r( &now, &local );
#endif
	char buffer[ 64 ] {};
	std::strftime( buffer, sizeof( buffer ), format, &local );
	return buffer;
}

void write( const char* level, const std::string& message )
{
	std::lock_guard<std::mutex> lock( g_mutex );
	if( !g_ready )
		return;

	// Opened and closed per line rather than held open. An effect logs a
	// handful of lines per session, and a plugin holding a file handle open
	// for the life of the host is a worse trade than the open() cost. It also
	// means nothing is buffered when the host exits.
	std::ofstream file( g_path, std::ios::app );
	if( !file )
		return;
	file << timestamp( "%Y-%m-%dT%H:%M:%S" ) << " " << level << " " << kAppName << ": " << message
		 << std::endl;
}
} // namespace

void init()
{
	{
		std::lock_guard<std::mutex> lock( g_mutex );
		if( g_ready )
			return;

		const std::string directory = logDirectory();
		createDirectories( directory );
#if defined( _WIN32 )
		g_path = directory + "\\" + kAppName + "." + timestamp( "%Y-%m-%d" ) + ".log";
#else
		g_path = directory + "/" + kAppName + "." + timestamp( "%Y-%m-%d" ) + ".log";
#endif
		g_ready = true;
	}

	info( std::string( "plugin loaded build=" ) + __DATE__ + " " + __TIME__ );
}

void info( const std::string& message )
{
	write( "INFO ", message );
}

void warn( const std::string& message )
{
	write( "WARN ", message );
}

void error( const std::string& message )
{
	write( "ERROR", message );
}

std::string logPath()
{
	std::lock_guard<std::mutex> lock( g_mutex );
	return g_path;
}

std::string modulePath()
{
#if defined( _WIN32 )
	HMODULE module = nullptr;
	if( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast< LPCSTR >( &modulePath ), &module )
	    && module != nullptr )
	{
		char buffer[ MAX_PATH ] = {};
		if( GetModuleFileNameA( module, buffer, MAX_PATH ) > 0 )
			return buffer;
	}
	return "unknown";
#else
	//The address of this function is inside whatever image this code was
	//linked into, so dladdr names that image -- the loaded bundle in a host,
	//and the executable itself in the offline harness.
	Dl_info info {};
	if( dladdr( reinterpret_cast< const void* >( &modulePath ), &info ) != 0 && info.dli_fname != nullptr )
		return info.dli_fname;
	return "unknown";
#endif
}

} // namespace relay::diag
