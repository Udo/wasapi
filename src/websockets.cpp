#include "websockets.h"
#include "config.h"
#include "memory.h"
#include "logger.h"
#include "dynamic_variable.h"
#include "worker.h"
#include "http.h"
#include "fastcgi.h"

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
	// Provide local inline definition to match declaration in http.h (not defined in header)
	inline void trim_spaces(std::string& s)
	{
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
	}

	struct Client
	{
		int fd = -1;
		bool handshake_done = false;
		std::string in_http;
		bool http_mode = false; // true if plain HTTP (non-upgrade)
		bool http_headers_parsed = false;
		size_t http_content_length = 0;
		std::string http_method;
		std::string http_path;
		std::string http_query;
		std::unordered_map<std::string, std::string> http_headers; // lowercase keys
		bool close_after_write = false; // for plain HTTP response
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
		if (fd == -1)
			return -1;
		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
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

	static void schedule_http(RequestReadyCallback cbhttp, Client& c, std::string&& request_text, std::string&& body)
	{
		if (!cbhttp) return;
		// Build Request analogous to FastCGI-populated request
		Arena* a = global_arena_manager.get();
		if (!a) return;
		void* mem = a->alloc(sizeof(Request), alignof(Request));
		if (!mem) { global_arena_manager.release(a); return; }
		Request* r = new (mem) Request(a);
		r->flags |= Request::INITIALIZED;
		// Parse request line
		size_t line_end = request_text.find("\r\n");
		std::string first_line = line_end == std::string::npos ? request_text : request_text.substr(0, line_end);
		{
			size_t msp = first_line.find(' ');
			if (msp != std::string::npos) {
				std::string method = first_line.substr(0, msp);
				size_t psp = first_line.find(' ', msp + 1);
				std::string target = psp == std::string::npos ? std::string() : first_line.substr(msp + 1, psp - (msp + 1));
				size_t q = target.find('?');
				std::string path = q == std::string::npos ? target : target.substr(0, q);
				std::string query = q == std::string::npos ? std::string() : target.substr(q + 1);
				
				// Strip ws_path_prefix if configured and path starts with it
				std::string stripped_target = target;
				std::string stripped_path = path;
				if (!global_config.ws_path_prefix.empty() && 
					target.substr(0, global_config.ws_path_prefix.length()) == global_config.ws_path_prefix) {
					stripped_target = target.substr(global_config.ws_path_prefix.length());
					if (stripped_target.empty()) stripped_target = "/";
					size_t stripped_q = stripped_target.find('?');
					stripped_path = stripped_q == std::string::npos ? stripped_target : stripped_target.substr(0, stripped_q);
				}
				
				r->env["REQUEST_METHOD"] = DynamicVariable::make_string(method);
				r->env["REQUEST_URI"] = DynamicVariable::make_string(stripped_target);
				r->env["PATH_INFO"] = DynamicVariable::make_string(stripped_path);
				r->env["QUERY_STRING"] = DynamicVariable::make_string(query);
			}
		}
		// Headers
		r->env["SERVER_PROTOCOL"] = DynamicVariable::make_string("HTTP/1.1"); // assume
		std::unordered_map<std::string,std::string> headers;
		size_t pos = line_end == std::string::npos ? request_text.size() : line_end + 2;
		while (pos < request_text.size()) {
			size_t next = request_text.find("\r\n", pos);
			if (next == std::string::npos) break;
			if (next == pos) { pos += 2; break; } // end headers
			std::string line = request_text.substr(pos, next - pos);
			pos = next + 2;
			size_t colon = line.find(':');
			if (colon == std::string::npos) continue;
			std::string name = line.substr(0, colon);
			std::string value = line.substr(colon + 1);
			trim_spaces(value);
			std::string lname = name; for (auto& ch: lname) ch = std::toupper((unsigned char)ch);
			std::string env_name = "HTTP_";
			for (char ch: lname) env_name.push_back(ch == '-' ? '_' : ch);
			r->env[env_name] = DynamicVariable::make_string(value);
			headers[name] = value;
		}
		// Copy select headers to canonical CGI vars
		if (auto it = headers.find("Content-Type"); it != headers.end()) r->env["CONTENT_TYPE"] = DynamicVariable::make_string(it->second);
		if (auto it = headers.find("Content-Length"); it != headers.end()) r->env["CONTENT_LENGTH"] = DynamicVariable::make_string(it->second);
		// Body
		r->body = std::move(body);
		r->body_bytes = r->body.size();
		r->flags |= Request::PARAMS_COMPLETE | Request::INPUT_COMPLETE; // no streaming for now
		// Parse query string into params
		parse_query_string(*r, r->env.find("QUERY_STRING"));
		// Cookies
		parse_cookie_header(*r, r->env.find("HTTP_COOKIE"));
		// Form data (json/multipart/urlencoded)
		parse_form_data(*r);
		// Tag origin
		r->env["WS"] = DynamicVariable::make_string("0");
		r->env["CLIENT_FD"] = DynamicVariable::make_string(std::to_string(c.fd));
		::global_worker_pool.enqueue([cbhttp, r, a, fd = c.fd]() {
			// Worker builds FastCGI-style output into resp_fcgi; we adapt to HTTP
			std::vector<uint8_t> resp_fcgi;
			cbhttp(*r, resp_fcgi);
			// Decode FCGI_STDOUT records to aggregate payload
			std::string body;
			const uint8_t* p = resp_fcgi.data();
			const uint8_t* end = resp_fcgi.data() + resp_fcgi.size();
			while (end - p >= (ptrdiff_t)sizeof(fcgi::Header)) {
				fcgi::Header h; std::memcpy(&h, p, sizeof(h));
				if (h.version != fcgi::VERSION_1) break;
				size_t contentLen = (size_t)ntohs(h.contentLength);
				size_t total = sizeof(h) + contentLen + h.paddingLength;
				if ((size_t)(end - p) < total) break;
				if (h.type == fcgi::FCGI_STDOUT && contentLen > 0) {
					body.append(reinterpret_cast<const char*>(p + sizeof(h)), contentLen);
				}
				p += total;
			}
			// If body contains embedded HTTP headers already from output_headers, leave as-is; otherwise add minimal headers
			bool has_http_prefix = body.rfind("HTTP/", 0) == 0;
			std::string payload;
			if (has_http_prefix) {
				payload = std::move(body); // already full response
			} else {
				std::string ct = "text/plain; charset=utf-8";
				const DynamicVariable* hct = r->headers.find("Content-Type");
				if (hct && hct->type == DynamicVariable::STRING) ct = hct->data.s;
				payload = "HTTP/1.1 200 OK\r\nContent-Type: " + ct + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
			}
			std::vector<uint8_t> frame(payload.begin(), payload.end());
			{
				std::lock_guard<std::mutex> lk(g_pending_mutex);
				g_pending_frames.push_back(PendingFrame{fd, std::move(frame)});
				if (g_eventfd != -1) { uint64_t v=1; ssize_t wr = write(g_eventfd, &v, sizeof(v)); (void)wr; }
			}
			r->~Request();
			if (a) global_arena_manager.release(a);
		});
	}

	int serve(int port, const std::string& unix_socket, RequestReadyCallback cbws, RequestReadyCallback cbhttp)
	{
		int listen_fd = -1;
		if (!unix_socket.empty())
			listen_fd = create_unix_listen_socket(unix_socket);
		else
			listen_fd = create_listen_socket(port);
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
						sockaddr_storage addr;
						socklen_t alen = sizeof(addr);
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
							bool is_upgrade = false;
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
								is_upgrade = true;
							}
							if (!is_upgrade)
							{
								// Parse headers to find Content-Length
								c.http_mode = true;
								std::string headers_part = c.in_http.substr(0, hdr_end + 4);
								size_t line_end = headers_part.find("\r\n");
								if (line_end == std::string::npos) { c.closed.store(true); continue; }
								// Find content-length manually
								size_t hpos = 0;
								while (true) {
									size_t lend = headers_part.find("\r\n", hpos);
									if (lend == std::string::npos || lend == hpos) break;
									std::string line = headers_part.substr(hpos, lend - hpos);
									hpos = lend + 2;
									size_t colon = line.find(':');
									if (colon != std::string::npos) {
										std::string name = line.substr(0, colon);
										for (auto &ch: name) ch = std::tolower((unsigned char)ch);
										if (name == "content-length") {
											std::string val = line.substr(colon+1); trim_spaces(val); c.http_content_length = (size_t)std::strtoull(val.c_str(), nullptr, 10); break; }
									}
								}
								c.http_headers_parsed = true;
								// Move any body bytes already read (after headers) into in_buf
								size_t already = c.in_http.size() - (hdr_end + 4);
								if (already) {
									std::string tail = c.in_http.substr(hdr_end + 4);
									c.in_buf.insert(c.in_buf.end(), tail.begin(), tail.end());
								}
								// If full body present (or none expected) schedule immediately
								if (c.in_buf.size() >= c.http_content_length) {
									std::string body;
									if (c.http_content_length) body.assign((char*)c.in_buf.data(), c.http_content_length);
									schedule_http(cbhttp, c, std::move(c.in_http), std::move(body));
									c.in_buf.clear();
									// close deferred via close_after_write
								}
							}
						}
						else if (c.http_mode && c.http_headers_parsed)
						{
							// Accumulate until content-length reached
							if (c.in_buf.size() >= c.http_content_length)
							{
								std::string body;
								if (c.http_content_length)
									body.assign((char*)c.in_buf.data(), c.http_content_length);
								schedule_http(cbhttp, c, std::move(c.in_http), std::move(body));
								c.in_buf.clear();
								// close deferred via close_after_write
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
									schedule_message(cbws, c, opcode, std::move(payload));
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
										schedule_message(cbws, c, final_opcode, std::move(complete));
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
				if (c.closed.load() || (c.close_after_write && c.out_buf.empty()))
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
