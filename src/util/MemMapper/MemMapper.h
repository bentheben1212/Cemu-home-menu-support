#pragma once

namespace MemMapper
{
	enum class PAGE_PERMISSION : uint32
	{
		P_READ = (0x01),
		P_WRITE = (0x02),
		P_EXECUTE = (0x04),
		// combined
		P_NONE = 0,
		P_RW = (0x03),
		P_RWX = (0x07)
	};
	DEFINE_ENUM_FLAG_OPERATORS(PAGE_PERMISSION);

	size_t GetPageSize();

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags);
	void FreeReservation(void* baseAddr, size_t size);

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation = false);
	void FreeMemory(void* baseAddr, size_t size, bool fromReservation = false);

	// shared memory objects that can be mapped (and remapped) at fixed addresses inside a reservation. Used to swap the
	// memory of a suspended guest process out of the way and back without copying. Platform specific, currently only
	// implemented for Unix (memfd). SupportsSharedMemory() returns false where it isn't implemented
	using SharedMemoryHandle = sint64;
	constexpr SharedMemoryHandle INVALID_SHARED_MEMORY = -1;

	bool SupportsSharedMemory();
	SharedMemoryHandle CreateSharedMemory(size_t size, const char* debugName); // zero filled
	void CloseSharedMemory(SharedMemoryHandle handle);
	// maps the shared memory at baseAddr (inside a reservation), replacing whatever is mapped there
	bool MapSharedMemory(SharedMemoryHandle handle, void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags);
	// replaces a shared memory mapping inside a reservation with an inaccessible reserved range again
	void UnmapSharedMemory(void* baseAddr, size_t size);
};