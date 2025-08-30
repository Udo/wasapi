#include "fcgi-connection.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <unordered_map>
#include <deque>
#include "fastcgi.h"
#include "config.h"
#include "logger.h"
#include "http.h"
#include "session.h"
#include "memory.h"
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <mutex>
#include "worker.h"

namespace fcgi_conn
{
	static int set_non_blocking(int fd)
	{
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags == -1)
			return -1;
		if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
			return -1;
		return 0;
	}

	struct Connection
	{
		int fd = -1;
		std::vector<uint8_t> in_buf;
		std::vector<uint8_t> out_buf; // outbound data buffer
		size_t out_pos = 0; // bytes already sent from start of out_buf
		std::unordered_map<uint16_t, Request*> requests; // managed via arenas
		std::atomic<bool> closed{ false }; // accessed from IO + worker threads
		enum State
		{
			ACTIVE,
			WAITING_ARENA,
			CLOSING
		} state = ACTIVE;
		bool waiting_for_arena = false; // BEGIN_REQUEST record deferred (legacy helper flag)
		bool in_wait_queue = false; // listed in g_waiting_conns
		std::mutex out_mutex; // protect out_buf/out_pos
		std::atomic<int> active_workers{ 0 };
		uint32_t epoll_mask = EPOLLIN | EPOLLET; // currently registered interest mask
		bool want_write_interest = false; // desired EPOLLOUT interest
	};

	static std::unordered_map<int, Connection> g_conns;
	static RequestReadyCallback g_user_request_ready = nullptr;
	static int g_epfd = -1; // epoll fd for worker-triggered wakeups
	static std::deque<int> g_pending_conns; // accepted but not yet registered (waiting for free arena)
	static std::deque<int> g_waiting_conns; // existing connections blocked on arena
	static int g_timerfd = -1; // periodic housekeeping timer
	static std::vector<int> g_close_queue; // deferred closes
	static Request* allocate_request(uint16_t id);
	static void flush_connection(Connection& c, int epfd);
	static void internal_on_request_ready(Request& r, std::vector<uint8_t>& out_buf);
	static bool should_close_connection(Connection& c); // forward
	static void finalize_request(Request& req); // forward (already defined later)
	static void release_request(Request* r); // forward
	static void maybe_update_epoll(Connection& c, int epfd, uint32_t desired); // forward
	thread_local Connection* tls_io_connection = nullptr;

	static inline void enqueue_waiting(Connection& c)
	{
		if (!c.in_wait_queue)
		{
			g_waiting_conns.push_back(c.fd);
			c.in_wait_queue = true;
		}
	}

	static void retry_waiting(int epfd)
	{
		if (g_waiting_conns.empty())
			return;
		int budget = (int)global_arena_manager.available_count.load(std::memory_order_relaxed);
		if (budget <= 0)
			return;
		int processed = 0;
		size_t qcount = g_waiting_conns.size();
		for (size_t i = 0; i < qcount && budget > 0 && !g_waiting_conns.empty(); ++i)
		{
			int fd = g_waiting_conns.front();
			g_waiting_conns.pop_front();
			auto it = g_conns.find(fd);
			if (it == g_conns.end())
				continue; // connection gone
			Connection& c = it->second;
			c.in_wait_queue = false; // will re-add if still waiting
			if (c.state != Connection::WAITING_ARENA)
				continue;
			if (c.closed.load(std::memory_order_relaxed))
			{
				c.state = Connection::CLOSING;
				continue;
			}
			bool waiting = false;
			tls_io_connection = &c;
			auto status = fcgi::process_buffer(c.in_buf, c.requests, c.out_buf, allocate_request, global_config.max_params_bytes, global_config.max_stdin_bytes, internal_on_request_ready, waiting);
			tls_io_connection = nullptr;
			if (status == fcgi::CLOSE)
			{
				c.closed.store(true, std::memory_order_relaxed);
				c.state = Connection::CLOSING;
			}
			if (waiting)
			{
				enqueue_waiting(c);
				break;
			}
			budget = (int)global_arena_manager.available_count.load(std::memory_order_relaxed);
			c.waiting_for_arena = false;
			c.state = c.closed.load(std::memory_order_relaxed) ? Connection::CLOSING : Connection::ACTIVE;
			if (c.out_pos != c.out_buf.size())
				flush_connection(c, epfd);
			processed++;
		}
	}

	static void housekeeping_close_idle(int epfd)
	{
		// Iterate over connections and close those eligible; collect fds first to avoid iterator invalidation.
		std::vector<int> to_close;
		to_close.reserve(g_conns.size());
		for (auto &kv : g_conns)
		{
			Connection &c = kv.second;
			if (should_close_connection(c))
				to_close.push_back(kv.first);
		}
		for (int fd : to_close)
		{
			auto it = g_conns.find(fd);
			if (it == g_conns.end()) continue;
			Connection &c = it->second;
			for (auto &rk : c.requests)
			{
				if (rk.second)
				{
					if (!(rk.second->flags & Request::RESPONDED))
						finalize_request(*rk.second);
					release_request(rk.second);
				}
			}
			c.requests.clear();
			epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
			::close(fd);
			log_debug("Closed fd=%d (housekeeping)", fd);
			g_conns.erase(it);
		}
	}

	static void finalize_request(Request& req);
	thread_local Connection* tls_current_connection = nullptr;

	static void internal_on_request_ready(Request& r, std::vector<uint8_t>& out_buf)
	{
		(void)out_buf; // worker thread now manages output
		if (r.flags & Request::RESPONDED)
			return;

		r.files = DynamicVariable::make_array();
		parse_endpoint_file(r, r.env.find(global_config.endpoint_file_path));
		parse_cookie_header(r, r.env.find(global_config.http_cookies_var));
		parse_query_string(r, r.env.find(global_config.http_query_var));
		parse_form_data(r);
		r.session = DynamicVariable::make_object();

		if (global_config.session_auto_load)
		{
			DynamicVariable* sid = r.cookies.find(global_config.session_cookie_name);
			if (sid && sid->type == DynamicVariable::STRING)
			{
				session_start(r);
			}
		}
		r.headers["Content-Type"] = global_config.default_content_type;

		Connection* c = static_cast<Connection*>(r.conn_ptr);
		if (!c)
			return;
		c->active_workers.fetch_add(1, std::memory_order_relaxed);
		r.worker_active.store(true, std::memory_order_release);
		::global_worker_pool.enqueue([&r, c]
									 {
		if (c->closed.load(std::memory_order_relaxed) && c->active_workers.load(std::memory_order_relaxed) == 0)
			{
				r.worker_active.store(false, std::memory_order_release);
				c->active_workers.fetch_sub(1, std::memory_order_relaxed);
				return;
			}
			if (!(r.flags & Request::RESPONDED))
			{
				tls_current_connection = c;
				std::vector<uint8_t> local_out; local_out.reserve(1024);
				if (g_user_request_ready)
					g_user_request_ready(r, local_out);
				bool added = false;
										if (!local_out.empty() && !c->closed.load(std::memory_order_relaxed))
				{
					std::lock_guard<std::mutex> lk(c->out_mutex);
					bool was_empty = (c->out_pos == c->out_buf.size());
					c->out_buf.insert(c->out_buf.end(), local_out.begin(), local_out.end());
					added = was_empty;
				}
				tls_current_connection = nullptr;
										if (added && g_epfd != -1 && !c->closed.load(std::memory_order_relaxed))
				{
					if (!c->want_write_interest)
					{
						maybe_update_epoll(*c, g_epfd, EPOLLIN | EPOLLOUT | EPOLLET);
						c->want_write_interest = true;
					}
				}
			}
			r.worker_active.store(false, std::memory_order_release);
			c->active_workers.fetch_sub(1, std::memory_order_relaxed); });
	}

	static void log_errno(const char* msg)
	{
		log_error("%s: %s", msg, std::strerror(errno));
	}

	static void maybe_update_epoll(Connection& c, int epfd, uint32_t desired)
	{
		if (desired == c.epoll_mask) return;
		epoll_event ev{}; ev.data.fd = c.fd; ev.events = desired;
		epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev);
		c.epoll_mask = desired;
	}

	static void flush_connection(Connection& c, int epfd)
	{
		while (true)
		{
			ssize_t n = 0;
			{
				std::lock_guard<std::mutex> lk(c.out_mutex);
				size_t remaining = c.out_pos < c.out_buf.size() ? (c.out_buf.size() - c.out_pos) : 0;
				if (remaining == 0)
				{
					if (c.want_write_interest)
					{
						maybe_update_epoll(c, epfd, EPOLLIN | EPOLLET);
						c.want_write_interest = false;
					}
					if (c.out_pos != 0)
					{
						c.out_buf.clear();
						c.out_pos = 0;
					}
					if (should_close_connection(c))
						g_close_queue.push_back(c.fd);
					return;
				}
				n = ::send(c.fd, c.out_buf.data() + c.out_pos, remaining, 0);
				if (n > 0)
				{
					c.out_pos += (size_t)n;
					continue; // keep draining
				}
			}
			if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				if (!c.want_write_interest)
				{
					maybe_update_epoll(c, epfd, EPOLLIN | EPOLLOUT | EPOLLET);
					c.want_write_interest = true;
				}
				return;
			}
			if (n <= 0)
			{
				log_errno("send");
				c.closed.store(true, std::memory_order_relaxed);
				return;
			}
		}
	}

	static Request* allocate_request(uint16_t id)
	{
		Arena* a = global_arena_manager.get();
		if (!a)
			return nullptr;
		void* mem = a->alloc(sizeof(Request), alignof(Request));
		if (!mem)
		{
			global_arena_manager.release(a);
			return nullptr;
		}
		Request* r = new (mem) Request(a);
		r->id = id;
		r->conn_ptr = tls_io_connection;
		return r;
	}

	static void release_request(Request* r)
	{
		if (!r)
			return;
		Arena* a = r->arena;
		r->~Request();
		if (a)
			global_arena_manager.release(a);
		if (!g_pending_conns.empty() && global_arena_manager.available_count.load(std::memory_order_relaxed) > 0 && g_epfd != -1)
		{
			int fd = g_pending_conns.front();
			g_pending_conns.pop_front();
			Connection& c = g_conns[fd];
			c.fd = fd;
			c.out_buf.reserve(global_config.output_buffer_initial);
			epoll_event cev{};
			cev.data.fd = fd;
			cev.events = EPOLLIN | EPOLLET;
			epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &cev);
			log_debug("Activated deferred fd=%d", fd);
		}
		if (g_epfd != -1)
			retry_waiting(g_epfd);
		if (!g_waiting_conns.empty() && global_arena_manager.available_count.load(std::memory_order_relaxed) > 0 && g_epfd != -1)
		{
			int budget = (int)global_arena_manager.available_count.load(std::memory_order_relaxed);
			while (budget-- > 0 && !g_waiting_conns.empty() && global_arena_manager.available_count.load(std::memory_order_relaxed) > 0)
			{
				int fd = g_waiting_conns.front();
				g_waiting_conns.pop_front();
				auto it = g_conns.find(fd);
				if (it == g_conns.end())
					continue;
				Connection& c = it->second;
				if (!c.waiting_for_arena)
					continue;
				bool waiting = false;
				tls_io_connection = &c;
				auto status = fcgi::process_buffer(c.in_buf, c.requests, c.out_buf, allocate_request, global_config.max_params_bytes, global_config.max_stdin_bytes, internal_on_request_ready, waiting);
				tls_io_connection = nullptr;
				if (status == fcgi::CLOSE)
					c.closed.store(true, std::memory_order_relaxed);
				if (!waiting)
				{
					c.waiting_for_arena = false;
					if (!c.out_buf.empty())
						flush_connection(c, g_epfd);
				}
				else
				{
					g_waiting_conns.push_back(fd);
					break; // stop; no more arenas likely free
				}
			}
		}
	}

	static void finalize_request(Request& req)
	{
		if (req.files.type == DynamicVariable::ARRAY && !req.files.data.a.empty())
		{
			for (auto& f : req.files.data.a)
			{
				if (f.type != DynamicVariable::OBJECT)
					continue;
				DynamicVariable* tp = f.find("temp_path");
				if (!global_config.keep_uploaded_files && global_config.cleanup_temp_on_disconnect && tp && tp->type == DynamicVariable::STRING && !tp->data.s.empty())
				{
					::unlink(tp->data.s.c_str());
					tp->data.s.clear();
				}
			}
			req.files = DynamicVariable::make_array();
		}
		req.body.clear();
	}

	static bool should_close_connection(Connection& c)
	{
		if (c.closed.load(std::memory_order_relaxed))
		{
			return c.active_workers.load(std::memory_order_relaxed) == 0 && (c.out_pos == c.out_buf.size());
		}

		bool all_responded = true;
		for (auto& kv : c.requests)
		{
			Request* rp = kv.second;
			if (!rp || !(rp->flags & Request::RESPONDED))
			{
				all_responded = false;
				break;
			}
		}

		bool any_keep = false;
		for (auto& kv : c.requests)
		{
			Request* rp = kv.second;
			if (rp && (rp->flags & Request::KEEP_CONNECTION))
			{
				any_keep = true;
				break;
			}
		}

		return all_responded && !any_keep && (c.out_pos == c.out_buf.size()) && c.active_workers.load(std::memory_order_relaxed) == 0;
	}

	static void init(int listen_fd, RequestReadyCallback cb)
	{
		(void)listen_fd; // currently unused; placeholder if future per-socket state needed
		g_user_request_ready = cb;
		auto& G = global_config;
		std::string addr = G.unix_path.empty() ? (std::string("tcp:") + std::to_string(G.port)) : G.unix_path;
		log_info("server listening on %s", addr.c_str());
	}

	static void handle_new_connections(int listen_fd, int epfd)
	{
		while (true)
		{
			sockaddr_storage ss;
			socklen_t slen = sizeof(ss);
			int cfd = ::accept(listen_fd, (sockaddr*)&ss, &slen);
			if (cfd == -1)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				else
				{
					log_errno("accept");
					break;
				}
			}
			set_non_blocking(cfd);
			if (global_arena_manager.available_count.load(std::memory_order_relaxed) == 0)
			{
				g_pending_conns.push_back(cfd);
				g_conns[cfd]; // default-construct in-place (no move/copy)
				log_debug("Deferred fd=%d (no free arenas)", cfd);
			}
			else
			{
				epoll_event cev{};
				cev.data.fd = cfd;
				cev.events = EPOLLIN | EPOLLET;
				epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
				Connection& c = g_conns[cfd];
				c.fd = cfd;
				c.out_buf.reserve(global_config.output_buffer_initial);
				log_debug("Accepted fd=%d", cfd);
			}
		}
	}

	static void handle_io(int fd, uint32_t events, int epfd)
	{
		auto it = g_conns.find(fd);
		if (it == g_conns.end())
			return;

		Connection& c = it->second;
		auto& G = global_config;

		if (events & (EPOLLHUP | EPOLLERR))
		{
			c.closed.store(true, std::memory_order_relaxed);
		}

		if (events & EPOLLIN)
		{
			while (true)
			{
				uint8_t buf[4096];
				ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
				if (r > 0)
				{
					c.in_buf.insert(c.in_buf.end(), buf, buf + r);
				}
				else if (r == 0)
				{
					c.closed.store(true, std::memory_order_relaxed);
					break;
				}
				else
				{
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					else
					{
						log_errno("recv");
						c.closed.store(true, std::memory_order_relaxed);
						break;
					}
				}
			}

			if (!c.closed.load(std::memory_order_relaxed))
			{
				tls_io_connection = &c;
				bool waiting_for_arena = false;
				auto status = fcgi::process_buffer(c.in_buf, c.requests, c.out_buf, allocate_request, G.max_params_bytes, G.max_stdin_bytes, internal_on_request_ready, waiting_for_arena);
				tls_io_connection = nullptr;
				if (status == fcgi::CLOSE)
					c.closed.store(true, std::memory_order_relaxed);
				if (waiting_for_arena)
				{
					if (!c.waiting_for_arena)
					{
						c.waiting_for_arena = true;
						c.state = Connection::WAITING_ARENA;
						enqueue_waiting(c);
					}
				}
				else
				{
					c.waiting_for_arena = false;
					c.state = c.closed.load(std::memory_order_relaxed) ? Connection::CLOSING : Connection::ACTIVE;
				}
			}

			if (c.out_pos != c.out_buf.size())
						flush_connection(c, epfd);
		}

		if (events & EPOLLOUT)
		{
			if (!c.out_buf.empty())
				flush_connection(c, epfd);
		}

		for (auto it2 = c.requests.begin(); it2 != c.requests.end();)
		{
			Request* rp = it2->second;
			if (rp && !rp->worker_active.load(std::memory_order_acquire))
			{
				if (rp->flags & Request::RESPONDED)
				{
					finalize_request(*rp);
					release_request(rp);
					it2 = c.requests.erase(it2);
					continue;
				}
				if (rp->flags & (Request::FAILED | Request::ABORTED))
				{
					release_request(rp);
					it2 = c.requests.erase(it2);
					continue;
				}
			}
			++it2;
		}

		if (should_close_connection(c))
			g_close_queue.push_back(fd);

		if (!g_close_queue.empty())
		{
			std::vector<int> local; local.swap(g_close_queue);
			for (int cfd : local)
			{
				auto itc = g_conns.find(cfd);
				if (itc == g_conns.end()) continue;
				Connection &cc = itc->second;
				if (!should_close_connection(cc)) continue;
				for (auto &rk : cc.requests)
				{
					if (rk.second)
					{
						if (!(rk.second->flags & Request::RESPONDED))
							finalize_request(*rk.second);
						release_request(rk.second);
					}
				}
				cc.requests.clear();
				epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, nullptr);
				::close(cfd);
				log_debug("Closed fd=%d", cfd);
				g_conns.erase(itc);
			}
		}
	}

	static int create_listen_socket()
	{
		int fd;
		if (!global_config.unix_path.empty())
		{
			::unlink(global_config.unix_path.c_str());
			fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
			if (fd == -1)
			{
				log_errno("socket unix");
				return -1;
			}
			mode_t old_umask = umask(0);
			sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", global_config.unix_path.c_str());
			if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
			{
				log_errno("bind unix");
				umask(old_umask);
				::close(fd);
				return -1;
			}
			umask(old_umask);
			if (::chmod(global_config.unix_path.c_str(), 0777) == -1)
			{
				log_errno("chmod unix socket (continuing despite error)");
			}
		}
		else
		{
			fd = ::socket(AF_INET, SOCK_STREAM, 0);
			if (fd == -1)
			{
				log_errno("socket inet");
				return -1;
			}
			int yes = 1;
			setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_ANY);
			addr.sin_port = htons(global_config.port);
			if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
			{
				log_errno("bind inet");
				::close(fd);
				return -1;
			}
		}
		if (::listen(fd, global_config.backlog) == -1)
		{
			log_errno("listen");
			::close(fd);
			return -1;
		}
		if (set_non_blocking(fd) == -1)
		{
			log_errno("nonblock listen");
			::close(fd);
			return -1;
		}
		return fd;
	}

	static int run(int listen_fd)
	{
		int epfd = epoll_create1(0);
		if (epfd == -1)
		{
			log_errno("epoll_create1");
			return 1;
		}
		g_epfd = epfd; // publish for worker wakeups
		epoll_event ev{};
		ev.data.fd = listen_fd;
		ev.events = EPOLLIN | EPOLLET;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1)
		{
			log_errno("epoll_ctl listen");
			::close(epfd);
			g_epfd = -1;
			return 1;
		}
		// Setup periodic timer to ensure progress for waiting connections (100ms interval)
		g_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		if (g_timerfd != -1)
		{
			itimerspec its{}; its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 100 * 1000 * 1000; its.it_value = its.it_interval; // first fire after 100ms
			if (timerfd_settime(g_timerfd, 0, &its, nullptr) == -1)
			{
				log_errno("timerfd_settime");
				::close(g_timerfd); g_timerfd = -1;
			}
			else
			{
				epoll_event tev{}; tev.data.fd = g_timerfd; tev.events = EPOLLIN | EPOLLET;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_timerfd, &tev) == -1)
				{
					log_errno("epoll_ctl timerfd");
					::close(g_timerfd); g_timerfd = -1;
				}
			}
		}
		const int MAX_EVENTS = 64;
		std::vector<epoll_event> events(MAX_EVENTS);
		while (true)
		{
			int n = epoll_wait(epfd, events.data(), MAX_EVENTS, 1000);
			if (n == -1)
			{
				if (errno == EINTR)
					continue;
				log_errno("epoll_wait");
				break;
			}
			for (int i = 0; i < n; ++i)
			{
				int fd = events[i].data.fd;
				uint32_t evs = events[i].events;
				if (fd == listen_fd)
					handle_new_connections(listen_fd, epfd);
				else if (fd == g_timerfd)
				{
					uint64_t expirations; ::read(g_timerfd, &expirations, sizeof(expirations));
					retry_waiting(epfd);
					housekeeping_close_idle(epfd);
				}
				else
					handle_io(fd, evs, epfd);
			}
		}
		::close(epfd);
		g_epfd = -1;
		if (g_timerfd != -1) { ::close(g_timerfd); g_timerfd = -1; }
		return 0;
	}

	int serve(RequestReadyCallback cb)
	{
		int listen_fd = create_listen_socket();
		if (listen_fd == -1)
			return 1;
		init(listen_fd, cb);
		int rc = run(listen_fd);
		for (auto& kv : g_conns)
		{
			for (auto& rk : kv.second.requests)
			{
				if (rk.second)
				{
					if (!(rk.second->flags & Request::RESPONDED))
						finalize_request(*rk.second);
					release_request(rk.second);
				}
			}
			::close(kv.first);
		}
		g_conns.clear();
		::close(listen_fd);
		if (!global_config.unix_path.empty())
			::unlink(global_config.unix_path.c_str());
		return rc;
	}
}
