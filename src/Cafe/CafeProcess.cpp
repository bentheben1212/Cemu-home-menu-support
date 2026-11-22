#include "Cafe/CafeProcess.h"
#include "Cafe/CafeProcessState.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Common/CafeStaticVar.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cemu/Logging/CemuLogging.h"
#include "util/helpers/helpers.h"

extern std::vector<uint32> g_latteResumeContextRegisters; // LatteThread.cpp
void Latte_LoadInitialRegisters(); // LatteThread.cpp
void RPLLoader_ReserveRecompilerRangesForLoadedCode(); // rpl.cpp

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

	/* per-process state registry */

	std::vector<StateEntry*>& GetStateRegistry()
	{
		static std::vector<StateEntry*> s_registry;
		return s_registry;
	}

	StateEntry::StateEntry(const char* name, StatePhase phase) : name(name), phase(phase)
	{
		GetStateRegistry().push_back(this);
	}

	/* suspend / resume */

	struct LatteGPUSnapshot
	{
		std::vector<uint32> contextRegister;
		std::vector<MPTR> contextRegisterShadowAddr;
		uint32 contextControl0;
		uint32 contextControl1;
		gx2GPUSharedArea_t* sharedArea;
		MPTR sharedAreaAddr;
		uint32 gx2InitCalled;
		uint64 flipRequestCount;
		bool isDRCPrimary;
		bool tvBufferUsesSRGB;
		bool drcBufferUsesSRGB;
		float tvGamma;
		float drcGamma;
		struct
		{
			bool isEnabled;
			MPTR physPtr;
			uint32 flipRequestCount;
			uint32 flipExecuteCount;
		}osScreen[2];
	};

	struct SuspendedTitle
	{
		uint64 titleId;
		std::vector<std::pair<StateEntry*, void*>> threadState;
		std::vector<std::pair<StateEntry*, void*>> generalState;
		void* cosStaticData{};
		MMUProcessMemory* memory{};
		LatteGPUSnapshot latte;
		// GPU: the GPU thread keeps running and the title's textures and buffers stay cached under gpuOwnerId. If false,
		// the GPU thread was restarted (fallback) and its caches are rebuilt from memory on resume
		bool gpuKept{false};
		uint32 gpuOwnerId{0};
	};
	static uint32 s_nextGPUOwnerId = 1;
	static std::unique_ptr<SuspendedTitle> s_suspended;
	static std::recursive_mutex s_suspendMutex;

	bool IsSuspendEnabled()
	{
		static const bool s_enabled = []() {
			const char* v = getenv("CEMU_PROCESS_SUSPEND");
			bool requested = v && strcmp(v, "1") == 0;
			if (requested && !memory_isProcessMemorySwappable())
				cemuLog_log(LogType::Force, "CEMU_PROCESS_SUSPEND=1 needs CEMU_PROCESS_MEMORY=shared (Linux), suspending is disabled");
			return requested && memory_isProcessMemorySwappable();
		}();
		return s_enabled;
	}

	// CEMU_PROCESS_SUSPEND_GPU=restart: stop the GPU thread on suspend and start it again on resume, like a title switch
	// (GPU-only data such as render-to-texture results is lost). Default: the GPU thread and its caches stay alive
	static bool _KeepGPUAlive()
	{
		static const bool s_keep = []() {
			const char* v = getenv("CEMU_PROCESS_SUSPEND_GPU");
			bool restart = v && strcmp(v, "restart") == 0;
			cemuLog_log(LogType::Force, "Process: GPU on suspend: {}", restart ? "restarted (CEMU_PROCESS_SUSPEND_GPU=restart)" : "kept alive with its caches");
			return !restart;
		}();
		return s_keep;
	}

	bool HasSuspendedTitle()
	{
		std::unique_lock _l(s_suspendMutex);
		return s_suspended != nullptr;
	}

	uint64 GetSuspendedTitleId()
	{
		std::unique_lock _l(s_suspendMutex);
		return s_suspended ? s_suspended->titleId : 0;
	}

	static void _SaveLatteState(LatteGPUSnapshot& snap)
	{
		snap.contextRegister.assign(LatteGPUState.contextRegister, LatteGPUState.contextRegister + LATTE_MAX_REGISTER);
		snap.contextRegisterShadowAddr.assign(LatteGPUState.contextRegisterShadowAddr, LatteGPUState.contextRegisterShadowAddr + LATTE_MAX_REGISTER);
		snap.contextControl0 = LatteGPUState.contextControl0;
		snap.contextControl1 = LatteGPUState.contextControl1;
		snap.sharedArea = LatteGPUState.sharedArea;
		snap.sharedAreaAddr = LatteGPUState.sharedAreaAddr;
		snap.gx2InitCalled = LatteGPUState.gx2InitCalled;
		snap.flipRequestCount = LatteGPUState.flipRequestCount.load();
		snap.isDRCPrimary = LatteGPUState.isDRCPrimary;
		snap.tvBufferUsesSRGB = LatteGPUState.tvBufferUsesSRGB;
		snap.drcBufferUsesSRGB = LatteGPUState.drcBufferUsesSRGB;
		snap.tvGamma = LatteGPUState.tvGamma;
		snap.drcGamma = LatteGPUState.drcGamma;
		for (sint32 i = 0; i < 2; i++)
		{
			snap.osScreen[i].isEnabled = LatteGPUState.osScreen.screen[i].isEnabled;
			snap.osScreen[i].physPtr = LatteGPUState.osScreen.screen[i].physPtr;
			snap.osScreen[i].flipRequestCount = LatteGPUState.osScreen.screen[i].flipRequestCount.load();
			snap.osScreen[i].flipExecuteCount = LatteGPUState.osScreen.screen[i].flipExecuteCount.load();
		}
		// the next title starts with a fresh GPU state (GX2Init allocates its own shared area)
		memset(LatteGPUState.contextRegisterShadowAddr, 0, sizeof(LatteGPUState.contextRegisterShadowAddr));
		LatteGPUState.contextControl0 = 0;
		LatteGPUState.contextControl1 = 0;
		LatteGPUState.sharedArea = nullptr;
		LatteGPUState.sharedAreaAddr = MPTR_NULL;
		LatteGPUState.gx2InitCalled = 0;
		LatteGPUState.flipRequestCount = 0;
		for (sint32 i = 0; i < 2; i++)
		{
			LatteGPUState.osScreen.screen[i].isEnabled = false;
			LatteGPUState.osScreen.screen[i].physPtr = MPTR_NULL;
			LatteGPUState.osScreen.screen[i].flipRequestCount = 0;
			LatteGPUState.osScreen.screen[i].flipExecuteCount = 0;
		}
	}

	static void _RestoreLatteState(LatteGPUSnapshot& snap, bool gpuRunning)
	{
		memcpy(LatteGPUState.contextRegister, snap.contextRegister.data(), LATTE_MAX_REGISTER * sizeof(uint32));
		memcpy(LatteGPUState.contextRegisterShadowAddr, snap.contextRegisterShadowAddr.data(), LATTE_MAX_REGISTER * sizeof(MPTR));
		LatteGPUState.contextControl0 = snap.contextControl0;
		LatteGPUState.contextControl1 = snap.contextControl1;
		LatteGPUState.sharedArea = snap.sharedArea;
		LatteGPUState.sharedAreaAddr = snap.sharedAreaAddr;
		LatteGPUState.gx2InitCalled = snap.gx2InitCalled;
		LatteGPUState.flipRequestCount = snap.flipRequestCount;
		LatteGPUState.isDRCPrimary = snap.isDRCPrimary;
		LatteGPUState.tvBufferUsesSRGB = snap.tvBufferUsesSRGB;
		LatteGPUState.drcBufferUsesSRGB = snap.drcBufferUsesSRGB;
		LatteGPUState.tvGamma = snap.tvGamma;
		LatteGPUState.drcGamma = snap.drcGamma;
		for (sint32 i = 0; i < 2; i++)
		{
			LatteGPUState.osScreen.screen[i].isEnabled = snap.osScreen[i].isEnabled;
			LatteGPUState.osScreen.screen[i].physPtr = snap.osScreen[i].physPtr;
			LatteGPUState.osScreen.screen[i].flipRequestCount = snap.osScreen[i].flipRequestCount;
			LatteGPUState.osScreen.screen[i].flipExecuteCount = snap.osScreen[i].flipExecuteCount;
		}
		// the GPU thread resets the registers when it starts, then applies these
		if (!gpuRunning)
			g_latteResumeContextRegisters = snap.contextRegister; // applied after the GPU thread loads its initial registers
	}

	// GPU thread: the GPU starts the next title with its initial state, like a freshly started GPU thread
	static void _ResetLatteStateForNextTitle()
	{
		LatteGPUSnapshot discard;
		_SaveLatteState(discard);
		memset(LatteGPUState.contextRegister, 0, sizeof(LatteGPUState.contextRegister));
		Latte_LoadInitialRegisters();
	}

	static void _SaveEntries(StatePhase phase, std::vector<std::pair<StateEntry*, void*>>& out)
	{
		for (StateEntry* entry : GetStateRegistry())
			if (entry->phase == phase)
				out.emplace_back(entry, entry->Save());
	}

	static void _RestoreEntries(std::vector<std::pair<StateEntry*, void*>>& saved)
	{
		for (auto& [entry, value] : saved)
			entry->Restore(value);
		saved.clear();
	}

	bool SuspendForegroundTitle()
	{
		std::unique_lock _l(s_suspendMutex);
		if (s_suspended)
		{
			cemuLog_log(LogType::Force, "Process: a title is already suspended");
			return false;
		}
		if (!memory_isProcessMemorySwappable())
		{
			cemuLog_log(LogType::Force, "Process: suspending needs CEMU_PROCESS_MEMORY=shared (Linux)");
			return false;
		}
		uint64 titleId = CafeSystem::GetForegroundTitleId();
		auto start = std::chrono::steady_clock::now();
		if (!FreezeForegroundTitle(2000))
		{
			cemuLog_log(LogType::Force, "Process: {:016x} did not come to a stop, not suspended", titleId);
			ThawForegroundTitle();
			return false;
		}
		auto suspended = std::make_unique<SuspendedTitle>();
		suspended->titleId = titleId;
		if (_KeepGPUAlive())
		{
			// the GPU thread keeps running for the next title: set this title's cached textures and buffers aside under
			// its owner id. Done while the title is still frozen, so no vsync or flip reaches it once its state is saved
			SuspendedTitle* s = suspended.get();
			uint32 nextOwnerId = s_nextGPUOwnerId++;
			bool done = Latte_RunOnGPUThreadWhenIdle([s, nextOwnerId]() {
				s->gpuOwnerId = LatteTC_GetActiveOwner();
				_SaveLatteState(s->latte);
				_ResetLatteStateForNextTitle();
				Latte_SwitchCacheOwner(nextOwnerId, false);
			}, 3000);
			if (!done)
			{
				cemuLog_log(LogType::Force, "Process: the GPU did not get idle, {:016x} not suspended", titleId);
				ThawForegroundTitle();
				return false;
			}
			suspended->gpuKept = true;
		}
		// threads and their host fibers, before the scheduler stops (it would delete the fibers)
		__OSLockScheduler();
		_SaveEntries(StatePhase::Threads, suspended->threadState);
		__OSUnlockScheduler();
		coreinit::OSSchedulerEnd();
		coreinit::OSSetSchedulerFrozen(false); // the scheduler is stopped; the next title runs normally
		s_frozen = false;
		if (!suspended->gpuKept)
		{
			// snapshot the GPU state before stopping the GPU thread: its exit clears LatteGPUState completely
			_SaveLatteState(suspended->latte);
			Latte_Stop();
		}
		_SaveEntries(StatePhase::General, suspended->generalState);
		suspended->cosStaticData = CafeStaticData::SaveCOSState();
		PPCRecompiler_Shutdown(); // the recompiler tables are keyed by guest address, the next title has its own code there
		GraphicPack2::Reset();
		CafeSystem::DetachSuspendedTitle();
		suspended->memory = memory_detachProcessMemory(true);
		s_suspended = std::move(suspended);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		cemuLog_log(LogType::Force, "Process: suspended {:016x} in {} ms ({} host state entries, GPU {})", titleId, ms, s_suspended->threadState.size() + s_suspended->generalState.size(),
			s_suspended->gpuKept ? fmt::format("kept, cache owner {}", s_suspended->gpuOwnerId) : std::string("restarted"));
		return true;
	}

	bool ResumeSuspendedTitle()
	{
		std::unique_lock _l(s_suspendMutex);
		if (!s_suspended)
			return false;
		if (CafeSystem::IsTitleRunning())
		{
			cemuLog_log(LogType::Force, "Process: cannot resume while a title is running");
			return false;
		}
		auto start = std::chrono::steady_clock::now();
		std::unique_ptr<SuspendedTitle> suspended = std::move(s_suspended);
		memory_attachProcessMemory(suspended->memory);
		suspended->memory = nullptr;
		memory_mapMissingTitleRanges(); // shared ranges (locked caches, FG bucket, tiling aperture) unmapped by the other title's shutdown
		CafeStaticData::RestoreCOSState(suspended->cosStaticData);
		suspended->cosStaticData = nullptr;
		_RestoreEntries(suspended->generalState);
		if (CafeSystem::ReattachSuspendedTitle(suspended->titleId) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			cemuLog_log(LogType::Force, "Process: unable to mount {:016x} again", suspended->titleId);
		RPLLoader_ReserveRecompilerRangesForLoadedCode(); // the recompiler was reinitialized, nothing is loaded again
		bool gpuResumed = false;
		if (suspended->gpuKept && Latte_IsRunning())
		{
			// the title's textures and buffers are still cached: make them active again (the other title's were deleted
			// when it shut down) and continue with the title's GPU registers
			SuspendedTitle* s = suspended.get();
			gpuResumed = Latte_RunOnGPUThreadWhenIdle([s]() {
				Latte_SwitchCacheOwner(s->gpuOwnerId, true);
				_RestoreLatteState(s->latte, true);
			}, 3000);
			if (!gpuResumed)
			{
				cemuLog_log(LogType::Force, "Process: the GPU did not get idle, restarting it (GPU-drawn images of {:016x} are lost)", suspended->titleId);
				Latte_Stop();
			}
		}
		if (!gpuResumed)
		{
			if (Latte_IsRunning())
				Latte_Stop(); // the restart fallback, after the other title kept the GPU thread
			_RestoreLatteState(suspended->latte, false);
			Latte_Start();
		}
		GraphicPack2::ActivateForCurrentTitle();
		if (snd_core::AXIsInit())
			snd_core::AXOut_initDevices(); // audio devices are recreated if the other title's shutdown destroyed them
		_RestoreEntries(suspended->threadState);
		CafeSystem::StartSchedulerForResume();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		cemuLog_log(LogType::Force, "Process: resumed {:016x} in {} ms (GPU {})", suspended->titleId, ms, gpuResumed ? "caches kept" : "restarted");
		return true;
	}

	bool IsGPUKeptForSuspendedTitle()
	{
		std::unique_lock _l(s_suspendMutex);
		return s_suspended && s_suspended->gpuKept;
	}

	void ReleaseForegroundGPUState()
	{
		uint32 nextOwnerId = s_nextGPUOwnerId++;
		bool done = Latte_RunOnGPUThreadWhenIdle([nextOwnerId]() {
			Latte_SwitchCacheOwner(nextOwnerId, true);
			_ResetLatteStateForNextTitle();
		}, 3000);
		if (done)
			return;
		cemuLog_log(LogType::Force, "Process: the GPU did not get idle, stopping it (GPU-drawn images of the suspended title are lost)");
		Latte_Stop();
		std::unique_lock _l(s_suspendMutex);
		if (s_suspended)
			s_suspended->gpuKept = false;
	}

	void DiscardSuspendedTitle()
	{
		std::unique_lock _l(s_suspendMutex);
		if (!s_suspended)
			return;
		cemuLog_log(LogType::Force, "Process: discarding suspended {:016x}", s_suspended->titleId);
		for (auto& [entry, value] : s_suspended->threadState)
			entry->Discard(value);
		for (auto& [entry, value] : s_suspended->generalState)
			entry->Discard(value);
		CafeStaticData::DiscardCOSState(s_suspended->cosStaticData);
		memory_discardProcessMemory(s_suspended->memory);
		s_suspended.reset();
	}

	void SuspendForegroundTitleAndLaunch(uint64 titleId)
	{
		if (!SuspendForegroundTitle())
		{
			cemuLog_log(LogType::Force, "Process: suspend failed, {:016x} not launched", titleId);
			return;
		}
		std::vector<std::string> noArgs;
		CafeSystem::SetOverrideArgs(noArgs);
		if (CafeSystem::PrepareForegroundTitle(titleId) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
		{
			cemuLog_log(LogType::Force, "Process: unable to prepare {:016x}, resuming the suspended title", titleId);
			CafeSystem::ShutdownTitleForSwitch();
			CafeSystem::RequestRecreateCanvas();
			ResumeSuspendedTitle();
			return;
		}
		CafeSystem::RequestRecreateCanvas();
		CafeSystem::LaunchForegroundTitle();
	}

	void StartDebugSuspendTest(uint32 seconds)
	{
		if (s_debugTestRunning.exchange(true))
		{
			cemuLog_log(LogType::Force, "Process: a test is already running");
			return;
		}
		std::thread([seconds]() {
			SetThreadName("SuspendTest");
			cemuLog_log(LogType::Force, "Process: suspend test ({} s)", seconds);
			if (SuspendForegroundTitle())
			{
				std::this_thread::sleep_for(std::chrono::seconds(seconds));
				CafeSystem::RequestRecreateCanvas();
				if (ResumeSuspendedTitle())
				{
					// diagnostics: is the resumed title running?
					uint32 frame0 = LatteGPUState.frameCounter, flip0 = LatteGPUState.flipCounter, draw0 = LatteGPUState.drawCallCounter;
					for (sint32 pass = 0; pass < 3; pass++)
					{
						std::this_thread::sleep_for(std::chrono::seconds(2));
						cemuLog_log(LogType::Force, "Process: {} s after resume: GPU frames +{} flips +{} draws +{}, gx2InitCalled {}, scheduler active {} frozen {}", (pass + 1) * 2,
							LatteGPUState.frameCounter - frame0, LatteGPUState.flipCounter - flip0, LatteGPUState.drawCallCounter - draw0, LatteGPUState.gx2InitCalled,
							coreinit::OSIsSchedulerActive(), coreinit::OSIsSchedulerFrozen());
						if (pass != 0)
							continue;
						static const char* stateNames[] = { "NONE", "READY", "RUNNING", "WAITING", "MORIBUND" };
						auto threads = coreinit::OSLockActiveThreadList();
						for (OSThread_t* thread : threads)
						{
							uint32 state = (uint32)thread->state.value();
							cemuLog_log(LogType::Force, "Process:   thread {:08x} {} suspend {} prio {} IP {:08x} LR {:08x} \"{}\"", MEMPTR<OSThread_t>(thread).GetMPTR(),
								state < 5 ? stateNames[state] : "?", (sint32)thread->suspendCounter, (sint32)thread->effectivePriority,
								(uint32)thread->context.srr0, (uint32)thread->context.lr, thread->threadName ? thread->threadName.GetPtr() : "");
						}
						coreinit::OSUnlockActiveThreadList();
					}
				}
			}
			s_debugTestRunning = false;
		}).detach();
	}
}
