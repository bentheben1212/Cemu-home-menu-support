#include "Common/CafeStaticVar.h"
#include "fsa_types.h"
#include "iosu_fsa.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/Filesystem/fsc.h"
#include "util/helpers/helpers.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"

#include "Cafe/OS/libs/coreinit/coreinit_FS.h"	 // get rid of this dependency, requires reworking some of the IPC stuff. See locations where we use coreinit::FSCmdBlockBody_t
#include "Cafe/HW/Latte/Core/LatteBufferCache.h" // also remove this dependency

using namespace iosu::kernel;

namespace iosu
{
	namespace fsa
	{
		IOSMsgQueueId sFSAIoMsgQueue;
		constinit IOSUStaticData s_fsaStaticData;
		CafeStaticVar<iosu::kernel::IOSMessage, 352> _m_sFSAIoMsgQueueMsgBuffer{s_fsaStaticData};
		std::thread sFSAIoThread;

		struct FSAClient // IOSU's counterpart to the coreinit FSClient struct
		{
			std::string workingDirectory;
			bool isAllocated{false};

			void AllocateAndInitialize()
			{
				isAllocated = true;
				workingDirectory = std::string("/");
			}

			void ReleaseAndCleanup()
			{
				isAllocated = false;
			}
		};

		std::array<FSAClient, 624> sFSAClientArray;

		IOS_ERROR FSAAllocateClient(sint32& indexOut)
		{
			for (size_t i = 0; i < sFSAClientArray.size(); i++)
			{
				if (sFSAClientArray[i].isAllocated)
					continue;
				sFSAClientArray[i].AllocateAndInitialize();
				indexOut = (sint32)i;
				return IOS_ERROR_OK;
			}
			return (IOS_ERROR)0xFFFCFFEE;
		}

		FSA_RESULT FSA_convertFSCtoFSAStatus(sint32 fscError)
		{
			if (fscError == FSC_STATUS_OK)
				return FSA_RESULT::OK;
			else if (fscError == FSC_STATUS_FILE_NOT_FOUND)
				return FSA_RESULT::NOT_FOUND;
			else if (fscError == FSC_STATUS_ALREADY_EXISTS)
				return FSA_RESULT::ALREADY_EXISTS;
			cemu_assert_unimplemented();
			return FSA_RESULT::FATAL_ERROR;
		}

		std::string __FSATranslatePath(FSAClient* fsaClient, std::string_view input, bool endWithSlash = false)
		{
			std::string tmp;
			if (input.empty())
			{
				tmp.assign(fsaClient->workingDirectory);
				if (endWithSlash)
					tmp.push_back('/');
				return tmp;
			}
			char c = input.front();
			cemu_assert_debug(c != '\\'); // how to handle backward slashes?

			if (c == '/')
			{
				// absolute path
				tmp.assign("/"); // keep the leading slash
			}
			else if (c == '~')
			{
				cemu_assert_debug(false);
				input.remove_prefix(1);
				tmp.assign("/");
			}
			else
			{
				// in all other cases the path is relative to the working directory
				tmp.assign(fsaClient->workingDirectory);
			}
			// parse path
			size_t idx = 0;
			while (idx < input.size())
			{
				c = input[idx];
				if (c == '/')
				{
					idx++;
					continue; // filter leading and repeated slashes
				}

				if (c == '.')
				{
					if ((input.size() - idx) >= 3 && input[idx + 1] == '.' && input[idx + 2] == '/')
					{
						// "../"
                        while(!tmp.empty())
                        {
                            if(tmp.back() == '/')
                            {
                                tmp.pop_back();
                                break;
                            }
                            tmp.pop_back();
                        }
						idx += 3;
						continue;
					}
					else if ((input.size() - idx) == 2 && input[idx + 1] == '.')
					{
						// ".." at the end
                        while(!tmp.empty())
                        {
                            if(tmp.back() == '/')
                            {
                                tmp.pop_back();
                                break;
                            }
                            tmp.pop_back();
                        }
						idx += 2;
						continue;
					}
					else if ((input.size() - idx) >= 2 && input[idx + 1] == '/')
					{
						// "./"
						idx += 2;
						continue;
					}
				}
				if (!tmp.empty() && tmp.back() != '/')
					tmp.push_back('/');

				while (idx < input.size())
				{
					c = input[idx];
					if (c == '/')
						break;
					tmp.push_back(c);
					idx++;
				}
			}
			if (endWithSlash)
			{
				if (tmp.empty() || tmp.back() != '/')
					tmp.push_back('/');
			}
			return tmp;
		}

		static std::unordered_map<uint32, std::string> s_dirHandlePaths; // open directory handle -> translated path (for quota directory sizes)

		FSCVirtualFile* __FSAOpenNode(FSAClient* client, std::string_view path, FSC_ACCESS_FLAG accessFlags, sint32& fscStatus)
		{
			std::string translatedPath = __FSATranslatePath(client, path);
			return fsc_open(translatedPath.c_str(), accessFlags, &fscStatus);
		}

		class _FSAHandleTable {
			struct _FSAHandleResource
			{
				bool isAllocated{false};
				FSCVirtualFile* fscFile;
				uint16 handleCheckValue;
			};

		public:
			FSA_RESULT AllocateHandle(FSResHandle& handleOut, FSCVirtualFile* fscFile)
			{
				for (size_t i = 0; i < m_handleTable.size(); i++)
				{
					auto& it = m_handleTable.at(i);
					if (it.isAllocated)
						continue;
					uint16 checkValue = (uint16)m_currentCounter;
					m_currentCounter++;
					it.handleCheckValue = checkValue;
					it.fscFile = fscFile;
					it.isAllocated = true;
					uint32 handleVal = ((uint32)i << 16) | (uint32)checkValue;
					handleOut = (FSResHandle)handleVal;
					return FSA_RESULT::OK;
				}
				cemuLog_log(LogType::Force, "FSA: Ran out of file handles");
				return FSA_RESULT::FATAL_ERROR;
			}

			FSA_RESULT ReleaseHandle(FSResHandle handle)
			{
				uint16 index = (uint16)((uint32)handle >> 16);
				uint16 checkValue = (uint16)(handle & 0xFFFF);
				if (index >= m_handleTable.size())
					return FSA_RESULT::INVALID_FILE_HANDLE;
				auto& it = m_handleTable.at(index);
				if (!it.isAllocated)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				if (it.handleCheckValue != checkValue)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				it.fscFile = nullptr;
				it.isAllocated = false;
				return FSA_RESULT::OK;
			}

			FSCVirtualFile* GetByHandle(FSResHandle handle)
			{
				uint16 index = (uint16)((uint32)handle >> 16);
				uint16 checkValue = (uint16)(handle & 0xFFFF);
				if (index >= m_handleTable.size())
					return nullptr;
				auto& it = m_handleTable.at(index);
				if (!it.isAllocated)
					return nullptr;
				if (it.handleCheckValue != checkValue)
					return nullptr;
				return it.fscFile;
			}

		private:
			uint32 m_currentCounter = 1;
			std::array<_FSAHandleResource, 0x3C0> m_handleTable;
		};

		_FSAHandleTable sFileHandleTable;
		_FSAHandleTable sDirHandleTable;

		// Failed opens of title meta files (icons, meta.xml, ...) in the virtual MLC storage are always logged, they
		// show titles that are missing from /vol/storage_mlc01/. Other failed opens are logged with "File access" logging
		static void _FSALogOpenFailure(const char* type, std::string_view path)
		{
			bool isTitleMeta = path.starts_with("/vol/storage_mlc01/") && path.find("/meta") != std::string_view::npos;
			if (isTitleMeta)
				cemuLog_log(LogType::Force, "FSA: Open {} {} failed (not found)", type, path);
			else
				cemuLog_log(LogType::CoreinitFile, "Open {} {} (result: not found)", type, path);
		}

		FSA_RESULT __FSAOpenFile(FSAClient* client, const char* path, const char* accessModifierStr, sint32* fileHandle)
		{
			*fileHandle = FS_INVALID_HANDLE_VALUE;
			FSC_ACCESS_FLAG accessModifier = FSC_ACCESS_FLAG::NONE;
			bool truncateFile = false; // todo: Support for this
			bool isAppend = false;	   // todo: proper support for this (all write operations should move cursor to the end of the file?)
			if (strcmp(accessModifierStr, "r") == 0 || strcmp(accessModifierStr, "rb") == 0)
				accessModifier = FSC_ACCESS_FLAG::READ_PERMISSION;
			else if (strcmp(accessModifierStr, "r+") == 0)
			{
				// the cursor will be set to the beginning of the file
				// allows read and write access
				accessModifier = FSC_ACCESS_FLAG::READ_PERMISSION | FSC_ACCESS_FLAG::WRITE_PERMISSION; // read, write
			}
			else if (strcmp(accessModifierStr, "w") == 0)
			{
				accessModifier = FSC_ACCESS_FLAG::READ_PERMISSION | FSC_ACCESS_FLAG::WRITE_PERMISSION | FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE; // create new file & write
				truncateFile = true;
			}
			else if (strcmp(accessModifierStr, "w+") == 0)
			{
				accessModifier = FSC_ACCESS_FLAG::READ_PERMISSION | FSC_ACCESS_FLAG::WRITE_PERMISSION | FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE; // create new file & read & write
				truncateFile = true;
			}
			else if (strcmp(accessModifierStr, "wb") == 0) // used in Super Meat Boy
			{
				// b flag is allowed has no effect
				accessModifier = FSC_ACCESS_FLAG::READ_PERMISSION | FSC_ACCESS_FLAG::WRITE_PERMISSION | FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE; // create new file & write
				truncateFile = true;
			}
			else if (strcmp(accessModifierStr, "a+") == 0)
			{
				accessModifier = FSC_ACCESS_FLAG::READ_PERMISSION | FSC_ACCESS_FLAG::WRITE_PERMISSION | FSC_ACCESS_FLAG::FILE_ALLOW_CREATE | FSC_ACCESS_FLAG::IS_APPEND;
				isAppend = true;
			}
			else if (strcmp(accessModifierStr, "a") == 0)
			{
				accessModifier = FSC_ACCESS_FLAG::WRITE_PERMISSION | FSC_ACCESS_FLAG::FILE_ALLOW_CREATE | FSC_ACCESS_FLAG::IS_APPEND;
				isAppend = true;
			}
			else
				cemu_assert_debug(false);

			accessModifier |= FSC_ACCESS_FLAG::OPEN_DIR | FSC_ACCESS_FLAG::OPEN_FILE;
			sint32 fscStatus;
			FSCVirtualFile* fscFile = __FSAOpenNode(client, path, accessModifier, fscStatus);
			if (!fscFile)
			{
				_FSALogOpenFailure("file", path);
				return FSA_RESULT::NOT_FOUND;
			}
			if (fscFile->fscGetType() != FSC_TYPE_FILE)
			{
				delete fscFile;
				return FSA_RESULT::NOT_FILE;
			}
			if (isAppend)
				fsc_setFileSeek(fscFile, fsc_getFileSize(fscFile));
			FSResHandle fsFileHandle;
			FSA_RESULT r = sFileHandleTable.AllocateHandle(fsFileHandle, fscFile);
			if (r != FSA_RESULT::OK)
			{
				cemuLog_log(LogType::Force, "Exceeded maximum number of FSA file handles");
				delete fscFile;
				return FSA_RESULT::MAX_FILES;
			}
			*fileHandle = fsFileHandle;
			cemuLog_log(LogType::CoreinitFile, "Open file {} (access: {} result: ok handle: 0x{})", path, accessModifierStr, (uint32)*fileHandle);
			return FSA_RESULT::OK;
		}

		FSA_RESULT __FSAOpenDirectory(FSAClient* client, std::string_view path, sint32* dirHandle)
		{
			*dirHandle = FS_INVALID_HANDLE_VALUE;
			sint32 fscStatus;
			FSCVirtualFile* fscFile = __FSAOpenNode(client, path, FSC_ACCESS_FLAG::OPEN_DIR | FSC_ACCESS_FLAG::OPEN_FILE, fscStatus);
			if (!fscFile)
			{
				_FSALogOpenFailure("directory", path);
				return FSA_RESULT::NOT_FOUND;
			}
			if (fscFile->fscGetType() != FSC_TYPE_DIRECTORY)
			{
				delete fscFile;
				return FSA_RESULT::NOT_DIR;
			}
			FSResHandle fsDirHandle;
			FSA_RESULT r = sDirHandleTable.AllocateHandle(fsDirHandle, fscFile);
			if (r != FSA_RESULT::OK)
			{
				delete fscFile;
				return FSA_RESULT::MAX_DIRS;
			}
			*dirHandle = fsDirHandle;
			{
				std::string dirPath = __FSATranslatePath(client, path);
				while (dirPath.size() > 1 && dirPath.back() == '/')
					dirPath.pop_back();
				s_dirHandlePaths[(uint32)fsDirHandle] = dirPath;
			}
			cemuLog_log(LogType::CoreinitFile, "Open directory {} (result: ok handle: 0x{})", path, (uint32)*dirHandle);
			return FSA_RESULT::OK;
		}

		FSA_RESULT __FSACloseFile(uint32 fileHandle)
		{
			uint8 handleType = 0;
			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
			{
				cemuLog_logDebug(LogType::Force, "__FSACloseFile(): Invalid handle (0x{:08x})", fileHandle);
				return FSA_RESULT::INVALID_FILE_HANDLE;
			}
			// unregister file
			sFileHandleTable.ReleaseHandle(fileHandle); // todo - use the error code of this
			fsc_close(fscFile);
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_remove(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			std::string path = __FSATranslatePath(client, (char*)shimBuffer->request.cmdRemove.path);
			sint32 fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			fsc_remove(path.c_str(), &fscStatus);
			return FSA_convertFSCtoFSAStatus(fscStatus);
		}

		FSA_RESULT FSAProcessCmd_makeDir(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			std::string path = __FSATranslatePath(client, (char*)shimBuffer->request.cmdMakeDir.path);
			sint32 fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			fsc_createDir(path.c_str(), &fscStatus);
			return FSA_convertFSCtoFSAStatus(fscStatus);
		}

		FSA_RESULT FSAProcessCmd_rename(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			std::string srcPath = __FSATranslatePath(client, (char*)shimBuffer->request.cmdRename.srcPath);
			std::string dstPath = __FSATranslatePath(client, (char*)shimBuffer->request.cmdRename.dstPath);
			sint32 fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			fsc_rename(srcPath.c_str(), dstPath.c_str(), &fscStatus);
			return FSA_convertFSCtoFSAStatus(fscStatus);
		}


		/* Storage sizes (Data Management, free space) */

		// A 32GB console has about 25GB of system memory available to the user. Free space is this minus what is
		// stored in mlc01/usr and, unless CEMU_STORAGE_EXCLUDE_GAME_PATHS=1 is set, the titles from Cemu's game paths
		constexpr uint64 STORAGE_MLC_TOTAL_SIZE = 32ull * 1000 * 1000 * 1000;
		constexpr uint64 STORAGE_MLC_USABLE_SIZE = 25ull * 1024 * 1024 * 1024;
		constexpr sint64 STORAGE_CACHE_LIFETIME_MS = 30 * 1000;

		static bool _StorageExcludeGamePaths()
		{
			static const bool s_exclude = []() {
				const char* v = getenv("CEMU_STORAGE_EXCLUDE_GAME_PATHS");
				return v && *v && strcmp(v, "0") != 0;
			}();
			return s_exclude;
		}

		static sint64 _StorageNowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// total size of all files below a virtual (fsc) directory
		static uint64 _FSCDirectoryTotalSize(const std::string& path, sint32 depth = 0)
		{
			if (depth > 32)
				return 0;
			sint32 fscStatus;
			FSCVirtualFile* dir = fsc_openDirIterator(path.c_str(), &fscStatus);
			if (!dir)
				return 0;
			uint64 total = 0;
			FSCDirEntry entry;
			while (fsc_nextDir(dir, &entry))
			{
				if (entry.isFile)
					total += entry.fileSize;
				else if (entry.isDirectory)
					total += _FSCDirectoryTotalSize(fmt::format("{}/{}", path, entry.GetPath()), depth + 1);
			}
			fsc_close(dir);
			return total;
		}

		// title and save folders are quota directories on a real console and report their size:
		// /vol/storage_mlc01/<usr|sys>/title/<hi>/<lo> and /vol/storage_mlc01/usr/save/<hi>/<lo>
		static bool _IsQuotaDirectory(std::string_view path)
		{
			while (path.size() > 1 && path.back() == '/')
				path.remove_suffix(1);
			auto isHex8 = [](std::string_view v) { return v.size() == 8 && std::all_of(v.begin(), v.end(), [](char c) { return isxdigit((unsigned char)c) != 0; }); };
			for (std::string_view prefix : { std::string_view("/vol/storage_mlc01/usr/title/"), std::string_view("/vol/storage_mlc01/sys/title/"), std::string_view("/vol/storage_mlc01/usr/save/") })
			{
				if (!path.starts_with(prefix))
					continue;
				std::string_view rest = path.substr(prefix.size());
				if (rest.size() != 17 || rest[8] != '/')
					return false;
				return isHex8(rest.substr(0, 8)) && isHex8(rest.substr(9, 8));
			}
			return false;
		}

		static std::mutex s_storageSizeMutex;
		static std::unordered_map<std::string, std::pair<uint64, sint64>> s_quotaDirSizeCache; // path -> (size, time)

		static uint64 _GetQuotaDirectorySize(std::string path)
		{
			while (path.size() > 1 && path.back() == '/')
				path.pop_back();
			std::transform(path.begin(), path.end(), path.begin(), [](char c) { return (char)tolower((unsigned char)c); });
			std::unique_lock _l(s_storageSizeMutex);
			auto it = s_quotaDirSizeCache.find(path);
			if (it != s_quotaDirSizeCache.end() && _StorageNowMs() - it->second.second < STORAGE_CACHE_LIFETIME_MS)
				return it->second.first;
			uint64 size = _FSCDirectoryTotalSize(path);
			s_quotaDirSizeCache[path] = { size, _StorageNowMs() };
			cemuLog_log(LogType::CoreinitFile, "Storage: size of {} = {} bytes", path, size);
			return size;
		}

		static uint64 _HostDirectoryTotalSize(const fs::path& path)
		{
			std::error_code ec;
			uint64 total = 0;
			for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				std::error_code ec2;
				if (it->is_regular_file(ec2))
					total += it->file_size(ec2);
			}
			return total;
		}

		static uint64 s_storageUsedCache = 0;
		static sint64 s_storageUsedCacheTime = -STORAGE_CACHE_LIFETIME_MS;

		// space used in the console's system memory: everything in mlc01/usr plus (optionally) titles from the game paths
		static uint64 _GetMlcUsedSpace()
		{
			{
				std::unique_lock _l(s_storageSizeMutex);
				if (_StorageNowMs() - s_storageUsedCacheTime < STORAGE_CACHE_LIFETIME_MS)
					return s_storageUsedCache;
			}
			uint64 mlcUsed = _HostDirectoryTotalSize(ActiveSettings::GetMlcPath("usr"));
			uint64 gamePathsUsed = 0;
			uint32 gamePathTitles = 0;
			if (!_StorageExcludeGamePaths())
			{
				CafeSystem::MlcStorageMountAllTitles();
				const fs::path mlcRoot = ActiveSettings::GetMlcPath().lexically_normal();
				std::set<uint64> counted;
				auto titleList = CafeTitleList::AcquireInternalList();
				std::vector<uint64> gamePathTitleIds;
				for (auto& it : titleList)
				{
					// titles stored in mlc01 are already counted above
					std::string titlePath = _pathToUtf8(it->GetPath().lexically_normal());
					if (titlePath.starts_with(_pathToUtf8(mlcRoot)))
						continue;
					if (TitleIdParser(it->GetAppTitleId()).IsSystemTitle())
						continue;
					if (counted.emplace(it->GetAppTitleId()).second)
						gamePathTitleIds.emplace_back(it->GetAppTitleId());
				}
				CafeTitleList::ReleaseInternalList();
				for (uint64 titleId : gamePathTitleIds)
				{
					gamePathsUsed += _FSCDirectoryTotalSize(CafeSystem::GetMlcStoragePath(titleId));
					gamePathTitles++;
				}
			}
			uint64 used = mlcUsed + gamePathsUsed;
			cemuLog_log(LogType::Force, "Storage: mlc01/usr uses {} MiB, {} game path titles use {} MiB{}, free {} MiB of {} MiB",
				mlcUsed >> 20, gamePathTitles, gamePathsUsed >> 20, _StorageExcludeGamePaths() ? " (excluded, CEMU_STORAGE_EXCLUDE_GAME_PATHS)" : "",
				(used >= STORAGE_MLC_USABLE_SIZE ? 0 : STORAGE_MLC_USABLE_SIZE - used) >> 20, STORAGE_MLC_USABLE_SIZE >> 20);
			std::unique_lock _l(s_storageSizeMutex);
			s_storageUsedCache = used;
			s_storageUsedCacheTime = _StorageNowMs();
			return used;
		}

		static uint64 _GetMlcFreeSpace()
		{
			uint64 used = _GetMlcUsedSpace();
			return used >= STORAGE_MLC_USABLE_SIZE ? 0 : STORAGE_MLC_USABLE_SIZE - used; // never negative
		}

		// fill the size fields of a quota directory's stat
		static void _FSASetQuotaDirectoryStat(std::string_view translatedPath, FSStat_t* fsStatOut)
		{
			// the root of the MLC volume reports the usable capacity as its quota and the used space as its size.
			// Data Management takes the capacity from here (with 0 it shows the memory as full)
			std::string_view rootPath = translatedPath;
			while (rootPath.size() > 1 && rootPath.back() == '/')
				rootPath.remove_suffix(1);
			if (rootPath == "/vol/storage_mlc01")
			{
				uint64 used = std::min<uint64>(_GetMlcUsedSpace(), STORAGE_MLC_USABLE_SIZE);
				fsStatOut->size = (uint32)std::min<uint64>(used, 0xFFFFFFFFull);
				fsStatOut->allocatedSize = (uint32)std::min<uint64>(used, 0xFFFFFFFFull);
				fsStatOut->quotaSize = STORAGE_MLC_USABLE_SIZE;
				fsStatOut->flag = fsStatOut->flag | FSFlag::IS_QUOTA;
				return;
			}
			if (!_IsQuotaDirectory(translatedPath))
				return;
			uint64 size = _GetQuotaDirectorySize(std::string(translatedPath));
			fsStatOut->size = (uint32)std::min<uint64>(size, 0xFFFFFFFFull);
			fsStatOut->allocatedSize = (uint32)std::min<uint64>(size, 0xFFFFFFFFull);
			fsStatOut->quotaSize = size;
			fsStatOut->flag = fsStatOut->flag | FSFlag::IS_QUOTA;
		}

		static void _FSALogStat(const char* what, std::string_view path, FSStat_t* st)
		{
			cemuLog_log(LogType::CoreinitFile, "{} {} -> flags 0x{:08x} size {} allocated {} quota {}", what, path, (uint32)(FSFlag)st->flag, (uint32)st->size, (uint32)st->allocatedSize, (uint64)st->quotaSize);
		}

		bool __FSA_GetStatFromFSCFile(FSCVirtualFile* fscFile, FSStat_t* fsStatOut)
		{
			memset(fsStatOut, 0x00, sizeof(FSStat_t));
			FSFlag statFlag = FSFlag::NONE;
			if (fsc_isDirectory(fscFile))
			{
				// note: Only quota (save) directories have the size field set. For other directories it's zero.
				// Hyrule Warriors relies on the size field being zero for /vol/content/data/. Otherwise it will try to read it like a file and get stuck in an endless loop.
				// Art Academy reads the size for save directories
				statFlag |= FSFlag::IS_DIR;
				fsStatOut->size = 0;
			}
			else if (fsc_isFile(fscFile))
			{
				fsStatOut->size = fsc_getFileSize(fscFile);
                statFlag |= FSFlag::IS_FILE;
			}
			else
			{
				cemu_assert_suspicious();
			}
			fsStatOut->permissions = 0x777; // format matches unix (fstat) permissions?
			fsStatOut->flag = statFlag;
			return true;
		}

		FSA_RESULT __FSA_GetFileStat(FSAClient* client, const char* path, FSStat_t* fsStatOut)
		{
			sint32 fscStatus;
			FSCVirtualFile* fscFile = __FSAOpenNode(client, path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::OPEN_DIR, fscStatus);
			if (!fscFile)
			{
				cemuLog_log(LogType::CoreinitFile, "Stat {} -> not found", path);
				return FSA_convertFSCtoFSAStatus(fscStatus);
			}
			__FSA_GetStatFromFSCFile(fscFile, fsStatOut);
			bool isDir = fsc_isDirectory(fscFile);
			delete fscFile;
			if (isDir)
				_FSASetQuotaDirectoryStat(__FSATranslatePath(client, path), fsStatOut);
			_FSALogStat("Stat", path, fsStatOut);
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_queryInfo(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			char* path = (char*)shimBuffer->request.cmdQueryInfo.query;
			uint32 queryType = shimBuffer->request.cmdQueryInfo.queryType;
			// handle query
			sint32 fscStatus = FSC_STATUS_OK;
			if (queryType == FSA_QUERY_TYPE_STAT)
			{
				FSStat_t* fsStat = &shimBuffer->response.cmdQueryInfo.queryStat.stat;
				FSA_RESULT fsaStatus = __FSA_GetFileStat(client, path, fsStat);
				return fsaStatus;
			}
			else if (queryType == FSA_QUERY_TYPE_FREESPACE)
			{
				sint32 fscStatus;
				FSCVirtualFile* fscFile = __FSAOpenNode(client, path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::OPEN_DIR, fscStatus);
				if (!fscFile)
					return FSA_convertFSCtoFSAStatus(fscStatus);
				betype<uint64>* fsStatSize = &shimBuffer->response.cmdQueryInfo.queryFreeSpace.freespace;
				delete fscFile;
				*fsStatSize = _GetMlcFreeSpace();
				cemuLog_log(LogType::CoreinitFile, "QueryFreeSpace {} -> {} bytes", path, (uint64)*fsStatSize);
				return FSA_RESULT::OK;
			}
			else if (queryType == FSA_QUERY_TYPE_DEVICE_INFO)
			{
				FSADeviceInfo_t* deviceInfo = &shimBuffer->response.cmdQueryInfo.queryDeviceInfo.info;
				// always report hardcoded values for now.
				deviceInfo->deviceSectorSize = 512;
				deviceInfo->deviceSizeInSectors = STORAGE_MLC_TOTAL_SIZE / deviceInfo->deviceSectorSize; // 32GB model
				cemuLog_log(LogType::CoreinitFile, "QueryDeviceInfo {} -> {} sectors of {} bytes", path, (uint64)deviceInfo->deviceSizeInSectors, (uint32)deviceInfo->deviceSectorSize);
				return FSA_RESULT::OK;
			}
			else
			{
				cemuLog_log(LogType::Force, "FSA: QueryInfo type {} for {} is not implemented", queryType, path);
				cemu_assert_unimplemented();
			}
			return FSA_convertFSCtoFSAStatus(fscStatus);
		}

		FSA_RESULT FSAProcessCmd_getStatFile(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			FSFileHandle2 fileHandle = shimBuffer->request.cmdGetStatFile.fileHandle;
			FSStat_t* statOut = &shimBuffer->response.cmdStatFile.statOut;
			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
				return FSA_RESULT::NOT_FOUND;
			cemu_assert_debug(fsc_isFile(fscFile));
			__FSA_GetStatFromFSCFile(fscFile, statOut);
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_read(FSAClient* client, FSAShimBuffer* shimBuffer, MEMPTR<void> destPtr, uint32be transferSize)
		{
			uint32 transferElementSize = shimBuffer->request.cmdReadFile.size;
			uint32 filePos = shimBuffer->request.cmdReadFile.filePos;
			uint32 fileHandle = shimBuffer->request.cmdReadFile.fileHandle;
			uint32 flags = shimBuffer->request.cmdReadFile.flag;

			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_FILE_HANDLE;

			uint32 bytesToRead = transferSize;
			// update file position if flag is set
			if ((flags & FSA_CMD_FLAG_SET_POS) != 0)
				fsc_setFileSeek(fscFile, filePos);
			// todo: File permissions
			uint32 bytesSuccessfullyRead = fsc_readFile(fscFile, destPtr, bytesToRead);
			if (transferElementSize == 0)
				return FSA_RESULT::OK;

			LatteBufferCache_notifyDCFlush(destPtr.GetMPTR(), bytesToRead);

			return (FSA_RESULT)(bytesSuccessfullyRead / transferElementSize); // return number of elements read
		}

		FSA_RESULT FSAProcessCmd_write(FSAClient* client, FSAShimBuffer* shimBuffer, MEMPTR<void> destPtr, uint32be transferSize)
		{
			uint32 transferElementSize = shimBuffer->request.cmdWriteFile.size;
			uint32 filePos = shimBuffer->request.cmdWriteFile.filePos;
			uint32 fileHandle = shimBuffer->request.cmdWriteFile.fileHandle;
			uint32 flags = shimBuffer->request.cmdWriteFile.flag;

			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_FILE_HANDLE;
			cemu_assert_debug((transferSize % transferElementSize) == 0);
			uint32 bytesToWrite = transferSize;
			// check for write permission (should this happen before or after setting file position?)
			if (!fsc_isWritable(fscFile))
			{
				cemu_assert_debug(false);
				return FSA_RESULT::PERMISSION_ERROR;
			}
			// update file position if flag is set
			if ((flags & FSA_CMD_FLAG_SET_POS) != 0)
				fsc_setFileSeek(fscFile, filePos);
			uint32 bytesSuccessfullyWritten = fsc_writeFile(fscFile, destPtr, bytesToWrite);
			debug_printf("FSAProcessCmd_write(): Writing 0x%08x bytes (bytes actually written: 0x%08x)\n", bytesToWrite, bytesSuccessfullyWritten);
			return (FSA_RESULT)(bytesSuccessfullyWritten / transferElementSize); // return number of elements read
		}

		FSA_RESULT FSAProcessCmd_setPos(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			uint32 fileHandle = shimBuffer->request.cmdSetPosFile.fileHandle;
			uint32 filePos = shimBuffer->request.cmdSetPosFile.filePos;
			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_FILE_HANDLE;
			fsc_setFileSeek(fscFile, filePos);
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_getPos(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			uint32 fileHandle = shimBuffer->request.cmdGetPosFile.fileHandle;
			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_FILE_HANDLE;
			uint32 filePos = fsc_getFileSeek(fscFile);
			shimBuffer->response.cmdGetPosFile.filePos = filePos;
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_openFile(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			sint32 fileHandle = 0;
			FSA_RESULT fsaResult = __FSAOpenFile(client, (char*)shimBuffer->request.cmdOpenFile.path, (char*)shimBuffer->request.cmdOpenFile.mode, &fileHandle);
			shimBuffer->response.cmdOpenFile.fileHandleOutput = fileHandle;
			return fsaResult;
		}

		FSA_RESULT FSAProcessCmd_closeFile(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			return __FSACloseFile(shimBuffer->request.cmdCloseFile.fileHandle);
		}

		FSA_RESULT FSAProcessCmd_openDir(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			sint32 dirHandle = 0;
			FSA_RESULT fsaResult = __FSAOpenDirectory(client, (const char*)shimBuffer->request.cmdOpenFile.path, &dirHandle);
			shimBuffer->response.cmdOpenDir.dirHandleOutput = dirHandle;
			return fsaResult;
		}

		FSA_RESULT FSAProcessCmd_readDir(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			FSCVirtualFile* fscFile = sDirHandleTable.GetByHandle((sint32)shimBuffer->request.cmdReadDir.dirHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_DIR_HANDLE;
			FSDirEntry_t* dirEntryOut = &shimBuffer->response.cmdReadDir.dirEntry;
			FSCDirEntry fscDirEntry;
			if (fsc_nextDir(fscFile, &fscDirEntry) == false)
				return FSA_RESULT::END_OF_DIRECTORY;
			strcpy(dirEntryOut->name, fscDirEntry.path);
			FSFlag statFlag = FSFlag::NONE;
			dirEntryOut->stat.size = 0;
			if (fscDirEntry.isDirectory)
			{
				statFlag |= FSFlag::IS_DIR;
			}
			else if (fscDirEntry.isFile)
			{
				statFlag |= FSFlag::IS_FILE;
				dirEntryOut->stat.size = fscDirEntry.fileSize;
			}
			dirEntryOut->stat.flag = statFlag;
			dirEntryOut->stat.permissions = 0x777;
			if (fscDirEntry.isDirectory)
			{
				auto parent = s_dirHandlePaths.find((uint32)shimBuffer->request.cmdReadDir.dirHandle);
				if (parent != s_dirHandlePaths.end())
				{
					std::string childPath = fmt::format("{}/{}", parent->second, fscDirEntry.GetPath());
					_FSASetQuotaDirectoryStat(childPath, &dirEntryOut->stat);
				}
			}
			cemuLog_log(LogType::CoreinitFile, "ReadDir 0x{:08x} -> {} flags 0x{:08x} size {} quota {}", (uint32)shimBuffer->request.cmdReadDir.dirHandle, dirEntryOut->name, (uint32)(FSFlag)dirEntryOut->stat.flag, (uint32)dirEntryOut->stat.size, (uint64)dirEntryOut->stat.quotaSize);
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_closeDir(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			FSCVirtualFile* fscFile = sDirHandleTable.GetByHandle((sint32)shimBuffer->request.cmdReadDir.dirHandle);
			if (!fscFile)
			{
				cemuLog_logDebug(LogType::Force, "CloseDir: Invalid handle (0x{:08x})", (sint32)shimBuffer->request.cmdReadDir.dirHandle);
				return FSA_RESULT::INVALID_DIR_HANDLE;
			}
			sDirHandleTable.ReleaseHandle(shimBuffer->request.cmdReadDir.dirHandle);
			s_dirHandlePaths.erase((uint32)shimBuffer->request.cmdReadDir.dirHandle);
			fsc_close(fscFile);
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_flushQuota(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_rewindDir(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			FSCVirtualFile* fscFile = sDirHandleTable.GetByHandle((sint32)shimBuffer->request.cmdRewindDir.dirHandle);
			if (!fscFile)
			{
				cemuLog_logDebug(LogType::Force, "RewindDir: Invalid handle (0x{:08x})", (sint32)shimBuffer->request.cmdRewindDir.dirHandle);
				return FSA_RESULT::INVALID_DIR_HANDLE;
			}
			if (!fscFile->fscRewindDir())
				return FSA_RESULT::FATAL_ERROR;

			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_flushFile(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_appendFile(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(shimBuffer->request.cmdAppendFile.fileHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_FILE_HANDLE;
#ifdef CEMU_DEBUG_ASSERT
			cemuLog_log(LogType::Force, "FSAProcessCmd_appendFile(): size 0x{:08x} count 0x{:08x} (todo)\n", shimBuffer->request.cmdAppendFile.size, shimBuffer->request.cmdAppendFile.count);
#endif
			return (FSA_RESULT)(shimBuffer->request.cmdAppendFile.count.value());
		}

		FSA_RESULT FSAProcessCmd_truncateFile(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			FSFileHandle2 fileHandle = shimBuffer->request.cmdTruncateFile.fileHandle;
			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_FILE_HANDLE;
			fsc_setFileLength(fscFile, fsc_getFileSeek(fscFile));
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_isEof(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			uint32 fileHandle = shimBuffer->request.cmdIsEof.fileHandle;
			FSCVirtualFile* fscFile = sFileHandleTable.GetByHandle(fileHandle);
			if (!fscFile)
				return FSA_RESULT::INVALID_FILE_HANDLE;
			uint32 filePos = fsc_getFileSeek(fscFile);
			uint32 fileSize = fsc_getFileSize(fscFile);
			if (filePos >= fileSize)
				return FSA_RESULT::END_OF_FILE;
			return FSA_RESULT::OK;
		}

		FSA_RESULT FSAProcessCmd_getCwd(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			char* pathOutput = shimBuffer->response.cmdGetCWD.path;
			sint32 pathOutputMaxLen = sizeof(shimBuffer->response.cmdGetCWD.path);
			cemu_assert(pathOutputMaxLen > 0);
			sint32 fscStatus = FSC_STATUS_OK;
			strncpy(pathOutput, client->workingDirectory.data(), std::min(client->workingDirectory.size() + 1, (size_t)pathOutputMaxLen));
			pathOutput[pathOutputMaxLen - 1] = '\0';
			return FSA_convertFSCtoFSAStatus(fscStatus);
		}

		FSA_RESULT FSAProcessCmd_changeDir(FSAClient* client, FSAShimBuffer* shimBuffer)
		{
			const char* path = (const char*)shimBuffer->request.cmdChangeDir.path;
			shimBuffer->request.cmdChangeDir.path[sizeof(shimBuffer->request.cmdChangeDir.path) - 1] = '\0';
			sint32 fscStatus = FSC_STATUS_OK;
			client->workingDirectory.assign(__FSATranslatePath(client, path, true));
			return FSA_convertFSCtoFSAStatus(fscStatus);
		}

		void FSAHandleCommandIoctlv(FSAClient* client, IPCCommandBody* cmd, FSA_CMD_OPERATION_TYPE operationId, uint32 numIn, uint32 numOut, IPCIoctlVector* vec)
		{
			FSA_RESULT fsaResult = FSA_RESULT::FATAL_ERROR;

			switch (operationId)
			{
			case FSA_CMD_OPERATION_TYPE::READ:
			{
				fsaResult = FSAProcessCmd_read(client, (FSAShimBuffer*)vec[0].basePhys.GetPtr(), vec[1].basePhys, vec[1].size);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::WRITE:
			{
				fsaResult = FSAProcessCmd_write(client, (FSAShimBuffer*)vec[0].basePhys.GetPtr(), vec[1].basePhys, vec[1].size);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::CHANGEDIR:
			case FSA_CMD_OPERATION_TYPE::GETCWD:
			case FSA_CMD_OPERATION_TYPE::MAKEDIR:
			case FSA_CMD_OPERATION_TYPE::RENAME:
			case FSA_CMD_OPERATION_TYPE::OPENDIR:
			case FSA_CMD_OPERATION_TYPE::READDIR:
			case FSA_CMD_OPERATION_TYPE::CLOSEDIR:
			case FSA_CMD_OPERATION_TYPE::OPENFILE:
			case FSA_CMD_OPERATION_TYPE::REMOVE:
			case FSA_CMD_OPERATION_TYPE::GETPOS:
			case FSA_CMD_OPERATION_TYPE::SETPOS:
			case FSA_CMD_OPERATION_TYPE::ISEOF:
			case FSA_CMD_OPERATION_TYPE::GETSTATFILE:
			case FSA_CMD_OPERATION_TYPE::CLOSEFILE:
			case FSA_CMD_OPERATION_TYPE::QUERYINFO:
			case FSA_CMD_OPERATION_TYPE::APPENDFILE:
			case FSA_CMD_OPERATION_TYPE::TRUNCATEFILE:
			case FSA_CMD_OPERATION_TYPE::FLUSHQUOTA:
			{
				// These are IOCTL and no IOCTLV
				cemu_assert_error();
				break;
			}
			default:
			{
				cemu_assert_unimplemented();
				break;
			}
			}

			IOS_ResourceReply(cmd, (IOS_ERROR)fsaResult);
		}

		void FSAHandleCommandIoctl(FSAClient* client, IPCCommandBody* cmd, FSA_CMD_OPERATION_TYPE operationId, void* ptrIn, void* ptrOut)
		{
			FSAShimBuffer* shimBuffer = (FSAShimBuffer*)ptrIn;
			FSA_RESULT fsaResult = FSA_RESULT::FATAL_ERROR;

			switch (operationId)
			{
			case FSA_CMD_OPERATION_TYPE::REMOVE:
			{
				fsaResult = FSAProcessCmd_remove(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::CHANGEDIR:
			{
				fsaResult = FSAProcessCmd_changeDir(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::GETCWD:
			{
				fsaResult = FSAProcessCmd_getCwd(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::MAKEDIR:
			{
				fsaResult = FSAProcessCmd_makeDir(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::RENAME:
			{
				fsaResult = FSAProcessCmd_rename(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::OPENDIR:
			{
				fsaResult = FSAProcessCmd_openDir(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::READDIR:
			{
				fsaResult = FSAProcessCmd_readDir(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::CLOSEDIR:
			{
				fsaResult = FSAProcessCmd_closeDir(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::OPENFILE:
			{
				fsaResult = FSAProcessCmd_openFile(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::GETPOS:
			{
				fsaResult = FSAProcessCmd_getPos(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::SETPOS:
			{
				fsaResult = FSAProcessCmd_setPos(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::ISEOF:
			{
				fsaResult = FSAProcessCmd_isEof(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::GETSTATFILE:
			{
				fsaResult = FSAProcessCmd_getStatFile(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::CLOSEFILE:
			{
				fsaResult = FSAProcessCmd_closeFile(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::QUERYINFO:
			{
				fsaResult = FSAProcessCmd_queryInfo(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::APPENDFILE:
			{
				fsaResult = FSAProcessCmd_appendFile(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::TRUNCATEFILE:
			{
				fsaResult = FSAProcessCmd_truncateFile(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::FLUSHQUOTA:
			{
				fsaResult = FSAProcessCmd_flushQuota(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::REWINDDIR:
			{
				fsaResult = FSAProcessCmd_rewindDir(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::FLUSHFILE:
			{
				fsaResult = FSAProcessCmd_flushFile(client, shimBuffer);
				break;
			}
			case FSA_CMD_OPERATION_TYPE::READ:
			case FSA_CMD_OPERATION_TYPE::WRITE:
			{
				// These commands are IOCTLVs not IOCTL
				cemu_assert_error();
			}
			}
			IOS_ResourceReply(cmd, (IOS_ERROR)fsaResult);
		}

		void FSAIoThread()
		{
			SetThreadName("IOSU-FSA");
			IOSMessage msg;
			while (true)
			{
				IOS_ERROR r = IOS_ReceiveMessage(sFSAIoMsgQueue, &msg, 0);
				cemu_assert(!IOS_ResultIsError(r));
				if (msg == 0)
					return; // shutdown signaled
				IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
				uint32 clientHandle = (uint32)cmd->devHandle;
				if (cmd->cmdId == IPCCommandId::IOS_OPEN)
				{
					sint32 clientIndex = 0;
					r = FSAAllocateClient(clientIndex);
					if (r != IOS_ERROR_OK)
					{
						IOS_ResourceReply(cmd, r);
						continue;
					}
					IOS_ResourceReply(cmd, (IOS_ERROR)clientIndex);
					continue;
				}
				else if (cmd->cmdId == IPCCommandId::IOS_CLOSE)
				{
					cemu_assert(clientHandle < sFSAClientArray.size());
					sFSAClientArray[clientHandle].ReleaseAndCleanup();
					IOS_ResourceReply(cmd, IOS_ERROR_OK);
					continue;
				}
				else if (cmd->cmdId == IPCCommandId::IOS_IOCTL)
				{
					cemu_assert(clientHandle < sFSAClientArray.size());
					cemu_assert(sFSAClientArray[clientHandle].isAllocated);
					FSAHandleCommandIoctl(sFSAClientArray.data() + clientHandle, cmd, (FSA_CMD_OPERATION_TYPE)cmd->args[0].value(), MEMPTR<void>(cmd->args[1]), MEMPTR<void>(cmd->args[3]));
				}
				else if (cmd->cmdId == IPCCommandId::IOS_IOCTLV)
				{
					cemu_assert(clientHandle < sFSAClientArray.size());
					cemu_assert(sFSAClientArray[clientHandle].isAllocated);
					FSA_CMD_OPERATION_TYPE requestId = (FSA_CMD_OPERATION_TYPE)cmd->args[0].value();
					uint32 numIn = cmd->args[1];
					uint32 numOut = cmd->args[2];
					IPCIoctlVector* vec = MEMPTR<IPCIoctlVector>{cmd->args[3]}.GetPtr();
					FSAHandleCommandIoctlv(sFSAClientArray.data() + clientHandle, cmd, requestId, numIn, numOut, vec);
				}
				else
				{
					cemuLog_log(LogType::Force, "/dev/fsa: Unsupported IPC cmdId");
					cemu_assert_suspicious();
					IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
				}
			}
		}

		void Initialize()
		{
			s_fsaStaticData.Allocate();
			for (auto& it : sFSAClientArray)
				it.ReleaseAndCleanup();
			sFSAIoMsgQueue = (IOSMsgQueueId)IOS_CreateMessageQueue(_m_sFSAIoMsgQueueMsgBuffer.GetPtr(), _m_sFSAIoMsgQueueMsgBuffer.GetCount());
			IOS_ERROR r = IOS_RegisterResourceManager("/dev/fsa", sFSAIoMsgQueue);
			IOS_DeviceAssociateId("/dev/fsa", 11);
			cemu_assert(!IOS_ResultIsError(r));
			sFSAIoThread = std::thread(FSAIoThread);
		}

		void Shutdown()
		{
			IOS_SendMessage(sFSAIoMsgQueue, 0, 0);
			sFSAIoThread.join();
			IOS_DestroyMessageQueue(sFSAIoMsgQueue);
			s_fsaStaticData.Deallocate();
		}
	} // namespace fsa
} // namespace iosu
