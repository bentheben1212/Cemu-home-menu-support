#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

std::unordered_set<std::string> s_unsupportedHLECalls;
std::unordered_map<uint32, uint64> s_unsupportedHLECallCount; // keyed by import stub address
std::mutex s_hleLogMutex;

// called from the interpreter and both recompiler backends; instructionPointer points to the import stub
void PPCInterpreter_handleUnsupportedHLECall(PPCInterpreter_t* hCPU)
{
	{
		std::unique_lock _l(s_hleLogMutex);
		const char* libFuncName = (char*)memory_getPointerFromVirtualOffset(hCPU->instructionPointer + 8);
		uint64 count = ++s_unsupportedHLECallCount[hCPU->instructionPointer];
		if (count == 1)
		{
			std::string tempString = fmt::format("Unsupported lib call: {}", libFuncName);
			if (s_unsupportedHLECalls.find(tempString) == s_unsupportedHLECalls.end())
			{
				cemuLog_log(LogType::UnsupportedAPI, "{}", tempString);
				s_unsupportedHLECalls.emplace(tempString);
			}
		}
		else if (count >= 64 && (count & (count - 1)) == 0)
		{
			// log repeated calls at power-of-two intervals so that polling loops on unsupported functions are visible
			cemuLog_log(LogType::UnsupportedAPI, "Unsupported lib call: {} (called {} times, LR 0x{:08x})", libFuncName, count, hCPU->spr.LR);
		}
	}
	hCPU->gpr[3] = 0;
	PPCInterpreter_nextInstruction(hCPU);
}

// forget which unsupported calls were already logged, so each title's log lists all unsupported calls it makes
void PPCInterpreter_resetUnsupportedHLECallTracking()
{
	std::unique_lock _l(s_hleLogMutex);
	s_unsupportedHLECalls.clear();
	s_unsupportedHLECallCount.clear();
}

static constexpr size_t HLE_TABLE_CAPACITY = 0x4000;
HLECALL s_ppcHleTable[HLE_TABLE_CAPACITY]{};
sint32 s_ppcHleTableWriteIndex = 0;
std::mutex s_ppcHleTableMutex;

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)
{
	std::unique_lock _l(s_ppcHleTableMutex);
	if (s_ppcHleTableWriteIndex >= HLE_TABLE_CAPACITY)
	{
		cemuLog_log(LogType::Force, "HLE table is full");
		cemu_assert(false);
	}
	for (sint32 i = 0; i < s_ppcHleTableWriteIndex; i++)
	{
		if (s_ppcHleTable[i] == hleCall)
		{
			return i;
		}
	}
	cemu_assert(s_ppcHleTableWriteIndex < HLE_TABLE_CAPACITY);
	s_ppcHleTable[s_ppcHleTableWriteIndex] = hleCall;
	HLEIDX funcIndex = s_ppcHleTableWriteIndex;
	s_ppcHleTableWriteIndex++;
	return funcIndex;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return nullptr;
	return s_ppcHleTable[funcIndex];
}

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
	if (hleFuncId == 0xFFD0) [[unlikely]]
	{
		PPCInterpreter_handleUnsupportedHLECall(hCPU);
	}
	else
	{
		// os lib function
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall);
		hleCall(hCPU);
	}
}