#ifndef WORKER_H
#define WORKER_H

#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

class WorkerPool
{
  public:
	using Task = std::function<void()>;

	WorkerPool();
	~WorkerPool();

	void start(size_t thread_count);

	bool enqueue(Task t);

	void shutdown();

  private:
	void run();

	std::vector<std::thread> threads;
	std::mutex mtx;
	std::condition_variable cv;
	std::queue<Task> q;
	std::atomic<bool> running{ false };
	bool stopping = false;
};

extern WorkerPool global_worker_pool;

#endif // WORKER_H
