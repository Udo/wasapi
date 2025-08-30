#ifndef MEMORY_H
#define MEMORY_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <new>
#include <cstdlib>
#include <type_traits>
#include <atomic>

#define ARENA_ZERO_CLEAR

struct Arena
{
	uint8_t* data = nullptr;
	size_t capacity = 0;
	size_t offset = 0;
	size_t management_flag = 0;

	~Arena();
	Arena(size_t cap);
	Arena() = default; // fuck
	Arena(const Arena&) = delete; // fuck
	Arena& operator=(const Arena&) = delete; // fuck
	void reset();
	void* alloc(size_t sz, size_t align = alignof(std::max_align_t));
};

struct ArenaManager
{
	std::vector<Arena*> arenas;
	std::mutex mutex;
	std::vector<bool> in_use;
	std::atomic<size_t> available_count{ 0 };

	~ArenaManager();
	void create_arenas(size_t count, size_t capacity);
	Arena* get();
	void release(Arena* arena);
};

extern ArenaManager global_arena_manager;

#endif
