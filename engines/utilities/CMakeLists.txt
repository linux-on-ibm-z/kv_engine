add_library(engine_utilities STATIC
        engine.cc
        engine_error.cc)
kv_enable_pch(engine_utilities)
target_link_libraries(engine_utilities PRIVATE platform)
