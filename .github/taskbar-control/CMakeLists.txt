cmake_minimum_required(VERSION 3.24)
project(ShellControl LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Test)
set(native "${CMAKE_CURRENT_SOURCE_DIR}/../../packages/native")
set(assets "${native}/../electron/resources")
add_executable(shell-control "${native}/tests/platform-shell/windows-taskbar.cpp"
    "${native}/src/native-desktop.cpp" "${native}/src/native-platform-shell-windows.cpp")
target_include_directories(shell-control PRIVATE "${native}/src")
target_compile_options(shell-control PRIVATE /utf-8)
target_link_libraries(shell-control PRIVATE Qt6::Widgets Qt6::Gui Qt6::Test ole32 uuid user32 shell32)
qt_add_resources(shell-control native_tray PREFIX "/workspace/tray" BASE "${assets}" FILES
    "${assets}/tray-icon.png" "${assets}/tray-icon-unread.png")
