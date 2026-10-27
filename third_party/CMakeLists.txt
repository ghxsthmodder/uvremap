add_library(xatlas STATIC xatlas/source/xatlas/xatlas.cpp)
target_include_directories(xatlas SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/xatlas/source/xatlas)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xatlas PRIVATE -w)
endif()
find_package(Threads REQUIRED)
target_link_libraries(xatlas PUBLIC Threads::Threads)

add_library(tinygltf INTERFACE)
target_include_directories(tinygltf SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinygltf)

add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)
