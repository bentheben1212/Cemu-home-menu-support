#include "Cafe/OS/common/OSCommon.h"
#include "drmapp.h"

namespace drmapp
{
	uint32 NupChkIsFinished(uint32 ukn)
	{
		cemuLog_logDebug(LogType::Force, "drmapp.NupChkIsFinished() - placeholder");
		return 1;
	}

	uint32 PatchChkIsFinished()
	{
		cemuLog_logDebug(LogType::Force, "drmapp.PatchChkIsFinished() - placeholder");
		return 1;
	}

	uint32 AocChkIsFinished()
	{
		cemuLog_logDebug(LogType::Force, "drmapp.AocChkIsFinished() - placeholder");
		return 1;
	}

	uint32 TicketChkIsFinished()
	{
		cemuLog_logDebug(LogType::Force, "drmapp.TicketChkIsFinished__3RplFv() - placeholder");
		return 1;
	}

	// System update (NUP) check used by the Wii U Menu during boot.
	// We have no network update service, so the check is reported as already
	// finished with no update required. The exact signatures and state enum are
	// not documented, so these are raw handlers that log their register args.

	// Value returned by NupChkGetState. Not confirmed against real hardware;
	// can be overridden at runtime with CEMU_DRMAPP_NUP_STATE=<int> for testing.
	static uint32 GetNupFinishedState()
	{
		static const uint32 s_state = []() -> uint32 {
			uint32 state = 1;
			if (const char* env = getenv("CEMU_DRMAPP_NUP_STATE"))
				state = (uint32)strtoul(env, nullptr, 0);
			cemuLog_log(LogType::Force, "drmapp: NupChkGetState will report state {}", state);
			return state;
		}();
		return s_state;
	}

	static void LogNupCall(PPCInterpreter_t* hCPU, const char* name, uint32 ret)
	{
		cemuLog_log(LogType::Force, "drmapp.{}(r3=0x{:08x}, r4=0x{:08x}, r5=0x{:08x}) -> {} (LR 0x{:08x})",
					name, hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5], ret, hCPU->spr.LR);
	}

	void export_NupChkStart(PPCInterpreter_t* hCPU)
	{
		LogNupCall(hCPU, "NupChkStart", 0);
		osLib_returnFromFunction(hCPU, 0); // success
	}

	void export_NupChkGetState(PPCInterpreter_t* hCPU)
	{
		uint32 state = GetNupFinishedState();
		static bool s_loggedOnce = false;
		if (!s_loggedOnce) // polled every frame, only log the first call
		{
			LogNupCall(hCPU, "NupChkGetState", state);
			s_loggedOnce = true;
		}
		osLib_returnFromFunction(hCPU, state);
	}

	void export_IsProgressBarVisible(PPCInterpreter_t* hCPU)
	{
		static bool s_loggedOnce = false;
		if (!s_loggedOnce)
		{
			LogNupCall(hCPU, "IsProgressBarVisible", 0);
			s_loggedOnce = true;
		}
		osLib_returnFromFunction(hCPU, 0); // no progress bar
	}

	void export_Calculate(PPCInterpreter_t* hCPU)
	{
		static bool s_loggedOnce = false;
		if (!s_loggedOnce)
		{
			LogNupCall(hCPU, "Calculate", 0);
			s_loggedOnce = true;
		}
		osLib_returnFromFunction(hCPU, 0); // nothing to update
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "drmapp";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(NupChkIsFinished, "drmapp", "NupChkIsFinished__3RplFv", LogType::Placeholder);
			cafeExportRegisterFunc(PatchChkIsFinished, "drmapp", "PatchChkIsFinished__3RplFv", LogType::Placeholder);
			cafeExportRegisterFunc(AocChkIsFinished, "drmapp", "AocChkIsFinished__3RplFv", LogType::Placeholder);
			cafeExportRegisterFunc(TicketChkIsFinished, "drmapp", "TicketChkIsFinished__3RplFv", LogType::Placeholder);

			// registered under their base names; mangled imports (e.g. NupChkStart__3RplFv)
			// are resolved to these by the drmapp base-name fallback in the RPL import resolver (rpl.cpp)
			osLib_addFunction("drmapp", "NupChkStart", export_NupChkStart);
			osLib_addFunction("drmapp", "NupChkGetState", export_NupChkGetState);
			osLib_addFunction("drmapp", "IsProgressBarVisible", export_IsProgressBarVisible);
			osLib_addFunction("drmapp", "Calculate", export_Calculate);
		}
	}s_COSdrmappModule;

	COSModule* GetModule()
	{
		return &s_COSdrmappModule;
	}

} // namespace drmapp
