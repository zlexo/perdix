add_executable(vx_app
    src/main.cpp)
target_link_libraries(vx_app PRIVATE vx::core vx::render)
set_target_properties(vx_app PROPERTIES OUTPUT_NAME perdix)
vx_target_warnings(vx_app)
vx_target_engine_options(vx_app)
