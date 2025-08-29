#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
//#include <algorithm>
#include <sstream>
#include "fastcgi.h"
#include "http.h"
#include "config.h"
#include "logger.h"
#include "memory.h"
#include "session.h"
#include <functional>

static int set_non_blocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -1;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		return -1;
	return 0;
}

static void log_errno(const char* msg)
{
	log_error("%s: %s", msg, std::strerror(errno));
}

struct Connection
{
	int fd = -1;
	std::vector<uint8_t> in_buf;
	std::vector<uint8_t> out_buf;
	std::unordered_map<uint16_t, fcgi::Request> requests; 
	bool closed = false;
};

static void print_any_limited(std::ostringstream& oss, const DynamicVariable& v, size_t limit, int indent, int depth = 0)
{
	auto ind = [&](int d){ oss << std::string(d * indent, ' '); };
	switch (v.type)
	{
		case DynamicVariable::NIL: oss << "null\n"; break;
		case DynamicVariable::STRING: oss << '"' << v.s << '"' << "\n"; break;
		case DynamicVariable::NUMBER: oss << v.num << "\n"; break;
		case DynamicVariable::BOOL: oss << (v.b?"true":"false") << "\n"; break;
		case DynamicVariable::BINARY: oss << "<binary:" << v.bin.size() << ">\n"; break;
		case DynamicVariable::ARRAY:
			oss << "[\n";
			if (!v.a.empty())
			{
				size_t printed = 0;
				for (size_t i = 0; i < v.a.size(); ++i)
				{
					if (limit && printed >= limit)
					{
						ind(depth+1); oss << "... (truncated)\n"; break;
					}
					ind(depth+1);
					print_any_limited(oss, v.a[i], 0, indent, depth+1);
					++printed;
				}
			}
			ind(depth); oss << "]\n"; break;
		case DynamicVariable::OBJECT:
			oss << "{\n";
			if (!v.o.empty())
			{
				size_t printed = 0;
				for (auto it = v.o.begin(); it != v.o.end(); ++it)
				{
					if (limit && printed >= limit)
					{
						ind(depth+1); oss << "... (truncated)\n"; break;
					}
					ind(depth+1); oss << it->first << ": ";
					print_any_limited(oss, it->second, 0, indent, depth+1); 
					++printed;
				}
			}
			ind(depth); oss << "}\n"; break;
	}
}

void parse_cookie_header(fcgi::Request& r, DynamicVariable* cookie_var)
{
	std::string cookie_string = cookie_var ? cookie_var->as<std::string>() : "";
	if (r.cookies.type != DynamicVariable::OBJECT)
		r.cookies = DynamicVariable::make_object();
	if (!cookie_string.empty())
	{
		size_t pos = 0;
		while (pos < cookie_string.size())
		{
			// find segment up to ';'
			size_t semi = cookie_string.find(';', pos);
			if (semi == std::string::npos) semi = cookie_string.size();
			std::string segment = cookie_string.substr(pos, semi - pos);
			pos = semi + 1; // advance
			// trim leading/trailing spaces
			auto trim = [](std::string& s)
			{
				while (!s.empty() && (unsigned char)s.front() <= ' ') s.erase(s.begin());
				while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
			};
			trim(segment);
			if (segment.empty()) continue;
			size_t eq = segment.find('=');
			std::string key;
			std::string value;
			if (eq == std::string::npos)
			{
				key = segment; // flag cookie gets empty value
			}
			else
			{
				key = segment.substr(0, eq);
				value = segment.substr(eq + 1);
				trim(key);
				trim(value);
				// Remove optional surrounding quotes
				if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
					value = value.substr(1, value.size() - 2);
			}
			if (!key.empty())
				r.cookies[key] = DynamicVariable::make_string(value);
		}
	}
}

void parse_query_string(fcgi::Request& r, DynamicVariable* query_string)
{
	std::string qs = query_string ? query_string->as<std::string>() : "";
	if (r.params.type != DynamicVariable::OBJECT)
		r.params = DynamicVariable::make_object();
	std::unordered_map<std::string, std::string> tmp;
	parse_query_string(qs, tmp);
	for (auto& kv : tmp)
		r.params[kv.first] = DynamicVariable::make_string(kv.second);
}

void parse_json_form_data(fcgi::Request& r)
{
	std::string body_str(reinterpret_cast<const char*>(r.body.data()), r.body.size());
	DynamicVariable parsed;
	size_t errpos = 0;
	if (parse_json(body_str, parsed, &errpos))
	{
		if (parsed.type == DynamicVariable::OBJECT)
		{
			if (r.params.type != DynamicVariable::OBJECT)
				r.params = DynamicVariable::make_object();
			for (auto& kv : parsed.o)
				r.params[kv.first] = kv.second;
		}
		else
		{
			if (r.params.type != DynamicVariable::OBJECT)
				r.params = DynamicVariable::make_object();
			r.params["_json"] = parsed;
		}
	}
	else
	{
		if (r.params.type != DynamicVariable::OBJECT)
			r.params = DynamicVariable::make_object();
		r.params["_json_error"] = DynamicVariable::make_string("parse error at position " + std::to_string(errpos));
	}
}

void parse_multipart_form_data(fcgi::Request& r)
{
	std::string body_str(reinterpret_cast<const char*>(r.body.data()), r.body.size());
	const DynamicVariable* it_ct = r.env.find("CONTENT_TYPE");
	if (!it_ct || it_ct->type != DynamicVariable::STRING) return;
	std::string ct = it_ct->s;
	std::string lct = ct; for (auto& c : lct) c = std::tolower(c);
	std::string boundary;
	std::string key = "boundary=";
	size_t bpos = lct.find(key);
	if (bpos != std::string::npos)
		boundary = ct.substr(bpos + key.size());
	if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"' && boundary.size() >= 2)
		boundary = boundary.substr(1, boundary.size() - 2);
	if (boundary.empty()) return;
	std::unordered_map<std::string, std::string> tmp;
	parse_multipart_formdata(body_str, boundary, global_config.upload_tmp_dir, tmp, r.files);
	r.params.type = DynamicVariable::OBJECT;
	for (auto& kv : tmp)
		r.params[kv.first] = DynamicVariable::make_string(kv.second);
}

void parse_urlencoded_form_data(fcgi::Request& r)
{
	std::string body_str(reinterpret_cast<const char*>(r.body.data()), r.body.size());
	std::unordered_map<std::string, std::string> tmp;
	parse_query_string(body_str, tmp);
	r.params.type = DynamicVariable::OBJECT;
	for (auto& kv : tmp)
		r.params[kv.first] = DynamicVariable::make_string(kv.second);
}

void parse_form_data(fcgi::Request& r)
{
	const DynamicVariable* it_ct = r.env.find("CONTENT_TYPE");
	if (!it_ct || it_ct->type != DynamicVariable::STRING)
		return;
	std::string ct = it_ct->s;
	std::string lct = ct; for (auto& c : lct) c = std::tolower(c);
	if (lct.find("application/json") != std::string::npos)
	{
		parse_json_form_data(r);
	}
	else if (lct.find("application/x-www-form-urlencoded") != std::string::npos)
	{
		parse_urlencoded_form_data(r);
	}
	else if (lct.find("multipart/form-data") != std::string::npos)
	{
		parse_multipart_form_data(r);
	}
}

void output_headers(fcgi::Request& r, std::ostringstream& oss)
{
	for (auto& kv : r.headers.o)
	{
		if (kv.second.type == DynamicVariable::STRING)
		{
			std::string lname = kv.first; for (auto& c : lname) c = std::tolower(c);
			oss << kv.first << ": " << kv.second.s << "\r\n";
		}
		else
		{
			oss << kv.first << ": " << to_json(kv.second, false, 0) << "\r\n";
		}
	}
	oss << "\r\n"; // header/body separator
}

static void on_request_ready(fcgi::Request& r, std::vector<uint8_t>& out_buf)
{
	if (r.responded)
		return;

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

	// --------- output ---------
	std::ostringstream oss;
	output_headers(r, oss);

	oss << "-- ENV --\n";
	print_any_limited(oss, r.env, global_config.print_env_limit, global_config.print_indent);

	oss << "-- COOKIES --\n";
	print_any_limited(oss, r.cookies, global_config.print_env_limit, global_config.print_indent);

	oss << "-- PARAMS --\n";
	print_any_limited(oss, r.params, global_config.print_env_limit, global_config.print_indent);

	oss << "-- HEADERS(OUT) --\n";
	print_any_limited(oss, r.headers, global_config.print_env_limit, global_config.print_indent);

	oss << "-- FILES --\n";
	print_any_limited(oss, r.files, global_config.print_env_limit, global_config.print_indent);

	oss << "-- SESSION --\n";
	print_any_limited(oss, r.session, global_config.print_env_limit, global_config.print_indent);

	oss << "\n-- BODY (" << r.body_bytes << " bytes) --\n";
	size_t preview_cap = global_config.body_preview_limit ? global_config.body_preview_limit : 1024;
	size_t show = r.body.size() < preview_cap ? r.body.size() : preview_cap;
	for (size_t i = 0; i < show; i++)
	{
		uint8_t b = r.body[i];
		if (b >= 32 && b < 127)
			oss << char(b);
		else if (b == '\n' || b == '\r' || b == '\t')
			oss << char(b);
		else
			oss << '.';
	}
	if (show < r.body.size())
		oss << "\n[truncated]";

	fcgi::append_stdout_text(out_buf, r.id, oss.str());

	if (!r.session_id.empty())
		session_save(r);

	fcgi::append_end_request(out_buf, r.id, 0, fcgi::REQUEST_COMPLETE);
	r.responded = true;
}

static void flush_connection(Connection& c, int epfd)
{
	while (!c.out_buf.empty())
	{
		ssize_t n = ::send(c.fd, c.out_buf.data(), c.out_buf.size(), 0);
		if (n > 0)
		{
			c.out_buf.erase(c.out_buf.begin(), c.out_buf.begin() + n);
		}
		else
		{
			if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				epoll_event ev{};
				ev.data.fd = c.fd;
				ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
				epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev); // ignore errors
				return;
			}
			else
			{
				log_errno("send");
				c.closed = true;
				return;
			}
		}
	}
	epoll_event ev{};
	ev.data.fd = c.fd;
	ev.events = EPOLLIN | EPOLLET;
	epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev);
}

static volatile sig_atomic_t g_stop = 0;
static void handle_signal(int)
{
	g_stop = 1;
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

static void usage(const char* prog)
{
	std::fprintf(stderr, "Usage: %s [options]\n\n"
						 "Options:\n"
						 "  --port N                TCP port (default 9000)\n"
						 "  --unix PATH             UNIX socket path instead of TCP\n"
						 "  --backlog N             listen backlog (default 128)\n"
						 "  --max-in-flight N       per-connection FastCGI request cap\n"
						 "  --max-params BYTES      max PARAMS bytes per request\n"
						 "  --max-stdin BYTES       max STDIN bytes per request\n"
						 "  --arena-capacity BYTES  arena capacity per request (default 262144)\n"
						 "  --workers N             number of worker arenas (default 1)\n"
						 "  --output-buffer BYTES   initial output buffer reserve (default 32768)\n"
						 "  --upload-tmp DIR        directory for uploaded multipart temp files\n"
						 "  --help                  show this help\n",
				 prog);
}

int main(int argc, char* argv[])
{
	auto& G = global_config;
	std::vector<std::string> errors;
	if (!config_parse_args(argc, argv, errors))
	{
		if (!errors.empty())
		{
			for (auto& e : errors)
				std::fprintf(stderr, "%s\n", e.c_str());
		}
		usage(argv[0]);
		if (!errors.empty())
			return 1;
		else
			return 0;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	int listen_fd = create_listen_socket();
	if (listen_fd == -1)
		return 1;
	int epfd = epoll_create1(0);
	if (epfd == -1)
	{
		log_errno("epoll_create1");
		return 1;
	}
	epoll_event ev{};
	ev.data.fd = listen_fd;
	ev.events = EPOLLIN | EPOLLET;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1)
	{
		log_errno("epoll_ctl listen");
		return 1;
	}
	log_info("fastcgi-dump-server listening on %s", G.unix_path.empty() ? ("tcp:" + std::to_string(G.port)).c_str() : G.unix_path.c_str());

	std::unordered_map<int, Connection> conns;

	const int MAX_EVENTS = 64;
	std::vector<epoll_event> events(MAX_EVENTS);
	uint64_t start_shutdown_ms = 0;
	auto now_ms = []()
	{ struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (uint64_t)ts.tv_sec*1000ULL + ts.tv_nsec/1000000ULL; };
	while (true)
	{
		if (g_stop)
		{
			if (!start_shutdown_ms)
				start_shutdown_ms = now_ms();
			uint64_t elapsed = now_ms() - start_shutdown_ms;
			if (conns.empty() || elapsed > G.graceful_shutdown_timeout_ms)
				break; // exit loop
		}
		int n = epoll_wait(epfd, events.data(), MAX_EVENTS, 1000);
		if (n == -1)
		{
			if (errno == EINTR)
				continue;
			log_errno("epoll_wait");
			break;
		}
		for (int i = 0; i < n; i++)
		{
			int fd = events[i].data.fd;
			uint32_t evs = events[i].events;
			if (fd == listen_fd)
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
					epoll_event cev{};
					cev.data.fd = cfd;
					cev.events = EPOLLIN | EPOLLET;
					epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
					Connection c;
					c.fd = cfd;
					c.out_buf.reserve(global_config.output_buffer_initial);
					log_debug("Accepted fd=%d", cfd);
					conns.emplace(cfd, std::move(c));
				}
			}
			else
			{
				auto it = conns.find(fd);
				if (it == conns.end())
					continue;
				Connection& c = it->second;
				if (evs & (EPOLLHUP | EPOLLERR))
				{
					c.closed = true;
				}
				if (evs & EPOLLIN)
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
							c.closed = true;
							break;
						}
						else
						{
							if (errno == EAGAIN || errno == EWOULDBLOCK)
								break;
							else
							{
								log_errno("recv");
								c.closed = true;
								break;
							}
						}
					}
					if (!c.closed)
					{
						auto status = fcgi::process_buffer(c.in_buf, c.requests, c.out_buf, G.max_in_flight, G.max_params_bytes, G.max_stdin_bytes, on_request_ready);
						if (status == fcgi::CLOSE)
							c.closed = true;
					}
					if (!c.out_buf.empty())
						flush_connection(c, epfd);
					for (auto& kv : c.requests)
					{
						auto& req = kv.second;
						if (req.responded && req.files.type == DynamicVariable::ARRAY && !req.files.a.empty())
						{
							for (auto& f : req.files.a)
							{
								if (f.type != DynamicVariable::OBJECT) continue;
								DynamicVariable* tp = f.find("temp_path");
								if (!global_config.keep_uploaded_files && tp && tp->type == DynamicVariable::STRING && !tp->s.empty())
								{
									::unlink(tp->s.c_str());
									tp->s.clear();
								}
							}
							req.files = DynamicVariable::make_array();
						}
					}
				}
				if (evs & EPOLLOUT)
				{
					if (!c.out_buf.empty())
						flush_connection(c, epfd);
				}
				bool all_responded = true;
				for (auto& kv : c.requests)
				{
					if (!kv.second.responded)
					{
						all_responded = false;
						break;
					}
				}
				bool any_keep = false;
				for (auto& kv : c.requests)
					if (kv.second.keep_conn)
					{
						any_keep = true;
						break;
					}
				if (c.closed || (all_responded && !any_keep && c.out_buf.empty()))
				{
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
					::close(fd);
					log_debug("Closed fd=%d", fd);
					conns.erase(it);
				}
			}
		}
	}

	for (auto& kv : conns)
	{
		::close(kv.first);
	}
	::close(listen_fd);
	::close(epfd);
	if (!G.unix_path.empty())
		::unlink(G.unix_path.c_str());
	log_info("fastcgi-dump-server shutdown.");
	return 0;
}
