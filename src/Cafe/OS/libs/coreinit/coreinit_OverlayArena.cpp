#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/CafeProcessState.h"
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_OverlayArena.h"

namespace coreinit
{
	struct
	{
		bool isEnabled;
	}g_coreinitOverlayArena = { 0 };

	uint32 OSIsEnabledOverlayArena()
	{
		return g_coreinitOverlayArena.isEnabled ? 1 : 0;
	}

	void OSEnableOverlayArena(uint32 uknParam, uint32be* areaOffset, uint32be* areaSize)
	{
		if (g_coreinitOverlayArena.isEnabled == false)
		{
			memory_enableOverlayArena();
			g_coreinitOverlayArena.isEnabled = true;
		}
		*areaOffset = MEMORY_OVERLAY_AREA_OFFSET;
		*areaSize = MEMORY_OVERLAY_AREA_SIZE;
	}

	void InitializeOverlayArena()
	{
		cafeExportRegister("coreinit", OSIsEnabledOverlayArena, LogType::Placeholder);
		cafeExportRegister("coreinit", OSEnableOverlayArena, LogType::Placeholder);
		g_coreinitOverlayArena.isEnabled = false;
	}
}

// per-process state, saved/restored when the title is suspended (see Cafe/CafeProcessState.h)
namespace coreinit
{
	CAFE_PROCESS_STATE(g_coreinitOverlayArena);
}
