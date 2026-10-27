add_library(uvremap_core STATIC
  core/image.cpp
  core/report.cpp
  core/normal_math.cpp
  core/gltf_io.cpp
  core/validate.cpp
  core/grouping.cpp
  core/xatlas_runner.cpp
  core/texture_baker.cpp
  core/preview.cpp
  core/regenerate.cpp
)
target_include_directories(uvremap_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uvremap_core PUBLIC xatlas tinygltf stb)
target_compile_definitions(uvremap_core PUBLIC
  UVREMAP_VERSION="${PROJECT_VERSION}"
  UVREMAP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  UVREMAP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  UVREMAP_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

add_executable(uvremap cli/main.cpp)
target_link_libraries(uvremap PRIVATE uvremap_core)
set_target_properties(uvremap PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
