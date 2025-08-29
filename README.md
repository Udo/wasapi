# WASAPI Project

A C++ project with CMake build infrastructure.

## Project Structure

```
├── src/           # Source files (.cpp, .hpp)
├── bin/           # Built binaries (executables, libraries)
├── build/         # Intermediate build files
├── CMakeLists.txt # Main CMake configuration
└── README.md      # This file
```

## Prerequisites

- CMake 3.16 or higher
- C++17 compatible compiler (GCC, Clang, MSVC)

## Building the Project

### Debug Build

```bash
# Create and enter build directory
cd build

# Configure the project
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build the project
cmake --build .
```

### Release Build

```bash
# Create and enter build directory
cd build

# Configure the project
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build the project
cmake --build .
```

### Clean Build

```bash
# Remove build directory and recreate
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
