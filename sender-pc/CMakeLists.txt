cmake_minimum_required(VERSION 3.20)
project(sender_pcc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/utf-8 /W3 /EHsc /permissive-)
add_executable(sender src/main.cpp)
target_compile_definitions(sender PRIVATE UNICODE _UNICODE NOMINMAX WINRT_LEAN_AND_MEAN)
target_link_libraries(sender PRIVATE
  d3d11 dxgi mfplat mf mfuuid wmcodecdspuuid
  windowsapp runtimeobject ole32 ws2_32)
