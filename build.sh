#!/bin/bash
set -e

echo "Building VastNova..."

g++ -std=c++17 src/main.cpp src/CodeGen.cpp -I include $(llvm-config --cxxflags --ldflags --libs core) -fexceptions -o vastnova

echo "Build successful!