#ifndef DYNAMIC_VARIABLE_H
#define DYNAMIC_VARIABLE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <type_traits>

struct DynamicVariable
{
	enum Type { NIL, STRING, OBJECT, ARRAY, BINARY, NUMBER, BOOL } type = NIL;
	std::string s;
	std::unordered_map<std::string, DynamicVariable> o;
	std::vector<DynamicVariable> a;
	std::vector<uint8_t> bin;
	double num = 0.0;
	bool b = false;

	DynamicVariable() = default;
	DynamicVariable(const char* lit) { if (lit) { type = STRING; s = lit; } }
	DynamicVariable(std::string str) { type = STRING; s = std::move(str); }
	DynamicVariable(double v) { type = NUMBER; num = v; }
	DynamicVariable(int v) { type = NUMBER; num = static_cast<double>(v); }
	DynamicVariable(bool v) { type = BOOL; b = v; }
	DynamicVariable(std::initializer_list<DynamicVariable> list) { type = ARRAY; a.assign(list.begin(), list.end()); }
	static DynamicVariable from_object(std::initializer_list<std::pair<std::string, DynamicVariable>> kvs)
	{
		DynamicVariable d = make_object();
		for (auto& kv : kvs)
			d.o.emplace(kv.first, kv.second);
		return d;
	}

	static DynamicVariable make_string(std::string v)
	{
		DynamicVariable d;
		d.type = STRING;
		d.s = std::move(v);
		return d;
	}

	static DynamicVariable make_number(double v)
	{
		DynamicVariable d;
		d.type = NUMBER;
		d.num = v;
		return d;
	}

	static DynamicVariable make_bool(bool v)
	{
		DynamicVariable d;
		d.type = BOOL;
		d.b = v;
		return d;
	}

	static DynamicVariable make_binary(std::vector<uint8_t> v)
	{
		DynamicVariable d;
		d.type = BINARY;
		d.bin = std::move(v);
		return d;
	}

	static DynamicVariable make_object()
	{
		DynamicVariable d;
		d.type = OBJECT;
		return d;
	}

	static DynamicVariable make_array()
	{
		DynamicVariable d;
		d.type = ARRAY;
		return d;
	}

	static DynamicVariable make_null()
	{
		return DynamicVariable();
	}

	void clear()
	{
		*this = DynamicVariable();
	}

	DynamicVariable& operator[](const std::string& key)
	{
		if (type != OBJECT)
		{
			clear();
			type = OBJECT;
		}
		return o[key];
	}

	DynamicVariable& operator=(const std::string& str) { type = STRING; s = str; return *this; }
	DynamicVariable& operator=(std::string&& str) { type = STRING; s = std::move(str); return *this; }
	DynamicVariable& operator=(const char* lit) { type = STRING; s = lit ? lit : ""; return *this; }
	DynamicVariable& operator=(double v) { type = NUMBER; num = v; return *this; }
	DynamicVariable& operator=(int v) { type = NUMBER; num = static_cast<double>(v); return *this; }
	DynamicVariable& operator=(bool v) { type = BOOL; b = v; return *this; }
	DynamicVariable& operator=(std::initializer_list<DynamicVariable> list)
	{
		type = ARRAY; a.assign(list.begin(), list.end()); return *this;
	}

	DynamicVariable* find(std::string key)
	{
		if (type != OBJECT)
			return nullptr;
		auto it = o.find(key);
		return it == o.end() ? nullptr : &it->second;
	}

	void push(DynamicVariable v)
	{
		if (type != ARRAY)
		{
			if (type == NIL)
			{
				type = ARRAY;
			}
			else
				return;
		}
		a.push_back(std::move(v));
	}

	template <typename T>
	T get(std::string key, T def_value = T())
	{
		DynamicVariable* v = find(key);
		if (!v)
			return def_value;
		return v->as<T>(def_value);
	}

	template <typename T>
	T as(T def_value = T())
	{
		return convert(*this, def_value);
	}

	static std::string to_string(DynamicVariable& v)
	{
		switch (v.type)
		{
			case STRING:
				return v.s;
			case NUMBER:
				return std::to_string(v.num);
			case BOOL:
				return v.b ? "true" : "false";
			case NIL:
				return "";
			default:
				return "";
		}
	}
	template <typename T>
	static T convert(DynamicVariable& v, T def_value)
	{
		return def_value;
	}
};

bool parse_json(const std::string& text, DynamicVariable& out, size_t* error_pos = nullptr);
std::string to_json(const DynamicVariable& v, bool pretty = false, int indent = 0);
std::string print_r(const DynamicVariable& v, int indent = 2);
template <>
inline std::string DynamicVariable::convert<std::string>(DynamicVariable& v, std::string def_value)
{
	(void)def_value;
	return to_string(v);
}
template <>
inline double DynamicVariable::convert<double>(DynamicVariable& v, double def_value)
{
	return v.type == NUMBER ? v.num : def_value;
}
template <>
inline bool DynamicVariable::convert<bool>(DynamicVariable& v, bool def_value)
{
	return v.type == BOOL ? v.b : (v.type == NUMBER ? (v.num != 0) : def_value);
}

#endif
