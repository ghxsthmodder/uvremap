add_executable(uvremap_unit
  unit_main.cpp
)
target_link_libraries(uvremap_unit PRIVATE uvremap_core)
add_test(NAME unit COMMAND uvremap_unit)
