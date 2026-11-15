cmake_minimum_required(VERSION 3.24)
project(WorkspaceFileStagingChecks LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "These checks require native Windows filesystem security")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Test)
enable_testing()
add_executable(windows-file-staging-test windows-file-staging-test.cpp)
target_link_libraries(windows-file-staging-test PRIVATE Qt6::Core Qt6::Test advapi32)
add_test(NAME windows-file-staging COMMAND windows-file-staging-test
    -o "${CMAKE_CURRENT_BINARY_DIR}/windows-file-staging.txt,txt")
set_tests_properties(windows-file-staging PROPERTIES TIMEOUT 60)
