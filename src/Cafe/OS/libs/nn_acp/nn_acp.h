#pragma once
#include "Cafe/IOSU/legacy/iosu_acp.h"
#include "Cafe/OS/RPL/COSModule.h"

namespace nn
{
namespace acp
{
	enum ACPStatus : uint32
	{
		SUCCESS = 0,
	};

	using ACPDeviceType = iosu::acp::ACPDeviceType;

	bool ACPIsTitleLaunchable(uint64 titleId); // installed and has valid app/cos/meta xml
	uint64 ACPGetLastLaunchCheckedTitleId(); // last title passed to ACPCheckTitleLaunchByTitleListType (0 if none)

	ACPStatus ACPGetApplicationBox(uint32be* applicationBox, uint64 titleId);
	ACPStatus ACPMountSaveDir();
    ACPStatus ACPUnmountSaveDir();
	ACPStatus ACPCreateSaveDir(uint32 persistentId, iosu::acp::ACPDeviceType type);
	ACPStatus ACPUpdateSaveTimeStamp(uint32 persistentId, uint64 titleId, iosu::acp::ACPDeviceType deviceType);

	COSModule* GetModule();
}
}
