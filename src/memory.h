#ifndef MEMORY_H
#define MEMORY_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <new>
#include <type_traits>

#define ARENA_ZERO_CLEAR

struct Arena
{
	uint8_t* data = nullptr;
	size_t capacity = 0;
	size_t offset = 0;
	size_t management_flag = 0;

	~Arena();
	Arena(size_t cap);
	void reset();
	void* alloc(size_t sz, size_t align = alignof(std::max_align_t));
};

struct ArenaManager
{
	std::vector<Arena> arenas;
	std::mutex mutex;
	std::vector<bool> in_use;

	void create_arenas(size_t count, size_t capacity);
	Arena* get();
	void release(Arena* arena);
};

#endif
