#include "fcgi-connection.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
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
		std::vector<uint8_t> out_buf; // outbound data buffer (IO thread only)
		size_t out_pos = 0; // bytes already sent from start of out_buf (IO thread only)
		std::unordered_map<uint16_t, Request*> requests; // managed via arenas
		std::atomic<bool> closed{ false }; // accessed from IO + worker threads
		bool waiting_for_arena = false; // BEGIN_REQUEST record deferred (legacy helper flag)
		std::atomic<int> active_workers{ 0 };
		uint32_t epoll_mask = EPOLLIN | EPOLLET; // currently registered interest mask
		bool want_write_interest = false; // desired EPOLLOUT interest
	};

	static std::unordered_map<int, Connection> g_conns;
	static RequestReadyCallback g_user_request_ready = nullptr;
	static int g_epfd = -1; // epoll fd for worker-triggered wakeups
	static int g_timerfd = -1; // periodic housekeeping timer
	static std::vector<int> g_close_queue; // deferred closes
	static std::vector<Request*> g_pending_output; // requests ready for output assembly
	static std::mutex g_pending_output_mutex; // protects g_pending_output
	static int g_listen_fd = -1; // current listening socket (for pausing/resuming accept)
	static bool g_accept_paused = false; // whether accept() is currently paused (socket removed from epoll)
	static std::deque<int> g_waiting_conns; // connections waiting for arena allocation

	static void resume_accept(); // fwd
	static void pause_accept(); // fwd
	static int g_eventfd = -1; // signals IO thread about pending work
	static Request* allocate_request(uint16_t id);
	static void flush_connection(Connection& c, int epfd);
	static void internal_on_request_ready(Request& r);
	static bool should_close_connection(Connection& c); // forward
	static void finalize_request(Request& req); // forward (already defined later)
	static void release_request(Request* r); // forward
	static void maybe_update_epoll(Connection& c, int epfd, uint32_t desired); // forward
	static void close_connection(int fd, int epfd); // forward
	static void cleanup_connection_requests(Connection& c); // forward
	static void process_fcgi(Connection& c); // forward
	static inline void update_write_interest(Connection& c, int epfd, bool want);
	static inline bool modify_listen_interest(bool add); // add/remove listen fd from epoll
	thread_local Connection* tls_io_connection = nullptr;

	static void process_waiting_connections()
	{
		int budget = (int)global_arena_manager.available_count.load(std::memory_order_relaxed);
		if (budget <= 0 || g_waiting_conns.empty())
			return;
		size_t initial = g_waiting_conns.size();
		for (size_t i = 0; i < initial && budget > 0 && !g_waiting_conns.empty(); ++i)
		{
			int fd = g_waiting_conns.front();
			g_waiting_conns.pop_front();
			auto it = g_conns.find(fd);
			if (it == g_conns.end())
				continue; // connection gone
			Connection& c = it->second;
			if (c.closed.load(std::memory_order_relaxed))
				continue; // will be closed soon
			bool was_waiting = c.waiting_for_arena;
			process_fcgi(c);
			if (was_waiting && !c.waiting_for_arena)
			{
				--budget;
				flush_connection(c, g_epfd);
			}
			else if (c.waiting_for_arena)
			{
				g_waiting_conns.push_back(fd);
			}
		}
	}

	static inline void update_write_interest(Connection& c, int epfd, bool want)
	{
		uint32_t base = EPOLLIN | EPOLLET;
		uint32_t desired = want ? (base | EPOLLOUT) : base;
		bool have = (c.epoll_mask & EPOLLOUT) != 0;
		if (have == want)
			return;
		maybe_update_epoll(c, epfd, desired);
		c.want_write_interest = want;
	}

	static inline bool modify_listen_interest(bool add)
	{
		if (g_epfd == -1 || g_listen_fd == -1)
			return false;
		if (add)
		{
			epoll_event ev{};
			ev.data.fd = g_listen_fd;
			ev.events = EPOLLIN | EPOLLET;
			if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ev) == -1)
			{
				log_error("epoll_ctl ADD listen: %s", std::strerror(errno));
				return false;
			}
		}
		else
		{
			if (epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_listen_fd, nullptr) == -1)
			{
				log_error("epoll_ctl DEL listen: %s", std::strerror(errno));
				return false;
			}
		}
		return true;
	}

	static void process_fcgi(Connection& c)
	{
		if (c.closed.load(std::memory_order_relaxed))
			return;
		bool waiting = false;
		tls_io_connection = &c;
		auto status = fcgi::process_buffer(c.in_buf, c.requests, c.out_buf, allocate_request, internal_on_request_ready, waiting);
		tls_io_connection = nullptr;
		if (status == fcgi::CLOSE)
			c.closed.store(true, std::memory_order_relaxed);
		c.waiting_for_arena = waiting;
		if (waiting)
		{
		}
	}

	static void close_connection(int fd, int epfd)
	{
		auto it = g_conns.find(fd);
		if (it == g_conns.end())
			return;
		Connection& c = it->second;
		cleanup_connection_requests(c);
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
		::close(fd);
		log_debug("Closed fd=%d", fd);
		g_conns.erase(it);
	}

	static void cleanup_connection_requests(Connection& c)
	{
		for (auto& rk : c.requests)
		{
			if (rk.second)
			{
				if (!(rk.second->flags & Request::RESPONDED))
					finalize_request(*rk.second);
				release_request(rk.second);
			}
		}
		c.requests.clear();
	}

	static void housekeeping_close_idle(int epfd)
	{
		std::vector<int> to_close;
		to_close.reserve(g_conns.size());
		for (auto& kv : g_conns)
		{
			Connection& c = kv.second;
			if (!c.requests.empty())
			{
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				double now = ts.tv_sec + ts.tv_nsec / 1e9;
				for (auto& rk : c.requests)
				{
					Request* rp = rk.second;
					if (!rp)
						continue;
					if (!(rp->flags & Request::RESPONDED) && global_config.max_request_time > 0 && (now - rp->start_time_sec) > global_config.max_request_time)
					{
						rp->flags |= Request::FAILED;
						rp->flags |= Request::RESPONDED;
						fcgi::append_end_request(c.out_buf, rp->id, 0, fcgi::OVERLOADED);
					}
				}
			}
			if (should_close_connection(c))
				to_close.push_back(kv.first);
		}
		for (int fd : to_close)
		{
			auto it = g_conns.find(fd);
			if (it == g_conns.end())
				continue;
			Connection& c = it->second;
			cleanup_connection_requests(c);
			epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
			::close(fd);
			log_debug("Closed fd=%d (housekeeping)", fd);
			g_conns.erase(it);
		}
	}

	static void finalize_request(Request& req);
	thread_local Connection* tls_current_connection = nullptr;

	static void internal_on_request_ready(Request& r)
	{
		if (r.flags & Request::RESPONDED)
			return;

		Connection* c = static_cast<Connection*>(r.conn_ptr);
		if (!c)
			return;

		c->active_workers.fetch_add(1, std::memory_order_relaxed);
		r.worker_active.store(true, std::memory_order_release);

		Request* rp_capture = &r;
		Connection* c_capture = c;
		::global_worker_pool.enqueue([rp_capture, c_capture]
									 {
			Request* rp = rp_capture;
			Connection* cp = c_capture;
			if (!rp || !cp) return;
			if (!(rp->flags & Request::RESPONDED) && !cp->closed.load(std::memory_order_relaxed))
			{
				parse_endpoint_file(*rp, rp->env.find(global_config.endpoint_file_path));
				parse_cookie_header(*rp, rp->env.find(global_config.http_cookies_var));
				parse_query_string(*rp, rp->env.find(global_config.http_query_var));
				parse_form_data(*rp);
				if (global_config.session_auto_load)
				{
					DynamicVariable* sid = rp->cookies.find(global_config.session_cookie_name);
					if (sid && sid->type == DynamicVariable::STRING)
					{
						session_start(*rp);
					}
				}
				rp->headers["Content-Type"] = global_config.default_content_type;

				std::lock_guard<std::mutex> lk(g_pending_output_mutex);
				g_pending_output.push_back(rp);
				if (g_eventfd != -1)
				{
					uint64_t val = 1;
					ssize_t wr = write(g_eventfd, &val, sizeof(val));
					(void)wr; // best-effort wakeup
				}
			}
			rp->worker_active.store(false, std::memory_order_release);
			cp->active_workers.fetch_sub(1, std::memory_order_relaxed); });
	}

	static void log_errno(const char* msg)
	{
		log_error("%s: %s", msg, std::strerror(errno));
	}

	static void process_pending_output()
	{
		std::vector<Request*> pending;
		{
			std::lock_guard<std::mutex> lk(g_pending_output_mutex);
			pending.swap(g_pending_output);
		}

		for (Request* r : pending)
		{
			if (!r || (r->flags & Request::RESPONDED))
				continue;

			Connection* c = static_cast<Connection*>(r->conn_ptr);
			if (!c || c->closed.load(std::memory_order_relaxed))
				continue;

			std::vector<uint8_t> local_out;
			local_out.reserve(1024);

			if (g_user_request_ready)
			{
				tls_current_connection = c;
				g_user_request_ready(*r, local_out);
				tls_current_connection = nullptr;
			}

			if (!local_out.empty())
			{
				bool was_empty = (c->out_pos == c->out_buf.size());
				if (was_empty && c->out_buf.capacity() == 0)
					c->out_buf.reserve(global_config.output_buffer_initial);
				c->out_buf.insert(c->out_buf.end(), local_out.begin(), local_out.end());

				if (was_empty && g_epfd != -1)
					update_write_interest(*c, g_epfd, true);
			}
		}
	}

	static void maybe_update_epoll(Connection& c, int epfd, uint32_t desired)
	{
		if (desired == c.epoll_mask)
			return;
		epoll_event ev{};
		ev.data.fd = c.fd;
		ev.events = desired;
		epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev);
		c.epoll_mask = desired;
	}

	static void flush_connection(Connection& c, int epfd)
	{
		while (true)
		{
			size_t remaining = c.out_pos < c.out_buf.size() ? (c.out_buf.size() - c.out_pos) : 0;
			if (remaining == 0)
			{
				update_write_interest(c, epfd, false);
				if (c.out_pos != 0)
				{
					c.out_buf.clear();
					c.out_pos = 0;
				}
				if (should_close_connection(c))
					g_close_queue.push_back(c.fd);
				return;
			}
			ssize_t n = ::send(c.fd, c.out_buf.data() + c.out_pos, remaining, 0);
			if (n > 0)
			{
				c.out_pos += (size_t)n;
				continue;
			}
			if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				update_write_interest(c, epfd, true);
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
		if (g_accept_paused && global_arena_manager.available_count.load(std::memory_order_relaxed) > 0)
			resume_accept();
		process_waiting_connections();
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
		bool any_keep = false;
		for (auto& kv : c.requests)
		{
			Request* rp = kv.second;
			if (!rp || !(rp->flags & Request::RESPONDED))
			{
				all_responded = false;
				break;
			}
			if (rp->flags & Request::KEEP_CONNECTION)
			{
				any_keep = true;
				break; // no need to continue scanning
			}
		}
		return all_responded && !any_keep && (c.out_pos == c.out_buf.size()) && c.active_workers.load(std::memory_order_relaxed) == 0;
	}

	static void init(int listen_fd, RequestReadyCallback cb)
	{
		(void)listen_fd;
		g_user_request_ready = cb;
		g_listen_fd = listen_fd;
		auto& G = global_config;
		std::string addr = G.fcgi_socket_path.empty() ? (std::string("tcp:") + std::to_string(G.fcgi_port)) : G.fcgi_socket_path;
		log_info("fastCGI server listening on %s", addr.c_str());
	}

	static void pause_accept()
	{
		if (g_accept_paused)
			return;
		if (modify_listen_interest(false))
			log_debug("Paused accept() (no arenas) fd=%d", g_listen_fd);
		g_accept_paused = true;
	}

	static void resume_accept()
	{
		if (!g_accept_paused)
			return;
		if (modify_listen_interest(true))
			log_debug("Resumed accept() fd=%d", g_listen_fd);
		g_accept_paused = false;
	}

	static void handle_new_connections(int listen_fd, int epfd)
	{
		if (global_arena_manager.available_count.load(std::memory_order_relaxed) == 0)
		{
			pause_accept();
			return;
		}
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
			epoll_event cev{};
			cev.data.fd = cfd;
			cev.events = EPOLLIN | EPOLLET;
			epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
			Connection& c = g_conns[cfd];
			c.fd = cfd;
			log_debug("Accepted fd=%d", cfd);
			if (global_arena_manager.available_count.load(std::memory_order_relaxed) == 0)
			{
				pause_accept();
				break;
			}
		}
	}

	static void handle_io(int fd, uint32_t events, int epfd)
	{
		auto it = g_conns.find(fd);
		if (it == g_conns.end())
			return;

		Connection& c = it->second;
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
					c.in_buf.insert(c.in_buf.end(), buf, buf + r);
				else if (r == 0)
				{
					c.closed.store(true, std::memory_order_relaxed);
					break;
				}
				else
				{
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					log_errno("recv");
					c.closed.store(true, std::memory_order_relaxed);
					break;
				}
			}
			bool prev_wait = c.waiting_for_arena;
			process_fcgi(c);
			if (!prev_wait && c.waiting_for_arena)
				g_waiting_conns.push_back(fd);
			flush_connection(c, epfd);
		}

		if (events & EPOLLOUT)
		{
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
			std::vector<int> local;
			local.swap(g_close_queue);
			for (int cfd : local)
				if (should_close_connection(g_conns[cfd]))
					close_connection(cfd, epfd);
		}
	}

	static int create_listen_socket()
	{
		int fd;
		if (!global_config.fcgi_socket_path.empty())
		{
			::unlink(global_config.fcgi_socket_path.c_str());
			fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
			if (fd == -1)
			{
				log_errno("socket unix");
				return -1;
			}
			mode_t old_umask = umask(0);
			sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", global_config.fcgi_socket_path.c_str());
			if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
			{
				log_errno("bind unix");
				umask(old_umask);
				::close(fd);
				return -1;
			}
			umask(old_umask);
			if (::chmod(global_config.fcgi_socket_path.c_str(), 0777) == -1)
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
			addr.sin_port = htons(global_config.fcgi_port);
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

		g_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (g_eventfd != -1)
		{
			epoll_event efd_ev{};
			efd_ev.data.fd = g_eventfd;
			efd_ev.events = EPOLLIN | EPOLLET;
			if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_eventfd, &efd_ev) == -1)
			{
				log_errno("epoll_ctl eventfd");
				::close(g_eventfd);
				g_eventfd = -1;
			}
		}

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
		g_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		if (g_timerfd != -1)
		{
			itimerspec its{};
			its.it_interval.tv_sec = 0;
			its.it_interval.tv_nsec = 100 * 1000 * 1000;
			its.it_value = its.it_interval; // first fire after 100ms
			if (timerfd_settime(g_timerfd, 0, &its, nullptr) == -1)
			{
				log_errno("timerfd_settime");
				::close(g_timerfd);
				g_timerfd = -1;
			}
			else
			{
				epoll_event tev{};
				tev.data.fd = g_timerfd;
				tev.events = EPOLLIN | EPOLLET;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_timerfd, &tev) == -1)
				{
					log_errno("epoll_ctl timerfd");
					::close(g_timerfd);
					g_timerfd = -1;
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
				else if (fd == g_eventfd)
				{
					uint64_t val;
					ssize_t ret = ::read(g_eventfd, &val, sizeof(val)); // drain eventfd
					(void)ret; // suppress unused variable warning
					process_pending_output();
				}
				else if (fd == g_timerfd)
				{
					uint64_t expirations;
					ssize_t ret = ::read(g_timerfd, &expirations, sizeof(expirations));
					(void)ret; // suppress unused variable warning
					housekeeping_close_idle(epfd);
				}
				else
					handle_io(fd, evs, epfd);
			}
		}
		::close(epfd);
		g_epfd = -1;
		if (g_eventfd != -1)
		{
			::close(g_eventfd);
			g_eventfd = -1;
		}
		if (g_timerfd != -1)
		{
			::close(g_timerfd);
			g_timerfd = -1;
		}
		return 0;
	}

	int serve(int port, const std::string& unix_socket, RequestReadyCallback cb)
	{
		// Override global_config temporarily for listener creation (non-thread-safe config mutating avoided by local copy of needed fields)
		uint16_t saved_port = global_config.fcgi_port;
		std::string saved_path = global_config.fcgi_socket_path;
		// Set globals used by create_listen_socket() path
		global_config.fcgi_port = (uint16_t)port;
		global_config.fcgi_socket_path = unix_socket;
		int listen_fd = create_listen_socket();
		// restore
		global_config.fcgi_port = saved_port;
		global_config.fcgi_socket_path = saved_path;
		if (listen_fd == -1)
			return 1;
		init(listen_fd, cb);
		int rc = run(listen_fd);
		for (auto& kv : g_conns)
		{
			cleanup_connection_requests(kv.second);
			::close(kv.first);
		}
		g_conns.clear();
		::close(listen_fd);
		if (!unix_socket.empty())
			::unlink(unix_socket.c_str());
		return rc;
	}
}
