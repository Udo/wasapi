#include "session.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>
#include <fstream>

static std::string session_path(const std::string& id)
{
    std::string dir = global_config.session_storage_path;
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    return dir + id + ".json";
}

static bool mkdir_if_not_exists(const std::string& dir)
{
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0)
    {
        if (S_ISDIR(st.st_mode)) return true;
        return false;
    }
    if (::mkdir(dir.c_str(), 0777) == 0) return true;
    return false;
}

static std::string random_hex(size_t bytes)
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    std::string out; out.reserve(bytes*2);
    while (bytes > 0)
    {
        uint64_t v = dist(gen);
        size_t chunk = bytes > sizeof(v) ? sizeof(v) : bytes;
        for (size_t i = 0; i < chunk; ++i)
        {
            unsigned b = (v >> (i*8)) & 0xFF;
            static const char* hex = "0123456789abcdef";
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0xF]);
        }
        bytes -= chunk;
    }
    return out;
}

std::string session_get_id(fcgi::Request& r, bool create)
{
    if (!r.session_id.empty()) return r.session_id;
    if (!create) return std::string();
    r.session_id = random_hex(16);
    return r.session_id;
}

bool session_load(fcgi::Request& r)
{
    if (r.session_id.empty()) return false;
    std::ifstream in(session_path(r.session_id));
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    DynamicVariable parsed;
    size_t err = 0;
    if (parse_json(content, parsed, &err))
    {
        r.session = parsed;
        return true;
    }
    return false;
}

bool session_start(fcgi::Request& r)
{
    session_get_id(r, true);
    if (!r.cookies.find(global_config.session_cookie_name))
        r.headers["Set-Cookie"] = global_config.session_cookie_name + "=" + r.session_id + "; Path=/; HttpOnly";
    if (!session_load(r))
        r.session.clear();
    return true;
}

bool session_save(fcgi::Request& r)
{
    if (r.session_id.empty()) return false;
    mkdir_if_not_exists(global_config.session_storage_path);
    std::string tmp = session_path(r.session_id) + ".tmp";
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out) return false;
    out << to_json(r.session, false, 0);
    out.close();
    if (!out.good()) { ::unlink(tmp.c_str()); return false; }
    std::string final_path = session_path(r.session_id);
    if (::rename(tmp.c_str(), final_path.c_str()) != 0)
    {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool session_clear(fcgi::Request& r)
{
    if (!r.session_id.empty())
    {
        std::string path = session_path(r.session_id);
        ::unlink(path.c_str());
    }
    r.session_id.clear();
    r.session = DynamicVariable::make_object();
    return true;
}
