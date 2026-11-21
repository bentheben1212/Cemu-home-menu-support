#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/Filesystem/fsc.h"
#include "nn_acp.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/OS/libs/coreinit/coreinit_IOS.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"

#include <cinttypes>
#include <filesystem>
#include <fstream>

#include "config/ActiveSettings.h"
#include "Cafe/IOSU/legacy/iosu_acp.h"
#include "Cafe/IOSU/legacy/iosu_ioctl.h"

#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "Common/FileStream.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/IOSU/legacy/iosu_mcp.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"

sint32 _SYSGetUPIDFromTitleId(uint64 titleId); // sysapp.cpp

using ACPDeviceType = iosu::acp::ACPDeviceType;

#define acpPrepareRequest() \
StackAllocator<iosuAcpCemuRequest_t> _buf_acpRequest; \
StackAllocator<ioBufferVector_t> _buf_bufferVector; \
iosuAcpCemuRequest_t* acpRequest = _buf_acpRequest.GetPointer(); \
ioBufferVector_t* acpBufferVector = _buf_bufferVector.GetPointer(); \
memset(acpRequest, 0, sizeof(iosuAcpCemuRequest_t)); \
memset(acpBufferVector, 0, sizeof(ioBufferVector_t)); \
acpBufferVector->buffer = (uint8*)acpRequest;

namespace nn
{
namespace acp
{
	// ACP result returned when a title can't be launched. Value assumed (ACP "not found"), not verified against hardware
	static constexpr sint32 ACP_RESULT_NOT_FOUND = -500;

	static std::atomic<uint64> s_lastLaunchCheckedTitleId = 0;

	bool ACPIsTitleLaunchable(uint64 titleId)
	{
		TitleInfo titleInfo;
		if (!CafeTitleList::GetFirstByTitleId(titleId, titleInfo))
			return false;
		return titleInfo.IsValid();
	}

	uint64 ACPGetLastLaunchCheckedTitleId()
	{
		return s_lastLaunchCheckedTitleId.load();
	}

	// ACPCheckTitleLaunchByTitleListType(MCPTitleListType* title, ?)
	// Called by the Wii U Menu before launching a title from its title list (e.g. an applet from the bottom row)
	void export_ACPCheckTitleLaunchByTitleListType(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamStructPtr(titleInfo, MCPTitleInfo, 0);
		if (!titleInfo)
		{
			cemuLog_log(LogType::Force, "ACPCheckTitleLaunchByTitleListType(null, 0x{:08x}) (LR 0x{:08x})", hCPU->gpr[4], hCPU->spr.LR);
			osLib_returnFromFunction(hCPU, (uint32)ACP_RESULT_NOT_FOUND);
			return;
		}
		uint64 titleId = ((uint64)(uint32)titleInfo->titleIdHigh << 32) | (uint64)(uint32)titleInfo->titleIdLow;
		bool launchable = ACPIsTitleLaunchable(titleId);
		s_lastLaunchCheckedTitleId.store(titleId);
		cemuLog_log(LogType::Force, "ACPCheckTitleLaunchByTitleListType({:016x}, 0x{:08x}) -> {} (LR 0x{:08x})",
					titleId, hCPU->gpr[4], launchable ? "launchable" : "not installed/invalid", hCPU->spr.LR);
		// trace what the caller does next, so a launch that silently does nothing shows where it stops
		PPCInterpreter_startHLETrace(fmt::format("after launch check of {:016x}", titleId), 3000);
		osLib_returnFromFunction(hCPU, launchable ? (uint32)ACPStatus::SUCCESS : (uint32)ACP_RESULT_NOT_FOUND);
	}

	ACPStatus ACPConvertResultToACPStatus(uint32* nnResult, const char* functionName, uint32 lineNumber)
	{
		// todo
		return ACPStatus::SUCCESS;
	}

	#define _ACPConvertResultToACPStatus(nnResult) ACPConvertResultToACPStatus(nnResult, __func__, __LINE__)

	ACPStatus ACPGetApplicationBox(uint32be* applicationBox, uint64 titleId)
	{
		// todo
		*applicationBox = 3; // storage mlc
		return ACPStatus::SUCCESS;
	}

	ACPStatus ACPGetOlvAccesskey(uint32be* accessKey)
	{
		nnResult r = iosu::acp::ACPGetOlvAccesskey(accessKey);
		return _ACPConvertResultToACPStatus(&r);
	}

    bool sSaveDirMounted{false};

	ACPStatus ACPMountSaveDir()
	{
        cemu_assert_debug(!sSaveDirMounted);
		uint64 titleId = CafeSystem::GetForegroundTitleId();
		uint32 high = GetTitleIdHigh(titleId) & (~0xC);
		uint32 low = GetTitleIdLow(titleId);

		// mount save path
		const auto mlc = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user/", high, low);
		FSCDeviceHostFS_Mount("/vol/save/", _pathToUtf8(mlc), FSC_PRIORITY_BASE);
		nnResult mountResult = BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_ACP, 0);
		return _ACPConvertResultToACPStatus(&mountResult);
	}

    ACPStatus ACPUnmountSaveDir()
    {
        cemu_assert_debug(!sSaveDirMounted);
        fsc_unmount("/vol/save/", FSC_PRIORITY_BASE);
        return ACPStatus::SUCCESS;
    }

	ACPStatus ACPUpdateSaveTimeStamp(uint32 persistentId, uint64 titleId, ACPDeviceType deviceType)
	{
		nnResult r = iosu::acp::ACPUpdateSaveTimeStamp(persistentId, titleId, deviceType);
		return ACPStatus::SUCCESS;
	}

	ACPStatus ACPCheckApplicationDeviceEmulation(uint32be* isEmulated)
	{
		*isEmulated = 0;
		return ACPStatus::SUCCESS;
	}

	ACPStatus ACPCreateSaveDir(uint32 persistentId, ACPDeviceType type)
	{
		nnResult result = iosu::acp::ACPCreateSaveDir(persistentId, type);
		return _ACPConvertResultToACPStatus(&result);
	}

	nnResult ACPCreateSaveDirEx(uint8 accountSlot, uint64 titleId)
	{
		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_CREATE_SAVE_DIR_EX;
		acpRequest->accountSlot = accountSlot;
		acpRequest->titleId = titleId;

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);
		
		nnResult result = BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_ACP, 0);
		return result;
	}

	void nnACPExport_ACPCreateSaveDirEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU8(accountSlot, 0);
		ppcDefineParamU64(titleId, 2); // index 2 because of alignment -> guessed parameters
		nnResult result = ACPCreateSaveDirEx(accountSlot, titleId);
		osLib_returnFromFunction(hCPU, _ACPConvertResultToACPStatus(&result));
	}

	void export_ACPGetSaveDataTitleIdList(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(uknType, 0);
		ppcDefineParamStructPtr(titleIdList, acpTitleId_t, 1);
		ppcDefineParamU32(maxCount, 2);
		ppcDefineParamU32BEPtr(count, 3);

		if (uknType != 3)
			assert_dbg();

		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_GET_SAVE_DATA_TITLE_ID_LIST;
		acpRequest->ptr = titleIdList;
		acpRequest->maxCount = maxCount;
		acpRequest->type = uknType;

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);

		*count = acpRequest->resultU32.u32;

		osLib_returnFromFunction(hCPU, acpRequest->returnCode);
	}

	void export_ACPGetTitleSaveMetaXml(PPCInterpreter_t* hCPU)
	{
		// r3/r4 = titleId
		// r5 = pointer
		// r6 = 3 (probably some sort of type? Same as in ACPGetSaveDataTitleIdList?)
		ppcDefineParamU64(titleId, 0);
		ppcDefineParamStructPtr(acpMetaXml, acpMetaXml_t, 2);
		ppcDefineParamU32(uknR6, 3);

		if (uknR6 != 3)
			assert_dbg();

		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_GET_TITLE_SAVE_META_XML;
		acpRequest->ptr = acpMetaXml;
		acpRequest->titleId = titleId;
		acpRequest->type = uknR6;

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);

		osLib_returnFromFunction(hCPU, acpRequest->returnCode);
	}

	static_assert(sizeof(acpSaveDirInfo_t) == 0x80, "acpSaveDirInfo_t has invalid size");

	void export_ACPGetTitleSaveDirEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU64(titleId, 0);
		ppcDefineParamU32(uknR5, 2); // storage device id?
		ppcDefineParamU32(uknR6, 3); // ukn
		ppcDefineParamStructPtr(saveDirInfoOut, acpSaveDirInfo_t, 4);
		ppcDefineParamU32(uknR8, 5); // max count?
		ppcDefineParamU32BEPtr(countOut, 6);

		if (uknR5 != 3)
			assert_dbg();
		if (uknR6 != 0)
			assert_dbg();

		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_GET_TITLE_SAVE_DIR;
		acpRequest->ptr = saveDirInfoOut;
		acpRequest->titleId = titleId;
		acpRequest->type = uknR5;
		acpRequest->maxCount = uknR8;

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);
		*countOut = acpRequest->resultU32.u32;

		osLib_returnFromFunction(hCPU, acpRequest->returnCode);
	}

	void export_ACPCheckTitleNotReferAccountLaunch(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU64(titleId, 0);
		cemuLog_logDebug(LogType::Force, "ACPCheckTitleNotReferAccountLaunch(): Placeholder");

		osLib_returnFromFunction(hCPU, 0);
	}

	void export_ACPGetLaunchMetaData(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamStructPtr(acpMetaData, acpMetaData_t, 0);
		
		cemuLog_logDebug(LogType::Force, "ACPGetLaunchMetaData(): Placeholder");

		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_GET_TITLE_META_DATA;
		acpRequest->ptr = acpMetaData;
		acpRequest->titleId = CafeSystem::GetForegroundTitleId();

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);

		osLib_returnFromFunction(hCPU, acpRequest->returnCode);
	}

	void export_ACPGetLaunchMetaXml(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamStructPtr(acpMetaXml, acpMetaXml_t, 0);

		cemuLog_logDebug(LogType::Force, "ACPGetLaunchMetaXml(): Placeholder");

		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_GET_TITLE_META_XML;
		acpRequest->ptr = acpMetaXml;
		acpRequest->titleId = CafeSystem::GetForegroundTitleId();

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);

		osLib_returnFromFunction(hCPU, acpRequest->returnCode);
	}

	static uint64 _GetMainApplicationTitleId()
	{
		uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
		if (((currentTitleId >> 32) & 0xFF) != 0x30)
			return currentTitleId;
		// applets (incl. the HOME Menu): the main application they were launched over
		uint64 mainAppTitleId = coreinit::__OSGetMainAppTitleId();
		if (mainAppTitleId != 0)
			return mainAppTitleId;
		return _SYSGetSystemApplicationTitleId(0); // Wii U Menu
	}

	void export_ACPGetTitleIdOfMainApplication(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamTypePtr(titleId, uint64be, 0);
		*titleId = _GetMainApplicationTitleId();
		osLib_returnFromFunction(hCPU, 0);
	}

	// used by the HOME Menu. Three output pointers: title id, ACP device type (1 = internal), UPID (assumed)
	void export_ACPGetTitleInfoOfMainApplication(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamTypePtr(titleId, uint64be, 0);
		ppcDefineParamTypePtr(outUkn1, uint32be, 1);
		ppcDefineParamTypePtr(outUkn2, uint32be, 2);
		uint64 mainTitleId = _GetMainApplicationTitleId();
		if (titleId)
			*titleId = mainTitleId;
		if (outUkn1)
			*outUkn1 = 1; // device type, the HOME Menu passes 1 for titles on internal storage
		// third output assumed to be the main app's UPID: 2 (Wii U Menu) makes the HOME Menu show "You have no suspended
		// software". Returning no title and an error code instead didn't work (it showed an empty suspended software box)
		sint32 upid = _SYSGetUPIDFromTitleId(mainTitleId);
		if (outUkn2)
			*outUkn2 = upid >= 0 ? (uint32)upid : 15;
		cemuLog_log(LogType::Force, "ACPGetTitleInfoOfMainApplication -> {:016x}, device 1, upid {} (out 0x{:08x} 0x{:08x} 0x{:08x})", mainTitleId, upid, hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
		osLib_returnFromFunction(hCPU, 0);
	}

	// ACPGetTitleMetaDir(titleId, path, pathSize). Used by the Electronic Manual, which then binds the folder with FSBindMount
	void export_ACPGetTitleMetaDir(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU64(titleId, 0);
		ppcDefineParamStr(path, 2);
		ppcDefineParamU32(pathSize, 3);
		uint32 titleType = (uint32)(titleId >> 32) & 0xFF;
		const char* storage = (titleType == 0x10 || titleType == 0x30 || titleType == 0x1B) ? "sys" : "usr"; // system apps, applets, system data
		std::string metaDir = fmt::format("/vol/storage_mlc01/{}/title/{:08x}/{:08x}/meta", storage, (uint32)(titleId >> 32), (uint32)(titleId & 0xFFFFFFFF));
		if (titleType == 0x10 && ((titleId >> 12) & 0xFFFF) == 0x40 && !fsc_doesFileExist((metaDir + "/Manual.bfma").c_str()))
		{
			// the Wii U Menu has no manual of its own. The Wii U console manual (WUP-N-HASE-xxx.bfma) is in the system data
			// title 0005001B-10059x00 (x = region digit of the menu). Its meta.xml has no title name, which the Electronic
			// Manual requires, so it keeps getting the Wii U Menu's title id and only its manual folder is redirected here
			uint64 systemManualTitleId = 0x0005001B10059000ULL | (titleId & 0xF00);
			std::string systemManualDir = fmt::format("/vol/storage_mlc01/sys/title/{:08x}/{:08x}/meta", (uint32)(systemManualTitleId >> 32), (uint32)systemManualTitleId);
			if (fsc_doesDirectoryExist(systemManualDir.c_str()))
			{
				cemuLog_log(LogType::Force, "ACPGetTitleMetaDir: Wii U Menu has no Manual.bfma, using the system manual {:016x}", systemManualTitleId);
				metaDir = systemManualDir;
			}
			else
				cemuLog_log(LogType::Force, "ACPGetTitleMetaDir: Wii U Menu has no Manual.bfma and the system manual {:016x} is not installed", systemManualTitleId);
		}
		cemuLog_log(LogType::Force, "ACPGetTitleMetaDir({:016x}) -> \"{}\"", titleId, metaDir);
		if (path == nullptr || pathSize <= metaDir.size())
		{
			osLib_returnFromFunction(hCPU, (uint32)-1);
			return;
		}
		strcpy(path, metaDir.c_str());
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_ACPGetTitleMetaDirByDevice(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU64(titleId, 0);
		ppcDefineParamStr(path, 2);
		ppcDefineParamU32(pathSize, 3);
		ppcDefineParamU32(deviceId, 4);

		if (deviceId != 3)
			cemuLog_logOnce(LogType::Force, "ACPGetTitleMetaDirByDevice: deviceId {} treated as internal storage", deviceId);

		if (((titleId >> 32) & 0xFF) == 0x10 || ((titleId >> 32) & 0xFF) == 0x30)
		{
			sprintf(path, "/vol/storage_mlc01/sys/title/%08x/%08x/meta", (uint32)(titleId>>32), (uint32)(titleId&0xFFFFFFFF));
		}
		else
		{
			sprintf(path, "/vol/storage_mlc01/usr/title/%08x/%08x/meta", (uint32)(titleId >> 32), (uint32)(titleId & 0xFFFFFFFF));
		}

		osLib_returnFromFunction(hCPU, 0);
	}

	void export_ACPGetTitleMetaXmlByDevice(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU64(titleId, 0);
		ppcDefineParamStructPtr(acpMetaXml, acpMetaXml_t, 2);
		ppcDefineParamU32(deviceId, 3);

		if (deviceId != 3)
			cemuLog_logDebug(LogType::Force, "ACPGetTitleMetaXmlByDevice(): Unsupported deviceId");

		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_GET_TITLE_META_XML;
		acpRequest->ptr = acpMetaXml;
		acpRequest->titleId = titleId;//CafeSystem::GetForegroundTitleId();

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);

		osLib_returnFromFunction(hCPU, acpRequest->returnCode);
	}

	uint32 ACPGetTitleMetaXml(uint64 titleId, acpMetaXml_t* acpMetaXml)
	{
		acpPrepareRequest();
		acpRequest->requestCode = IOSU_ACP_GET_TITLE_META_XML;
		acpRequest->ptr = acpMetaXml;
		acpRequest->titleId = titleId;

		__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, acpBufferVector);

		return acpRequest->returnCode;
	}

	void export_ACPIsOverAgeEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(age, 0);
		ppcDefineParamU8(slot, 1);

		bool isOverAge = true;
		osLib_returnFromFunction(hCPU, isOverAge ? 1 : 0);
	}

	void export_ACPGetNetworkTime(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32BEPtr(timestamp64, 0);
		ppcDefineParamU32BEPtr(ukn, 1); // probably timezone or offset? Could also be a bool for success/failed

		uint64 t = coreinit::OSGetTime() + (uint64)((sint64)(ppcCyclesSince2000_UTC - ppcCyclesSince2000) / 20LL);

		timestamp64[0] = (uint32)(t >> 32);
		timestamp64[1] = (uint32)(t & 0xFFFFFFFF);

		*ukn = 1; // E-Shop doesnt want this to be zero (games also check for it)

		osLib_returnFromFunction(hCPU, 0); // error code
	}

	void export_ACPConvertNetworkTimeToOSCalendarTime(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU64(networkTime, 0);
		ppcDefineParamStructPtr(calendarTime, coreinit::OSCalendarTime_t, 2);

		coreinit::OSTicksToCalendarTime(networkTime, calendarTime);

		osLib_returnFromFunction(hCPU, 0);
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "nn_acp";
		}

		void RPLMapped() override
		{
			cafeExportRegister("nn_acp", ACPCheckApplicationDeviceEmulation, LogType::Placeholder);

			osLib_addFunction("nn_acp", "ACPCreateSaveDirEx", nnACPExport_ACPCreateSaveDirEx);
			cafeExportRegister("nn_acp", ACPUpdateSaveTimeStamp, LogType::Placeholder);

			osLib_addFunction("nn_acp", "ACPGetSaveDataTitleIdList", export_ACPGetSaveDataTitleIdList);
			osLib_addFunction("nn_acp", "ACPGetTitleSaveMetaXml", export_ACPGetTitleSaveMetaXml);
			osLib_addFunction("nn_acp", "ACPGetTitleSaveDirEx", export_ACPGetTitleSaveDirEx);

			osLib_addFunction("nn_acp", "ACPCheckTitleNotReferAccountLaunch", export_ACPCheckTitleNotReferAccountLaunch);
			osLib_addFunction("nn_acp", "ACPCheckTitleLaunchByTitleListType", export_ACPCheckTitleLaunchByTitleListType);
			osLib_addFunction("nn_acp", "ACPGetLaunchMetaData", export_ACPGetLaunchMetaData);
			osLib_addFunction("nn_acp", "ACPGetLaunchMetaXml", export_ACPGetLaunchMetaXml);
			osLib_addFunction("nn_acp", "ACPGetTitleIdOfMainApplication", export_ACPGetTitleIdOfMainApplication);
			osLib_addFunction("nn_acp", "ACPGetTitleInfoOfMainApplication", export_ACPGetTitleInfoOfMainApplication);

			osLib_addFunction("nn_acp", "ACPGetTitleMetaDirByDevice", export_ACPGetTitleMetaDirByDevice);
			osLib_addFunction("nn_acp", "ACPGetTitleMetaDir", export_ACPGetTitleMetaDir);
			osLib_addFunction("nn_acp", "ACPGetTitleMetaXmlByDevice", export_ACPGetTitleMetaXmlByDevice);
			cafeExportRegister("nn_acp", ACPGetTitleMetaXml, LogType::Placeholder);

			cafeExportRegister("nn_acp", ACPGetApplicationBox, LogType::Placeholder);

			cafeExportRegister("nn_acp", ACPGetOlvAccesskey, LogType::Placeholder);

			osLib_addFunction("nn_acp", "ACPIsOverAgeEx", export_ACPIsOverAgeEx);

			osLib_addFunction("nn_acp", "ACPGetNetworkTime", export_ACPGetNetworkTime);
			osLib_addFunction("nn_acp", "ACPConvertNetworkTimeToOSCalendarTime", export_ACPConvertNetworkTimeToOSCalendarTime);
		};

	}s_COSnnAcpModule;

	COSModule* GetModule()
	{
		return &s_COSnnAcpModule;
	}
}
}
