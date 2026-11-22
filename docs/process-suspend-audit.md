# Suspending a title: per-process state audit (Phase 2, Step 0)

Goal: a title (the Wii U Menu or a game) stays alive and frozen while the HOME Menu, and applets opened from it, run.
Resume continues where it stopped. Approach: the remap described in decaf-emu's kernel (`cafe_kernel_mmu.cpp`,
`cafe_kernel_process.cpp`): every process uses the same guest virtual addresses; switching process remaps them onto
that process's memory. Nothing is copied.

This audit lists what belongs to one title, what is shared by all titles, and how each part is handled.
It was produced with a scan of all file/namespace-scope variables in `src/Cafe/OS`, `src/Cafe/HW`, `src/Cafe/IOSU`,
`src/audio` and `CafeSystem.cpp` (about 1070 declarations: ~140 live in guest memory, ~940 are host C++ variables,
many of those constants, locks or module registrations).

## 1. Moves with the memory swap (no work needed)

Everything a title keeps in guest memory. Swapped by `memory_detachProcessMemory` / `memory_attachProcessMemory`
(CEMU_PROCESS_MEMORY=shared, Linux):

| Range | Contents |
|---|---|
| `CODE_MAIN` 0x02000000, `TRAMPOLINE_AREA`, `CODECAVE`, `CODE_LOW0` | loaded RPX/RPL code, import trampolines, code caves |
| `MEM2` 0x10000000 | title heaps, stacks, data, GX2 command buffers, textures |
| `MEM1` 0xF4000000 | MEM1 heap (render targets) |
| `RPLLOADER_AREA` 0xF6000000 | loader data |
| `CEMU_AREA` 0x0E000000 | all `CafeStaticVar` / `SysAllocator` data of Cemu's HLE libraries: `OSThread_t` of system threads (alarm, callback, IPC, ProcUI core threads), message queues, events, GX2 context state, AX voice arrays, FSA IPC pool, ... |

Debug test (Debug menu, "Freeze title for 5 seconds (swap memory)") confirmed a title survives having these ranges
swapped out and back.

Missing so far: memory mapped with `OSMapMemory` (`coreinit_MemoryMapping.cpp`, MAPABLE area) and the optional
`OVERLAY_AREA` once a title enables it. Both must become swappable ranges too.

## 2. Host state that belongs to one title (save/restore per process)

These are C++ variables that describe the running title. Step 1 moves them into a per-process record that is swapped
together with the memory. Pattern: each subsystem keeps its globals in one struct and exposes
`SaveProcessState(record)` / `RestoreProcessState(record)` (or simply swaps a pointer to its context struct).

### coreinit
| Where | State | Notes |
|---|---|---|
| `coreinit_Thread.cpp` | `s_activeThreads`, `s_threadToFiber`, `__currentCoreThread`, run queue counters (`g_coreRunQueueThreadCount`) | Guest threads of the frozen title keep their host fibers; they must be removed from the active list and run queues while frozen. Per-process "frozen" instead of the global flag used in Step 0 |
| `coreinit_Alarm.cpp` | `g_activeAlarmList`, `g_activeAlarms` (host alarms pointing at guest `OSAlarm`) | must not fire while frozen; set aside and re-armed on resume (time shift: fire late, like the console) |
| `coreinit_Callbacks.cpp` | async callback queue | pending callbacks of the frozen title stay queued with it |
| `coreinit_MEM.cpp`, `coreinit_SysHeap.cpp` | `s_cosSysHeap` (host allocator for CEMU_AREA), `g_heapTable`, `gDefaultHeap`, `sHeapBaseHandle`, heap lists, sys heap handle | allocator bookkeeping must match the swapped memory |
| `coreinit_MemoryMapping.cpp` | `s_allocatedVirtMemory` | plus the mapped memory itself (see section 1) |
| `coreinit_FG.cpp`, `coreinit_OverlayArena.cpp` | foreground bucket / overlay arena state | FG bucket memory is shared between processes on a console (used to hand data over); the bookkeeping is per process |
| `coreinit_FS.cpp` | `g_fsRegisteredClientBodies`, `s_fsa_activeClients`, bind mounts (`s_bindAliasTargets`), SD/MLC mount flags | open FSA handles stay open in IOSU while frozen |
| `coreinit_IPC.cpp` | `s_openIOSHandles` | per process |
| `coreinit_DynLoad.cpp`, `coreinit_Init.cpp` | DynLoad alloc/free funcs, notify callbacks, argv, entry point | |
| `coreinit_Misc.cpp` | current title id, SDK version, exit/transition flags, HOME button state | becomes the process record itself (title id, UPID, caller, main app) |
| `coreinit_LockedCache.cpp` | locked cache allocation masks | per process (locked cache contents are transient) |
| `coreinit_GHS.cpp`, `coreinit_MCP.cpp` | file lock masks, UC mutex | mostly locks; `_flockMutexMask` per process |

### proc_ui
All statics in `proc_ui.cpp` (foreground state, callbacks, core thread data pointers, save callbacks, driver state).
Per process. The ProcUI core threads themselves are guest threads (section 1 + thread list above).

### GX2
| Where | State |
|---|---|
| `GX2_Command.cpp` | per-core command buffer state, internally allocated CB flag |
| `GX2_ContextState.cpp` | `gx2CurrentContextStateMPTR` |
| `GX2_Event.cpp` | event callbacks, callback thread flag, event queue |
| `GX2_Memory.cpp`, `GX2_Resource.cpp` | default alloc/free funcs, GX2R funcs |
| `GX2_TilingAperture.cpp` | active apertures |
| `GX2.cpp`, `GX2_Misc.cpp` | `lastSwapTime`, main core index, TV/DRC scan buffer config, last frame capture |

### snd_core (AX)
Voices (`__AXVoicesPerPriority`, `__AXFreeVoices`, voice arrays pointers, protection arrays), frame/aux/final mix
callbacks, AX mode, master volumes, upsampler history, mix and DMA buffers. Per process. While frozen the title's
voices are silent; the HOME Menu initializes its own AX.

### RPL loader (`rpl.cpp`)
`s_rplDependencyList` (loaded modules), trampoline heap, TLS/handle counters, sdata addresses, data allocator address,
`g_map_callableExports` (host function -> trampoline address), mapped imports. Per process: the HOME Menu loads its own
copies of the libraries at the same addresses.

### PPC recompiler (`PPCRecompiler.cpp`, `BackendX64`)
Function lookup / direct jump tables and generated code are keyed by guest address, and the same address holds
different code in each process. Per process: keep one table set per process and swap the table pointer
(`ppcRecompilerInstanceData`); generated code stays in host memory, so resume needs no recompilation.

### CafeSystem
Foreground title id, `sGameInfo_ForegroundTitle`, executable paths and hashes, entry point, compatibility profile,
mounted title (`UnmountCurrentTitle`), graphic packs. The frozen title stays mounted; the HOME Menu's title is mounted
alongside it.

### GPU (Latte)
Texture cache, buffer cache, texture views, render target state, active shaders, queries, readbacks, streamout are
all keyed by guest physical address. Images only the GPU drew (render targets, render-to-texture such as the Wii U
Menu's Mii icons) exist nowhere else, so restarting the GPU thread loses them.

Implemented: the GPU thread keeps running across suspend and resume. Every cached texture carries the owner id of the
title that created it (`LatteTexture::ownerId`, `LatteTC_SetActiveOwner`); view lookups, memory-overlap tracking and
eviction only consider the active owner, so the frozen title's textures are neither found nor evicted while another
title runs. The buffer cache (an interval tree, which can't hold one range twice) has one tree and node list per owner;
the frozen owner's set is parked unchanged (`LatteBufferCache_SwitchOwner`). Switching owner also finishes readbacks
and queries, clears the index cache and the shader state cache (keyed by shader program address) and resets the
render target bindings (`Latte_SwitchCacheOwner`). This work runs on the GPU thread while it waits for commands
(`Latte_RunOnGPUThreadWhenIdle`).
- Suspend: park the title's caches and GPU registers, start the next title with initial GPU state.
- Title switch while a title is suspended (`CafeSystem::ShutdownTitleForSwitch`): delete only the ending title's
  textures and buffers, keep the GPU thread.
- Resume: make the suspended title's caches active again and restore its registers. CPU writes recorded by texture
  write tracking while the other title ran are dropped (they went to the other title's memory).
- Fallback: `CEMU_PROCESS_SUSPEND_GPU=restart` stops and restarts the GPU thread; GPU-only images are lost then.
Shader and pipeline caches (compiled code keyed by shader hash) are shared and stay.

### Input and the rest
`vpad` / `padscore` sampling callbacks and connection state, `nsysnet` sockets (Step 5), `nn_*` small states
(act, boss, fp, olv, nim, acp), `swkbd`, `erreula`, `camera`, `mic`, `nfc`, `nsyshid`. Handled per library as each
step needs it; most are re-initialized by the HOME Menu and only need to be set aside for the frozen title.

## 3. Shared by all processes (stays as is)

IOSU modules (FSA, ACP, MCP, ACT, ...), except per-client handle tables; the MMU reservation and shared areas
(`SHARED_AREA` fonts, `IOSU_AREA`, `TILING_APERTURE`, foreground bucket memory); shader and pipeline caches; the
host audio device; logging, overlays and notifications; the title list and MLC mounts; input devices.

## 4. Host code that writes into a title's memory

Anything that writes guest memory while the title is swapped out writes into the other process. Candidates:
GPU retire timestamps and EOP writes, GX2 shared area (vsync/flip counters, in `CEMU_AREA`), IOSU async replies,
AX DMA buffers, texture readbacks. The swap test logs every range written while swapped out
(`MMU: ... was written while swapped out`). Step 1 has to drain or redirect each writer before the swap.

Found so far (swap test with Super Mario 3D World):
- Vsync/flip handling (`LatteTiming_signalVsync`): writes `flipExecuteCount` in the GX2 shared area (CEMU_AREA) and
  notifies GX2 flip/vsync events, while decrementing the host-side `LatteGPUState.flipRequestCount`. With the guest
  write lost, host and title disagree and the title waits for a flip forever after the thaw (frozen picture, audio
  still playing). Fixed: no vsync/flip handling while the title is frozen. Two pages were written
  (0x0E38D000, 0x0E39E000); rerun the test to confirm nothing else writes.

## 5. Step 1 sequence (Wii U Menu frozen under the HOME Menu)

1. HOME pressed on the Wii U Menu: capture the last frame (as now), send the menu ReleaseForeground and wait for its
   `OSReleaseForeground` (the menu stops rendering and audio itself, like on a console).
2. Freeze the menu's threads, drain the GPU, detach its memory and save its per-process host state.
3. Start the HOME Menu in fresh memory and state (the normal title start, without tearing down the frozen menu).
4. HOME Menu exits (Close/Resume): tear down only the HOME Menu, restore the menu's memory and host state, invalidate
   GPU caches, thaw, send the menu AcquireForeground. The menu continues where it was.
5. Applets from the HOME Menu replace the HOME Menu, not the frozen menu (Step 2).
