cmake_minimum_required(VERSION 3.2)

project(StatsdClient LANGUAGES CXX)

add_library(StatsdClient STATIC src/statsd_client.cpp)

target_include_directories(StatsdClient PUBLIC inc)

add_subdirectory(demo)