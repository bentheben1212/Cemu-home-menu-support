#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"
#include <atomic>

// HLE call trace: after PPCInterpreter_startHLETrace, log the OS library calls guest code makes for a short time.
// Used to see what a title does right after an event (e.g. the Wii U Menu after a launch check)
static std::atomic<sint64> s_hleTraceDeadline{0}; // steady_clock ms, 0 = inactive
static std::atomic<sint32> s_hleTraceLinesLeft{0};
static std::mutex s_hleTraceMutex;
static std::unordered_map<std::string, uint32> s_hleTraceNameCount;
static std::string s_hleTableNames[0x4000];

// Logging the trace ("HLE trace:" lines) is off by default since it makes logs very long. Set the environment
// variable CEMU_HLE_TRACE=1 to enable it. The trace windows still open without it, so OSPanic can list the last
// OS calls of the panicking thread
static bool _HLETraceLoggingEnabled()
{
	static const bool s_enabled = []() {
		const char* v = getenv("CEMU_HLE_TRACE");
		const char* t = getenv("CEMU_HLE_TRACE_TITLE");
		return (v && *v && strcmp(v, "0") != 0) || (t && *t);
	}();
	return s_enabled;
}

static sint64 _HLETraceNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool PPCInterpreter_isHLETraceActive()
{
	sint64 deadline = s_hleTraceDeadline.load(std::memory_order_relaxed);
	if (deadline == 0)
		return false;
	if (_HLETraceNowMs() > deadline)
	{
		sint64 expected = deadline;
		if (s_hleTraceDeadline.compare_exchange_strong(expected, 0) && _HLETraceLoggingEnabled())
			cemuLog_log(LogType::Force, "HLE trace: end");
		return false;
	}
	if (!_HLETraceLoggingEnabled())
		return false;
	// past the line limit: stop logging, but keep recording calls for OSPanic until the time window ends
	return s_hleTraceLinesLeft.load(std::memory_order_relaxed) > 0;
}

static uint32 s_hleTraceMaxPerName = 4; // each function name is logged at most this many times per trace

void PPCInterpreter_startHLETraceEx(std::string_view reason, uint32 durationMs, sint32 maxLines)
{
	std::unique_lock _l(s_hleTraceMutex);
	s_hleTraceNameCount.clear();
	s_hleTraceMaxPerName = 4;
	s_hleTraceLinesLeft = maxLines;
	s_hleTraceDeadline = _HLETraceNowMs() + durationMs;
	if (_HLETraceLoggingEnabled())
		cemuLog_log(LogType::Force, "HLE trace: start ({}), logging OS library calls for {} ms (max {} lines)", reason, durationMs, maxLines);
}

// CEMU_HLE_TRACE_TITLE=<title id>: trace the whole run of that title, with up to 1000 lines per function name
void PPCInterpreter_startHLETraceTitleRun(uint64 titleId)
{
	PPCInterpreter_startHLETraceEx(fmt::format("whole run of {:016x}, CEMU_HLE_TRACE_TITLE", titleId), 24 * 60 * 60 * 1000, 50000);
	std::unique_lock _l(s_hleTraceMutex);
	s_hleTraceMaxPerName = 1000;
}

void PPCInterpreter_startHLETrace(std::string_view reason, uint32 durationMs)
{
	PPCInterpreter_startHLETraceEx(reason, durationMs, 300);
}

// While a trace window is open, every HLE call (unfiltered, including mutex/atomic/memory calls) is also recorded
// in a ring buffer. OSPanic dumps the entries of the panicking thread, identified by its stack pointer range
struct HLERecentCall
{
	const char* name;
	uint32 r3, r4, r5, r6, lr, sp, result;
	bool hasResult;
};
static HLERecentCall s_hleRecentCalls[1024];
static uint32 s_hleRecentCallIndex = 0;
static std::mutex s_hleRecentCallMutex;

static bool _HLERecordingActive()
{
	sint64 deadline = s_hleTraceDeadline.load(std::memory_order_relaxed);
	return deadline != 0 && _HLETraceNowMs() <= deadline;
}

static uint32 _HLERecordCall(const char* name, PPCInterpreter_t* hCPU)
{
	std::unique_lock _l(s_hleRecentCallMutex);
	uint32 idx = s_hleRecentCallIndex++;
	HLERecentCall& e = s_hleRecentCalls[idx % 1024];
	e.name = name;
	e.r3 = hCPU->gpr[3]; e.r4 = hCPU->gpr[4]; e.r5 = hCPU->gpr[5]; e.r6 = hCPU->gpr[6];
	e.lr = hCPU->spr.LR; e.sp = hCPU->gpr[1];
	e.result = 0; e.hasResult = false;
	return idx;
}

static void _HLERecordResult(uint32 idx, uint32 result)
{
	std::unique_lock _l(s_hleRecentCallMutex);
	if (s_hleRecentCallIndex - idx > 1024)
		return; // overwritten
	HLERecentCall& e = s_hleRecentCalls[idx % 1024];
	e.result = result;
	e.hasResult = true;
}

// log the most recent recorded calls made with a stack pointer close to sp (= by the same thread)
void PPCInterpreter_dumpRecentHLECallsForStack(uint32 sp, uint32 maxEntries)
{
	std::unique_lock _l(s_hleRecentCallMutex);
	std::vector<const HLERecentCall*> matches;
	uint32 count = std::min<uint32>(s_hleRecentCallIndex, 1024);
	for (uint32 i = 0; i < count && matches.size() < maxEntries; i++)
	{
		const HLERecentCall& e = s_hleRecentCalls[(s_hleRecentCallIndex - 1 - i) % 1024];
		if (e.sp + 0x20000 >= sp && e.sp <= sp + 0x20000)
			matches.emplace_back(&e);
	}
	if (matches.empty())
		return;
	cemuLog_log(LogType::Force, "Last {} OS calls of this thread (oldest first):", matches.size());
	for (auto it = matches.rbegin(); it != matches.rend(); ++it)
	{
		const HLERecentCall& e = **it;
		if (e.hasResult)
			cemuLog_log(LogType::Force, "  {} (r3 0x{:08x} r4 0x{:08x} r5 0x{:08x} r6 0x{:08x}) LR 0x{:08x} -> 0x{:08x}", e.name, e.r3, e.r4, e.r5, e.r6, e.lr, e.result);
		else
			cemuLog_log(LogType::Force, "  {} (r3 0x{:08x} r4 0x{:08x} r5 0x{:08x} r6 0x{:08x}) LR 0x{:08x} -> (no return yet)", e.name, e.r3, e.r4, e.r5, e.r6, e.lr);
	}
}

// skip high frequency calls that say nothing about what the title decides
static bool _HLETraceIsNoise(std::string_view name)
{
	static const std::string_view noisyLibs[] = { "gx2.", "snd_core.", "sndcore2.", "snd_user.", "padscore.", "vpad.", "avm.", "tcl.", "dc.", "erreula.", "swkbd.", "nlibcurl." };
	for (auto& lib : noisyLibs)
		if (name.starts_with(lib))
			return true;
	if (name.starts_with("coreinit.FS") || name.starts_with("coreinit.MCP") || name.starts_with("coreinit.IOS"))
		return false; // filesystem, title and IPC calls are never noise
	if (name.starts_with("coreinit."))
	{
		static const std::string_view noisyCoreinit[] = { "Mutex", "Lock", "Event", "Time", "Tick", "Alarm", "Cond", "Spin", "Message", "Thread", "Interrupt", "Coroutine", "MEM", "memcpy", "memset", "memmove", "BlockMove", "BlockSet", "snprintf", "sprintf", "Cache", "Semaphore", "Rendezvous", "FastMutex", "Atomic", "OSGetCoreId", "OSIsMainCore", "OSGetSystemTime", "Heap", "Alloc", "Free" };
		for (auto& n : noisyCoreinit)
			if (name.find(n) != std::string_view::npos)
				return true;
	}
	return false;
}

// returns true if the call should be logged (limits identical names to a few lines per trace)
static bool _HLETraceShouldLog(std::string_view name)
{
	if (_HLETraceIsNoise(name))
		return false;
	std::unique_lock _l(s_hleTraceMutex);
	uint32& count = s_hleTraceNameCount[std::string(name)];
	count++;
	if (count > s_hleTraceMaxPerName)
		return false;
	if (s_hleTraceLinesLeft.fetch_sub(1) <= 0)
		return false;
	return true;
}

void PPCInterpreter_traceHLECallUnsupported(PPCInterpreter_t* hCPU, const char* libFuncName)
{
	if (!_HLETraceShouldLog(libFuncName))
		return;
	cemuLog_log(LogType::Force, "HLE trace: {} UNSUPPORTED (r3 0x{:08x} r4 0x{:08x} r5 0x{:08x} r6 0x{:08x}) LR 0x{:08x} -> 0",
		libFuncName, hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6], hCPU->spr.LR);
}

// calls a supported HLE function and, while a trace is active, logs it with its arguments and result
void PPCInterpreter_callHLE(PPCInterpreter_t* hCPU, HLEIDX funcIndex)
{
	HLECALL hleCall = PPCInterpreter_getHLECall(funcIndex);
	cemu_assert(hleCall);
	if (s_hleTraceDeadline.load(std::memory_order_relaxed) == 0) [[likely]]
	{
		hleCall(hCPU);
		return;
	}
	const std::string& name = s_hleTableNames[funcIndex];
	if (_HLERecordingActive())
	{
		uint32 recordIdx = _HLERecordCall(name.c_str(), hCPU);
		if (!PPCInterpreter_isHLETraceActive() || !_HLETraceShouldLog(name))
		{
			hleCall(hCPU);
			_HLERecordResult(recordIdx, hCPU->gpr[3]);
			return;
		}
		uint32 r3 = hCPU->gpr[3], r4 = hCPU->gpr[4], r5 = hCPU->gpr[5], r6 = hCPU->gpr[6], lr = hCPU->spr.LR;
		hleCall(hCPU);
		_HLERecordResult(recordIdx, hCPU->gpr[3]);
		cemuLog_log(LogType::Force, "HLE trace: {} (r3 0x{:08x} r4 0x{:08x} r5 0x{:08x} r6 0x{:08x}) LR 0x{:08x} -> 0x{:08x}", name, r3, r4, r5, r6, lr, hCPU->gpr[3]);
		return;
	}
	if (!PPCInterpreter_isHLETraceActive())
	{
		hleCall(hCPU);
		return;
	}
	if (!_HLETraceShouldLog(name))
	{
		hleCall(hCPU);
		return;
	}
	uint32 r3 = hCPU->gpr[3], r4 = hCPU->gpr[4], r5 = hCPU->gpr[5], r6 = hCPU->gpr[6], lr = hCPU->spr.LR;
	hleCall(hCPU);
	cemuLog_log(LogType::Force, "HLE trace: {} (r3 0x{:08x} r4 0x{:08x} r5 0x{:08x} r6 0x{:08x}) LR 0x{:08x} -> 0x{:08x}", name, r3, r4, r5, r6, lr, hCPU->gpr[3]);
}

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
		if (_HLERecordingActive())
			_HLERecordResult(_HLERecordCall(libFuncName, hCPU), 0);
		if (PPCInterpreter_isHLETraceActive())
			PPCInterpreter_traceHLECallUnsupported(hCPU, libFuncName);
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
	s_hleTableNames[s_ppcHleTableWriteIndex] = hleName;
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
		PPCInterpreter_callHLE(hCPU, hleFuncId);
	}
}