add_executable(vx_app
    src/main.cpp)
target_link_libraries(vx_app PRIVATE vx::core)
set_target_properties(vx_app PROPERTIES OUTPUT_NAME voxel-engine)
vx_target_warnings(vx_app)
