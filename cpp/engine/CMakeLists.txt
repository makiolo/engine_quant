add_library(engine STATIC
    src/engine.cpp
    src/params.cpp
    src/model.cpp
    src/product.cpp
    src/measure.cpp
    src/bootstrap.cpp
)

target_include_directories(engine PUBLIC include)

target_link_libraries(engine PUBLIC engine-ffi-cxx)
