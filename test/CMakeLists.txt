include(CTest)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/test)

# Deterministic HW4 runtime tests use the checked-in allocation-free runner.
add_executable(back-tester-tests
    BasicTypesTest.cpp
    CoreMarketTest.cpp
    CoreContractsTest.cpp
    SchedulerTest.cpp
    TypedSimulatedLOBTest.cpp
    TradingTest.cpp
    ResultsTest.cpp
    RuntimeTest.cpp
    NWayMarketMergerTest.cpp
    TestMain.cpp
)

target_link_libraries(back-tester-tests PRIVATE
    back-tester-common
    back-tester-lib
)

# Register one ctest entry per TEST_CASE instead of a single opaque target, so
# a failure names the case and `ctest -R <case>` can run it alone.
set(MINITEST_CTEST_FILE
    ${CMAKE_CURRENT_BINARY_DIR}/back-tester-tests-cases.cmake)

add_custom_command(TARGET back-tester-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:back-tester-tests>
        -DCTEST_FILE=${MINITEST_CTEST_FILE}
        -DTEST_PREFIX=unit.
        -P ${CMAKE_CURRENT_SOURCE_DIR}/DiscoverMiniTests.cmake
    BYPRODUCTS ${MINITEST_CTEST_FILE}
    COMMENT "Discovering MiniTest cases in back-tester-tests"
    VERBATIM
)

# Present before the first build, so a stale configure never silently drops the
# whole suite from ctest.
if(NOT EXISTS ${MINITEST_CTEST_FILE})
    file(WRITE ${MINITEST_CTEST_FILE}
        "add_test(unit.NOT_BUILT \"${CMAKE_COMMAND}\" -E false)\n")
endif()

set_property(DIRECTORY APPEND
    PROPERTY TEST_INCLUDE_FILES ${MINITEST_CTEST_FILE})

add_test(
    NAME cli-missing-path
    COMMAND ${CMAKE_COMMAND}
        -DCLI_PATH=$<TARGET_FILE:back-tester>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCliUsage.cmake
)

add_test(
    NAME cli-tiny-fixture
    COMMAND $<TARGET_FILE:back-tester> ${CMAKE_CURRENT_SOURCE_DIR}/data/tiny_mbo.jsonl
)

add_test(
    NAME cli-decimal-fixture
    COMMAND ${CMAKE_COMMAND}
        -DCLI_PATH=$<TARGET_FILE:back-tester>
        -DDATA_PATH=${CMAKE_CURRENT_SOURCE_DIR}/data/decimal_mbo.jsonl
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCliDecimal.cmake
)

add_test(
    NAME cli-invalid-unsigned-instrument
    COMMAND $<TARGET_FILE:back-tester>
        ${CMAKE_CURRENT_SOURCE_DIR}/data/invalid_unsigned_instrument.jsonl
)
set_tests_properties(cli-invalid-unsigned-instrument PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME cli-invalid-subnanounit-price
    COMMAND $<TARGET_FILE:back-tester>
        ${CMAKE_CURRENT_SOURCE_DIR}/data/invalid_subnanounit_price.jsonl
)
set_tests_properties(cli-invalid-subnanounit-price PROPERTIES WILL_FAIL TRUE)

add_executable(back-tester-scheduler-benchmark
    SchedulerBenchmark.cpp
)

target_link_libraries(back-tester-scheduler-benchmark PRIVATE
    back-tester-common
    back-tester-lib
)

target_compile_definitions(back-tester-scheduler-benchmark PRIVATE
    BACKTESTER_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

add_executable(back-tester-price-cross-benchmark
    PriceCrossBenchmark.cpp
)

target_link_libraries(back-tester-price-cross-benchmark PRIVATE
    back-tester-common
    back-tester-lib
)

target_compile_definitions(back-tester-price-cross-benchmark PRIVATE
    BACKTESTER_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

if(BUILD_COMPONENT_PIPELINE)
    # Catch2 and MiniTest both provide TEST_CASE macros and their own main, so
    # the component suite runs in a separate process.
    add_executable(back-tester-component-tests
        TypesBasicTypesTest.cpp
        DataParserTest.cpp
        HedgingTest.cpp
        MergerTest.cpp
        OrderBookRouterTest.cpp
        OrderBookTest.cpp
        PricingTest.cpp
        QueueTest.cpp
    )

    target_compile_definitions(back-tester-component-tests PRIVATE
        FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures"
    )

    target_link_libraries(back-tester-component-tests PRIVATE
        Catch2-static-lib
        Threads::Threads
        back-tester-ingestion
        back-tester-order-book
        back-tester-pricing
        back-tester-hedging
    )

    add_test(NAME component-unit-tests COMMAND back-tester-component-tests)
endif()
