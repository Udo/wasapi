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

	DynamicVariable();
	DynamicVariable(const char* lit);
	DynamicVariable(std::string str);
	DynamicVariable(double v);
	DynamicVariable(int v);
	DynamicVariable(bool v);

	static DynamicVariable make_string(std::string v);
	static DynamicVariable make_number(double v);
	static DynamicVariable make_bool(bool v);
	static DynamicVariable make_binary(std::vector<uint8_t> v);
	static DynamicVariable make_object();
	static DynamicVariable make_array();
	static DynamicVariable make_null();

	void clear();
	DynamicVariable& operator[](const std::string& key);

	DynamicVariable& operator=(const std::string& str);
	DynamicVariable& operator=(std::string&& str);
	DynamicVariable& operator=(const char* lit);
	DynamicVariable& operator=(double v);
	DynamicVariable& operator=(int v);
	DynamicVariable& operator=(bool v);
	DynamicVariable& operator=(std::initializer_list<DynamicVariable> list);

	DynamicVariable* find(std::string key);
	void push(DynamicVariable v);

	std::string to_string() const; 
	double to_number(double def_value = 0.0) const; 
	bool to_bool(bool def_value = false) const; 
};

bool parse_json(const std::string& text, DynamicVariable& out, size_t* error_pos = nullptr);
std::string to_json(const DynamicVariable& v, bool pretty = false, int indent = 0);
std::string print_r(const DynamicVariable& v, int indent = 2);

#endif
