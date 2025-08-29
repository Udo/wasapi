#include "memory.h"
#include <cstdlib>
#include <algorithm>

Arena::~Arena()
{
	std::free(data);
}

void Arena::reserve(size_t cap)
{
	if (cap <= capacity)
		return;
	uint8_t* nd = static_cast<uint8_t*>(std::realloc(data, cap));
	if (!nd)
		throw std::bad_alloc();
	data = nd;
	capacity = cap;
}

ArenaPool::ArenaPool(size_t arena_capacity, size_t preallocate) : arena_capacity_(arena_capacity)
{
	free_list_.reserve(preallocate);
	all_.reserve(preallocate);
	for (size_t i = 0; i < preallocate; i++)
	{
		Arena* a = new Arena(arena_capacity_);
		free_list_.push_back(a);
		all_.push_back(a);
	}
}

ArenaPool::~ArenaPool()
{
	for (Arena* a : all_)
	{
		delete a;
	}
}

Arena* ArenaPool::acquire()
{
	std::lock_guard<std::mutex> lock(mtx_);
	Arena* a;
	if (free_list_.empty())
	{
		a = new Arena(arena_capacity_);
		all_.push_back(a);
	}
	else
	{
		a = free_list_.back();
		free_list_.pop_back();
	}
	a->reset();
	return a;
}

void ArenaPool::release(Arena* a)
{
	if (!a)
		return;
	a->reset();
	std::lock_guard<std::mutex> lock(mtx_);
	free_list_.push_back(a);
}
