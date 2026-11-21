#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h" // todo - remove dependency
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "WindowSystem.h"

#include "Cafe/HW/Latte/Core/LatteBufferCache.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "util/helpers/helpers.h"

#include <imgui.h>
#include "config/ActiveSettings.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"

LatteGPUState_t LatteGPUState = {};

std::atomic_bool sLatteThreadRunning = false;
std::atomic_bool sLatteThreadFinishedInit = false;

void LatteThread_Exit();

void Latte_LoadInitialRegisters()
{
	LatteGPUState.contextNew.CB_TARGET_MASK.set_MASK(0xFFFFFFFF);
	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.set_RESTART_INDEX(0xFFFFFFFF);
	LatteGPUState.contextNew.VGT_DMA_NUM_INSTANCES.set_NUM_INSTANCES(1);
	LatteGPUState.contextRegister[Latte::REGADDR::PA_CL_CLIP_CNTL] = 0;
	*(float*)&LatteGPUState.contextRegister[mmDB_DEPTH_CLEAR] = 1.0f;
}

extern bool gx2WriteGatherInited;

std::vector<uint32> g_latteResumeContextRegisters; // set by CafeProcess when resuming a suspended title

LatteTextureView* osScreenTVTex[2] = { nullptr };
LatteTextureView* osScreenDRCTex[2] = { nullptr };

LatteTextureView* LatteHandleOSScreen_getOrCreateScreenTex(MPTR physAddress, uint32 width, uint32 height, uint32 pitch)
{
	LatteTextureView* texView = LatteTextureViewLookupCache::lookup(physAddress, width, height, 1, pitch, 0, 1, 0, 1, Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM, Latte::E_DIM::DIM_2D);
	if (texView)
		return texView;
	return LatteTexture_CreateTexture(Latte::E_DIM::DIM_2D, physAddress, 0, Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM, width, height, 1, pitch, 1, 0, Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED, false);
}

void LatteHandleOSScreen_prepareTextures()
{
	osScreenTVTex[0] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[0].physPtr, 1280, 720, 1280);
	osScreenTVTex[1] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[0].physPtr + 1280 * 720 * 4, 1280, 720, 1280);
	osScreenDRCTex[0] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[1].physPtr, 854, 480, 0x380);
	osScreenDRCTex[1] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[1].physPtr + 896 * 480 * 4, 854, 480, 0x380);
}

void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView);

bool LatteHandleOSScreen_TV()
{
	if (!LatteGPUState.osScreen.screen[0].isEnabled)
		return false;
	if (LatteGPUState.osScreen.screen[0].flipExecuteCount == LatteGPUState.osScreen.screen[0].flipRequestCount)
		return false;
	LatteHandleOSScreen_prepareTextures();

	sint32 bufferDisplayTV = (LatteGPUState.osScreen.screen[0].flipRequestCount & 1) ^ 1;
	sint32 bufferDisplayDRC = (LatteGPUState.osScreen.screen[1].flipRequestCount & 1) ^ 1;

	const uint32 bufferIndexTV = (bufferDisplayTV);
	const uint32 bufferIndexDRC = bufferDisplayDRC;

	LatteTexture_ReloadData(osScreenTVTex[bufferIndexTV]->baseTexture);

	// TV screen
	LatteRenderTarget_copyToBackbuffer(osScreenTVTex[bufferIndexTV]->baseTexture->baseView, false);
	
	if (LatteGPUState.osScreen.screen[0].flipExecuteCount != LatteGPUState.osScreen.screen[0].flipRequestCount)
		LatteGPUState.osScreen.screen[0].flipExecuteCount.store(LatteGPUState.osScreen.screen[0].flipRequestCount);
	return true;
}

bool LatteHandleOSScreen_DRC()
{
	if (!LatteGPUState.osScreen.screen[1].isEnabled)
		return false;
	if (LatteGPUState.osScreen.screen[1].flipExecuteCount == LatteGPUState.osScreen.screen[1].flipRequestCount)
		return false;
	LatteHandleOSScreen_prepareTextures();

	sint32 bufferDisplayDRC = (LatteGPUState.osScreen.screen[1].flipRequestCount & 1) ^ 1;

	const uint32 bufferIndexDRC = bufferDisplayDRC;

	LatteTexture_ReloadData(osScreenDRCTex[bufferIndexDRC]->baseTexture);

	// GamePad screen
	LatteRenderTarget_copyToBackbuffer(osScreenDRCTex[bufferIndexDRC]->baseTexture->baseView, true);

	if (LatteGPUState.osScreen.screen[1].flipExecuteCount != LatteGPUState.osScreen.screen[1].flipRequestCount)
		LatteGPUState.osScreen.screen[1].flipExecuteCount.store(LatteGPUState.osScreen.screen[1].flipRequestCount);
	return true;
}

void LatteThread_HandleOSScreen()
{
	bool swapTV = LatteHandleOSScreen_TV();
	bool swapDRC = LatteHandleOSScreen_DRC();
	if(swapTV || swapDRC)
		g_renderer->SwapBuffers(swapTV, swapDRC);
}

int Latte_ThreadEntry()
{
	SetThreadName("LatteThread");
	sint32 w,h;
	WindowSystem::GetWindowPhysSize(w,h);

	// renderer
	g_renderer->Initialize();
	RendererOutputShader::InitializeStatic();

	LatteTiming_Init();
	LatteTexture_init();
	LatteTC_Init();
	LatteBufferCache_init(164 * 1024 * 1024);
	LatteQuery_Init();
	LatteSHRC_Init();
	LatteStreamout_InitCache();

	g_renderer->renderTarget_setViewport(0, 0, w, h, 0.0f, 1.0f);
	
	// enable GLSL gl_PointSize support
	// glEnable(GL_PROGRAM_POINT_SIZE); // breaks shader caching on AMD (as of 2018)
	
	LatteGPUState.glVendor = GLVENDOR_UNKNOWN;
	switch(g_renderer->GetVendor())
	{
	case GfxVendor::AMD: 
		LatteGPUState.glVendor = GLVENDOR_AMD;
		break;
	case GfxVendor::Intel:
		LatteGPUState.glVendor = GLVENDOR_INTEL; 
		break;
	case GfxVendor::Nvidia: 
		LatteGPUState.glVendor = GLVENDOR_NVIDIA; 
		break;
	case GfxVendor::Apple:
		LatteGPUState.glVendor = GLVENDOR_APPLE;
	default:
		break;
	}

	sLatteThreadFinishedInit = true;

	// register debug handler
	if (cemuLog_isLoggingEnabled(LogType::OpenGLLogging))
		g_renderer->EnableDebugMode();

	// wait till a game is started
	while( true )
	{
		if( CafeSystem::IsTitleRunning() )
			break;

		g_renderer->DrawEmptyFrame(true);
		g_renderer->DrawEmptyFrame(false);
		g_renderer->CancelScreenshotRequest(); // keep the screenshot request queue empty
		std::this_thread::sleep_for(std::chrono::milliseconds(1000/60));
	}

	g_renderer->DrawEmptyFrame(true);

	// before doing anything with game specific shaders, we need to wait for graphic packs to finish loading
	GraphicPack2::WaitUntilReady();
	// if legacy packs are enabled we cannot use the colorbuffer resolution optimization
	LatteGPUState.allowFramebufferSizeOptimization = true;
	for(auto& pack : GraphicPack2::GetActiveGraphicPacks())
	{
		if(pack->AllowRendertargetSizeOptimization())
			continue;
		for(auto& rule : pack->GetTextureRules())
		{
			if(rule.filter_settings.width >= 0 || rule.filter_settings.height >= 0 || rule.filter_settings.depth >= 0 ||
				rule.overwrite_settings.width >= 0 || rule.overwrite_settings.height >= 0 || rule.overwrite_settings.depth >= 0)
			{
				LatteGPUState.allowFramebufferSizeOptimization = false;
				cemuLog_log(LogType::Force, "Graphic pack \"{}\" prevents rendertarget size optimization. This warning can be ignored and is intended for graphic pack developers", pack->GetName());
				break;
			}
		}
	}
	// load disk shader cache
    LatteShaderCache_Load();
	// init registers
	Latte_LoadInitialRegisters();
	// a resumed title continues with its own register values (see CafeProcess)
	if (!g_latteResumeContextRegisters.empty())
	{
		cemu_assert(g_latteResumeContextRegisters.size() == LATTE_MAX_REGISTER);
		memcpy(LatteGPUState.contextRegister, g_latteResumeContextRegisters.data(), LATTE_MAX_REGISTER * sizeof(uint32));
		g_latteResumeContextRegisters.clear();
	}
	// let CPU thread know the GPU is done initializing
	g_isGPUInitFinished = true;
	// wait until CPU has called GX2Init()
	while (LatteGPUState.gx2InitCalled == 0)
	{
		std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		LatteThread_HandleOSScreen();
		Latte_RunPendingIdleTask();
		if (Latte_GetStopSignal())
			LatteThread_Exit();
	}
	LatteCP_ProcessRingbuffer();
	cemu_assert_debug(false); // should never reach
	return 0;
}

std::thread sLatteThread;
std::mutex sLatteThreadStateMutex;

// initializes GPU thread which in turn also activates graphic packs
// does not return until the thread finished initialization
void Latte_Start()
{
	std::unique_lock _lock(sLatteThreadStateMutex);
	cemu_assert_debug(!sLatteThreadRunning);
	sLatteThreadRunning = true;
	sLatteThreadFinishedInit = false;
	sLatteThread = std::thread(Latte_ThreadEntry);
	// wait until initialized
	while (!sLatteThreadFinishedInit)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void Latte_Stop()
{
	std::unique_lock _lock(sLatteThreadStateMutex);
	if (!sLatteThreadRunning)
		return;
	sLatteThreadRunning = false;
	_lock.unlock();
	sLatteThread.join();
}

bool Latte_GetStopSignal()
{
	return !sLatteThreadRunning;
}

bool Latte_IsRunning()
{
	return sLatteThreadRunning && sLatteThreadFinishedInit;
}

// tasks run on the GPU thread while it waits for commands
std::mutex s_gpuIdleTaskCallerMutex; // one task at a time
std::mutex s_gpuIdleTaskMutex;
std::condition_variable s_gpuIdleTaskCond;
std::function<void()> s_gpuIdleTask;
std::atomic<bool> s_gpuIdleTaskPending = false;
bool s_gpuIdleTaskRunning = false;

void Latte_RunPendingIdleTask()
{
	if (!s_gpuIdleTaskPending.load(std::memory_order_acquire))
		return;
	std::function<void()> task;
	{
		std::unique_lock _l(s_gpuIdleTaskMutex);
		if (!s_gpuIdleTaskPending)
			return;
		task = std::move(s_gpuIdleTask);
		s_gpuIdleTask = nullptr;
		s_gpuIdleTaskPending = false;
		s_gpuIdleTaskRunning = true;
	}
	task();
	{
		std::unique_lock _l(s_gpuIdleTaskMutex);
		s_gpuIdleTaskRunning = false;
	}
	s_gpuIdleTaskCond.notify_all();
}

bool Latte_RunOnGPUThreadWhenIdle(std::function<void()> task, uint32 timeoutMs)
{
	std::unique_lock _callerLock(s_gpuIdleTaskCallerMutex);
	if (!Latte_IsRunning())
		return false;
	std::unique_lock _l(s_gpuIdleTaskMutex);
	s_gpuIdleTask = std::move(task);
	s_gpuIdleTaskPending = true;
	auto isDone = []() { return !s_gpuIdleTaskPending && !s_gpuIdleTaskRunning; };
	if (s_gpuIdleTaskCond.wait_for(_l, std::chrono::milliseconds(timeoutMs), isDone))
		return true;
	if (s_gpuIdleTaskPending)
	{
		// never started: withdraw it
		s_gpuIdleTaskPending = false;
		s_gpuIdleTask = nullptr;
		return false;
	}
	s_gpuIdleTaskCond.wait(_l, isDone); // started, let it finish
	return true;
}

void Latte_SwitchCacheOwner(uint32 newOwnerId, bool deleteCurrent)
{
	uint32 oldOwnerId = LatteTC_GetActiveOwner();
	auto start = std::chrono::steady_clock::now();
	// finish operations that write into the current owner's memory
	LatteTextureReadback_UpdateFinishedTransfers(true);
	LatteQuery_UpdateFinishedQueriesForceFinishAll();
	LatteIndices_invalidateAll();
	// nothing stays bound
	for (uint32 i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
		LatteMRT::SetColorAttachment(i, nullptr);
	LatteMRT::SetDepthAndStencilAttachment(nullptr, false);
	if (deleteCurrent)
		LatteTC_DeleteTexturesOfOwner(oldOwnerId);
	LatteBufferCache_SwitchOwner(newOwnerId, deleteCurrent);
	LatteTC_SetActiveOwner(newOwnerId);
	LatteTC_PrepareTexturesForResume(newOwnerId);
	// caches keyed by shader program addresses
	LatteSHRC_ResetShaderStateCache();
	LatteGPUState.frameCounter++; // fetch shader lookups made in an earlier frame are verified against the program data
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	cemuLog_log(LogType::Force, "GPU: cache owner {} -> {} ({} the old one's textures and buffers) in {} ms", oldOwnerId, newOwnerId, deleteCurrent ? "deleted" : "kept", ms);
}

std::atomic<bool> g_latteReadbackOnExit = false; // set by CafeProcess when suspending a title

#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"

// suspending a title: images the GPU drew (render targets, render-to-texture results like the Wii U Menu's Mii
// textures) exist only in host GPU memory. The GPU thread is restarted for the next title, so copy them back into the
// title's memory first; on resume they are reloaded from there like any other texture
static void LatteThread_ReadbackGPUWrittenTextures()
{
	auto start = std::chrono::steady_clock::now();
	uint32 numTextures = 0, numSlices = 0, numFailed = 0, numAllZero = 0, numWithData = 0;
	for (LatteTexture* texture : LatteTexture::GetAllTextures())
	{
		if (!texture || !texture->isUpdatedOnGPU || texture->isDepth)
			continue;
		if (texture->overwriteInfo.hasResolutionOverwrite)
			continue; // readback doesn't support resized textures
		if (texture->dim == Latte::E_DIM::DIM_3D)
			continue;
		numTextures++;
		uint32 sliceCount = std::max<uint32>(1, texture->depth);
		for (uint32 slice = 0; slice < sliceCount; slice++)
		{
			LatteTextureView* view = texture->GetOrCreateView(0, 1, slice, 1);
			if (!view)
				continue;
			numSlices++;
			// synchronous transfer per slice (the queued path gives no feedback)
			LatteTextureReadbackInfo* info = g_renderer->texture_createReadback(view);
			if (!info)
			{
				numFailed++;
				continue;
			}
			info->StartTransfer();
			info->ForceFinish();
			if (!info->IsFinished())
			{
				numFailed++;
				delete info;
				continue;
			}
			uint8* data = info->GetData();
			bool allZero = true;
			uint32 checkSize = std::min<uint32>(info->m_rowPitch * std::max<uint32>(1, texture->height), 4 * 1024 * 1024);
			for (uint32 i = 0; data && i < checkSize; i++)
			{
				if (data[i] != 0)
				{
					allZero = false;
					break;
				}
			}
			if (allZero)
				numAllZero++;
			else
			{
				numWithData++;
				if (numWithData <= 8)
					cemuLog_log(LogType::Force, "Process:   readback {:08x} {}x{} fmt {:04x} tm {} slice {}: has data", texture->physAddress, texture->width, texture->height, (uint32)texture->format, (uint32)texture->tileMode, slice);
			}
			if (data)
				LatteTextureLoader_writeReadbackTextureToMemory(&info->hostTextureCopy, info->m_firstSlice, info->m_firstMip, data, info->m_rowPitch);
			info->ReleaseData();
			delete info;
		}
	}
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	cemuLog_log(LogType::Force, "Process: copied {} GPU-written textures ({} slices: {} with data, {} all zero, {} failed) back to the title's memory in {} ms", numTextures, numSlices, numWithData, numAllZero, numFailed, ms);
}

void LatteThread_Exit()
{
	if (g_latteReadbackOnExit.exchange(false) && g_renderer)
		LatteThread_ReadbackGPUWrittenTextures();
	if (g_renderer)
		g_renderer->Shutdown();
    // clean up vertex/uniform cache
    LatteBufferCache_UnloadAll();
	// clean up texture cache
	LatteTC_UnloadAllTextures();
	// clean up runtime shader cache
    LatteSHRC_UnloadAll();
    // close disk cache
    LatteShaderCache_Close();
	RendererOutputShader::ShutdownStatic();
    // destroy renderer but make sure that g_renderer remains valid until the destructor has finished
	if (g_renderer)
	{
		Renderer* renderer = g_renderer.get();
		delete renderer;
		g_renderer.release();
	}
	// reset GPU7 state
	std::memset(&LatteGPUState, 0, sizeof(LatteGPUState));
	#if BOOST_OS_WINDOWS
	ExitThread(0);
	#else
	pthread_exit(nullptr);
	#endif
	cemu_assert_unimplemented();
}
