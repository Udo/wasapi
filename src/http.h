#ifndef HTTP_H
#define HTTP_H

#include <string>
#include <unordered_map>
#include <vector>
#include "dynamic_variable.h"

void parse_query_string(const std::string& input, std::unordered_map<std::string, std::string>& out);
std::string url_decode(const std::string& s);
std::string url_encode(const std::string& s);
std::string build_query(const std::unordered_map<std::string, std::string>& params);

bool parse_multipart_formdata(const std::string& body, const std::string& boundary, const std::string& upload_dir,
					 std::unordered_map<std::string, std::string>& form_fields, DynamicVariable& files_out);

#endif
