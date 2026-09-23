#include "Logging.h"

#include <windows.h>

#include <fstream>
#include <mutex>

namespace
{
	std::mutex g_logMutex;
	std::once_flag g_resetLogOnceFlag;

	// Use a fixed, well-known system-wide location instead of %TEMP%.
	// %TEMP% resolves per-user (and differently again when the host process
	// runs elevated/as a different user, e.g. iRacing launched via Steam or
	// with admin rights), so the log could silently end up in a folder the
	// managed app never looks at. %ProgramData% is the same physical path
	// regardless of which user/session the DLL is loaded into.
	std::wstring BaseDir()
	{
		wchar_t programData[MAX_PATH]{};
		DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
		std::wstring baseDir = (length > 0 && length < MAX_PATH)
			? std::wstring(programData)
			: L"C:\\ProgramData";

		std::wstring dir = baseDir + L"\\DonutzVrHud";
		CreateDirectoryW(dir.c_str(), nullptr);
		return dir;
	}

	std::wstring LogFilePath()
	{
		return BaseDir() + L"\\DonutzVrHudLayer.log";
	}

	// The log level is read from a small config file instead of an
	// environment variable: this DLL is loaded into the host application's
	// process (e.g. iRacing.exe), which the managed app does not launch
	// itself, so it has no way to set an environment variable the host
	// process would inherit. Writing to a shared, well-known file lets the
	// managed UI toggle verbose logging without needing to touch iRacing's
	// environment. Re-read on every call (cheap - the file just holds a few
	// bytes) so a toggle in the UI takes effect without restarting iRacing.
	std::wstring LogLevelFilePath()
	{
		return BaseDir() + L"\\LogLevel.txt";
	}

	Logging::LogLevel CurrentLogLevel()
	{
		std::ifstream file(LogLevelFilePath());
		std::string value;
		if (file.is_open())
		{
			std::getline(file, value);
		}

		if (_stricmp(value.c_str(), "Debug") == 0)
		{
			return Logging::LogLevel::Debug;
		}
		return Logging::LogLevel::Info;
	}
}

namespace Logging
{
	void Log(LogLevel level, const std::string& message)
	{
		if (level > CurrentLogLevel())
		{
			return;
		}

		std::lock_guard<std::mutex> lock(g_logMutex);

		// Truncate the log file the first time this process writes to it,
		// so each new run (e.g. each time the host app/layer is (re)started)
		// starts with a fresh log instead of appending indefinitely.
		std::call_once(g_resetLogOnceFlag, []()
			{
				std::ofstream resetFile(LogFilePath(), std::ios::trunc);
			});

		std::ofstream file(LogFilePath(), std::ios::app);
		if (!file.is_open())
		{
			return;
		}

		SYSTEMTIME time;
		GetLocalTime(&time);

		char timestamp[32];
		sprintf_s(timestamp, "%02d:%02d:%02d.%03d", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

		file << "[" << timestamp << "] [PID " << GetCurrentProcessId() << "] " << message << "\n";
	}
}
