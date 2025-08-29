# WASAPI Project

## Prerequisites

- CMake 3.16 or higher
- C++17 compatible compiler (GCC, Clang, MSVC)

## Building the Project

### Debug Build

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

### Release Build

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Clean Build

```bash
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

## Running the Application

After building, the executable will be in the `bin/` directory:

```bash
./bin/main
```

## Adding New Source Files

1. Add your `.cpp` and `.hpp` files to the `src/` directory
2. Update `src/CMakeLists.txt` if you need to:
   - Add new executables
   - Create libraries
   - Link additional libraries
3. Rebuild the project

## Project Configuration

The CMake configuration:
- Uses C++17 standard
- Outputs binaries to `bin/` directory
- Uses `build/` for intermediate files
- Includes compiler warnings (`-Wall -Wextra`)
- Supports both Debug and Release builds
