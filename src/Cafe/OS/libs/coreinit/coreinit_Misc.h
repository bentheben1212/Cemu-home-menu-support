#pragma once

namespace coreinit
{
	uint32 OSGetUPID();
	uint32 OSGetPFID();
	uint64 OSGetTitleID();
	uint32 __OSGetProcessSDKVersion();
	uint32 OSLaunchTitleByPathl(const char* path, uint32 pathLength, uint32 argc);
	uint32 OSRestartGame(uint32 argc, MEMPTR<char>* argv);
	uint32 __LaunchByTitleId(uint64 titleId, uint32 argc, MEMPTR<char>* argv);
	uint64 __OSGetMainAppTitleId(); // title that launched the current applet (0 if unknown)
	void __OSQueueTitleSwitch(uint64 titleId); // switch to this title when the process exits
	uint64 __OSGetCallerTitleId(); // title that launched the current one (0 if started directly)
	void __OSDumpRecentConsoleOutput(); // log the last console (OSReport) lines regardless of log settings
	void __OSRequestProcessExit(); // ask the app to exit via ProcUI (ReleaseForeground -> Exit system messages)
	void __OSHomeButtonInput(uint32 source, bool isDown); // HOME button state of an emulated controller, from the input reads

	void OSReleaseForeground();

	void StartBackgroundForegroundTransition();

	struct OSDriverInterface
	{
		MEMPTR<void> getDriverName;
		MEMPTR<void> init;
		MEMPTR<void> onAcquireForeground;
		MEMPTR<void> onReleaseForeground;
		MEMPTR<void> done;
	};
	static_assert(sizeof(OSDriverInterface) == 0x14);

	uint32 OSDriver_Register(uint32 moduleHandle, sint32 priority, OSDriverInterface* driverCallbacks, sint32 driverId, uint32be* outUkn1, uint32be* outUkn2, uint32be* outUkn3);
	uint32 OSDriver_Deregister(uint32 moduleHandle, sint32 driverId);

	enum class COSReportModule
	{
		coreinit = 0,
	};

	enum class COSReportLevel
	{
		Error = 0,
		Warn = 1,
		Info = 2
	};

	void OSFatal(const char* msg);

	sint32 ppc_vprintf(const char* formatStr, char* strOut, sint32 maxLength, ppc_va_list* vargs);

	void miscInit();
};