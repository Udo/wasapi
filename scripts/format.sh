#!/bin/bash

# Format code with clang-format
bash -lc 'find src -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -print0 | xargs -0 clang-format -i'

# Remove single line comments that start with // (ignoring leading whitespace)
# This preserves inline comments that come after code
find src -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -exec sed -i '/^[[:space:]]*\/\//d' {} \;