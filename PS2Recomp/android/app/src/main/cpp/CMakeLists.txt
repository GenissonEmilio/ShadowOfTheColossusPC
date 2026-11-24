cmake_minimum_required(VERSION 3.22)
project(SotcBootstrap LANGUAGES CXX)

add_library(SotcBootstrap SHARED bootstrap.cpp)
target_compile_features(SotcBootstrap PRIVATE cxx_std_17)
target_compile_options(SotcBootstrap PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_libraries(SotcBootstrap PRIVATE dl)
