#include "http.h"
#include <cctype>
#include <vector>
#include <unistd.h>

inline int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

std::string url_decode(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i)
	{
		char c = s[i];
		if (c == '+')
		{
			out.push_back(' ');
			continue;
		}
		if (c == '%' && i + 2 < s.size())
		{
			int h1 = hexval(s[i + 1]);
			int h2 = hexval(s[i + 2]);
			if (h1 >= 0 && h2 >= 0)
			{
				out.push_back(char((h1 << 4) | h2));
				i += 2;
				continue;
			}
		}
		out.push_back(c);
	}
	return out;
}

static inline bool unreserved(char c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return true;
	switch (c)
	{
		case '-':
		case '_':
		case '.':
		case '~':
			return true;
		default:
			return false;
	}
}

static inline char hex_digit(int v)
{
	return v < 10 ? char('0' + v) : char('A' + (v - 10));
}

std::string url_encode(const std::string& s)
{
	std::string out;
	out.reserve(s.size() * 3 / 2 + 8);
	for (unsigned char c : s)
	{
		if (unreserved(c))
		{
			out.push_back(char(c));
		}
		else
		{
			out.push_back('%');
			out.push_back(hex_digit((c >> 4) & 0xF));
			out.push_back(hex_digit(c & 0xF));
		}
	}
	return out;
}

std::string build_query(const std::unordered_map<std::string, std::string>& params)
{
	std::string out;
	out.reserve(params.size() * 16);
	bool first = true;
	for (auto& kv : params)
	{
		if (!first)
			out.push_back('&');
		else
			first = false;
		out.append(url_encode(kv.first));
		out.push_back('=');
		out.append(url_encode(kv.second));
	}
	return out;
}

void parse_query_string(const std::string& input, std::unordered_map<std::string, std::string>& out)
{
	size_t start = 0;
	while (start <= input.size())
	{
		size_t amp = input.find('&', start);
		if (amp == std::string::npos)
			amp = input.size();
		size_t eq = input.find('=', start);
		if (eq == std::string::npos || eq > amp)
		{
			if (amp > start)
			{
				std::string key = url_decode(input.substr(start, amp - start));
				if (!key.empty())
					out[key] = ""; // present with empty value
			}
		}
		else
		{
			std::string key = url_decode(input.substr(start, eq - start));
			std::string val = url_decode(input.substr(eq + 1, amp - (eq + 1)));
			if (!key.empty())
				out[key] = val;
		}
		if (amp == input.size())
			break;
		else
			start = amp + 1;
	}
}

bool extract_files_from_formdata(const std::string& body, const std::string& boundary, const std::string& upload_dir,
					 std::unordered_map<std::string, std::string>& form_fields, DynamicVariable& files)
{
	if (boundary.empty())
		return false;
	files = DynamicVariable::make_array();
	std::string delim = "--" + boundary;
	size_t pos = 0;
	while (true)
	{
		size_t start = body.find(delim, pos);
		if (start == std::string::npos)
			break;
		start += delim.size();
		if (start + 2 <= body.size() && body.compare(start, 2, "--") == 0)
			break;
		if (start < body.size() && body[start] == '\r' && start + 1 < body.size() && body[start + 1] == '\n')
			start += 2;
		else
			return false;
		size_t header_end = body.find("\r\n\r\n", start);
		if (header_end == std::string::npos)
			return false;
		std::string headers = body.substr(start, header_end - start);
		size_t content_start = header_end + 4;
		size_t part_end = body.find("\r\n" + delim, content_start);
		if (part_end == std::string::npos)
			return false;
		size_t content_len = part_end - content_start;
		if (content_len >= 2 && body[content_start + content_len - 2] == '\r' && body[content_start + content_len - 1] == '\n')
			content_len -= 2;
		std::string field_name, filename, ctype;
		size_t hpos = 0;
		while (hpos < headers.size())
		{
			size_t line_end = headers.find("\r\n", hpos);
			if (line_end == std::string::npos)
				line_end = headers.size();
			std::string line = headers.substr(hpos, line_end - hpos);
			hpos = line_end + 2;
			if (line.size() == 0)
				break;
			auto colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			std::string name = line.substr(0, colon);
			std::string value = line.substr(colon + 1);
			// trim spaces
			auto ltrim = [&](std::string& s)
			{ while(!s.empty() && (s.front()==' '||s.front()=='\t')) s.erase(s.begin()); };
			auto rtrim = [&](std::string& s)
			{ while(!s.empty() && (s.back()==' '||s.back()=='\t')) s.pop_back(); };
			ltrim(value);
			rtrim(value);
			for (auto& c : name)
				c = std::tolower(c);
			if (name == "content-disposition")
			{
				
				size_t p = 0;
				while (p < value.size())
				{
					// skip token until ; or end
					size_t sc = value.find(';', p);
					if (sc == std::string::npos)
						sc = value.size();
					if (p == 0)
					{ /* form-data token */
					}
					size_t semi_next = sc;
					// process attribute inside segment
					size_t eqp = value.find('=', p);
					if (eqp != std::string::npos && eqp < semi_next)
					{
						std::string attr = value.substr(p, eqp - p);
						ltrim(attr);
						rtrim(attr);
						// strip quotes
						size_t valstart = eqp + 1;
						while (valstart < semi_next && (value[valstart] == ' ' || value[valstart] == '\t'))
							++valstart;
						if (valstart < semi_next && value[valstart] == '"' && semi_next > valstart + 1 && value[semi_next - 1] == '"')
						{
							std::string aval = value.substr(valstart + 1, semi_next - valstart - 2);
							if (attr == "name")
								field_name = aval;
							else if (attr == "filename")
								filename = aval;
						}
					}
					if (sc == value.size())
						break;
					else
						p = sc + 1;
				}
			}
			else if (name == "content-type")
			{
				ctype = value;
			}
		}
		if (filename.empty())
		{
			form_fields[field_name] = std::string(body, content_start, content_len);
		}
		else
		{
			std::string pattern = upload_dir;
			if (!pattern.empty() && pattern.back() != '/')
				pattern.push_back('/');
			pattern += "fcgi_upload_XXXXXX";
			std::vector<char> tmpl(pattern.begin(), pattern.end());
			tmpl.push_back('\0');
			int fd = mkstemp(tmpl.data());
			if (fd != -1)
			{
				size_t written = 0;
				const char* data_ptr = body.data() + content_start;
				uint64_t hv = 1469598103934665603ULL;
				for (size_t i = 0; i < content_len; ++i) { hv ^= (unsigned char)data_ptr[i]; hv *= 1099511628211ULL; }
				while (written < content_len) {
					ssize_t w = ::write(fd, data_ptr + written, content_len - written);
					if (w <= 0) break;
					written += (size_t)w;
				}
				::close(fd);
				char hash_hex[17]; std::snprintf(hash_hex, sizeof(hash_hex), "%016llx", (unsigned long long)hv);
				DynamicVariable file = DynamicVariable::make_object();
				file["field_name"] = field_name;
				file["filename"] = filename;
				if (!ctype.empty()) file["content_type"] = ctype;
				file["temp_path"] = std::string(tmpl.data());
				file["size"] = (double)written;
				file["expected_size"] = (double)content_len;
				file["hash_fnv1a64"] = std::string(hash_hex);
				if (written != content_len) file["partial"] = true;
				files.push(std::move(file));
			}
		}
		pos = part_end + 2; // skip CRLF before next delimiter search
	}
	return true;
}
