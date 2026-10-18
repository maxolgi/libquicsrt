# Stand-in for an external consumer (the quicsrt-gateway-cpp repo does
# add_subdirectory(deps/libquicsrt)): this directory is a separate CMake
# scope that touches ONLY the exported ALIAS targets - no source-tree
# include paths, no deps/ paths. If this breaks, the export contract broke.
add_executable(consumer_test ../consumer_test.cpp)
target_compile_features(consumer_test PRIVATE cxx_std_20)
set_target_properties(consumer_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(consumer_test PRIVATE quicsrt::quicsrt quicsrt::wt-deps)
add_test(NAME consumer_test COMMAND consumer_test)
set_tests_properties(consumer_test PROPERTIES TIMEOUT 30)
