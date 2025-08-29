#ifndef MEMORY_H
#define MEMORY_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <new>
#include <type_traits>

struct Arena // we don't use this yet, basically untested
{
	uint8_t* data = nullptr;
	size_t capacity = 0;
	size_t offset = 0;

	Arena() = default;
	Arena(size_t cap)
	{
		reserve(cap);
	}
	~Arena();

	void reserve(size_t cap);
	void reset()
	{
		offset = 0;
	}

	void* alloc(size_t sz, size_t align = alignof(std::max_align_t))
	{
		size_t base = reinterpret_cast<size_t>(data);
		size_t cur = base + offset;
		size_t aligned = (cur + (align - 1)) & ~(align - 1);
		size_t new_off = aligned - base + sz;
		if (new_off > capacity)
			return nullptr; 
		offset = new_off;
		return reinterpret_cast<void*>(aligned);
	}

	template <typename T, typename... Args>
	T* make(Args&&... args)
	{
		void* p = alloc(sizeof(T), alignof(T));
		if (!p)
			throw std::bad_alloc();
		return new (p) T(std::forward<Args>(args)...);
	}
};

class ArenaPool
{
  public:
	explicit ArenaPool(size_t arena_capacity = 256 * 1024, size_t preallocate = 0);
	~ArenaPool();

	Arena* acquire();
	void release(Arena* a); 

	void set_arena_capacity(size_t cap)
	{
		arena_capacity_ = cap;
	}
	size_t arena_capacity()
	{
		return arena_capacity_;
	}

  private:
	size_t arena_capacity_;
	std::vector<Arena*> free_list_;
	std::vector<Arena*> all_;
	std::mutex mtx_;
};

template <class T>
struct ArenaAllocator
{
	using value_type = T;
	Arena* arena = nullptr;
	ArenaAllocator() noexcept : arena(nullptr) {}
	explicit ArenaAllocator(Arena* a) noexcept : arena(a) {}
	template <class U>
	ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena(other.arena) {}

	T* allocate(std::size_t n)
	{
		if (arena)
		{
			void* p = arena->alloc(n * sizeof(T), alignof(T));
			if (!p)
				throw std::bad_alloc();
			return static_cast<T*>(p);
		}
		else
		{
			void* p = ::operator new(n * sizeof(T));
			return static_cast<T*>(p);
		}
	}
	void deallocate(T* p, std::size_t n) noexcept
	{
		if (!arena)
		{
			::operator delete(p, n * sizeof(T));
		}
	}

	template <class U>
	struct rebind
	{
		using other = ArenaAllocator<U>;
	};
};

template <class T, class U>
inline bool operator==(const ArenaAllocator<T>& a, const ArenaAllocator<U>& b) noexcept
{
	return a.arena == b.arena;
}

template <class T, class U>
inline bool operator!=(const ArenaAllocator<T>& a, const ArenaAllocator<U>& b) noexcept
{
	return !(a == b);
}

struct RequestArenaHandle
{
	ArenaPool& pool;
	Arena* arena;
	explicit RequestArenaHandle(ArenaPool& p) : pool(p), arena(p.acquire()) {}
	~RequestArenaHandle()
	{
		pool.release(arena);
	}
	RequestArenaHandle(const RequestArenaHandle&) = delete;
	RequestArenaHandle& operator=(const RequestArenaHandle&) = delete;
};

#endif
