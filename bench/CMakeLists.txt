# Benchmarks are not tests: they are not registered with ctest and never fail a
# build on a timing threshold, because CI runners are too noisy for absolute
# latency assertions. They are built so they cannot rot.
#
# Only meaningful in a Release build; the reported build type makes it obvious
# when someone benchmarks a debug binary by accident.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/bench)

set(BENCH_TARGETS
    back-tester-scheduler-benchmark
    back-tester-price-cross-benchmark
    back-tester-engine-round-trip-benchmark
)

add_executable(back-tester-scheduler-benchmark SchedulerBench.cpp)
add_executable(back-tester-price-cross-benchmark PriceCrossBench.cpp)
add_executable(back-tester-engine-round-trip-benchmark EngineRoundTripBench.cpp)

foreach(target IN LISTS BENCH_TARGETS)
    target_link_libraries(${target} PRIVATE
        back-tester-common
        back-tester-lib
    )
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE
        BACKTESTER_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
endforeach()
