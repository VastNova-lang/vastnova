#!/bin/bash
# Build script for VastNova compiler
#
# Usage:
#   ./build.sh
#
# After building, you can set the standard library directory via:
#   export VASTNOVA_STD="$(pwd)/std"
# If not set, the compiler will look for 'std/' in the current directory.

set -e  # exit on error

echo "=========================================="
echo "  VastNova Compiler Builder"
echo "=========================================="
echo ""

# Check if LLVM is available
if ! command -v llvm-config &> /dev/null; then
    echo "Error: llvm-config not found. Please install LLVM development packages."
    exit 1
fi

echo "Building VastNova..."
echo ""

# Compile
g++ -std=c++17 src/main.cpp src/CodeGen.cpp -I include $(llvm-config --cxxflags --ldflags --libs core) -fexceptions -o vastnova

if [ $? -eq 0 ]; then
    echo "✅ Build successful!"
    echo ""
    echo "------------------------------------------"
    echo "  Usage:"
    echo "    ./vastnova source.vn [output] [--no-run]"
    echo ""
    echo "  Standard library directory:"
    CURRENT_STD="$(pwd)/std"
    if [ -n "$VASTNOVA_STD" ]; then
        echo "    VASTNOVA_STD is set to: $VASTNOVA_STD"
    else
        echo "    VASTNOVA_STD is not set. Using './std/' in current directory."
        echo "    To use a custom location, set it before running:"
        echo "      export VASTNOVA_STD=\"$CURRENT_STD\""
        echo "    (or set it permanently in your shell profile)"
    fi
    echo "------------------------------------------"
else
    echo "❌ Build failed."
    exit 1
fi