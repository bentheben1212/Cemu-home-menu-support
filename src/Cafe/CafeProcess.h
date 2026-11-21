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
}
