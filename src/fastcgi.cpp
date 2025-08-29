#include "fastcgi.h"
#include <cstring>
#include <arpa/inet.h>

namespace fcgi
{

	static size_t decode_length(const uint8_t*& p, const uint8_t* end)
	{
		if (p >= end)
			return 0;
		uint8_t b = *p;
		if ((b & 0x80) == 0)
		{
			++p;
			return b;
		}
		if (end - p < 4)
		{
			p = end;
			return 0;
		}
		size_t v = ((size_t)(p[0] & 0x7F) << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | (size_t)p[3];
		p += 4;
		return v;
	}

	void append_record(std::vector<uint8_t>& out, uint8_t type, uint16_t reqId, const uint8_t* data, uint16_t len)
	{
		Header h{};
		h.version = VERSION_1;
		h.type = type;
		h.requestId = htons(reqId);
		h.contentLength = htons(len);
		h.paddingLength = 0;
		h.reserved = 0;
		size_t start = out.size();
		out.resize(start + sizeof(Header) + len);
		std::memcpy(out.data() + start, &h, sizeof(h));
		if (len)
			std::memcpy(out.data() + start + sizeof(h), data, len);
	}

	void append_stdout_text(std::vector<uint8_t>& out, uint16_t reqId, const std::string& body)
	{
		const uint8_t* data = reinterpret_cast<const uint8_t*>(body.data());
		size_t remaining = body.size();
		size_t offset = 0;
		while (remaining > 0)
		{
			uint16_t chunk = remaining > 0xFFFF ? 0xFFFF : (uint16_t)remaining;
			append_record(out, FCGI_STDOUT, reqId, data + offset, chunk);
			offset += chunk;
			remaining -= chunk;
		}
		append_record(out, FCGI_STDOUT, reqId, nullptr, 0);
	}

	void append_end_request(std::vector<uint8_t>& out, uint16_t reqId, uint32_t appStatus, uint8_t protoStatus)
	{
		EndRequestBody b{};
		b.appStatus = htonl(appStatus);
		b.protocolStatus = protoStatus;
		append_record(out, FCGI_END_REQUEST, reqId, reinterpret_cast<uint8_t*>(&b), sizeof(b));
	}

	static void fail_request(Request& r, std::vector<uint8_t>& out_buf, uint8_t status)
	{
		if (!r.responded)
		{
			append_end_request(out_buf, r.id, 0, status);
			r.responded = true;
			r.failed = true;
		}
	}

	ProcessStatus process_buffer(std::vector<uint8_t>& in_buf, std::unordered_map<uint16_t, Request>& requests, std::vector<uint8_t>& out_buf, uint32_t max_in_flight, size_t max_params_bytes, size_t max_stdin_bytes, void (*on_request_ready)(Request&, std::vector<uint8_t>&))
	{
		size_t offset = 0;
		bool close_needed = false;
		while (true)
		{
			if (in_buf.size() - offset < sizeof(Header))
				break;
			Header h;
			std::memcpy(&h, in_buf.data() + offset, sizeof(h));
			if (h.version != VERSION_1)
			{
				close_needed = true;
				break;
			}
			uint16_t reqId = ntohs(h.requestId);
			uint16_t contentLength = ntohs(h.contentLength);
			size_t totalLen = sizeof(Header) + contentLength + h.paddingLength;
			if (in_buf.size() - offset < totalLen)
				break;
			const uint8_t* content = in_buf.data() + offset + sizeof(Header);
			auto current_req = [&](uint16_t id) -> Request*
			{
				auto it = requests.find(id);
				if (it == requests.end())
				{
					if (requests.size() >= max_in_flight)
					{
						return nullptr;
					}
					it = requests.emplace(id, Request{}).first;
					it->second.id = id;
				}
				return &it->second;
			};
			Request* rptr = nullptr;
			switch (h.type)
			{
				case FCGI_BEGIN_REQUEST:
				{
					if (contentLength >= sizeof(BeginRequestBody))
					{
						BeginRequestBody br{};
						std::memcpy(&br, content, sizeof(br));
						if (Request* nr = current_req(reqId))
						{
							rptr = nr;
							nr->got_begin = true;
							nr->keep_conn = (br.flags & KEEP_CONN) != 0;
						}
					}
					break;
				}
				case FCGI_PARAMS:
				{
					if (Request* r = current_req(reqId))
					{
						rptr = r;
						if (contentLength == 0)
						{
							r->params_complete = true;
						}
						else if (!r->failed)
						{
							const uint8_t* p = content;
							const uint8_t* end = content + contentLength;
							while (p < end)
							{
								size_t nameLen = decode_length(p, end);
								size_t valueLen = decode_length(p, end);
								if (p + nameLen + valueLen > end)
								{
									break;
								}
								if (r->params_bytes + nameLen + valueLen > max_params_bytes)
								{
									fail_request(*r, out_buf, OVERLOADED);
									break;
								}
								std::string name(reinterpret_cast<const char*>(p), nameLen);
								p += nameLen;
								std::string value(reinterpret_cast<const char*>(p), valueLen);
								p += valueLen;
								r->env[name] = DynamicVariable::make_string(std::move(value));
								r->params_bytes += nameLen + valueLen;
							}
						}
					}
					else
					{
						// overflow
					}
					break;
				}
				case FCGI_STDIN:
				{
					if (Request* r = current_req(reqId))
					{
						rptr = r;
						if (contentLength == 0)
						{
							r->stdin_complete = true;
						}
						else if (!r->failed)
						{
							if (r->body_bytes + contentLength > max_stdin_bytes)
							{
								fail_request(*r, out_buf, OVERLOADED);
							}
							else
							{
								r->body.insert(r->body.end(), content, content + contentLength);
								r->body_bytes += contentLength;
							}
						}
					}
					else
					{
						// overflow
					}
					break;
				}
				case FCGI_ABORT_REQUEST:
				{
					if (Request* r = current_req(reqId))
					{
						rptr = r;
						r->aborted = true;
						fail_request(*r, out_buf, REQUEST_COMPLETE);
					}
					break;
				}
				default:
				{
					break;
				}
			}
			offset += totalLen;
			if (rptr && !rptr->failed && !rptr->responded && rptr->params_complete && rptr->stdin_complete)
			{
				if (on_request_ready)
					on_request_ready(*rptr, out_buf);
			}
		}
		if (offset)
			in_buf.erase(in_buf.begin(), in_buf.begin() + offset);
		return close_needed ? CLOSE : OK;
	}

} // namespace fcgi
