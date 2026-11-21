#pragma once

// Suspending a guest process (a title stays alive, frozen, while another one runs). This is being built in steps:
// step 0 (this) provides freezing/thawing the running title and swapping its memory out and back in
namespace CafeProcess
{
	// stops all guest threads of the running title (they finish their current time slice) and waits until the GPU has
	// processed everything that was submitted. Returns false if the title didn't come to a stop within timeoutMs
	bool FreezeForegroundTitle(uint32 timeoutMs);
	void ThawForegroundTitle();
	bool IsForegroundTitleFrozen();

	// debug command: freeze the running title for the given time, optionally with its memory swapped out (replaced by
	// zeroed memory) while frozen, then thaw it. Runs on its own thread; logs each step
	void StartDebugFreezeTest(uint32 seconds, bool swapMemory);

	/* Suspend (Phase 2, step 1)
	   The running title is frozen and set aside completely: its memory (remapped, see MMU), its host state (registered
	   with CAFE_PROCESS_STATE), its threads and fibers. The scheduler is stopped as on a title switch, but nothing of the
	   title is destroyed. Another title can then be started normally. Resuming tears that title down and brings the
	   suspended one back exactly where it was. Needs CEMU_PROCESS_MEMORY=shared (Linux) and is enabled for the HOME Menu
	   with CEMU_PROCESS_SUSPEND=1.
	   GPU: the GPU thread keeps running across the switch. The suspended title's cached textures and buffers (including
	   images only the GPU drew) stay on the GPU under its cache owner id and are made active again on resume.
	   CEMU_PROCESS_SUSPEND_GPU=restart stops and restarts the GPU thread instead (fallback) */
	bool IsSuspendEnabled();
	bool HasSuspendedTitle();
	uint64 GetSuspendedTitleId();
	bool SuspendForegroundTitle(); // call from a host thread, not from a PPC thread
	bool ResumeSuspendedTitle(); // no title may be running (shut it down first)
	void DiscardSuspendedTitle();
	// title shutdown while a title is suspended: if true, the GPU thread is not stopped; ReleaseForegroundGPUState
	// deletes the ending title's GPU caches instead (the suspended title's stay)
	bool IsGPUKeptForSuspendedTitle();
	void ReleaseForegroundGPUState();
	// suspends the running title and launches another one in its place (e.g. the HOME Menu over the Wii U Menu)
	void SuspendForegroundTitleAndLaunch(uint64 titleId);
	// debug command: suspend the running title for the given time and resume it, without running anything in between
	void StartDebugSuspendTest(uint32 seconds);
}
