cmake_minimum_required(VERSION 3.13)
project(socketcan_asio VERSION 1.0 LANGUAGES CXX)
find_package(Boost COMPONENTS system)
add_executable(${PROJECT_NAME})
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
target_sources(${PROJECT_NAME} PUBLIC main.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC ${Boost_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PUBLIC ${Boost_LIBRARIES} pthread)