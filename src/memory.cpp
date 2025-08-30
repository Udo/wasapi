#include "memory.h"
#include <cstdlib>
#include <algorithm>
#include <cstring>

ArenaManager global_arena_manager;

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

ArenaManager::~ArenaManager()
{
	for (auto* a : arenas)
		delete a;
	arenas.clear();
	in_use.clear();
	available_count.store(0, std::memory_order_relaxed);
}

void ArenaManager::create_arenas(size_t count, size_t capacity)
{
	for (auto* a : arenas)
		delete a;
	arenas.clear();
	in_use.clear();
	for (size_t i = 0; i < count; ++i)
	{
		Arena* a = new Arena(capacity);
		a->management_flag = i;
		arenas.push_back(a);
		in_use.push_back(false);
	}
	available_count.store(count, std::memory_order_relaxed);
}

Arena* ArenaManager::get()
{
	std::lock_guard<std::mutex> lock(mutex);
	for (size_t i = 0; i < arenas.size(); ++i)
	{
		if (!in_use[i])
		{
			in_use[i] = true;
			available_count.fetch_sub(1, std::memory_order_relaxed);
			return arenas[i];
		}
	}
	return nullptr;
}

void ArenaManager::release(Arena* arena)
{
	if (!arena)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	auto i = arena->management_flag;
	if (i < in_use.size() && in_use[i])
	{
		in_use[i] = false;
		available_count.fetch_add(1, std::memory_order_relaxed);
		arena->reset();
	}
}
