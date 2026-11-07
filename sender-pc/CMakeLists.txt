cmake_minimum_required(VERSION 3.20)
project(sender_pcc LANGUAGES CXX C)   # C 供 fdk-aac(嵌套 project 不声明 LANGUAGES 会继承此处)
set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # fdk 内部旧策略会无视普通变量，必须 FORCE 进缓存
add_subdirectory(../third_party/fdk-aac ${CMAKE_BINARY_DIR}/fdk-aac EXCLUDE_FROM_ALL)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/utf-8 /W3 /EHsc /permissive-)
add_executable(sender src/main.cpp)
target_compile_definitions(sender PRIVATE UNICODE _UNICODE NOMINMAX WINRT_LEAN_AND_MEAN)
target_link_libraries(sender PRIVATE
  d3d11 dxgi mfplat mf mfuuid wmcodecdspuuid
  windowsapp runtimeobject ole32 ws2_32 uuid
  FDK-AAC::fdk-aac)
target_link_options(sender PRIVATE /MAP)
