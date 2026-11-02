#!/bin/sh
# Builds and runs the bench for ek_link.h with strict warnings. Needs g++.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -o test_link test_link.cpp
./test_link
