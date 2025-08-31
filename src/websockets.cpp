#include "websockets.h"
#include "config.h"
#include "memory.h"
#include "logger.h"
#include "dynamic_variable.h"
#include "worker.h"
#include "http.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/un.h>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <string>
#include <algorithm>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <mutex>

namespace ws
{

	struct Client
	{
		int fd = -1;
		bool handshake_done = false;
		std::string in_http;
		std::vector<uint8_t> in_buf;
		std::vector<uint8_t> out_buf; // guarded by IO thread only; workers queue via pending list
		std::atomic<bool> closed{ false };
		bool assembling = false;
		uint8_t assemble_opcode = 0; // original opcode (text/binary)
		std::vector<uint8_t> assemble_data;
	};

	struct PendingFrame
	{
		int fd;
		std::vector<uint8_t> frame; // already encoded websocket frame
	};

	static int g_eventfd = -1; // notify IO thread of pending frames
	static std::mutex g_pending_mutex;
	static std::vector<PendingFrame> g_pending_frames;

	static int set_non_block(int fd)
	{
		int f = fcntl(fd, F_GETFL, 0);
		if (f == -1)
			return -1;
		return fcntl(fd, F_SETFL, f | O_NONBLOCK);
	}

	static int create_listen_socket(int port)
	{
		int fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (fd == -1)
			return -1;
		int yes = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(port);
		if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) == -1)
		{
			::close(fd);
			return -1;
		}
		if (::listen(fd, 128) == -1)
		{
			::close(fd);
			return -1;
		}
		set_non_block(fd);
		return fd;
	}

	static void send_all(int fd, std::vector<uint8_t>& buf)
	{
		while (!buf.empty())
		{
			ssize_t n = ::send(fd, buf.data(), buf.size(), 0);
			if (n > 0)
				buf.erase(buf.begin(), buf.begin() + n);
			else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return;
			else
			{
				buf.clear();
				return;
			}
		}
	}

	static std::vector<uint8_t> build_ws_frame(uint8_t opcode, const uint8_t* payload, size_t len)
	{
		std::vector<uint8_t> out;
		out.push_back((uint8_t)(0x80 | (opcode & 0x0F)));
		if (len < 126)
		{
			out.push_back((uint8_t)len);
		}
		else if (len <= 0xFFFF)
		{
			out.push_back(126);
			uint16_t n = htons((uint16_t)len);
			uint8_t* p = reinterpret_cast<uint8_t*>(&n);
			out.insert(out.end(), p, p + 2);
		}
		else
		{
			out.push_back(127);
			uint64_t n = htobe64((uint64_t)len);
			uint8_t* p = reinterpret_cast<uint8_t*>(&n);
			out.insert(out.end(), p, p + 8);
		}
		out.insert(out.end(), payload, payload + len);
		return out;
	}

	static void schedule_message(RequestReadyCallback cb, Client& c, uint8_t opcode, std::vector<uint8_t>&& data)
	{
		Arena* a = global_arena_manager.get();
		if (!a)
			return; // backpressure: drop if no arena
		void* mem = a->alloc(sizeof(Request), alignof(Request));
		if (!mem)
		{
			global_arena_manager.release(a);
			return;
		}
		Request* r = new (mem) Request(a);
		r->id = 0;
		r->body.assign(reinterpret_cast<const char*>(data.data()), data.size()); // binary safe
		r->body_bytes = data.size();
		r->env["WS"] = DynamicVariable::make_string("1");
		r->env["MESSAGE_TYPE"] = DynamicVariable::make_string(opcode == 0x2 ? "binary" : "text");
		r->env["OPCODE"] = DynamicVariable::make_string(std::to_string(opcode));
		r->env["CLIENT_FD"] = DynamicVariable::make_string(std::to_string(c.fd));
		r->flags |= Request::INITIALIZED | Request::PARAMS_COMPLETE | Request::INPUT_COMPLETE;
		::global_worker_pool.enqueue([cb, r, a, fd = c.fd, opcode]()
									 {
		std::vector<uint8_t> resp;
		if (cb) cb(*r, resp);
		std::vector<uint8_t> frame;
		if (!resp.empty())
			frame = build_ws_frame(opcode, resp.data(), resp.size());
		if (!frame.empty())
		{
			std::lock_guard<std::mutex> lk(g_pending_mutex);
			g_pending_frames.push_back(PendingFrame{fd, std::move(frame)});
			if (g_eventfd != -1)
			{
				uint64_t v=1; ssize_t wr = write(g_eventfd, &v, sizeof(v)); (void)wr;
			}
		}
		r->~Request();
		if (a) global_arena_manager.release(a); });
	}

	static bool parse_http_headers(const std::string& http, std::string& key, std::string& response_key)
	{
		size_t pos = http.find("Sec-WebSocket-Key:");
		if (pos == std::string::npos)
			return false;
		pos += 18;
		while (pos < http.size() && (http[pos] == ' ' || http[pos] == '\t'))
			++pos;
		size_t end = http.find('\r', pos);
		if (end == std::string::npos)
			return false;
		key = http.substr(pos, end - pos);
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
			key.pop_back();
		std::string accept_src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		uint8_t sha[SHA_DIGEST_LENGTH];
		SHA1(reinterpret_cast<const uint8_t*>(accept_src.data()), accept_src.size(), sha);
		response_key = base64_encode(sha, SHA_DIGEST_LENGTH);
		return true;
	}

	static int create_unix_listen_socket(const std::string& path)
	{
		::unlink(path.c_str());
		int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd == -1) return -1;
		sockaddr_un addr{}; addr.sun_family = AF_UNIX;
		std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
		if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) == -1) { ::close(fd); return -1; }
		if (::listen(fd, 128) == -1) { ::close(fd); return -1; }
		set_non_block(fd);
		return fd;
	}

	int serve(int port, const std::string& unix_socket, RequestReadyCallback cb)
	{
		int listen_fd = -1;
		if (!unix_socket.empty()) listen_fd = create_unix_listen_socket(unix_socket);
		else listen_fd = create_listen_socket(port);
		if (listen_fd == -1)
		{
			log_error("websocket listen failed");
			return 1;
		}
		{
			std::string addr = unix_socket.empty() ? (std::string("tcp:") + std::to_string(port)) : unix_socket;
			log_info("Websocket server listening on %s", addr.c_str());
		}
		int epfd = epoll_create1(0);
		if (epfd == -1)
		{
			::close(listen_fd);
			return 1;
		}
		epoll_event lev{};
		lev.data.fd = listen_fd;
		lev.events = EPOLLIN | EPOLLET;
		epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &lev);
		g_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (g_eventfd != -1)
		{
			epoll_event eev{};
			eev.data.fd = g_eventfd;
			eev.events = EPOLLIN | EPOLLET;
			epoll_ctl(epfd, EPOLL_CTL_ADD, g_eventfd, &eev);
		}
		std::unordered_map<int, Client> clients;
		const int MAX_EVENTS = 64;
		std::vector<epoll_event> events(MAX_EVENTS);
		while (true)
		{
			int n = epoll_wait(epfd, events.data(), MAX_EVENTS, 1000);
			if (n == -1)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			for (int i = 0; i < n; ++i)
			{
				int fd = events[i].data.fd;
				uint32_t ev = events[i].events;
				if (fd == listen_fd)
				{
					while (true)
					{
						sockaddr_storage addr; socklen_t alen = sizeof(addr);
						int cfd = ::accept(listen_fd, (sockaddr*)&addr, &alen);
						if (cfd == -1)
						{
							if (errno == EAGAIN || errno == EWOULDBLOCK)
								break;
							else
								break;
						}
						set_non_block(cfd);
						epoll_event cev{};
						cev.data.fd = cfd;
						cev.events = EPOLLIN | EPOLLET;
						epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
						clients[cfd].fd = cfd;
					}
					continue;
				}
				if (fd == g_eventfd)
				{
					uint64_t val;
					while (::read(g_eventfd, &val, sizeof(val)) > 0)
					{
					}
					std::vector<PendingFrame> local;
					{
						std::lock_guard<std::mutex> lk(g_pending_mutex);
						local.swap(g_pending_frames);
					}
					for (auto& pf : local)
					{
						auto itc = clients.find(pf.fd);
						if (itc == clients.end())
							continue;
						Client& cc = itc->second;
						cc.out_buf.insert(cc.out_buf.end(), pf.frame.begin(), pf.frame.end());
						epoll_event mod{};
						mod.data.fd = cc.fd;
						mod.events = EPOLLIN | EPOLLOUT | EPOLLET;
						epoll_ctl(epfd, EPOLL_CTL_MOD, cc.fd, &mod);
					}
					continue;
				}
				auto it = clients.find(fd);
				if (it == clients.end())
					continue;
				Client& c = it->second;
				if (ev & (EPOLLHUP | EPOLLERR))
				{
					c.closed.store(true);
				}
				if (ev & EPOLLIN)
				{
					while (true)
					{
						uint8_t buf[4096];
						ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
						if (r > 0)
							c.in_buf.insert(c.in_buf.end(), buf, buf + r);
						else if (r == 0)
						{
							c.closed.store(true);
							break;
						}
						else
						{
							if (errno == EAGAIN || errno == EWOULDBLOCK)
								break;
							c.closed.store(true);
							break;
						}
					}
					if (!c.handshake_done)
					{
						c.in_http.append((char*)c.in_buf.data(), c.in_buf.size());
						c.in_buf.clear();
						size_t hdr_end = c.in_http.find("\r\n\r\n");
						if (hdr_end != std::string::npos)
						{
							std::string key, accept_key;
							if (parse_http_headers(c.in_http.substr(0, hdr_end + 4), key, accept_key))
							{
								std::string response =
									"HTTP/1.1 101 Switching Protocols\r\n"
									"Upgrade: websocket\r\n"
									"Connection: Upgrade\r\n"
									"Sec-WebSocket-Accept: " +
									accept_key + "\r\n\r\n";
								c.out_buf.insert(c.out_buf.end(), response.begin(), response.end());
								c.handshake_done = true;
							}
							else
							{
								c.closed.store(true);
							}
						}
					}
					if (c.handshake_done)
					{
						while (true)
						{
							if (c.in_buf.size() < 2)
								break;
							uint8_t b0 = c.in_buf[0];
							uint8_t b1 = c.in_buf[1];
							bool fin = (b0 & 0x80) != 0;
							uint8_t opcode = b0 & 0x0F;
							bool masked = (b1 & 0x80) != 0;
							size_t payload_len = b1 & 0x7F;
							size_t header_len = 2;
							if (payload_len == 126)
							{
								if (c.in_buf.size() < 4)
									break;
								payload_len = (c.in_buf[2] << 8) | c.in_buf[3];
								header_len = 4;
							}
							else if (payload_len == 127)
							{
								if (c.in_buf.size() < 10)
									break;
								payload_len = 0;
								for (int k = 0; k < 8; ++k)
									payload_len = (payload_len << 8) | c.in_buf[2 + k];
								header_len = 10;
							}
							size_t mask_len = masked ? 4 : 0;
							if (c.in_buf.size() < header_len + mask_len + payload_len)
								break;
							const uint8_t* mask = masked ? &c.in_buf[header_len] : nullptr;
							const uint8_t* data = &c.in_buf[header_len + mask_len];
							std::vector<uint8_t> payload;
							payload.assign(data, data + payload_len);
							if (masked)
								for (size_t k = 0; k < payload_len; ++k)
									payload[k] ^= mask[k % 4];
							if (opcode == 0x8) // close
							{
								c.closed.store(true);
							}
							else if (opcode == 0x9) // ping
							{
								std::vector<uint8_t> pong = build_ws_frame(0xA, payload.data(), payload.size());
								c.out_buf.insert(c.out_buf.end(), pong.begin(), pong.end());
							}
							else if (opcode == 0xA)
							{
							}
							else if (opcode == 0x1 || opcode == 0x2)
							{
								if (c.assembling)
								{
									c.assemble_data.clear();
									c.assembling = false;
								}
								if (fin)
								{
									schedule_message(cb, c, opcode, std::move(payload));
								}
								else
								{
									c.assembling = true;
									c.assemble_opcode = opcode;
									c.assemble_data = std::move(payload);
								}
							}
							else if (opcode == 0x0) // continuation
							{
								if (!c.assembling)
								{
									c.closed.store(true);
								}
								else
								{
									c.assemble_data.insert(c.assemble_data.end(), payload.begin(), payload.end());
									if (fin)
									{
										uint8_t final_opcode = c.assemble_opcode;
										std::vector<uint8_t> complete;
										complete.swap(c.assemble_data);
										c.assembling = false;
										schedule_message(cb, c, final_opcode, std::move(complete));
									}
								}
							}
							c.in_buf.erase(c.in_buf.begin(), c.in_buf.begin() + header_len + mask_len + payload_len);
						}
					}
				}
				if (ev & EPOLLOUT)
				{
					send_all(fd, c.out_buf);
				}
				send_all(fd, c.out_buf);
				if (c.out_buf.empty())
				{
					epoll_event mod{};
					mod.data.fd = fd;
					mod.events = EPOLLIN | EPOLLET;
					epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &mod);
				}
				else
				{
					epoll_event mod{};
					mod.data.fd = fd;
					mod.events = EPOLLIN | EPOLLOUT | EPOLLET;
					epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &mod);
				}
				if (c.closed.load())
				{
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
					::close(fd);
					clients.erase(it);
				}
			}
		}
		if (g_eventfd != -1)
		{
			::close(g_eventfd);
			g_eventfd = -1;
		}
		::close(epfd);
		::close(listen_fd);
		return 0;
	}

} // namespace ws
