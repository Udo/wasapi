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
#include <sstream>
#include <thread>
#include "fastcgi.h"
#include "http.h"
#include "config.h"
#include "logger.h"
#include "memory.h"
#include "session.h"
#include "request.h"
#include <functional>

#include "fcgi-connection.h"
#include "websockets.h"
#include "worker.h"

static void on_request_ready(Request& r, std::vector<uint8_t>& out_buf)
{
	if (r.flags & Request::RESPONDED)
		return; // already handled

	std::ostringstream oss;
	output_headers(r, oss);

	r.env["DBG_ARENA_ALLOC"] = DynamicVariable::make_number(r.arena->offset);
	oss << "-- ENV --\n";
	print_any_limited(oss, r.env, global_config.print_env_limit, global_config.print_indent);

	oss << "-- CONTEXT --\n";
	print_any_limited(oss, r.context, global_config.print_env_limit, global_config.print_indent);

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
	r.flags |= Request::RESPONDED;
}

static void handle_signal(int)
{
	_exit(0); // immediate terminate, no cleanup
}

static void usage(const char* prog)
{
	std::fprintf(stderr, "Usage: %s [options]\n\n"
						 "Options:\n"
						 "  --fcgi-port N                TCP port (default 9000)\n"
						 "  --fcgi-socket PATH           alt. UNIX socket path for FastCGI\n"
						 "  --ws-port N                  WebSocket port (default 9001)\n"
						 "  --ws-socket PATH             alt. UNIX socket path for WebSocket\n",
				 prog);
}

static bool initialize_server_config(int argc, char* argv[])
{
	std::vector<std::string> errors;
	if (!config_parse_args(argc, argv, errors))
	{
		if (!errors.empty())
		{
			for (auto& e : errors)
				std::fprintf(stderr, "%s\n", e.c_str());
		}
		usage(argv[0]);
		return false;
	}
	return true;
}

static void setup_signal_handlers()
{
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
}

int main(int argc, char* argv[])
{
	if (!initialize_server_config(argc, argv))
	{
		return 1;
	}

	setup_signal_handlers();

	global_arena_manager.create_arenas(global_config.max_in_flight, global_config.arena_capacity);
	global_worker_pool.start(global_config.max_in_flight);

	register_thread_name("main");
	std::thread fcgi_thread([]()
							{
		register_thread_name("fcgi");
		fcgi_conn::serve(global_config.fcgi_port, global_config.fcgi_socket_path, on_request_ready); });
	std::thread ws_thread([]()
						  {
		register_thread_name("ws");
		ws::serve(global_config.ws_port, global_config.ws_socket_path, on_request_ready, on_request_ready); });

	fcgi_thread.join();
	ws_thread.join();
	return 0;
}
