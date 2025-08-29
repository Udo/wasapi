#ifndef SESSION_H
#define SESSION_H

#include <string>
#include "dynamic_variable.h"
#include "config.h"
#include "fastcgi.h"

std::string session_get_id(fcgi::Request& r, bool create);

bool session_start(fcgi::Request& r);

bool session_load(fcgi::Request& r);

bool session_save(fcgi::Request& r);

bool session_clear(fcgi::Request& r);

#endif
