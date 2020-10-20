cmake_minimum_required(VERSION 3.1)
project(LogFileGenerator CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS_RELEASE "-g -O3")

add_executable(logFileGenerator LogFileGenerator.cpp)
#add_executable(logStatistics LogStatistics.cpp)
