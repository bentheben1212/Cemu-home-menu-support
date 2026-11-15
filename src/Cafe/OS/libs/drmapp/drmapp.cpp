#include "Cafe/OS/common/OSCommon.h"
#include "drmapp.h"

namespace drmapp
{
	uint32 NupChkIsFinished(uint32 ukn)
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			cemuLog_log(LogType::Force, "drmapp.NupChkIsFinished() -> 1 (first call, LR 0x{:08x})", PPCInterpreter_getCurrentInstance()->spr.LR);
			s_logged = true;
		}
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
	// There is no update server, so the check reports itself as finished with no update needed.
	// The state enum is undocumented. Defaults can be overridden for testing:
	//   CEMU_DRMAPP_NUP_STATE = value written to NupChkGetState's int* out parameter (default 0)
	//   CEMU_DRMAPP_NUP_RET   = return value of NupChkGetState (default 1)

	static uint32 GetEnvU32(const char* name, uint32 defaultValue)
	{
		const char* env = getenv(name);
		if (!env || !*env)
			return defaultValue;
		return (uint32)strtoul(env, nullptr, 0);
	}

	static bool ShouldLogCall(uint64& counter)
	{
		counter++;
		return counter == 1 || (counter >= 64 && (counter & (counter - 1)) == 0);
	}

	static void LogCall(PPCInterpreter_t* hCPU, const char* name, uint64 count)
	{
		cemuLog_log(LogType::Force, "drmapp.{}(r3=0x{:08x}, r4=0x{:08x}) call #{} (LR 0x{:08x})",
					name, hCPU->gpr[3], hCPU->gpr[4], count, hCPU->spr.LR);
	}

	// Rpl::Initialize(const nn::drmapp::InitializeArg&)
	void export_Initialize(PPCInterpreter_t* hCPU)
	{
		static uint64 s_count = 0;
		if (ShouldLogCall(s_count))
			LogCall(hCPU, "Initialize", s_count);
		osLib_returnFromFunction(hCPU, 0);
	}

	// Rpl::NupChkStart(const nn::drmapp::StartArg&)
	void export_NupChkStart(PPCInterpreter_t* hCPU)
	{
		static uint64 s_count = 0;
		if (ShouldLogCall(s_count))
			LogCall(hCPU, "NupChkStart", s_count);
		osLib_returnFromFunction(hCPU, 0);
	}

	// Rpl::NupChkGetState(int* state)
	void export_NupChkGetState(PPCInterpreter_t* hCPU)
	{
		static const uint32 s_state = GetEnvU32("CEMU_DRMAPP_NUP_STATE", 0);
		static const uint32 s_ret = GetEnvU32("CEMU_DRMAPP_NUP_RET", 1);
		static uint64 s_count = 0;
		ppcDefineParamMPTR(statePtr, 0);
		uint32 prevState = 0;
		if (statePtr != MPTR_NULL)
		{
			prevState = memory_readU32(statePtr);
			memory_writeU32(statePtr, s_state);
		}
		if (ShouldLogCall(s_count))
		{
			cemuLog_log(LogType::Force, "drmapp.NupChkGetState(state*=0x{:08x}) call #{}: *state {} -> {}, return {} (LR 0x{:08x})",
						statePtr, s_count, prevState, s_state, s_ret, hCPU->spr.LR);
		}
		osLib_returnFromFunction(hCPU, s_ret);
	}

	// Rpl::IsProgressBarVisible()
	void export_IsProgressBarVisible(PPCInterpreter_t* hCPU)
	{
		static uint64 s_count = 0;
		if (ShouldLogCall(s_count))
			LogCall(hCPU, "IsProgressBarVisible", s_count);
		osLib_returnFromFunction(hCPU, 0);
	}

	// Rpl::Calculate(const nn::drmapp::ControllerInfo&) - per-frame update, nothing to do
	void export_Calculate(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, 0);
	}

	// Rpl::DrawTV() / Rpl::DrawDRC() - progress bar is never visible, nothing to draw
	void export_DrawTV(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_DrawDRC(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, 0);
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

			osLib_addFunction("drmapp", "Initialize__3RplFRCQ3_2nn6drmapp13InitializeArg", export_Initialize);
			osLib_addFunction("drmapp", "NupChkStart__3RplFRCQ3_2nn6drmapp8StartArg", export_NupChkStart);
			osLib_addFunction("drmapp", "NupChkGetState__3RplFPi", export_NupChkGetState);
			osLib_addFunction("drmapp", "IsProgressBarVisible__3RplFv", export_IsProgressBarVisible);
			osLib_addFunction("drmapp", "Calculate__3RplFRCQ3_2nn6drmapp14ControllerInfo", export_Calculate);
			osLib_addFunction("drmapp", "DrawTV__3RplFv", export_DrawTV);
			osLib_addFunction("drmapp", "DrawDRC__3RplFv", export_DrawDRC);
		}
	}s_COSdrmappModule;

	COSModule* GetModule()
	{
		return &s_COSdrmappModule;
	}

} // namespace drmapp
