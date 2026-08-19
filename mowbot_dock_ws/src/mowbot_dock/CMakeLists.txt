cmake_minimum_required(VERSION 3.16)
project(mowbot_dock)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)

# Protocol + serial layer without ROS dependencies, shared with the tests.
add_library(dock_core STATIC
  src/frame_parser.cpp
  src/serial_port.cpp
)
target_include_directories(dock_core PUBLIC src)

add_executable(dock_agent_node src/dock_agent_node.cpp)
target_link_libraries(dock_agent_node dock_core)
ament_target_dependencies(dock_agent_node rclcpp std_msgs sensor_msgs)

install(TARGETS dock_agent_node DESTINATION lib/${PROJECT_NAME})
install(PROGRAMS tools/mock_nano.py DESTINATION lib/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_frame_parser test/test_frame_parser.cpp)
  target_link_libraries(test_frame_parser dock_core)
endif()

ament_package()
