// Throwaway smoke-test node: validates that the arm64 build container can
// compile and link against rclcpp/std_msgs, the same dependency chain the
// real mowbot_dock_ros2 package will use.
#include <chrono>
#include <memory>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

using namespace std::chrono_literals;

class SmokeNode : public rclcpp::Node
{
public:
  SmokeNode() : Node("smoke_node")
  {
    publisher_ = create_publisher<std_msgs::msg::String>("dock/smoke", 10);
    timer_ = create_wall_timer(1s, [this]() {
      std_msgs::msg::String msg;
      msg.data = "dock build environment OK";
      publisher_->publish(msg);
      RCLCPP_INFO(get_logger(), "%s", msg.data.c_str());
    });
  }

private:
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<SmokeNode>());
  rclcpp::shutdown();
  return 0;
}
