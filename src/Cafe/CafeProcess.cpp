#include "Cafe/CafeProcess.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cemu/Logging/CemuLogging.h"
#include "util/helpers/helpers.h"

namespace CafeProcess
{
	static std::atomic<bool> s_frozen{false};
	static std::atomic<bool> s_debugTestRunning{false};

	static bool _WaitUntil(uint32 timeoutMs, const std::function<bool()>& condition)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (!condition())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

	bool FreezeForegroundTitle(uint32 timeoutMs)
	{
		if (!CafeSystem::IsTitleRunning() || s_frozen.exchange(true))
			return false;
		auto start = std::chrono::steady_clock::now();
		coreinit::OSSetSchedulerFrozen(true);
		bool threadsStopped = _WaitUntil(timeoutMs, []() { return coreinit::OSCountRunningThreads() == 0; });
		auto threadsMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		bool gpuIdle = _WaitUntil(timeoutMs, []() { return GX2::GX2GetRetiredTimeStamp() >= GX2::GX2GetLastSubmittedTimeStamp(); });
		auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		cemuLog_log(LogType::Force, "Process: froze {:016x}: threads stopped {} ({} ms, {} still running), GPU idle {} ({} ms total)",
			CafeSystem::GetForegroundTitleId(), threadsStopped ? "yes" : "NO", threadsMs, coreinit::OSCountRunningThreads(), gpuIdle ? "yes" : "NO", totalMs);
		return threadsStopped && gpuIdle;
	}

	void ThawForegroundTitle()
	{
		if (!s_frozen.exchange(false))
			return;
		coreinit::OSSetSchedulerFrozen(false);
		cemuLog_log(LogType::Force, "Process: thawed {:016x}", CafeSystem::GetForegroundTitleId());
	}

	bool IsForegroundTitleFrozen()
	{
		return s_frozen.load();
	}

	void StartDebugFreezeTest(uint32 seconds, bool swapMemory)
	{
		if (s_debugTestRunning.exchange(true))
		{
			cemuLog_log(LogType::Force, "Process: a freeze test is already running");
			return;
		}
		std::thread([seconds, swapMemory]() {
			SetThreadName("FreezeTest");
			cemuLog_log(LogType::Force, "Process: freeze test ({} s, memory swap {})", seconds, swapMemory ? "on" : "off");
			if (!FreezeForegroundTitle(2000))
			{
				cemuLog_log(LogType::Force, "Process: freeze incomplete, thawing without a memory swap");
				ThawForegroundTitle();
				s_debugTestRunning = false;
				return;
			}
			MMUProcessMemory* swappedOut = nullptr;
			if (swapMemory)
			{
				if (memory_isProcessMemorySwappable())
					swappedOut = memory_detachProcessMemory();
				else
					cemuLog_log(LogType::Force, "Process: memory swap needs CEMU_PROCESS_MEMORY=shared (Linux), freezing without it");
			}
			std::this_thread::sleep_for(std::chrono::seconds(seconds));
			if (swappedOut)
				memory_attachProcessMemory(swappedOut);
			ThawForegroundTitle();
			s_debugTestRunning = false;
		}).detach();
	}
}
