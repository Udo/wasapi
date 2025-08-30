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

void ArenaManager::create_arenas(size_t count, size_t capacity)
{
	arenas.clear();
	in_use.clear();
	for (size_t i = 0; i < count; ++i)
	{
		arenas.emplace_back(capacity);
		arenas[i].management_flag = i;
		in_use.push_back(false);
	}
}

Arena* ArenaManager::get()
{
	std::lock_guard<std::mutex> lock(mutex);
	for (size_t i = 0; i < arenas.size(); ++i)
	{
		if (!in_use[i])
		{
			in_use[i] = true;
			return &arenas[i];
		}
	}
	return nullptr;
}

void ArenaManager::release(Arena* arena)
{
	std::lock_guard<std::mutex> lock(mutex);
	auto i = arena->management_flag;
	in_use[i] = false;
	arena->reset();
}
