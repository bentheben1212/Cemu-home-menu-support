#include "Cafe/OS/common/OSCommon.h"
#include "sysapp.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/OS/libs/nn_acp/nn_acp.h"

typedef struct  
{
	MEMPTR<char> argStr;
	uint32be size;
}sysStandardArguments_t;

typedef struct
{
	// guessed
	/* +0x00 */ MEMPTR<uint8>	sysAnchor;
	/* +0x04 */ uint32be		sysAnchorSize;
	/* +0x08 */ MEMPTR<uint8>	sysResult;
	/* +0x0C */ uint32be		sysResultSize;
}sysDeserializeStandardArguments_t;

typedef struct
{
	// used for setting arguments, maybe wrong?
	/* +0x00 */ sysStandardArguments_t standardArguments;
	/* +0x08 */ uint32be mode;
	/* +0x0C */ uint32be slot_id;
	/* +0x10 */ MEMPTR<uint8> mii;
}sysMiiStudioArguments_t;

typedef struct  
{
	// used for getting arguments
	/* +0x00 */ sysStandardArguments_t standardArguments; // ?
	/* +0x08 */ uint32 ukn08;
	/* +0x0C */ uint32 ukn0C;
	/* +0x10 */ uint32be mode;
	/* +0x14 */ uint32be slot_id;
	/* +0x18 */ MEMPTR<uint8> mii;
	/* +0x1C */ uint32be miiSize;
}sysMiiStudioArguments2_t;

typedef struct  
{
	/* +0x00 */ sysDeserializeStandardArguments_t standardArguments;
	/* +0x10 */ uint32be jumpTo; // see below for list of available values
	/* +0x14 */ uint32be needsReturn;
	/* +0x18 */ uint32be firstBootMode;
}sysSettingsArguments_t;

static_assert(sizeof(sysSettingsArguments_t) == 0x1C);
static_assert(offsetof(sysSettingsArguments_t, jumpTo) == 0x10);
static_assert(offsetof(sysSettingsArguments_t, needsReturn) == 0x14);
static_assert(offsetof(sysSettingsArguments_t, firstBootMode) == 0x18);

/*
Sys settings jumpTo values:
0x00 -> Normal launch
0x01 -> Internet settings
0x02 -> Data management
0x03 -> TV remote
0x04 -> Date settings
0x05 -> Country settings
0x06 -> System update

0x64 -> Initial system configuration (after Wii U GamePad was configured)
0x65 -> Crashes
0x66 -> Delete All Content and Settings (throws erreula after a second)
0x67 -> Quick Start Settings
0x68 -> TV connection type
0x69 -> Data management
0xFF -> Data transfer
*/

typedef struct
{
	/* +0x00 */ sysDeserializeStandardArguments_t standardArguments; // guessed
	/* +0x10 */ uint32be ukn10;
	/* +0x14 */ uint32be ukn14;
}eshopArguments_t;

static_assert(sizeof(eshopArguments_t) == 0x18);

#define SYS_STANDARD_ARGS_MAX_LEN		0x1000

#define OS_COPY_MAX_DATA_SIZE			(0x400000-4)

void appendDataToBuffer(uint32 currentCopyDataSize, char* str, sint32 size)
{
	cemu_assert_unimplemented();
}

sint32 _serializeArgsToBuffer(uint32 currentCopyDataSize, const char* argName, char* str, sint32 size, void (*appendFunc)(uint32 currentCopyDataSize, char* str, sint32 size))
{
	if (strnlen(argName, 0x41) == 0x40)
		return -0x2710;
	if (size > 0x200000)
		return -0x7530;
	char tempStr[0x80];
	if (size != 0 && str != nullptr)
	{
		sint32 textSize = sprintf(tempStr, "<%s %s=%u>", argName, "size", size);
		if ((currentCopyDataSize + textSize + size) >= OS_COPY_MAX_DATA_SIZE)
			return 0xFFFF63C0;
		appendFunc(currentCopyDataSize, str, size);
		appendFunc(currentCopyDataSize, tempStr, textSize);
	}
	else
	{
		sint32 textSize = sprintf(tempStr, "<%s>", argName);
		appendFunc(currentCopyDataSize, tempStr, textSize);
	}
	return 0;
}

sint32 SYSPackArgs()
{
	uint32 currentCopyDataSize = coreinit::__OSGetCopyDataSize();
	char sysPackStr[128];
	sint32 textSize = sprintf(sysPackStr, "<%s %s=%u>", "sys:pack", "size", currentCopyDataSize);
	if ((currentCopyDataSize + textSize) > OS_COPY_MAX_DATA_SIZE)
		return 0xFFFF4480;
	coreinit::__OSAppendCopyData((uint8*)sysPackStr, textSize);
	return 0;
}

sint32 SYSSerializeSysArgs(const char* argName, char* str, sint32 size)
{
	uint32 currentCopyDataSize = coreinit::__OSGetCopyDataSize();
	sint32 r = _serializeArgsToBuffer(currentCopyDataSize, argName, str, size, appendDataToBuffer);
	if (r < 0)
		return r;
	return 0;
}

sint32 _SYSSerializeStandardArgsIn(sysStandardArguments_t* standardArgs)
{
	if (strnlen(standardArgs->argStr.GetPtr(), SYS_STANDARD_ARGS_MAX_LEN + 4) > SYS_STANDARD_ARGS_MAX_LEN)
	{
		return 0xFFFF63C0;
	}
	SYSSerializeSysArgs("sys:anchor", standardArgs->argStr.GetPtr(), standardArgs->size);
	SYSPackArgs();
	return 0;
}

sint32 _SYSAppendCallerInfo()
{
	// todo
	return 0;
}

typedef struct  
{
	uint32be id;
	uint32be type;
	// union data here - todo
}sysArgSlot_t;

#define SYS_ARG_ID_END				0	// indicates end of sysArgSlot_t array
#define SYS_ARG_ID_ANCHOR			100

typedef struct
{
	char* argument;
	uint32 size;
	uint8* data;
}deserializedArg_t;

uint32 _sysArg_packSize = 0;

void clearSysArgs()
{
	_sysArg_packSize = 0;
}

void deserializeSysArg(deserializedArg_t* deserializedArg)
{
	if (strcmp(deserializedArg->argument, "sys:pack") == 0)
		_sysArg_packSize = deserializedArg->size;
	// todo
}

sint32 _deserializeSysArgsEx2(uint8* copyDataPtr, sint32 copyDataSize, void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam), void* customParam)
{
	sint32 idx = copyDataSize - 1;
	sint32 argSlotIndex = 0;
	while (idx >= 0)
	{
		if (copyDataPtr[idx] == '>')
		{
			// find matching '<'
			sint32 idxStart = idx;
			while (true)
			{
				idxStart--;
				if (idxStart < 0)
					return 1;
				if (copyDataPtr[idxStart] == '<')
					break;
			}
			sint32 idxDataEnd = idxStart;
			idxStart++;
			// parse argument name
			char argumentName[64];
			sint32 idxCurrent = idxStart;
			while( idxCurrent <= idx )
			{
				argumentName[idxCurrent - idxStart] = copyDataPtr[idxCurrent];
				if ((idxCurrent - idxStart) >= 60)
					return 1;
				if (copyDataPtr[idxCurrent] == ' ' || copyDataPtr[idxCurrent] == '>')
				{
					argumentName[idxCurrent - idxStart] = '\0';
					break;
				}
				idxCurrent++;
			}
			// parse size (if any)
			while (copyDataPtr[idxCurrent] == ' ')
				idxCurrent++;
			sint32 argumentDataSize;
			if (copyDataPtr[idxCurrent] == '>')
			{
				argumentDataSize = 0;
			}
			else if ((copyDataSize - idxCurrent) >= 5 && memcmp(copyDataPtr+idxCurrent, "size", 4) == 0)
			{
				idxCurrent += 4;
				// skip whitespaces and '='
				while (copyDataPtr[idxCurrent] == ' ' || copyDataPtr[idxCurrent] == '=')
					idxCurrent++;
				// parse size value
				char tempSizeStr[32];
				sint32 sizeParamLen = idx - idxCurrent;
				if (sizeParamLen < 0 || sizeParamLen >= 30)
					return 1;
				memcpy(tempSizeStr, copyDataPtr+idxCurrent, sizeParamLen);
				tempSizeStr[sizeParamLen] = '\0';
				argumentDataSize = atol(tempSizeStr);
			}
			else
			{
				assert_dbg();
				return 1;
			}
			cemu_assert(argumentDataSize >= 0 && argumentDataSize < 1024*1024*8); // sanity check. Figure out what the limits are on real HW
			cemu_assert(idxStart >= (argumentDataSize + 1));
			idx = idxStart - argumentDataSize - 1; // beginning of data
			deserializedArg_t deserializedArg = { 0 };
			deserializedArg.argument = argumentName;
			cemu_assert((idx + argumentDataSize) <= copyDataSize);
			deserializedArg.size = argumentDataSize;
			deserializedArg.data = copyDataPtr + idx;
			deserializeSysArg(&deserializedArg);
			if (cbDeserializeArg)
				cbDeserializeArg(&deserializedArg, customParam);

			idx--;
		}
		else if (copyDataPtr[idx] == ']')
		{
			assert_dbg();
		}
		else
			return 1;
	}
	return 0;
}

void _deserializeSysArgsEx(void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam), void* customParam)
{
	sint32 copyDataSize = coreinit::__OSGetCopyDataSize();
	uint8* copyDataPtr = coreinit::__OSGetCopyDataPtr();
	_deserializeSysArgsEx2(copyDataPtr, copyDataSize, cbDeserializeArg, customParam);
}

void SYSDeserializeSysArgs(void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam), sysArgSlot_t* argSlots)
{
	_deserializeSysArgsEx(cbDeserializeArg, argSlots);
}

sint32 _deserializeArgs(void* customParam, sint32 customParamSize, void(*cbDeserializeArg)(deserializedArg_t* deserializedArg, void* customParam))
{
	memset(customParam, 0, customParamSize);
	clearSysArgs();
	_deserializeSysArgsEx(cbDeserializeArg, customParam);
	return 0;
}

void _unpackSysWorkaround()
{
	// unpack sys
	clearSysArgs();
	_deserializeSysArgsEx(NULL, NULL);
	if (_sysArg_packSize != 0)
	{
		coreinit::__OSResizeCopyData(_sysArg_packSize);
	}
}

void cbDeserializeArg_MiiMaker(deserializedArg_t* deserializedArg, void* customParam)
{
	sysMiiStudioArguments2_t* miiStudioArgs = (sysMiiStudioArguments2_t*)customParam;

	if (strcmp(deserializedArg->argument, "mode") == 0)
	{
		miiStudioArgs->mode = *(uint32be*)deserializedArg->data;
	}
	else if (strcmp(deserializedArg->argument, "mii") == 0)
	{
		assert_dbg();
	}
	else if (strcmp(deserializedArg->argument, "slot_id") == 0)
	{
#ifdef CEMU_DEBUG_ASSERT
		assert_dbg();
#endif
	}
	else
	{
#ifdef CEMU_DEBUG_ASSERT
		assert_dbg();
#endif
	}
}

sint32 _SYSGetMiiStudioArgs(sysMiiStudioArguments2_t* miiStudioArgs)
{
	_unpackSysWorkaround();
	_deserializeArgs(miiStudioArgs, sizeof(sysMiiStudioArguments2_t), cbDeserializeArg_MiiMaker);
	return 0;
}

void sysappExport__SYSGetMiiStudioArgs(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(miiStudioArgs, sysMiiStudioArguments2_t, 0);
	sint32 r = _SYSGetMiiStudioArgs(miiStudioArgs);
	osLib_returnFromFunction(hCPU, r);
}

void cbDeserializeArg_SysSettings(deserializedArg_t* deserializedArg, void* customParam)
{
	sysSettingsArguments_t* settingsArgs = (sysSettingsArguments_t*)customParam;
	if (boost::iequals(deserializedArg->argument, "jump_to"))
	{
		cemu_assert_unimplemented();
	}
	else if (boost::iequals(deserializedArg->argument, "first_boot_kind"))
	{
		cemu_assert_unimplemented();
	}
	else if (boost::iequals(deserializedArg->argument, "needs_return"))
	{
		settingsArgs->needsReturn = 1;
	}
	else
		cemu_assert_unimplemented();
}

sint32 _SYSGetSettingsArgs(sysSettingsArguments_t* settingsArgs)
{
	_unpackSysWorkaround();
	memset(settingsArgs, 0, sizeof(sysSettingsArguments_t));
	return _deserializeArgs(settingsArgs, sizeof(sysSettingsArguments_t), cbDeserializeArg_SysSettings);
}

void sysappExport__SYSGetSettingsArgs(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(settingsArgs, sysSettingsArguments_t, 0);
	sint32 r = _SYSGetSettingsArgs(settingsArgs);

	if (settingsArgs->jumpTo == 0) // workaround when launching sys settings directly
		settingsArgs->jumpTo = 0x00;

	osLib_returnFromFunction(hCPU, r);
}

struct  
{
	uint64 t0;
	uint64 t1;
	uint64 t2;
}systemApplicationTitleId[] = {
	{ 0x5001010040000, 0x5001010040100, 0x5001010040200 }, // Wii U Menu 
	{ 0x5001010047000, 0x5001010047100, 0x5001010047200 }, // System Settings 
	{ 0x5001010048000, 0x5001010048100, 0x5001010048200 }, // Parental Controls
	{ 0x5001010049000, 0x5001010049100, 0x5001010049200 }, // User Settings 
	{ 0x500101004A000, 0x500101004A100, 0x500101004A200 }, // Mii Maker 
	{ 0x500101004B000, 0x500101004B100, 0x500101004B200 }, // Account Settings
	{ 0x500101004C000, 0x500101004C100, 0x500101004C200 }, // Daily Log 
	{ 0x500101004D000, 0x500101004D100, 0x500101004D200 }, // Notifications
	{ 0x500101004E000, 0x500101004E100, 0x500101004E200 }, // Health and Safety Information 
	{ 0x5001B10059000, 0x5001B10059100, 0x5001B10059200 }, // Wii U Electronic Manual 
	{ 0x500101005A000, 0x500101005A100, 0x500101005A200 }, // Wii U Chat 
	{ 0x5001010062000, 0x5001010062100, 0x5001010062200 }  // Software/Data Transfer 
};

uint64 _SYSGetSystemApplicationTitleIdByProdArea(uint32 systemApplicationId, uint32 platformRegion)
{
	if (systemApplicationId >= (sizeof(systemApplicationTitleId) / sizeof(systemApplicationTitleId[0])) )
		assert_dbg();

	if (platformRegion == 1)
	{
		return systemApplicationTitleId[systemApplicationId].t0;
	}
	else if (platformRegion == 4 || platformRegion == 8)
	{
		return systemApplicationTitleId[systemApplicationId].t2;
	}
	return systemApplicationTitleId[systemApplicationId].t1;
}

uint64 _SYSGetSystemApplicationTitleId(sint32 index)
{
	uint32 region = (uint32)CafeSystem::GetPlatformRegion();
	return _SYSGetSystemApplicationTitleIdByProdArea(index, region);
}

void sysappExport__SYSGetSystemApplicationTitleId(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(systemApplicationId, 0);
	cemuLog_logDebug(LogType::Force, "_SYSGetSystemApplicationTitleId(0x{:x})", hCPU->gpr[3]);

	uint64 titleId = _SYSGetSystemApplicationTitleId(systemApplicationId);
	osLib_returnFromFunction64(hCPU, titleId);
}

void __LaunchMiiMaker(sysMiiStudioArguments_t* args, uint32 platformRegion)
{
	coreinit::__OSClearCopyData();
	if (args)
	{
		_SYSSerializeStandardArgsIn(&args->standardArguments);
		SYSSerializeSysArgs("mode", (char*)&args->mode, 4);
		SYSSerializeSysArgs("slot_id", (char*)&args->slot_id, 4);
		if(args->mii)
			SYSSerializeSysArgs("mii", (char*)args->mii.GetPtr(), 0x60);
	}
	_SYSAppendCallerInfo();
	uint64 titleIdToLaunch = _SYSGetSystemApplicationTitleIdByProdArea(4, platformRegion);
	cemu_assert_unimplemented();
}

void _SYSLaunchMiiStudio(sysMiiStudioArguments_t* args)
{
	__LaunchMiiMaker(args, (uint32)CafeSystem::GetPlatformRegion());
}

void sysappExport__SYSLaunchMiiStudio(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(args, sysMiiStudioArguments_t, 0);
	_SYSLaunchMiiStudio(args);
}

void sysappExport__SYSReturnToCallerWithStandardResult(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32BEPtr(resultPtr, 0);
	cemuLog_log(LogType::Force, "_SYSReturnToCallerWithStandardResult(0x{:08x}) result: 0x{:08x}", hCPU->gpr[3], (uint32)*resultPtr);
	while (true) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void sysappExport__SYSGetEShopArgs(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(args, eshopArguments_t, 0);
	cemuLog_logDebug(LogType::Force, "_SYSGetEShopArgs() - placeholder");
	memset(args, 0, sizeof(eshopArguments_t));

	osLib_returnFromFunction(hCPU, 0);
}

// maps a title id to its PFID/UPID (SysAppPFID). Returns -1 if unknown
sint32 _SYSGetUPIDFromTitleId(uint64 titleId)
{
	uint32 titleIdHigh = (titleId >> 32);
	uint32 titleIdLow = (uint32)(titleId & 0xFFFFFFFF);
	if ((titleIdHigh & 0xFFFF0000) != 0x50000)
		return -1;
	if ((titleIdLow & 0xF0000000) != 0x10000000)
		return -1;
	uint32 titleId_type = (titleIdHigh & 0xFF);
	if (titleId_type == 0x30)
	{
		// applet
		static const uint8 appletTable[10] = { 5,6,8,3,0xA,0xB,9,4,0xC,7 };
		uint32 ukn = ((titleIdLow >> 12) & 0xFFFF);
		if (ukn >= 0x10 && ukn <= 0x19)
			return appletTable[ukn - 0x10];
		if (ukn >= 0x20 && ukn <= 0x29)
			return appletTable[ukn - 0x20];
		return -1;
	}
	else if (titleId_type == 0x10)
	{
		// system application
		if (((titleIdLow >> 12) & 0xFFFF) == 0x40)
			return 2; // Wii U menu
		return 0xF;
	}
	else if (titleId_type == 0x00 || titleId_type == 0x02)
		return 0xF;
	return -1;
}

void sysappExport_SYSGetUPIDFromTitleID(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU64(titleId, 0);
	osLib_returnFromFunction(hCPU, (uint32)_SYSGetUPIDFromTitleId(titleId));
}

void sysappExport_SYSGetVodArgs(PPCInterpreter_t* hCPU)
{
	cemuLog_logDebug(LogType::Force, "SYSGetVodArgs() - todo");
	osLib_returnFromFunction(hCPU, 1);
}

struct SysLauncherArgs18
{
	uint64be caller_id; // titleId
	uint64be launch_title; // titleId
	uint32be mode;
	uint32be slot_id;
};

static_assert(sizeof(SysLauncherArgs18) == 0x18);

struct SysLauncherArgs28
{
	uint32 ukn00;
	uint32 ukn04;
	uint32 ukn08;
	uint32 ukn0C;
	// standard args above
	uint64be caller_id; // titleId
	uint64be launch_title; // titleId
	uint32be mode;
	uint32be slot_id;
};

static_assert(sizeof(SysLauncherArgs28) == 0x28);

// Notifications (system application index 7) reads its launch arguments with this.
// The full struct layout is unknown. Like the other _SYSGet*Args structs it starts with the standard
// deserialize arguments (anchor/result buffers), so only that header is cleared
uint32 _SYSGetNotificationsArgs(sysDeserializeStandardArguments_t* argsOut)
{
	cemuLog_log(LogType::Force, "_SYSGetNotificationsArgs(0x{:08x}) -> 0 (LR 0x{:08x})", MEMPTR<void>(argsOut).GetMPTR(), PPCInterpreter_getCurrentInstance()->spr.LR);
	if (argsOut)
		memset(argsOut, 0, sizeof(sysDeserializeStandardArguments_t));
	return 0;
}

uint32 _SYSGetLauncherArgs(void* argsOut)
{
	uint32 sdkVersion = coreinit::__OSGetProcessSDKVersion();
	if(sdkVersion < 21103)
	{
		// old format
		SysLauncherArgs18* launcherArgs18 = (SysLauncherArgs18*)argsOut;
		memset(launcherArgs18, 0, sizeof(SysLauncherArgs18));
	}
	else
	{
		// new format
		SysLauncherArgs28* launcherArgs28 = (SysLauncherArgs28*)argsOut;
		memset(launcherArgs28, 0, sizeof(SysLauncherArgs28));
	}
	return 0; // return argument is todo
}

struct SysAccountArgs18
{
	uint32be ukn00;
	uint32be ukn04;
	uint32be ukn08;
	uint32be ukn0C;
	// shares part above with Standard arg
	uint32be slotId; // "slot_id"
	uint32be mode; // "mode"
};

uint32 _SYSGetAccountArgs(SysAccountArgs18* argsOut)
{
	memset(argsOut, 0, sizeof(SysAccountArgs18));

	// sysDeserializeStandardArguments_t ?

	return 0;
}

void sysappExport_SYSGetStandardResult(PPCInterpreter_t* hCPU)
{
	cemuLog_logDebug(LogType::Force, "SYSGetStandardResult(0x{:08x},0x{:08x},0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	memset(memory_getPointerFromVirtualOffset(hCPU->gpr[3]), 0, 4);

	// r3 = uint32be* output
	// r4 = pointer to data to parse?
	// r5 = size to parse?

	osLib_returnFromFunction(hCPU, 0);
}

namespace sysapp
{
	void SYSClearSysArgs()
	{
		cemuLog_logDebug(LogType::Force, "SYSClearSysArgs()");
		coreinit::__OSClearCopyData();
	}

	uint32 _SYSLaunchTitleByPathFromLauncher(const char* path, uint32 pathLength)
	{
		coreinit::__OSClearCopyData();
		_SYSAppendCallerInfo();
		return coreinit::OSLaunchTitleByPathl(path, pathLength, 0);
	}

	// returns the Wii U Menu title for the platform region, or an installed menu of another region as fallback
	static uint64 _SYSFindWiiUMenuTitleId()
	{
		uint64 preferredTitleId = _SYSGetSystemApplicationTitleId(0);
		uint16 version;
		if (CafeTitleList::HasTitle(preferredTitleId, version))
			return preferredTitleId;
		for (uint64 titleId : { systemApplicationTitleId[0].t1, systemApplicationTitleId[0].t2, systemApplicationTitleId[0].t0 })
		{
			if (CafeTitleList::HasTitle(titleId, version))
			{
				cemuLog_log(LogType::Force, "SYSLaunchMenu: Wii U Menu {:016x} for platform region is not installed, using {:016x}", preferredTitleId, titleId);
				return titleId;
			}
		}
		return 0;
	}

	uint64 GetWiiUMenuTitleId()
	{
		return _SYSFindWiiUMenuTitleId();
	}

	void SYSLaunchMenu()
	{
		uint64 menuTitleId = _SYSFindWiiUMenuTitleId();
		if (menuTitleId == 0)
		{
			cemuLog_log(LogType::Force, "SYSLaunchMenu: No Wii U Menu title found (expected {:016x}). Title switch aborted", _SYSGetSystemApplicationTitleId(0));
			return;
		}
		cemuLog_log(LogType::Force, "SYSLaunchMenu: Switching from {:016x} to Wii U Menu {:016x}", CafeSystem::GetForegroundTitleId(), menuTitleId);
		coreinit::__OSClearCopyData();
		_SYSAppendCallerInfo();
		coreinit::__LaunchByTitleId(menuTitleId, 0, nullptr);
	}

	// finds an installed, launchable applet for the given PFID. If several match, prefer the title the menu just checked
	// via ACPCheckTitleLaunchByTitleListType. Falls back to that checked title if no applet matches the PFID
	static uint64 _SYSResolvePFIDToTitleId(sint32 pfid)
	{
		uint64 checkedTitleId = nn::acp::ACPGetLastLaunchCheckedTitleId();
		if (pfid == 2) // Wii U Menu
			return _SYSFindWiiUMenuTitleId();
		if (checkedTitleId != 0 && _SYSGetUPIDFromTitleId(checkedTitleId) == pfid && nn::acp::ACPIsTitleLaunchable(checkedTitleId))
			return checkedTitleId;
		// applet titles end in 0x0A (e.g. 00050030-1001010A); other titles in the same range (e.g. ...09) map to the
		// same PFID but are not the applet, so prefer ...0A
		TitleId fallbackTitleId = 0;
		for (TitleId titleId : CafeTitleList::GetAllTitleIds())
		{
			if (TitleIdParser(titleId).GetType() != TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE)
				continue;
			if (_SYSGetUPIDFromTitleId(titleId) != pfid || !nn::acp::ACPIsTitleLaunchable(titleId))
				continue;
			if ((titleId & 0xFF) == 0x0A)
				return titleId;
			if (fallbackTitleId == 0)
				fallbackTitleId = titleId;
		}
		if (fallbackTitleId != 0)
			return fallbackTitleId;
		if (checkedTitleId != 0 && nn::acp::ACPIsTitleLaunchable(checkedTitleId))
		{
			cemuLog_log(LogType::Force, "No installed applet matches PFID {}, using last launch-checked title {:016x} (PFID {})", pfid, checkedTitleId, _SYSGetUPIDFromTitleId(checkedTitleId));
			return checkedTitleId;
		}
		return 0;
	}

	static uint32 _SYSSwitchToTitle(const char* funcName, uint64 titleId)
	{
		if (titleId == 0 || !nn::acp::ACPIsTitleLaunchable(titleId))
		{
			cemuLog_log(LogType::Force, "{}: Title {:016x} is not installed or not launchable. Switch aborted", funcName, titleId);
			return 0xFFFFFFFF;
		}
		cemuLog_log(LogType::Force, "{}: Switching from {:016x} to {:016x}", funcName, CafeSystem::GetForegroundTitleId(), titleId);
		_SYSAppendCallerInfo();
		coreinit::__LaunchByTitleId(titleId, 0, nullptr); // records the current title as the caller (main app) of applets
		return 0;
	}

	uint32 _SYSSwitchTo(sint32 pfid)
	{
		cemuLog_log(LogType::Force, "_SYSSwitchTo(pfid {}) (LR 0x{:08x})", pfid, PPCInterpreter_getCurrentInstance()->spr.LR);
		return _SYSSwitchToTitle("_SYSSwitchTo", _SYSResolvePFIDToTitleId(pfid));
	}

	uint32 SYSSwitchTo(sint32 pfid)
	{
		cemuLog_log(LogType::Force, "SYSSwitchTo(pfid {}) (LR 0x{:08x})", pfid, PPCInterpreter_getCurrentInstance()->spr.LR);
		coreinit::__OSClearCopyData();
		return _SYSSwitchToTitle("SYSSwitchTo", _SYSResolvePFIDToTitleId(pfid));
	}

	uint32 _SYSDirectlySwitchTo(sint32 pfid)
	{
		cemuLog_log(LogType::Force, "_SYSDirectlySwitchTo(pfid {}) (LR 0x{:08x})", pfid, PPCInterpreter_getCurrentInstance()->spr.LR);
		return _SYSSwitchToTitle("_SYSDirectlySwitchTo", _SYSResolvePFIDToTitleId(pfid));
	}

	// eShop is the applet with PFID 10. The menu calls this instead of SYSSwitchTo(10). args (SysEShopArgs) may be null
	static constexpr sint32 SYS_PFID_ESHOP = 10;

	// used by other libraries that start an applet themselves (e.g. nn_olv StartPortalApp for Miiverse)
	uint32 SwitchToPFID(sint32 pfid, const char* funcName)
	{
		coreinit::__OSClearCopyData();
		return _SYSSwitchToTitle(funcName, _SYSResolvePFIDToTitleId(pfid));
	}

	uint32 SYSSwitchToEShop(void* args)
	{
		cemuLog_log(LogType::Force, "SYSSwitchToEShop(args 0x{:08x}) (LR 0x{:08x})", MEMPTR<void>(args).GetMPTR(), PPCInterpreter_getCurrentInstance()->spr.LR);
		coreinit::__OSClearCopyData();
		return _SYSSwitchToTitle("SYSSwitchToEShop", _SYSResolvePFIDToTitleId(SYS_PFID_ESHOP));
	}

	uint32 _SYSSwitchToEShop(void* args)
	{
		cemuLog_log(LogType::Force, "_SYSSwitchToEShop(args 0x{:08x}) (LR 0x{:08x})", MEMPTR<void>(args).GetMPTR(), PPCInterpreter_getCurrentInstance()->spr.LR);
		return _SYSSwitchToTitle("_SYSSwitchToEShop", _SYSResolvePFIDToTitleId(SYS_PFID_ESHOP));
	}

	uint32 SYSLaunchTitle(uint64 titleId)
	{
		cemuLog_log(LogType::Force, "SYSLaunchTitle({:016x}) (LR 0x{:08x})", titleId, PPCInterpreter_getCurrentInstance()->spr.LR);
		coreinit::__OSClearCopyData();
		return _SYSSwitchToTitle("SYSLaunchTitle", titleId);
	}

	uint32 _SYSLaunchTitleWithStdArgsInNoSplash(uint64 titleId, sysStandardArguments_t* stdArgs)
	{
		cemuLog_log(LogType::Force, "_SYSLaunchTitleWithStdArgsInNoSplash({:016x}) (LR 0x{:08x})", titleId, PPCInterpreter_getCurrentInstance()->spr.LR);
		coreinit::__OSClearCopyData();
		if (stdArgs)
			_SYSSerializeStandardArgsIn(stdArgs);
		return _SYSSwitchToTitle("_SYSLaunchTitleWithStdArgsInNoSplash", titleId);
	}

	// called by applets when closing. The applet then finalizes its libraries and calls _Exit(),
	// at which point the queued switch back to the caller happens (see coreinit::__ExitProcessOrSwitch)
	uint32 _SYSSwitchToMainApp()
	{
		uint64 mainAppTitleId = coreinit::__OSGetMainAppTitleId();
		if (mainAppTitleId == 0)
		{
			mainAppTitleId = _SYSFindWiiUMenuTitleId();
			cemuLog_log(LogType::Force, "_SYSSwitchToMainApp: Caller unknown, falling back to Wii U Menu {:016x}", mainAppTitleId);
		}
		if (mainAppTitleId == 0)
		{
			cemuLog_log(LogType::Force, "_SYSSwitchToMainApp: No main app or Wii U Menu found. Process exit will end emulation");
			return 0;
		}
		cemuLog_log(LogType::Force, "_SYSSwitchToMainApp: Queued switch from {:016x} back to {:016x}", CafeSystem::GetForegroundTitleId(), mainAppTitleId);
		coreinit::__OSClearCopyData();
		_SYSAppendCallerInfo();
		coreinit::__OSQueueTitleSwitch(mainAppTitleId);
		// apps like Download Management call _Exit() themselves right after this. Others (e.g. the Internet Browser)
		// wait for the system to send ReleaseForeground + Exit through ProcUI and then return from main
		coreinit::__OSRequestProcessExit();
		return 0;
	}

	// fills the standard (anchor/result) arguments of the current title. No arguments are passed between titles yet
	uint32 SYSGetStandardArgs(sysDeserializeStandardArguments_t* argsOut)
	{
		if (argsOut)
			memset(argsOut, 0, sizeof(sysDeserializeStandardArguments_t));
		return 0;
	}

	// called by the HOME Menu when an applet in its bottom row is tapped. Arguments not documented: the first one is assumed to be the PFID
	uint32 _SYSSwitchToOverlayFromHBM(uint32 arg0, uint32 arg1, uint32 arg2)
	{
		cemuLog_log(LogType::Force, "_SYSSwitchToOverlayFromHBM(0x{:08x}, 0x{:08x}, 0x{:08x}) (LR 0x{:08x})", arg0, arg1, arg2, PPCInterpreter_getCurrentInstance()->spr.LR);
		if (arg0 == 0xFFFFFFFF)
		{
			// HOME Menu closed (Close / B / HOME): no applet, go back to the main app
			cemuLog_log(LogType::Force, "_SYSSwitchToOverlayFromHBM: closing the HOME Menu, returning to the main app");
			coreinit::__OSClearCopyData();
			return _SYSSwitchToMainApp();
		}
		if (arg0 == 0 || arg0 >= 32)
		{
			cemuLog_log(LogType::Force, "_SYSSwitchToOverlayFromHBM: first argument is not a PFID, ignored");
			return 0;
		}
		uint64 titleId = _SYSResolvePFIDToTitleId((sint32)arg0);
		if (titleId == 0)
		{
			cemuLog_log(LogType::Force, "_SYSSwitchToOverlayFromHBM: no title installed for PFID {}", arg0);
			return 0;
		}
		coreinit::__OSClearCopyData();
		return _SYSSwitchToTitle("_SYSSwitchToOverlayFromHBM", titleId);
	}

	// Switches to the HOME Menu overlay. Cemu has no HOME Menu overlay: applets (e.g. Friend List after an error)
	// use this to give control back, so return to the main app like _SYSSwitchToMainApp does
	uint32 _SYSSwitchToHBMWithMode(uint32 mode)
	{
		uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
		cemuLog_log(LogType::Force, "_SYSSwitchToHBMWithMode({}) from {:016x} (LR 0x{:08x})", mode, currentTitleId, PPCInterpreter_getCurrentInstance()->spr.LR);
		if (TitleIdParser(currentTitleId).GetType() != TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE)
		{
			cemuLog_log(LogType::Force, "_SYSSwitchToHBMWithMode: HOME Menu overlay not supported, ignored");
			return 0;
		}
		return _SYSSwitchToMainApp();
	}

	// launches the Notifications system application (system application index 7). Arguments are unknown and ignored
	void export__SYSLaunchNotificationsDirect(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::Force, "_SYSLaunchNotificationsDirect(r3=0x{:08x}, r4=0x{:08x}) (LR 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->spr.LR);
		coreinit::__OSClearCopyData();
		uint32 r = _SYSSwitchToTitle("_SYSLaunchNotificationsDirect", _SYSGetSystemApplicationTitleId(7));
		osLib_returnFromFunction(hCPU, r);
	}

	// title that launched the current one, recorded on each title switch
	uint64 SYSGetCallerTitleId()
	{
		uint64 callerTitleId = coreinit::__OSGetCallerTitleId();
		cemuLog_log(LogType::Force, "SYSGetCallerTitleId() -> {:016x}", callerTitleId);
		return callerTitleId;
	}

	sint32 SYSGetCallerUPID()
	{
		uint64 callerTitleId = coreinit::__OSGetCallerTitleId();
		sint32 upid = callerTitleId ? _SYSGetUPIDFromTitleId(callerTitleId) : -1;
		cemuLog_log(LogType::Force, "SYSGetCallerUPID() -> {} (caller {:016x})", upid, callerTitleId);
		return upid;
	}

	uint32 SYSRelaunchTitle(uint32 argc, MEMPTR<char>* argv)
	{
		// calls ACPCheckSelfTitleNotReferAccountLaunch?
		coreinit::__OSClearCopyData();
		_SYSAppendCallerInfo();
		return coreinit::OSRestartGame(argc, argv);
	}

	struct EManualArgs
	{
		sysStandardArguments_t stdArgs;
		uint64be titleId;
	};
	static_assert(sizeof(EManualArgs) == 0x10);

	// Electronic Manual applet (PFID 3). Copy data (sys args) serialization isn't implemented, so the title whose manual
	// was requested is kept here and handed to the manual through _SYSGetEManualArgs when it starts
	static constexpr sint32 SYS_PFID_EMANUAL = 3;
	static uint64 s_eManualRequestedTitleId = 0;

	void _SYSSwitchToEManual(EManualArgs* args)
	{
		// the struct has the titleId at offset 8 and standard args at 0 (total size is most likely 0x10)
		uint64 requestedTitleId = args ? (uint64)args->titleId : 0;
		uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
		cemuLog_log(LogType::Force, "_SYSSwitchToEManual(args 0x{:08x}, title {:016x}) from {:016x} (LR 0x{:08x})", MEMPTR<void>(args).GetMPTR(), requestedTitleId, currentTitleId, PPCInterpreter_getCurrentInstance()->spr.LR);
		if (requestedTitleId == 0)
		{
			// opened from an applet (e.g. the HOME Menu): show the manual of the main app
			requestedTitleId = coreinit::__OSGetMainAppTitleId();
			if (requestedTitleId == 0)
				requestedTitleId = currentTitleId;
		}
		uint64 eManualTitleId = _SYSResolvePFIDToTitleId(SYS_PFID_EMANUAL);
		if (eManualTitleId == 0 || !nn::acp::ACPIsTitleLaunchable(eManualTitleId))
		{
			cemuLog_log(LogType::Force, "_SYSSwitchToEManual: Electronic Manual (PFID 3) not installed, manual not opened");
			coreinit::StartBackgroundForegroundTransition();
			return;
		}
		s_eManualRequestedTitleId = requestedTitleId;
		cemuLog_log(LogType::Force, "_SYSSwitchToEManual: opening the manual of {:016x} in {:016x}", requestedTitleId, eManualTitleId);
		coreinit::__OSClearCopyData();
		_SYSSwitchToTitle("_SYSSwitchToEManual", eManualTitleId);
	}

	// called by the HOME Menu's Manual button. Arguments not documented (logged): opens the manual of the main app
	uint32 _SYSSwitchToEManualFromHBM(uint32 arg0, uint32 arg1, uint32 arg2)
	{
		cemuLog_log(LogType::Force, "_SYSSwitchToEManualFromHBM(0x{:08x}, 0x{:08x}, 0x{:08x}) (LR 0x{:08x})", arg0, arg1, arg2, PPCInterpreter_getCurrentInstance()->spr.LR);
		_SYSSwitchToEManual(nullptr);
		return 0;
	}

	// called by the Electronic Manual when it starts. Layout assumed to match EManualArgs (standard args, title id at +8)
	uint32 _SYSGetEManualArgs(EManualArgs* argsOut)
	{
		uint64 titleId = s_eManualRequestedTitleId;
		if (titleId == 0)
			titleId = coreinit::__OSGetMainAppTitleId();
		cemuLog_log(LogType::Force, "_SYSGetEManualArgs(0x{:08x}) -> title {:016x} (LR 0x{:08x})", MEMPTR<void>(argsOut).GetMPTR(), titleId, PPCInterpreter_getCurrentInstance()->spr.LR);
		if (argsOut)
		{
			memset(argsOut, 0, sizeof(EManualArgs));
			argsOut->titleId = titleId;
		}
		return 0;
	}

	void SYSSwitchToEManual()
	{
		EManualArgs args{};
		args.titleId = coreinit::OSGetTitleID();
		_SYSSwitchToEManual(&args);
	}

	void load()
	{
		cafeExportRegisterFunc(SYSClearSysArgs, "sysapp", "SYSClearSysArgs", LogType::Placeholder);
		cafeExportRegisterFunc(_SYSLaunchTitleByPathFromLauncher, "sysapp", "_SYSLaunchTitleByPathFromLauncher", LogType::Placeholder);
		cafeExportRegisterFunc(SYSRelaunchTitle, "sysapp", "SYSRelaunchTitle", LogType::Placeholder);
		cafeExportRegister("sysapp", SYSLaunchMenu, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchToMainApp, LogType::Placeholder);
		cafeExportRegister("sysapp", SYSGetStandardArgs, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchToHBMWithMode, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchToOverlayFromHBM, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchTo, LogType::Placeholder);
		osLib_addFunction("sysapp", "_SYSLaunchNotificationsDirect", export__SYSLaunchNotificationsDirect);
		cafeExportRegister("sysapp", SYSGetCallerTitleId, LogType::Placeholder);
		cafeExportRegister("sysapp", SYSGetCallerUPID, LogType::Placeholder);
		cafeExportRegister("sysapp", SYSSwitchTo, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSDirectlySwitchTo, LogType::Placeholder);
		cafeExportRegister("sysapp", SYSLaunchTitle, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSLaunchTitleWithStdArgsInNoSplash, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchToEManual, LogType::Placeholder);
		cafeExportRegister("sysapp", SYSSwitchToEManual, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSGetEManualArgs, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchToEManualFromHBM, LogType::Placeholder);
		cafeExportRegister("sysapp", SYSSwitchToEShop, LogType::Placeholder);
		cafeExportRegister("sysapp", _SYSSwitchToEShop, LogType::Placeholder);
	}
}

namespace sysapp
{
	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "sysapp";
		}

		void RPLMapped() override
		{
			osLib_addFunction("sysapp", "_SYSLaunchMiiStudio", sysappExport__SYSLaunchMiiStudio);
			osLib_addFunction("sysapp", "_SYSGetMiiStudioArgs", sysappExport__SYSGetMiiStudioArgs);
			osLib_addFunction("sysapp", "_SYSGetSettingsArgs", sysappExport__SYSGetSettingsArgs);
			osLib_addFunction("sysapp", "_SYSReturnToCallerWithStandardResult", sysappExport__SYSReturnToCallerWithStandardResult);

			osLib_addFunction("sysapp", "_SYSGetSystemApplicationTitleId", sysappExport__SYSGetSystemApplicationTitleId);
			osLib_addFunction("sysapp", "SYSGetUPIDFromTitleID", sysappExport_SYSGetUPIDFromTitleID);

			osLib_addFunction("sysapp", "_SYSGetEShopArgs", sysappExport__SYSGetEShopArgs);

			osLib_addFunction("sysapp", "SYSGetVodArgs", sysappExport_SYSGetVodArgs);

			osLib_addFunction("sysapp", "SYSGetStandardResult", sysappExport_SYSGetStandardResult);

			cafeExportRegisterFunc(_SYSGetLauncherArgs, "sysapp", "_SYSGetLauncherArgs", LogType::Placeholder);
			cafeExportRegisterFunc(_SYSGetNotificationsArgs, "sysapp", "_SYSGetNotificationsArgs", LogType::Placeholder);
			cafeExportRegisterFunc(_SYSGetAccountArgs, "sysapp", "_SYSGetAccountArgs", LogType::Placeholder);

			sysapp::load();
		};

	}s_COSsysappModule;

	COSModule* GetModule()
	{
		return &s_COSsysappModule;
	}
}
