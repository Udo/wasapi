#ifndef HTTP_H
#define HTTP_H

#include <string>
#include <unordered_map>
#include <vector>
#include "dynamic_variable.h"
#include "request.h"

void parse_query_string(const std::string& input, std::unordered_map<std::string, std::string>& out);
std::string url_decode(const std::string& s);
std::string url_encode(const std::string& s);
std::string build_query(const std::unordered_map<std::string, std::string>& params);

bool extract_files_from_formdata(const std::string& body, const std::string& boundary, const std::string& upload_dir, std::unordered_map<std::string, std::string>& form_fields, DynamicVariable& files_out);

void parse_cookie_header(Request& r, DynamicVariable* cookie_var);
void parse_query_string(Request& r, DynamicVariable* query_string);
void parse_json_form_data(Request& r);
void parse_multipart_form_data(Request& r);
void parse_urlencoded_form_data(Request& r);
void parse_form_data(Request& r);
void output_headers(Request& r, std::ostringstream& oss);
void parse_endpoint_file(Request& r, DynamicVariable* file_path);

std::string base64_encode(const uint8_t* data, size_t len);
inline void trim_spaces(std::string& s);
inline int hexval(char c);

#endif
