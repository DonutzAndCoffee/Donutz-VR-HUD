#pragma once

#include <string>

// Minimal file-based logging so we can verify whether the native API layer
// DLL is actually being loaded by the host application (e.g. iRacing.exe)
// and whether the IPC pipe server starts up. Writes to
// %ProgramData%\DonutzVrHud\DonutzVrHudLayer.log (a fixed, session/user
// independent location - %TEMP% would resolve differently if the host
// process runs elevated or as another user). This is intentionally
// dependency-free so it can be called from DllMain.
namespace Logging
{
	// Two simple levels: Info for events worth keeping by default (startup,
	// shutdown, errors, state changes), and Debug for high-frequency/verbose
	// output (e.g. per-frame texture updates) that's only useful when
	// actively troubleshooting. Default level is Info; the managed app can
	// switch to Debug via a checkbox, which writes "Debug" into
	// %ProgramData%\DonutzVrHud\LogLevel.txt (see Logging.cpp).
	enum class LogLevel
	{
		Info,
		Debug,
	};

	void Log(LogLevel level, const std::string& message);

	// Convenience overload: defaults to Info to keep existing call sites unchanged.
	inline void Log(const std::string& message)
	{
		Log(LogLevel::Info, message);
	}
}
