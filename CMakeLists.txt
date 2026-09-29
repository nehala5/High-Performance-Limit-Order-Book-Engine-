cmake_minimum_required(VERSION 3.14)
project(OrderBook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(orderbook
    src/order.cpp
    src/trade.cpp
    src/event.cpp
    src/command.cpp
    src/execution.cpp
    src/order_book.cpp
    src/feed.cpp
    src/pipeline.cpp
    src/microstructure.cpp
    src/anomaly.cpp
)

target_include_directories(orderbook PUBLIC include)

add_executable(demo main.cpp)
target_link_libraries(demo PRIVATE orderbook)

enable_testing()
add_executable(test_book test/test_book.cpp)
target_link_libraries(test_book PRIVATE orderbook)
add_test(NAME orderbook_tests COMMAND test_book)

add_executable(test_pipeline test/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE orderbook)
add_test(NAME pipeline_tests COMMAND test_pipeline)

add_executable(test_microstructure test/test_microstructure.cpp)
target_link_libraries(test_microstructure PRIVATE orderbook)
add_test(NAME microstructure_tests COMMAND test_microstructure)

add_executable(test_anomaly test/test_anomaly.cpp)
target_link_libraries(test_anomaly PRIVATE orderbook)
add_test(NAME anomaly_tests COMMAND test_anomaly)
