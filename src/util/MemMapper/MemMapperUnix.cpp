#include "util/MemMapper/MemMapper.h"

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#if __linux__
#include <sys/syscall.h>
#endif

namespace MemMapper
{
	const size_t sPageSize{ []()
		{
		return (size_t)getpagesize();
	}()
	};

	size_t GetPageSize()
	{
		return sPageSize;
	}

	int GetProt(PAGE_PERMISSION permissionFlags)
	{
		int  p = 0;
		if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ | PROT_WRITE | PROT_EXEC;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ | PROT_WRITE;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ;
		else
			cemu_assert_unimplemented();
		return p;
	}

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		return mmap(baseAddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		munmap(baseAddr, size);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		void* r;
		if(fromReservation)
		{
		    uint64 page_size = sysconf(_SC_PAGESIZE);
		    void* page = baseAddr;
		    if ( (uint64) baseAddr % page_size != 0 )
		        page = (void*) ((uint64)baseAddr & ~(page_size - 1));
			if( mprotect(page, size, GetProt(permissionFlags)) == 0 )
                r = baseAddr;
			else
                r = nullptr;
		}
		else
			r = mmap(baseAddr, size, GetProt(permissionFlags), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		return r;
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		if (fromReservation)
		{
			// Discard the pages so they read back as zero on the next AllocateMemory, like decommitting
			// does on Windows. Without this a title started after another one (e.g. an applet launched
			// from the Wii U Menu) would see the previous title's leftover data in MEM2 and other ranges
			madvise(baseAddr, size, MADV_DONTNEED);
			mprotect(baseAddr, size, PROT_NONE);
		}
		else
			munmap(baseAddr, size);
	}


	/* shared memory (memfd on Linux) */

	bool SupportsSharedMemory()
	{
#if __linux__
		return true;
#else
		return false; // todo: shm_open based implementation for macOS/BSD
#endif
	}

	SharedMemoryHandle CreateSharedMemory(size_t size, const char* debugName)
	{
#if __linux__
		int fd = (int)syscall(SYS_memfd_create, debugName, (unsigned int)MFD_CLOEXEC);
		if (fd < 0)
			return INVALID_SHARED_MEMORY;
		if (ftruncate(fd, (off_t)size) != 0)
		{
			close(fd);
			return INVALID_SHARED_MEMORY;
		}
		return fd;
#else
		return INVALID_SHARED_MEMORY;
#endif
	}

	void CloseSharedMemory(SharedMemoryHandle handle)
	{
		if (handle >= 0)
			close((int)handle);
	}

	bool MapSharedMemory(SharedMemoryHandle handle, void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		if (handle < 0)
			return false;
		void* r = mmap(baseAddr, size, GetProt(permissionFlags), MAP_SHARED | MAP_FIXED, (int)handle, 0);
		return r == baseAddr;
	}

	void UnmapSharedMemory(void* baseAddr, size_t size)
	{
		// put an inaccessible anonymous reservation back in place (same state as after ReserveMemory)
		mmap(baseAddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	}

	std::vector<std::pair<size_t, size_t>> GetSharedMemoryWrittenRanges(SharedMemoryHandle handle, size_t size)
	{
		std::vector<std::pair<size_t, size_t>> ranges;
#if __linux__ && defined(SEEK_DATA)
		off_t pos = 0;
		while ((size_t)pos < size)
		{
			off_t dataStart = lseek((int)handle, pos, SEEK_DATA);
			if (dataStart < 0)
				break;
			off_t dataEnd = lseek((int)handle, dataStart, SEEK_HOLE);
			if (dataEnd < 0)
				dataEnd = (off_t)size;
			ranges.emplace_back((size_t)dataStart, (size_t)(dataEnd - dataStart));
			pos = dataEnd;
		}
#endif
		return ranges;
	}
};
