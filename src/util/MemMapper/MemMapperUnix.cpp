#include "util/MemMapper/MemMapper.h"

#include <unistd.h>
#include <sys/mman.h>

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

};
