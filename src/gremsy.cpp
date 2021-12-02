#include <cstdio>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ros2_gremsy/gremsy.hpp"

namespace ros2_gremsy
{
using namespace std::chrono_literals;
using std::placeholders::_1;
GremsyDriver::GremsyDriver(const rclcpp::NodeOptions & options)
: Node("ros2_gremsy", options), com_port_("COM3")
{
  GremsyDriver(options, "COM3");
}

GremsyDriver::GremsyDriver(const rclcpp::NodeOptions & options, const std::string & com_port)
: Node("ros2_gremsy", options)
{

  declareParameters();
  com_port_ = this->get_parameter("com_port").as_string();
  baud_rate_ = this->get_parameter("baud_rate").as_int();
  state_poll_rate_ = this->get_parameter("state_poll_rate").as_int();
  goal_push_rate_ = this->get_parameter("goal_push_rate").as_int();
  gimbal_mode_ = this->get_parameter("gimbal_mode").as_int();
  tilt_axis_input_mode_ = this->get_parameter("tilt_axis_input_mode").as_int();
  tilt_axis_stabilize_ = this->get_parameter("tilt_axis_stabilize").as_bool();
  roll_axis_input_mode_ = this->get_parameter("roll_axis_input_mode").as_int();
  roll_axis_stabilize_ = this->get_parameter("roll_axis_stabilize").as_bool();
  pan_axis_input_mode_ = this->get_parameter("pan_axis_input_mode").as_int();
  pan_axis_stabilize_ = this->get_parameter("pan_axis_stabilize").as_bool();
  lock_yaw_to_vehicle_ = this->get_parameter("lock_yaw_to_vehicle").as_bool();


  // Initialize publishers
  this->imu_pub_ = this->create_publisher<sensor_msgs::msg::Imu>("~/imu", 10);
  this->encoder_pub_ = this->create_publisher<geometry_msgs::msg::Vector3Stamped>("~/encoder", 10);
  this->mount_orientation_global_pub_ =
    this->create_publisher<geometry_msgs::msg::QuaternionStamped>(
    "~/mount_orientation_global",
    10);
  this->mount_orientation_local_pub_ =
    this->create_publisher<geometry_msgs::msg::QuaternionStamped>(
    "~/mount_orientation_local",
    10);

  // Initialize subscribers
  this->desired_mount_orientation_sub_ =
    this->create_subscription<geometry_msgs::msg::QuaternionStamped>(
    "~/mount_orientation_local", 10,
    std::bind(&GremsyDriver::desiredOrientationCallback, this, std::placeholders::_1));


  // Define SDK objects
  serial_port_ = new Serial_Port(com_port_.c_str(), baud_rate_);
  gimbal_interface_ = new Gimbal_Interface(serial_port_);

  // Start ther serial interface and the gimbal SDK
  serial_port_->start();
  gimbal_interface_->start();

  if (gimbal_interface_->get_gimbal_status().mode == GIMBAL_STATE_OFF) {
    RCLCPP_INFO(this->get_logger(), "Gimbal is off, turning it on");
    gimbal_interface_->set_gimbal_motor_mode(TURN_ON);
  }
  while (gimbal_interface_->get_gimbal_status().mode < GIMBAL_STATE_ON) {
    RCLCPP_INFO(this->get_logger(), "Waiting for gimbal to turn on");
    std::this_thread::sleep_for(100ms);
  }

  // Set gimbal control modes

  gimbal_interface_->set_gimbal_mode(convertIntGimbalMode(gimbal_mode_));

  // Set modes for each axis

  control_gimbal_axis_mode_t tilt_axis_mode, roll_axis_mode, pan_axis_mode;
  tilt_axis_mode.input_mode = convertIntToAxisInputMode(tilt_axis_input_mode_);
  tilt_axis_mode.stabilize = tilt_axis_stabilize_;
  roll_axis_mode.input_mode = convertIntToAxisInputMode(roll_axis_input_mode_);
  roll_axis_mode.stabilize = roll_axis_stabilize_;
  pan_axis_mode.input_mode = convertIntToAxisInputMode(pan_axis_input_mode_);
  pan_axis_mode.stabilize = pan_axis_stabilize_;

  gimbal_interface_->set_gimbal_axes_mode(tilt_axis_mode, roll_axis_mode, pan_axis_mode);

  pool_timer_ = this->create_wall_timer(
    std::chrono::duration<double>(1.0 / state_poll_rate_),
    std::bind(&GremsyDriver::gimbalStateTimerCallback, this));

  goal_timer_ = this->create_wall_timer(
    std::chrono::duration<double>(1.0 / goal_push_rate_),
    std::bind(&GremsyDriver::gimbalGoalTimerCallback, this));


}
GremsyDriver::~GremsyDriver()
{
  // TODO: Close serial port
}

void GremsyDriver::gimbalStateTimerCallback()
{
  RCLCPP_INFO(this->get_logger(), "Gimbal state timer callback");
  // Publish Gimbal IMU
  mavlink_raw_imu_t imu_mav = gimbal_interface_->get_gimbal_raw_imu();
  imu_mav.time_usec = gimbal_interface_->get_gimbal_time_stamps().raw_imu;   // TODO implement rostime
  sensor_msgs::msg::Imu imu_ros_mag = convertImuMavlinkMessageToROSMessage(imu_mav);
  imu_pub_->publish(imu_ros_mag);

  // Publish Gimbal Encoder Values
  mavlink_mount_status_t mount_status = gimbal_interface_->get_gimbal_mount_status();
  geometry_msgs::msg::Vector3Stamped encoder_ros_msg;
  encoder_ros_msg.header.stamp = this->get_clock()->now();
  encoder_ros_msg.vector.x = ((float) mount_status.pointing_b) * DEG_TO_RAD;
  encoder_ros_msg.vector.y = ((float) mount_status.pointing_a) * DEG_TO_RAD;
  encoder_ros_msg.vector.z = ((float) mount_status.pointing_c) * DEG_TO_RAD;
  // encoder_ros_msg.header TODO time stamps

  encoder_pub_->publish(encoder_ros_msg);

  // Get Mount Orientation
  mavlink_mount_orientation_t mount_orientation = gimbal_interface_->get_gimbal_mount_orientation();

  yaw_difference_ = DEG_TO_RAD * (mount_orientation.yaw_absolute - mount_orientation.yaw);

  // Publish Camera Mount Orientation in global frame (drifting)
  mount_orientation_global_pub_->publish(
    stampQuaternion(
      tf2::toMsg(
        convertYXZtoQuaternion(
          mount_orientation.roll,
          mount_orientation.pitch,
          mount_orientation.yaw_absolute)),
      "gimbal_link", this->get_clock()->now()));

  // Publish Camera Mount Orientation in local frame (yaw relative to vehicle)
  mount_orientation_local_pub_->publish(
    stampQuaternion(
      tf2::toMsg(
        convertYXZtoQuaternion(
          mount_orientation.roll,
          mount_orientation.pitch,
          mount_orientation.yaw)),
      "gimbal_link", this->get_clock()->now()));
}

void GremsyDriver::gimbalGoalTimerCallback()
{
  RCLCPP_INFO(this->get_logger(), "Gimbal goal timer callback");
  double z = goals_->vector.z;

  if (lock_yaw_to_vehicle_) {
    z += yaw_difference_;
  }

  gimbal_interface_->set_gimbal_move(
    RAD_TO_DEG * goals_->vector.y,
    RAD_TO_DEG * goals_->vector.x,
    RAD_TO_DEG * z);
}

void GremsyDriver::desiredOrientationCallback(const geometry_msgs::msg::Vector3Stamped::SharedPtr msg)
{

  goals_ = msg;
}

void GremsyDriver::declareParameters()
{
  this->declare_parameter(
    "com_port", "/dev/ttyUSB0",
    getParamDescriptor(
      "com_port", "Serial device for the gimbal connection",
      rcl_interfaces::msg::ParameterType::PARAMETER_STRING));

  this->declare_parameter(
    "baudrate", 115200,
    getParamDescriptor(
      "baudrate", "Baudrate for the gimbal connection",
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER));

  this->declare_parameter(
    "state_poll_rate", 10.0,
    getParamDescriptor(
      "state_poll_rate", "Rate in which the gimbal data is polled and published",
      rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE, 0.0, 300.0, 1.0));

  this->declare_parameter(
    "goal_push_rate", 60.0,
    getParamDescriptor(
      "goal_push_rate", "Rate in which the gimbal are pushed to the gimbal",
      rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE, 0.0, 300.0, 1.0));

  this->declare_parameter(
    "gimbal_mode", 1,
    getParamDescriptor(
      "gimbal_mode", "Control mode of the gimbal",
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER, 0, 2, 1));

  this->declare_parameter(
    "tilt_axis_input_mode", 2,
    getParamDescriptor(
      "tilt_axis_input_mode", "Input mode of the gimbals tilt axis",
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER, 0, 2, 1));

  this->declare_parameter(
    "tilt_axis_stabilize", true,
    getParamDescriptor(
      "tilt_axis_stabilize", "Input mode of the gimbals tilt axis",
      rcl_interfaces::msg::ParameterType::PARAMETER_BOOL));

  this->declare_parameter(
    "roll_axis_input_mode", 2,
    getParamDescriptor(
      "roll_axis_input_mode", "Input mode of the gimbals tilt roll",
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER, 0, 2, 1));

  this->declare_parameter(
    "roll_axis_stabilize", true,
    getParamDescriptor(
      "roll_axis_stabilize", "Input mode of the gimbals tilt roll",
      rcl_interfaces::msg::ParameterType::PARAMETER_BOOL));

  this->declare_parameter(
    "pan_axis_input_mode", 2,
    getParamDescriptor(
      "pan_axis_input_mode", "Input mode of the gimbals tilt pan",
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER, 0, 2, 1));

  this->declare_parameter(
    "pan_axis_stabilize", true,
    getParamDescriptor(
      "pan_axis_stabilize", "Input mode of the gimbals tilt pan",
      rcl_interfaces::msg::ParameterType::PARAMETER_BOOL));

  this->declare_parameter(
    "lock_yaw_to_vehicle", true,
    getParamDescriptor(
      "lock_yaw_to_vehicle",
      "Uses the yaw relative to the gimbal mount to prevent drift issues. Only a light stabilization is applied.",
      rcl_interfaces::msg::ParameterType::PARAMETER_BOOL));
}


} // namespace ros2_gremsy
