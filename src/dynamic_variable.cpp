#include "dynamic_variable.h"
#include "memory.h"
#include <cctype>
#include <sstream>
#include <cstring>

DynamicString::DynamicString(Arena* a)
{
	arena = a;
	reserve(32);
}

DynamicString::~DynamicString() // we don't need to free anything since this is intended to work with an arena
{
}

bool DynamicString::reserve(size_t sz)
{
	if (sz > capacity)
	{
		void* new_data = arena->alloc(sz);
		if (!new_data)
			return false;
		if (data)
			std::memcpy(new_data, data, capacity);
		data = new_data;
		capacity = sz;
	}
	return true;
}

DynamicVariable::DynamicVariable() = default;

DynamicVariable::DynamicVariable(const DynamicVariable& other) : type(other.type)
{
	switch (type)
	{
		case STRING:
			new (&data.s) std::string(other.data.s);
			break;
		case OBJECT:
			new (&data.o) std::unordered_map<std::string, DynamicVariable>(other.data.o);
			break;
		case ARRAY:
			new (&data.a) std::vector<DynamicVariable>(other.data.a);
			break;
		case NUMBER:
			data.num = other.data.num;
			break;
		case BOOL:
			data.b = other.data.b;
			break;
		case NIL:
			break;
	}
}

DynamicVariable::DynamicVariable(DynamicVariable&& other) noexcept : type(other.type)
{
	switch (type)
	{
		case STRING:
			new (&data.s) std::string(std::move(other.data.s));
			break;
		case OBJECT:
			new (&data.o) std::unordered_map<std::string, DynamicVariable>(std::move(other.data.o));
			break;
		case ARRAY:
			new (&data.a) std::vector<DynamicVariable>(std::move(other.data.a));
			break;
		case NUMBER:
			data.num = other.data.num;
			break;
		case BOOL:
			data.b = other.data.b;
			break;
		case NIL:
			break;
	}
	other.type = NIL;
}

DynamicVariable::~DynamicVariable()
{
	clear();
}

DynamicVariable::DynamicVariable(const char* lit)
{
	if (lit)
	{
		type = STRING;
		new (&data.s) std::string(lit);
	}
}

DynamicVariable::DynamicVariable(std::string str)
{
	type = STRING;
	new (&data.s) std::string(std::move(str));
}

DynamicVariable::DynamicVariable(double v)
{
	type = NUMBER;
	data.num = v;
}

DynamicVariable::DynamicVariable(int v)
{
	type = NUMBER;
	data.num = static_cast<double>(v);
}

DynamicVariable::DynamicVariable(bool v)
{
	type = BOOL;
	data.b = v;
}

DynamicVariable DynamicVariable::make_string(std::string v)
{
	DynamicVariable d;
	d.type = STRING;
	new (&d.data.s) std::string(std::move(v));
	return d;
}

DynamicVariable DynamicVariable::make_number(double v)
{
	DynamicVariable d;
	d.type = NUMBER;
	d.data.num = v;
	return d;
}

DynamicVariable DynamicVariable::make_bool(bool v)
{
	DynamicVariable d;
	d.type = BOOL;
	d.data.b = v;
	return d;
}

DynamicVariable DynamicVariable::make_object()
{
	DynamicVariable d;
	d.type = OBJECT;
	new (&d.data.o) std::unordered_map<std::string, DynamicVariable>();
	return d;
}

DynamicVariable DynamicVariable::make_array()
{
	DynamicVariable d;
	d.type = ARRAY;
	new (&d.data.a) std::vector<DynamicVariable>();
	return d;
}

DynamicVariable DynamicVariable::make_null()
{
	return DynamicVariable();
}

void DynamicVariable::clear()
{
	switch (type)
	{
		case STRING:
			data.s.~basic_string();
			break;
		case OBJECT:
			data.o.~unordered_map();
			break;
		case ARRAY:
			data.a.~vector();
			break;
		case NUMBER:
		case BOOL:
		case NIL:
			break;
	}
	type = NIL;
	data.num = 0.0; // Reset payload
}

DynamicVariable& DynamicVariable::operator[](const std::string& key)
{
	if (type != OBJECT)
	{
		clear();
		type = OBJECT;
		new (&data.o) std::unordered_map<std::string, DynamicVariable>();
	}
	return data.o[key];
}

DynamicVariable& DynamicVariable::operator=(const DynamicVariable& other)
{
	if (this != &other)
	{
		clear();
		type = other.type;
		switch (type)
		{
			case STRING:
				new (&data.s) std::string(other.data.s);
				break;
			case OBJECT:
				new (&data.o) std::unordered_map<std::string, DynamicVariable>(other.data.o);
				break;
			case ARRAY:
				new (&data.a) std::vector<DynamicVariable>(other.data.a);
				break;
			case NUMBER:
				data.num = other.data.num;
				break;
			case BOOL:
				data.b = other.data.b;
				break;
			case NIL:
				break;
		}
	}
	return *this;
}

DynamicVariable& DynamicVariable::operator=(DynamicVariable&& other) noexcept
{
	if (this != &other)
	{
		clear();
		type = other.type;
		switch (type)
		{
			case STRING:
				new (&data.s) std::string(std::move(other.data.s));
				break;
			case OBJECT:
				new (&data.o) std::unordered_map<std::string, DynamicVariable>(std::move(other.data.o));
				break;
			case ARRAY:
				new (&data.a) std::vector<DynamicVariable>(std::move(other.data.a));
				break;
			case NUMBER:
				data.num = other.data.num;
				break;
			case BOOL:
				data.b = other.data.b;
				break;
			case NIL:
				break;
		}
		other.clear();
	}
	return *this;
}

DynamicVariable& DynamicVariable::operator=(const std::string& str)
{
	clear();
	type = STRING;
	new (&data.s) std::string(str);
	return *this;
}

DynamicVariable& DynamicVariable::operator=(std::string&& str)
{
	clear();
	type = STRING;
	new (&data.s) std::string(std::move(str));
	return *this;
}

DynamicVariable& DynamicVariable::operator=(const char* lit)
{
	clear();
	type = STRING;
	new (&data.s) std::string(lit ? lit : "");
	return *this;
}

DynamicVariable& DynamicVariable::operator=(double v)
{
	clear();
	type = NUMBER;
	data.num = v;
	return *this;
}

DynamicVariable& DynamicVariable::operator=(int v)
{
	clear();
	type = NUMBER;
	data.num = static_cast<double>(v);
	return *this;
}

DynamicVariable& DynamicVariable::operator=(bool v)
{
	clear();
	type = BOOL;
	data.b = v;
	return *this;
}

DynamicVariable& DynamicVariable::operator=(std::initializer_list<DynamicVariable> list)
{
	clear();
	type = ARRAY;
	new (&data.a) std::vector<DynamicVariable>(list.begin(), list.end());
	return *this;
}

DynamicVariable* DynamicVariable::find(std::string key)
{
	if (type != OBJECT)
		return nullptr;
	auto it = data.o.find(key);
	return it == data.o.end() ? nullptr : &it->second;
}

void DynamicVariable::push(DynamicVariable v)
{
	if (type != ARRAY)
	{
		if (type == NIL)
		{
			type = ARRAY;
			new (&data.a) std::vector<DynamicVariable>();
		}
		else
			return;
	}
	data.a.push_back(std::move(v));
}

std::string DynamicVariable::to_string() const
{
	switch (type)
	{
		case STRING:
			return data.s;
		case NUMBER:
			return std::to_string(data.num);
		case BOOL:
			return data.b ? "true" : "false";
		case NIL:
			return "";
		default:
			return "";
	}
}

double DynamicVariable::to_number(double def_value) const
{
	return type == NUMBER ? data.num : def_value;
}

bool DynamicVariable::to_bool(bool def_value) const
{
	if (type == BOOL)
		return data.b;
	if (type == NUMBER)
		return data.num != 0.0;
	return def_value;
}

struct JsonCursor
{
	const std::string* s;
	size_t i = 0;
};

void skip_ws(JsonCursor& c)
{
	while (c.i < c.s->size() && std::isspace((unsigned char)(*c.s)[c.i]))
	{
		++c.i;
	}
}

bool match(JsonCursor& c, char ch)
{
	skip_ws(c);
	if (c.i < c.s->size() && (*c.s)[c.i] == ch)
	{
		++c.i;
		return true;
	}
	return false;
}

bool parse_string(JsonCursor& c, std::string& out)
{
	skip_ws(c);
	if (c.i >= c.s->size() || (*c.s)[c.i] != '"')
	{
		return false;
	}
	++c.i; // consume opening quote
	out.clear();
	while (c.i < c.s->size())
	{
		char ch = (*c.s)[c.i++];
		if (ch == '"')
		{
			return true; // done
		}
		if (ch == '\\')
		{
			if (c.i >= c.s->size())
			{
				return false; // dangling escape
			}
			char esc = (*c.s)[c.i++];
			switch (esc)
			{
				case '"':
				case '\\':
				case '/':
					out.push_back(esc);
					break;
				case 'b':
					out.push_back('\b');
					break;
				case 'f':
					out.push_back('\f');
					break;
				case 'n':
					out.push_back('\n');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case 't':
					out.push_back('\t');
					break;
				case 'u':
				{
					if (c.i + 4 > c.s->size())
					{
						return false;
					}
					unsigned v = 0;
					for (int k = 0; k < 4; k++)
					{
						char h = (*c.s)[c.i++];
						v <<= 4;
						if (h >= '0' && h <= '9')
							v += h - '0';
						else if (h >= 'a' && h <= 'f')
							v += 10 + h - 'a';
						else if (h >= 'A' && h <= 'F')
							v += 10 + h - 'A';
						else
							return false;
					}
					if (v <= 0x7F)
					{
						out.push_back(char(v));
					}
					else if (v <= 0x7FF)
					{
						out.push_back(char(0xC0 | (v >> 6)));
						out.push_back(char(0x80 | (v & 0x3F)));
					}
					else if (v <= 0xFFFF)
					{
						out.push_back(char(0xE0 | (v >> 12)));
						out.push_back(char(0x80 | ((v >> 6) & 0x3F)));
						out.push_back(char(0x80 | (v & 0x3F)));
					}
					break;
				}
				default:
					return false;
			}
		}
		else
		{
			out.push_back(ch);
		}
	}
	return false; // unterminated string
}

bool parse_number(JsonCursor& c, double& num)
{
	skip_ws(c);
	size_t start = c.i;
	bool dot = false;
	if (c.i < c.s->size() && (((*c.s)[c.i] == '-') || ((*c.s)[c.i] == '+')))
	{
		++c.i;
	}
	while (c.i < c.s->size())
	{
		char ch = (*c.s)[c.i];
		if (ch >= '0' && ch <= '9')
		{
			++c.i;
			continue;
		}
		if (ch == '.' && !dot)
		{
			dot = true;
			++c.i;
			continue;
		}
		break;
	}
	if (start == c.i)
	{
		return false;
	}
	try
	{
		num = std::stod(c.s->substr(start, c.i - start));
	}
	catch (...)
	{
		return false;
	}
	return true;
}

bool parse_value(JsonCursor& c, DynamicVariable& out);

bool parse_array(JsonCursor& c, DynamicVariable& out)
{
	if (!match(c, '['))
	{
		return false;
	}
	out.clear();
	out.type = DynamicVariable::ARRAY;
	new (&out.data.a) std::vector<DynamicVariable>();
	skip_ws(c);
	if (match(c, ']'))
	{
		return true; // empty array
	}
	while (true)
	{
		DynamicVariable elem;
		if (!parse_value(c, elem))
		{
			return false;
		}
		out.data.a.push_back(std::move(elem));
		skip_ws(c);
		if (match(c, ']'))
		{
			return true;
		}
		if (!match(c, ','))
		{
			return false;
		}
	}
}

bool parse_object(JsonCursor& c, DynamicVariable& out)
{
	if (!match(c, '{'))
	{
		return false;
	}
	out.clear();
	out.type = DynamicVariable::OBJECT;
	new (&out.data.o) std::unordered_map<std::string, DynamicVariable>();
	skip_ws(c);
	if (match(c, '}'))
	{
		return true; // empty object
	}
	while (true)
	{
		std::string key;
		if (!parse_string(c, key))
		{
			return false;
		}
		if (!match(c, ':'))
		{
			return false;
		}
		DynamicVariable val;
		if (!parse_value(c, val))
		{
			return false;
		}
		out.data.o.emplace(std::move(key), std::move(val));
		skip_ws(c);
		if (match(c, '}'))
		{
			return true;
		}
		if (!match(c, ','))
		{
			return false;
		}
	}
}

bool parse_value(JsonCursor& c, DynamicVariable& out)
{
	skip_ws(c);
	if (c.i >= c.s->size())
	{
		return false;
	}
	char ch = (*c.s)[c.i];
	if (ch == '"')
	{
		std::string tmp;
		if (!parse_string(c, tmp))
		{
			return false;
		}
		out = DynamicVariable::make_string(std::move(tmp));
		return true;
	}
	if (ch == '{')
	{
		return parse_object(c, out);
	}
	if (ch == '[')
	{
		return parse_array(c, out);
	}
	if (ch == 't' && c.s->compare(c.i, 4, "true") == 0)
	{
		c.i += 4;
		out = DynamicVariable::make_bool(true);
		return true;
	}
	if (ch == 'f' && c.s->compare(c.i, 5, "false") == 0)
	{
		c.i += 5;
		out = DynamicVariable::make_bool(false);
		return true;
	}
	if (ch == 'n' && c.s->compare(c.i, 4, "null") == 0)
	{
		c.i += 4;
		out = DynamicVariable::make_null();
		return true;
	}
	double num;
	if (parse_number(c, num))
	{
		out = DynamicVariable::make_number(num);
		return true;
	}
	return false;
}

bool parse_json(const std::string& text, DynamicVariable& out, size_t* error_pos)
{
	JsonCursor c{ &text, 0 };
	if (!parse_value(c, out))
	{
		if (error_pos)
			*error_pos = c.i;
		return false;
	}
	skip_ws(c);
	if (c.i != text.size())
	{
		if (error_pos)
			*error_pos = c.i;
		return false;
	}
	return true;
}
static void json_escape(const std::string& s, std::string& out)
{
	out.push_back('"');
	for (char ch : s)
	{
		switch (ch)
		{
			case '"':
				out.append("\\\"");
				break;
			case '\\':
				out.append("\\\\");
				break;
			case '\b':
				out.append("\\b");
				break;
			case '\f':
				out.append("\\f");
				break;
			case '\n':
				out.append("\\n");
				break;
			case '\r':
				out.append("\\r");
				break;
			case '\t':
				out.append("\\t");
				break;
			default:
				if ((unsigned char)ch < 0x20)
				{
					char buf[7];
					std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)ch);
					out.append(buf);
				}
				else
					out.push_back(ch);
		}
	}
	out.push_back('"');
}
static void to_json_inner(const DynamicVariable& v, std::string& out, bool pretty, int indent, int depth)
{
	auto indent_fn = [&](int d)
	{ if(pretty) out.append(d*indent,' '); };
	switch (v.type)
	{
		case DynamicVariable::NIL:
			out += "null";
			break;
		case DynamicVariable::BOOL:
			out += (v.data.b ? "true" : "false");
			break;
		case DynamicVariable::NUMBER:
		{
			std::ostringstream oss;
			oss << v.data.num;
			out += oss.str();
			break;
		}
		case DynamicVariable::STRING:
			json_escape(v.data.s, out);
			break;
		case DynamicVariable::ARRAY:
		{
			out.push_back('[');
			if (!v.data.a.empty())
			{
				if (pretty)
					out.push_back('\n');
				for (size_t i = 0; i < v.data.a.size(); ++i)
				{
					if (pretty)
						indent_fn(depth + 1);
					to_json_inner(v.data.a[i], out, pretty, indent, depth + 1);
					if (i + 1 < v.data.a.size())
						out.push_back(',');
					if (pretty)
						out.push_back('\n');
				}
				if (pretty)
					indent_fn(depth);
			}
			out.push_back(']');
			break;
		}
		case DynamicVariable::OBJECT:
		{
			out.push_back('{');
			if (!v.data.o.empty())
			{
				if (pretty)
					out.push_back('\n');
				size_t i = 0;
				for (auto& kv : v.data.o)
				{
					if (pretty)
						indent_fn(depth + 1);
					json_escape(kv.first, out);
					out.push_back(':');
					if (pretty)
						out.push_back(' ');
					to_json_inner(kv.second, out, pretty, indent, depth + 1);
					if (++i < v.data.o.size())
						out.push_back(',');
					if (pretty)
						out.push_back('\n');
				}
				if (pretty)
					indent_fn(depth);
			}
			out.push_back('}');
			break;
		}
	}
}
std::string to_json(const DynamicVariable& v, bool pretty, int indent)
{
	std::string out;
	out.reserve(128);
	to_json_inner(v, out, pretty, indent, 0);
	return out;
}

static void print_r_inner(const DynamicVariable& v, std::string& out, int indent, int depth)
{
	auto ind = [&](int d)
	{ out.append(d * indent, ' '); };
	switch (v.type)
	{
		case DynamicVariable::NIL:
			out += "null";
			break;
		case DynamicVariable::STRING:
			out += '"';
			out += v.data.s;
			out += '"';
			break;
		case DynamicVariable::NUMBER:
		{
			std::ostringstream oss;
			oss << v.data.num;
			out += oss.str();
			break;
		}
		case DynamicVariable::BOOL:
			out += (v.data.b ? "true" : "false");
			break;
		case DynamicVariable::ARRAY:
		{
			out += "[\n";
			for (size_t i = 0; i < v.data.a.size(); ++i)
			{
				ind(depth + 1);
				print_r_inner(v.data.a[i], out, indent, depth + 1);
				if (i + 1 < v.data.a.size())
					out += ',';
				out += '\n';
			}
			ind(depth);
			out += ']';
			break;
		}
		case DynamicVariable::OBJECT:
		{
			out += "{\n";
			size_t i = 0;
			for (auto& kv : v.data.o)
			{
				ind(depth + 1);
				out += kv.first;
				out += ": ";
				print_r_inner(kv.second, out, indent, depth + 1);
				if (++i < v.data.o.size())
					out += ',';
				out += '\n';
			}
			ind(depth);
			out += '}';
			break;
		}
	}
}

std::string print_r(const DynamicVariable& v, int indent)
{
	std::string out;
	out.reserve(128);
	print_r_inner(v, out, indent, 0);
	if (!out.empty() && out.back() != '\n')
		out.push_back('\n');
	return out;
}

void print_any_limited(std::ostringstream& oss, const DynamicVariable& v, size_t limit, int indent, int depth)
{
	auto ind = [&](int d)
	{ oss << std::string(d * indent, ' '); };
	switch (v.type)
	{
		case DynamicVariable::NIL:
			oss << "null\n";
			break;
		case DynamicVariable::STRING:
			oss << '"' << v.data.s << '"' << "\n";
			break;
		case DynamicVariable::NUMBER:
			oss << v.data.num << "\n";
			break;
		case DynamicVariable::BOOL:
			oss << (v.data.b ? "true" : "false") << "\n";
			break;
		case DynamicVariable::ARRAY:
			oss << "[\n";
			if (!v.data.a.empty())
			{
				size_t printed = 0;
				for (size_t i = 0; i < v.data.a.size(); ++i)
				{
					if (limit && printed >= limit)
					{
						ind(depth + 1);
						oss << "... (truncated)\n";
						break;
					}
					ind(depth + 1);
					print_any_limited(oss, v.data.a[i], 0, indent, depth + 1);
					++printed;
				}
			}
			ind(depth);
			oss << "]\n";
			break;
		case DynamicVariable::OBJECT:
			oss << "{\n";
			if (!v.data.o.empty())
			{
				size_t printed = 0;
				for (auto it = v.data.o.begin(); it != v.data.o.end(); ++it)
				{
					if (limit && printed >= limit)
					{
						ind(depth + 1);
						oss << "... (truncated)\n";
						break;
					}
					ind(depth + 1);
					oss << it->first << ": ";
					print_any_limited(oss, it->second, 0, indent, depth + 1);
					++printed;
				}
			}
			ind(depth);
			oss << "}\n";
			break;
	}
}