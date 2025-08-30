# Source files CMakeLists.txt

# Collect all source files
file(GLOB_RECURSE SOURCES "*.cpp" "*.c")
file(GLOB_RECURSE HEADERS "*.h" "*.hpp")

add_executable(wasapi-server wasapi-server.cpp fastcgi.cpp http.cpp dynamic_variable.cpp memory.cpp config.cpp session.cpp request.cpp fileio.cpp)

target_compile_definitions(wasapi-server PRIVATE _GNU_SOURCE)
target_link_libraries(wasapi-server PRIVATE)

# If you have additional libraries or modules, you can add them here
# Example:
# add_library(mylib STATIC mylib.cpp)
# target_link_libraries(main mylib)

# Add any system libraries you need
# Example:
# find_package(Threads REQUIRED)
# target_link_libraries(main ${CMAKE_THREAD_LIBS_INIT})
