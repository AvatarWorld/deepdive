/*
  This ROS node creates an instance of the libdeepdive driver, which it uses
  to pull data from all available trackers, as well as lighthouse/tracker info.
*/

// ROS includes
#include <ros/ros.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/transform_listener.h>

// Data messages
#include <sensor_msgs/Imu.h>
#include <deepdive_ros/Trackers.h>
#include <deepdive_ros/Light.h>

// Eigen includes
#include <Eigen/Core>
#include <Eigen/Geometry>

// UKF includes
#include <UKF/Types.h>
#include <UKF/Integrator.h>
#include <UKF/StateVector.h>
#include <UKF/MeasurementVector.h>
#include <UKF/Core.h>

// C++ includes
#include <vector>
#include <functional>

// Deepdive internal
#include "deepdive.hh"

// STATE AND PROCESS MODEL

// State indexes
enum StateElement {
  Position,               // Position (world frame, m)
  Attitude,               // Attitude as a quaternion (world to body frame)
  Velocity,               // Velocity (world frame, m/s)
  Acceleration,           // Acceleration (body frame, m/s^2)
  Omega,                  // Angular velocity (body frame, rad/s)
  GyroBias                // Gyro bias (body frame, rad/s)
};

// State vector
using State = UKF::StateVector<
  UKF::Field<Position, UKF::Vector<3>>,
  UKF::Field<Attitude, UKF::Quaternion>,
  UKF::Field<Velocity, UKF::Vector<3>>,
  UKF::Field<Acceleration, UKF::Vector<3>>,
  UKF::Field<Omega, UKF::Vector<3>>,
  UKF::Field<GyroBias, UKF::Vector<3>>
>;

// Measurement indices
enum MeasurementElement {
  Accelerometer,         // Acceleration (body frame, m/s^2)
  Gyroscope,             // Gyroscope (body frame, rads/s)
  Angle                  // Angle between lighthouse and sensor (radians)
};

// Measurement vector
using Measurement = UKF::DynamicMeasurementVector<
  UKF::Field<Accelerometer, UKF::Vector<3>>,
  UKF::Field<Gyroscope, UKF::Vector<3>>,
  UKF::Field<Angle, real_t>
>;

using Filter = UKF::Core<State, Measurement, UKF::IntegratorRK4>;

// Memory allocation for this tracker

UKF::Vector<3> gravity_;                        // Gravity vector
std::string serial_;                            // Tracker serial
deepdive_ros::Tracker tracker_;                 // Tracker parameters
std::string frame_;                             // Frame ID
Filter filter_;                                 // Filter
ros::Time last_(0);                             // Last update time
UKF::Vector<3> extrinsics_(0, 0, 0);            // Measurement sensor
UKF::Quaternion lh_att_;                        // Lighthouse attitude
UKF::Vector<3> lh_pos_;                         // Lighthouse position
uint8_t axis_ = 0;                              // Measurement axis
bool ready_ = false;                            // Parameters received
bool initialized_ = false;                      // One light measurement

namespace UKF {

template <> template <>
State State::derivative<>() const {
  State output;
  output.set_field<Position>(get_field<Velocity>());
  output.set_field<Velocity>(
    get_field<Attitude>().conjugate() * get_field<Acceleration>());
  output.set_field<Acceleration>(UKF::Vector<3>(0, 0, 0));
  UKF::Quaternion omega_q;
  omega_q.vec() = get_field<Omega>() * 0.5;
  omega_q.w() = 0;
  output.set_field<Attitude>(omega_q.conjugate() * get_field<Attitude>());
  output.set_field<Omega>(UKF::Vector<3>(0, 0, 0));
  output.set_field<GyroBias>(UKF::Vector<3>(0, 0, 0));
  return output;
}

template <> template <>
UKF::Vector<3> Measurement::expected_measurement
  <State, Accelerometer>(const State& state) {
    return state.get_field<Acceleration>()
      + state.get_field<Attitude>() * gravity_;
}

template <> template <>
UKF::Vector<3> Measurement::expected_measurement
  <State, Gyroscope>(const State& state) {
    return state.get_field<Omega>() + state.get_field<GyroBias>();
}

template <> template <>
real_t Measurement::expected_measurement
  <State, Angle>(State const& state) {
    // Position of the sensor in the tracker frame
    UKF::Vector<3> p = extrinsics_;
    // Position of the sensor in the world frame
    p = state.get_field<Attitude>().conjugate() * p + state.get_field<Position>();
    // Position of the sensor in the lighthouse frame
    p = lh_att_ * (p - lh_pos_);
    // Vertical or horizontal angle
    return (axis_ ? -atan2(p[1], p[2]) : atan2(p[0], p[2]));
}

// The angle error is about 1mm over 10m, therefore tan(1/100)^2 = 1e-08
template <>
Measurement::CovarianceVector Measurement::measurement_covariance(
  (Measurement::CovarianceVector() << 
    1.0e-4, 1.0e-4, 1.0e-4,   // Accel
    3.0e-6, 3.0e-6, 3.0e-6,   // Gyro
    1.0e-8).finished());      // Range
}

// ROS CALLBACKS

void Convert(geometry_msgs::Quaternion const& from, UKF::Quaternion & to) {
  to.w() = from.w;
  to.x() = from.x;
  to.y() = from.y;
  to.z() = from.z;
}

void Convert(geometry_msgs::Point const& from, UKF::Vector<3> & to) {
  to[0] = from.x;
  to[1] = from.y;
  to[2] = from.z;
}

void Convert(geometry_msgs::Vector3 const& from, UKF::Vector<3> & to) {
  to[0] = from.x;
  to[1] = from.y;
  to[2] = from.z;
}

// Calculate a delta t and return if it is smaller than 1s
bool Delta(ros::Time const& now, double & dt) {
  dt = (now - last_).toSec();
  last_ = now;
  // ROS_INFO_STREAM(dt);
  return (dt > 0 && dt < 1.0);
}

void TimerCallback(ros::TimerEvent const& info) {
  if (!initialized_) return;

  // Check that we are being called fast enough
  static double dt;
  if (!Delta(ros::Time::now(), dt))
    return;

  // Get the filter from the tracker information
  filter_.a_priori_step(dt);

  // Broadcast the tracker pose
  static tf2_ros::TransformBroadcaster br;
  static geometry_msgs::TransformStamped tf;
  tf.header.stamp = info.current_real;
  tf.header.frame_id = "world";
  tf.child_frame_id = frame_;
  tf.transform.translation.x = filter_.state.get_field<Position>()[0];
  tf.transform.translation.y = filter_.state.get_field<Position>()[1];
  tf.transform.translation.z = filter_.state.get_field<Position>()[2];
  tf.transform.rotation.w = filter_.state.get_field<Attitude>().w();
  tf.transform.rotation.x = filter_.state.get_field<Attitude>().x();
  tf.transform.rotation.y = filter_.state.get_field<Attitude>().y();
  tf.transform.rotation.z = filter_.state.get_field<Attitude>().z();
  br.sendTransform(tf);
}

// This will be called at approximately 120Hz
// - Single lighthouse in 'A' mode : 120Hz (60Hz per axis)
// - Dual lighthouses in b/A or b/c modes : 120Hz (30Hz per axis)
void LightCallback(deepdive_ros::Light::ConstPtr const& msg) {
  if (!ready_ || msg->header.frame_id != serial_) return;

  // Check that we are being called fast enough
  static double dt;
  if (!Delta(ros::Time::now(), dt))
    return;

  // Pull the calibrated pose of the tracker
  static tf2_ros::Buffer buffer;
  static tf2_ros::TransformListener listener(buffer);
  try {
    geometry_msgs::TransformStamped tf;
    tf = buffer.lookupTransform("world", msg->lighthouse, ros::Time(0));
    lh_att_.w() = tf.transform.rotation.w;
    lh_att_.x() = tf.transform.rotation.x;
    lh_att_.y() = tf.transform.rotation.y;
    lh_att_.z() = tf.transform.rotation.z;
    lh_pos_[0] = tf.transform.translation.x;
    lh_pos_[1] = tf.transform.translation.y;
    lh_pos_[2] = tf.transform.translation.z;
  }
  catch (tf2::TransformException &ex) {
    ROS_INFO_STREAM("LH " << msg->lighthouse << " NOT FOUND");
    return;
  }

  // Process update
  filter_.a_priori_step(dt);

  // Innovation update
  for (size_t i = 0; i < msg->pulses.size(); i++) {
    // Set the sensor ID and axis, used in the measurement model
    axis_ = msg->axis;
    extrinsics_[0] = tracker_.sensors[msg->pulses[i].sensor].position.x;
    extrinsics_[1] = tracker_.sensors[msg->pulses[i].sensor].position.y;
    extrinsics_[2] = tracker_.sensors[msg->pulses[i].sensor].position.z;
    // Prepare a measurement
    Measurement measurement;
    measurement.set_field<Angle>(msg->pulses[i].angle);
    filter_.innovation_step(measurement);
  }
 
  // Correction step 
  filter_.a_posteriori_step();
 
  // We are now initialized
  initialized_ = true;
}

// This will be called at approximately 250Hz
void ImuCallback(sensor_msgs::Imu::ConstPtr const& msg) {
  if (!ready_ || msg->header.frame_id != serial_) return;

  // Check that we are being called fast enough
  static double dt;
  if (!Delta(ros::Time::now(), dt))
    return;

  // Add the accelerometer and gyro data
  UKF::Vector<3> acc(
    tracker_.acc_scale.x * msg->linear_acceleration.x - tracker_.acc_bias.x,
    tracker_.acc_scale.y * msg->linear_acceleration.y - tracker_.acc_bias.y,
    tracker_.acc_scale.z * msg->linear_acceleration.z - tracker_.acc_bias.z);
  UKF::Vector<3> gyr(
    tracker_.gyr_scale.x * msg->angular_velocity.x - tracker_.gyr_bias.x,
    tracker_.gyr_scale.y * msg->angular_velocity.y - tracker_.gyr_bias.y,
    tracker_.gyr_scale.z * msg->angular_velocity.z - tracker_.gyr_bias.z);

  // A priori step
  filter_.a_priori_step(dt);
  
  // Measurement update
  Measurement measurement;
  measurement.set_field<Accelerometer>(acc);
  measurement.set_field<Gyroscope>(gyr);
  filter_.innovation_step(measurement);

  // A posterori step
  filter_.a_posteriori_step();
}

// This will be called once on startup by a latched topic
void TrackerCallback(deepdive_ros::Trackers::ConstPtr const& msg) {
  std::vector<deepdive_ros::Tracker>::const_iterator it;
  for (it = msg->trackers.begin(); it != msg->trackers.end(); it++) {
    if (it->serial == serial_) {
      ready_ = true;
      tracker_ = *it;
    }
  }
}

// HELPER FUNCTIONS FOR CONFIG

bool GetVectorParam(ros::NodeHandle &nh,
  std::string const& name, UKF::Vector<3> & data) {
  std::vector<double> tmp;
  if (!nh.getParam(name, tmp) || tmp.size() != 3)
    return false;
  data[0] = tmp[0];
  data[1] = tmp[1];
  data[2] = tmp[2];
  return true;
}

bool GetQuaternionParam(ros::NodeHandle &nh,
  std::string const& name, UKF::Quaternion & data) {
  std::vector<double> tmp;
  if (!nh.getParam(name, tmp) || tmp.size() != 4) {
    ROS_INFO_STREAM(tmp.size());
    return false;
  }
  data.x() = tmp[0];
  data.y() = tmp[1];
  data.z() = tmp[2];
  data.w() = tmp[3];
  return true;
}

// Main entry point of application
int main(int argc, char **argv) {
  // Initialize ROS and create node handle
  ros::init(argc, argv, "deepdive_filter");
  ros::NodeHandle nh("~");

  // Get some global information
  if (!nh.getParam("serial", serial_))
    ROS_FATAL("Failed to get serial parameter.");
  if (!nh.getParam("frame", frame_))
    ROS_FATAL("Failed to get frame parameter.");
  if (!GetVectorParam(nh, "gravity", gravity_))
    ROS_FATAL("Failed to get gravity parameter.");

  // Get the tracker update rate. Anything over the IMU rate is really not
  // adding much, since we don't have a good dynamics model.
  double rate = 100;
  if (!nh.getParam("rate",rate))
    ROS_FATAL("Failed to get rate parameter.");

  // Initial estimates
  UKF::Vector<3> est_position(0, 0, 0);
  if (!GetVectorParam(nh, "initial_estimate/position", est_position))
    ROS_FATAL("Failed to get position parameter.");
  UKF::Quaternion est_attitude(1, 0, 0, 0);
  if (!GetQuaternionParam(nh, "initial_estimate/attitude", est_attitude))
    ROS_FATAL("Failed to get attitude parameter.");
  UKF::Vector<3> est_velocity(0, 0, 0);
  if (!GetVectorParam(nh, "initial_estimate/velocity", est_velocity))
    ROS_FATAL("Failed to get velocity parameter.");
  UKF::Vector<3> est_acceleration(0, 0, 0);
  if (!GetVectorParam(nh, "initial_estimate/acceleration", est_acceleration))
    ROS_FATAL("Failed to get acceleration parameter.");
  UKF::Vector<3> est_omega(0, 0, 0);
  if (!GetVectorParam(nh, "initial_estimate/omega", est_omega))
    ROS_FATAL("Failed to get omega parameter.");
  UKF::Vector<3> est_gyro_bias(0, 0, 0);
  if (!GetVectorParam(nh, "initial_estimate/gyro_bias", est_gyro_bias))
    ROS_FATAL("Failed to get gyro_bias parameter.");

  // Initial covariances
  UKF::Vector<3> cov_position(0, 0, 0);
  if (!GetVectorParam(nh, "initial_covariance/position", cov_position))
    ROS_FATAL("Failed to get position parameter.");
  UKF::Vector<3> cov_attitude(0, 0, 0);
  if (!GetVectorParam(nh, "initial_covariance/attitude", cov_attitude))
    ROS_FATAL("Failed to get attitude parameter.");
  UKF::Vector<3> cov_velocity(0, 0, 0);
  if (!GetVectorParam(nh, "initial_covariance/velocity", cov_velocity))
    ROS_FATAL("Failed to get velocity parameter.");
  UKF::Vector<3> cov_accel(0, 0, 0);
  if (!GetVectorParam(nh, "initial_covariance/acceleration", cov_accel))
    ROS_FATAL("Failed to get acceleration parameter.");
  UKF::Vector<3> cov_omega(0, 0, 0);
  if (!GetVectorParam(nh, "initial_covariance/omega", cov_omega))
    ROS_FATAL("Failed to get omega parameter.");
  UKF::Vector<3> cov_gyro_bias(0, 0, 0);
  if (!GetVectorParam(nh, "initial_covariance/gyro_bias", cov_gyro_bias))
    ROS_FATAL("Failed to get gyro_bias parameter.");
  UKF::Vector<3> cov_accel_bias(0, 0, 0);

  // Noise
  UKF::Vector<3> noise_position(0, 0, 0);
  if (!GetVectorParam(nh, "process_noise/position", noise_position))
    ROS_FATAL("Failed to get position parameter.");
  UKF::Vector<3> noise_attitude(0, 0, 0);
  if (!GetVectorParam(nh, "process_noise/attitude", noise_attitude))
    ROS_FATAL("Failed to get attitude parameter.");
  UKF::Vector<3> noise_velocity(0, 0, 0);
  if (!GetVectorParam(nh, "process_noise/velocity", noise_velocity))
    ROS_FATAL("Failed to get velocity parameter.");
  UKF::Vector<3> noise_accel(0, 0, 0);
  if (!GetVectorParam(nh, "process_noise/acceleration", noise_accel))
    ROS_FATAL("Failed to get acceleration parameter.");
  UKF::Vector<3> noise_omega(0, 0, 0);
  if (!GetVectorParam(nh, "process_noise/omega", noise_omega))
    ROS_FATAL("Failed to get omega parameter.");
  UKF::Vector<3> noise_gyro_bias(0, 0, 0);
  if (!GetVectorParam(nh, "process_noise/gyro_bias", noise_gyro_bias))
    ROS_FATAL("Failed to get gyro_bias parameter.");

  // Setup the filter
  filter_.state.set_field<Position>(est_position);
  filter_.state.set_field<Attitude>(est_attitude);
  filter_.state.set_field<Velocity>(est_velocity);
  filter_.state.set_field<Acceleration>(est_acceleration);
  filter_.state.set_field<Omega>(est_omega);
  filter_.state.set_field<GyroBias>(est_gyro_bias);
  filter_.covariance = State::CovarianceMatrix::Zero();
  filter_.covariance.diagonal() <<
    cov_position[0], cov_position[1], cov_position[2],
    cov_attitude[0], cov_attitude[1], cov_attitude[2],
    cov_velocity[0], cov_velocity[1], cov_velocity[2],
    cov_accel[0], cov_accel[1], cov_accel[2],
    cov_omega[0], cov_omega[1], cov_omega[2],
    cov_gyro_bias[0], cov_gyro_bias[1], cov_gyro_bias[2];
  filter_.process_noise_covariance = State::CovarianceMatrix::Zero();
  filter_.process_noise_covariance.diagonal() <<
    noise_position[0], noise_position[1], noise_position[2],
    noise_attitude[0], noise_attitude[1], noise_attitude[2],
    noise_velocity[0], noise_velocity[1], noise_velocity[2],
    noise_accel[0], noise_accel[1], noise_accel[2],
    noise_omega[0], noise_omega[1], noise_omega[2],
    noise_gyro_bias[0], noise_gyro_bias[1], noise_gyro_bias[2];

  // Start a timer to callback
  ros::Timer timer = nh.createTimer(
    ros::Duration(ros::Rate(rate)), TimerCallback, false, true);

  // Subscribe to the motion and light callbacks
  ros::Subscriber sub_tracker  =
    nh.subscribe("/trackers", 10, TrackerCallback);
  //ros::Subscriber sub_imu =
  //  nh.subscribe("/imu", 10, ImuCallback);
  ros::Subscriber sub_light =
    nh.subscribe("/light", 10, LightCallback);

  // Block until safe shutdown
  ros::spin();

  // Success!
  return 0;
}
