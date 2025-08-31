#include "logger.h"
#include <unordered_map>
#include <string>
#include <mutex>
#include <thread>
#include <cstdio>
#include <pthread.h>
std::unordered_map<std::thread::id, std::string> thread_names; // NOLINT
