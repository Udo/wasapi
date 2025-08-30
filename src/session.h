#ifndef SESSION_H
#define SESSION_H

#include <string>
#include "dynamic_variable.h"
#include "config.h"
#include "request.h"

std::string session_get_id(Request& r, bool create);

bool session_start(Request& r);

bool session_load(Request& r);

bool session_save(Request& r);

bool session_clear(Request& r);

#endif
