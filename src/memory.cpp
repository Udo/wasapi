#include "memory.h"
#include <cstdlib>
#include <algorithm>
#include <cstring>

Arena::~Arena()
{
	std::free(data);
}

Arena::Arena(size_t cap)
{
	data = (uint8_t*)std::malloc(cap);
	if (data)
	{
		capacity = cap;
		reset();
	}
}

void Arena::reset()
{
	offset = 0;
}

void* Arena::alloc(size_t sz, size_t align)
{
	size_t base_addr = reinterpret_cast<size_t>(data);
	size_t cur = base_addr + offset;
	size_t aligned = (cur + (align - 1)) & ~(align - 1);
	size_t new_off = aligned - base_addr + sz;
	if (new_off > capacity)
		return nullptr;
	offset = new_off;
	void* ptr = reinterpret_cast<void*>(aligned);
#ifdef ARENA_ZERO_CLEAR
	memset(ptr, 0, sz);
#endif
	return ptr;
}