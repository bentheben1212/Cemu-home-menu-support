#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"

namespace coreinit
{
	struct
	{
		bool rangeIsAllocated;
		MPTR rangeStart;
		uint32 rangeSize;
		uint8* cacheStateCopy; // holds a copy of the entire range, simulates icache state (updated via ICBI)
	}coreinitCodeGen = {0};

	void codeGenArea_memoryWriteCallback(void* pageStart, size_t size)
	{
		uint32 ea = memory_getVirtualOffsetFromPointer(pageStart);
		uint32 eaEnd = ea + (uint32)size;
		while (ea <= eaEnd)
		{
			codeGenHandleICBI(ea);
			ea += 0x20;
		}
	}

	void OSGetCodegenVirtAddrRange(betype<uint32>* rangeStart, betype<uint32>* rangeSize)
	{
		// on first call, allocate range
		if( coreinitCodeGen.rangeIsAllocated == false )
		{
			// the size comes from the title's cos.xml (codegen_size). WebKit based titles (eShop, Miiverse, Internet Browser)
			// size their JIT heap from it. Fall back to 16MiB if the title doesn't specify it
			uint32 codegenSize = CafeSystem::GetForegroundTitleCodegenSize();
			uint32 cosCodegenSize = codegenSize;
			if (codegenSize == 0)
				codegenSize = 0x01000000;
			codegenSize = (codegenSize + 0xFFF) & ~0xFFF;
			cemuLog_log(LogType::Force, "CodeGen: allocating codegen range of 0x{:x} bytes (cos.xml codegen_size 0x{:x})", codegenSize, cosCodegenSize);
			coreinitCodeGen.rangeStart = RPLLoader_AllocateCodeSpace(codegenSize, 0x1000);
			coreinitCodeGen.rangeSize = codegenSize;
			coreinitCodeGen.cacheStateCopy = new uint8[codegenSize];
			memset(coreinitCodeGen.cacheStateCopy, 0, codegenSize);
			coreinitCodeGen.rangeIsAllocated = true;
		}
		*rangeStart = coreinitCodeGen.rangeStart;
		*rangeSize = coreinitCodeGen.rangeSize;
	}

	void OSGetCodegenVirtAddrRangeInternal(uint32& rangeStart, uint32& rangeSize)
	{
		if (coreinitCodeGen.rangeIsAllocated == 0)
		{
			rangeStart = 0;
			rangeSize = 0;
			return;
		}
		rangeStart = coreinitCodeGen.rangeStart;
		rangeSize = coreinitCodeGen.rangeSize;
	}

	void ICInvalidateRange(uint32 startAddress, uint32 size)
	{
		uint32 ea = startAddress & ~0x1F;
		uint32 eaEnd = (startAddress + size + 0x1F)&~0x1F;
		while (ea <= eaEnd)
		{
			codeGenHandleICBI(ea);
			ea += 0x20;
		}

	}

	void coreinitExport_OSCodegenCopy(PPCInterpreter_t* hCPU)
	{
		if( coreinitCodeGen.rangeIsAllocated == false )
			assert_dbg();

		uint32 codeAddrDest = hCPU->gpr[3];
		uint32 memAddrSrc = hCPU->gpr[4];
		uint32 size = hCPU->gpr[5];

		if( codeAddrDest < coreinitCodeGen.rangeStart || codeAddrDest >= (coreinitCodeGen.rangeStart+coreinitCodeGen.rangeSize) )
			assert_dbg();
		if( (codeAddrDest+size) < coreinitCodeGen.rangeStart || (codeAddrDest+size) > (coreinitCodeGen.rangeStart+coreinitCodeGen.rangeSize) )
			assert_dbg();

		memcpy(memory_getPointerFromVirtualOffset(codeAddrDest), memory_getPointerFromVirtualOffset(memAddrSrc), size);

		// invalidate recompiler range
		uint32 ea = codeAddrDest & ~0x1F;
		uint32 eaEnd = (codeAddrDest + size + 0x1F)&~0x1F;
		while (ea <= eaEnd)
		{
			codeGenHandleICBI(ea);
			ea += 0x20;
		}

		osLib_returnFromFunction(hCPU, 0);
	}

	void codeGenHandleICBI(uint32 ea)
	{
		cemu_assert_debug((ea & 0x1F) == 0);
		if (coreinitCodeGen.rangeIsAllocated == false)
			return;
		cemu_assert_debug((coreinitCodeGen.rangeStart & 0x1F) == 0);
		cemu_assert_debug((coreinitCodeGen.rangeSize & 0x1F) == 0);
		if (ea >= coreinitCodeGen.rangeStart && ea < (coreinitCodeGen.rangeStart + coreinitCodeGen.rangeSize))
		{
			uint8* cacheCopy = coreinitCodeGen.cacheStateCopy + (ea - coreinitCodeGen.rangeStart);
			uint8* currentState = memory_getPointerFromVirtualOffset(ea);
			if (memcmp(currentState, cacheCopy, 32) != 0)
			{
				// instructions changed
				// flush cache
				PPCRecompiler_invalidateRange(ea, ea+0x20);
				// update icache copy
				memcpy(cacheCopy, currentState, 32);
			}
		}
	}

	bool _avoidCodeGenJIT = false;

	// currently we dont handle code invalidation well for direct write access
	// therefore if we detect attempts to write we will disable JITing the area
	bool codeGenShouldAvoid()
	{
		return _avoidCodeGenJIT;
	}

	bool OSSwitchSecCodeGenMode(bool isRXOnly)
	{
		if (!_avoidCodeGenJIT)
		{
			cemuLog_log(LogType::Force, "Disable JIT on dynamic code area");
		}
		_avoidCodeGenJIT = true; // this function getting called is usually a sign that the title will directly write to the executable memory
		// does this have a return value?
		return true;
	}

	void InitializeCodeGen()
	{
		_avoidCodeGenJIT = false;
		// The codegen (JIT) range is allocated from the RPL loader's code area, which starts over for every title.
		// Forget a range left over from a previous title, otherwise a title launched after another JIT user
		// (e.g. eShop after Miiverse or Internet Browser) gets a range that overlaps its own modules
		if (coreinitCodeGen.rangeIsAllocated)
		{
			cemuLog_log(LogType::Force, "CodeGen: releasing codegen range 0x{:08x} (size 0x{:x}) of the previous title", coreinitCodeGen.rangeStart, coreinitCodeGen.rangeSize);
			RPLLoader_FreeCodeSpace(coreinitCodeGen.rangeStart);
		}
		delete[] coreinitCodeGen.cacheStateCopy;
		coreinitCodeGen = {};
		cafeExportRegister("coreinit", OSGetCodegenVirtAddrRange, LogType::Placeholder);
		cafeExportRegister("coreinit", ICInvalidateRange, LogType::Placeholder);

		osLib_addFunction("coreinit", "OSCodegenCopy", coreinitExport_OSCodegenCopy);

		cafeExportRegister("coreinit", OSSwitchSecCodeGenMode, LogType::Placeholder);
	}
}
