cmake_minimum_required(VERSION 3.14)
project(OrderBook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(orderbook
    src/order.cpp
    src/trade.cpp
    src/event.cpp
    src/order_book.cpp
)

target_include_directories(orderbook PUBLIC include)

add_executable(demo main.cpp)
target_link_libraries(demo PRIVATE orderbook)

enable_testing()
add_executable(test_book test/test_book.cpp)
target_link_libraries(test_book PRIVATE orderbook)
add_test(NAME orderbook_tests COMMAND test_book)
