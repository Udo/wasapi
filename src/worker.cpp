#include "worker.h"

WorkerPool global_worker_pool;

WorkerPool::WorkerPool() {}
WorkerPool::~WorkerPool()
{
	shutdown();
}

void WorkerPool::start(size_t thread_count)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (running || thread_count == 0)
		return;
	stopping = false;
	running = true;
	threads.reserve(thread_count);
	for (size_t i = 0; i < thread_count; ++i)
	{
		threads.emplace_back([this]
							 { run(); });
	}
}

bool WorkerPool::enqueue(Task t)
{
	std::unique_lock<std::mutex> lock(mtx);
	if (stopping)
		return false;
	q.push(std::move(t));
	cv.notify_one();
	return true;
}

void WorkerPool::shutdown()
{
	{
		std::unique_lock<std::mutex> lock(mtx);
		if (!running)
			return;
		stopping = true;
	}
	cv.notify_all();
	for (auto& th : threads)
	{
		if (th.joinable())
			th.join();
	}
	threads.clear();
	running = false;
	while (!q.empty())
		q.pop();
}

void WorkerPool::run()
{
	while (true)
	{
		Task task;
		{
			std::unique_lock<std::mutex> lock(mtx);
			cv.wait(lock, [&]
					{ return stopping || !q.empty(); });
			if (stopping && q.empty())
				break;
			task = std::move(q.front());
			q.pop();
		}
		if (task)
			task();
	}
}
