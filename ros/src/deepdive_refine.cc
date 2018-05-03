// ROS includes
#include <ros/ros.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/static_transform_broadcaster.h>

// Third-party includes
#include <geometry_msgs/TransformStamped.h>
#include <visualization_msgs/MarkerArray.h>
#include <nav_msgs/Path.h>
#include <std_srvs/Trigger.h>

// Non-standard datra messages
#include <deepdive_ros/Light.h>
#include <deepdive_ros/Lighthouses.h>
#include <deepdive_ros/Trackers.h>

// Ceres and logging
#include <ceres/ceres.h>
#include <ceres/rotation.h>

// We will use OpenCV to bootstrap solution
#include <opencv2/calib3d/calib3d.hpp>

// Ceres and logging
#include <Eigen/Core>
#include <Eigen/Geometry>

// C++ libraries
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

// Shared local code
#include "deepdive.hh"

// GLOBAL PARAMETERS

// List of lighthouses
LighthouseMap lighthouses_;
TrackerMap trackers_;
MeasurementMap measurements_;
CorrectionMap corrections_;

// Global strings
std::string calfile_ = "deepdive.tf2";
std::string perfile_ = "/tmp/performance.csv";
std::string frame_world_ = "world";     // World frame
std::string frame_vive_ = "vive";       // Vive frame
std::string frame_body_ = "body";       // EKF / external solution
std::string frame_truth_ = "truth";     // Vive solution

// Whether to apply corrections
bool correct_ = false;

// Reesolution of the graph
double res_ = 0.1;

// Rejection thresholds
int thresh_count_ = 4;
double thresh_angle_ = 60.0;
double thresh_duration_ = 1.0;

// Solver parameters
ceres::Solver::Options options_;

// What to solve for
bool refine_registration_ = true;
bool refine_lighthouses_ = false;
bool refine_extrinsics_ = false;
bool refine_sensors_ = false;
bool refine_head_ = false;
bool refine_params_ = false;

// Are we running in "offline" mode
bool offline_ = false;

// Should we publish rviz markers
bool visualize_ = true;

// Are we recording right now?
bool recording_ = false;

// Force the body frame to move on a plane
bool force2d_ = false;

// World -> vive registration
double wTv_[6];

// Smoothing factor
double smoothing_ = 10.0;

// Sensor visualization publisher
ros::Publisher pub_sensors_;
ros::Publisher pub_path_;
ros::Publisher pub_ekf_;

// Timer for managing offline
ros::Timer timer_;

// CERES SOLVER

// Helper function to apply a transform b = Ra + t
template <typename T> inline
void TransformInPlace(const T transform[6], T x[3]) {
  T tmp[3];
  ceres::AngleAxisRotatePoint(&transform[3], x, tmp);
  x[0] = tmp[0] + transform[0];
  x[1] = tmp[1] + transform[1];
  x[2] = tmp[2] + transform[2];
}

// Helper function to invert a transform a = R'(b - t)
template <typename T> inline
void InverseTransformInPlace(const T transform[6], T x[3]) {
  T aa[3], tmp[3];
  tmp[0] = x[0] - transform[0];
  tmp[1] = x[1] - transform[1];
  tmp[2] = x[2] - transform[2];
  aa[0] = -transform[3];
  aa[1] = -transform[4];
  aa[2] = -transform[5];
  ceres::AngleAxisRotatePoint(aa, tmp, x);
}

// Group of light measurements -- essential for accuracy
typedef std::map<std::pair<uint16_t, uint8_t>, double> Group;

// Residual error between predicted angles to a lighthouse
struct GroupCost {
  explicit GroupCost(Group const& group) : group_(group) {}
  // Called by ceres-solver to calculate error
  template <typename T>
  bool operator()(const T* const wTv,         // Vive -> World
                  const T* const vTl,         // Lighthouse -> vive
                  const T* const wTb_pos_xy,  // Body -> world (pos xy)
                  const T* const wTb_pos_z,   // Body -> world (pos z)
                  const T* const wTb_rot_xy,  // Body -> world (rot xy)
                  const T* const wTb_rot_z,   // Body -> world (rot z)
                  const T* const bTh,         // Head -> body
                  const T* const tTh,         // Head -> tracking (light)
                  const T* const sensors,     // Lighthouse calibration
                  const T* const params,      // Tracker extrinsics
                  T* residual) const {
    // The position of the sensor
    T x[3], angle[2], wTb[6];
    // Reconstruct a transform from the components
    wTb[0] = wTb_pos_xy[0];
    wTb[1] = wTb_pos_xy[1];
    wTb[2] = wTb_pos_z[0];
    wTb[3] = wTb_rot_xy[0];
    wTb[4] = wTb_rot_xy[1];
    wTb[5] = wTb_rot_z[0];
    // Used to index the residual
    size_t cnt = 0;
    // Iterate over all measurements
    Group::const_iterator gt;
    for (gt = group_.begin(); gt != group_.end(); gt++) {
      // Get the sensor and axis for this group
      uint16_t const& s = gt->first.first;
      uint8_t const& a = gt->first.second;
      // Get the sensor position in the tracking frame
      x[0] = sensors[6*s+0];
      x[1] = sensors[6*s+1];
      x[2] = sensors[6*s+2];
      // Project the sensor position into the lighthouse frame
      InverseTransformInPlace(tTh, x);    // light -> head
      TransformInPlace(bTh, x);           // head -> body
      TransformInPlace(wTb, x);           // body -> world
      InverseTransformInPlace(wTv, x);    // world -> vive
      InverseTransformInPlace(vTl, x);    // vive -> lighthouse
      // Predict the angles - Note that the 
      Predict(params, x, angle, correct_);
      // The residual angle error for the specific axis
      residual[cnt++] = angle[a] - T(gt->second);
    }
    return true;
  }
 // Internal variables
 private:
  Group group_;
};

// Residual error between sequential poses
struct MotionCost {
  explicit MotionCost() {}
  // Called by ceres-solver to calculate error
  template <typename T>
  bool operator()(const T* const prev_pos_xy,  // PREV Body -> world (pos xy)
                  const T* const prev_pos_z,   // PREV Body -> world (pos z)
                  const T* const prev_rot_xy,  // PREV Body -> world (rot xy)
                  const T* const prev_rot_z,   // PREV Body -> world (rot z)
                  const T* const next_pos_xy,  // NEXT Body -> world (pos xy)
                  const T* const next_pos_z,   // NEXT Body -> world (pos z)
                  const T* const next_rot_xy,  // NEXT Body -> world (rot xy)
                  const T* const next_rot_z,   // NEXT Body -> world (rot z)
                  T* residual) const {
    residual[0] =  prev_pos_xy[0] - next_pos_xy[0];
    residual[1] =  prev_pos_xy[1] - next_pos_xy[1];
    residual[2] =  prev_pos_z[0] - next_pos_z[0];
    residual[3] =  prev_rot_xy[0] - next_rot_xy[0];
    residual[4] =  prev_rot_xy[1] - next_rot_xy[1];
    residual[5] =  prev_rot_z[0] - next_rot_z[0];
    for (size_t i = 0; i < 6; i++)
      residual[i] *= T(smoothing_);
    return true;
  }
};

// Solve the problem
bool Solve() {
  // Create the ceres problem
  ceres::Problem problem;

  // BASIC SANITY CHECKS

  // Check measurements
  if (measurements_.empty()) {
    ROS_WARN("No measurements received, so cannot solve the problem.");
    return false;
  } else {
    double t = (measurements_.rbegin()->first - measurements_.begin()->first).toSec();
    ROS_INFO_STREAM("Processing " << measurements_.size()
      << " measurements running for " << t << " seconds from "
      << measurements_.begin()->first << " to "
      << measurements_.rbegin()->first);
  }

  // Check corrections
  if (corrections_.empty()) {
    ROS_INFO("No corrections in dataset. Assuming first body pose at origin.");
  } else {
    double t = (corrections_.rbegin()->first - corrections_.begin()->first).toSec();
    ROS_INFO_STREAM("Processing " << corrections_.size()
      << " corrections running for " << t << " seconds from "
      << corrections_.begin()->first << " to "
      << corrections_.rbegin()->first);
  }

  // BUNDLE DATA AND CORRECTIONS

  typedef std::map<ros::Time,             // Time
            std::map<uint8_t,             // Sensor
              std::map<uint8_t,           // Axis
                std::vector<double>       // Sample
              >
            >
          > Bundle;
  std::map<std::string,                   // Tracker
    std::map<std::string,                 // Lighthouse
      Bundle
    >
  > bundle;

  std::map<ros::Time, double[6]> corr;

  // We bundle measurements into into bins of width "resolution". This allows
  // us to take the average of the measurements to improve accuracy
  {
    ROS_INFO("Bundling measurements into larger discrete time units.");
    MeasurementMap::iterator mt;
    for (mt = measurements_.begin(); mt != measurements_.end(); mt++) {
      std::string const& tserial = mt->second.light.header.frame_id;
      std::string const& lserial = mt->second.light.lighthouse;
      size_t const& a = mt->second.light.axis;
      ros::Time t = ros::Time(round(mt->first.toSec() / res_) * res_);
      std::vector<deepdive_ros::Pulse>::iterator pt;
      for (pt = mt->second.light.pulses.begin(); pt != mt->second.light.pulses.end(); pt++)
        bundle[tserial][lserial][t][pt->sensor][a].push_back(pt->angle);
    }
    ROS_INFO("Bundling corrections into larger discrete time units.");
    CorrectionMap::iterator ct;
    for (ct = corrections_.begin(); ct != corrections_.end(); ct++) {
      ros::Time t = ros::Time(round(ct->first.toSec() / res_) * res_);
      Eigen::Quaterniond q(
        ct->second.transform.rotation.w,
        ct->second.transform.rotation.x,
        ct->second.transform.rotation.y,
        ct->second.transform.rotation.z);
      Eigen::AngleAxisd aa(q.toRotationMatrix());
      corr[t][0] = ct->second.transform.translation.x;
      corr[t][1] = ct->second.transform.translation.y;
      corr[t][2] = ct->second.transform.translation.z;
      corr[t][3] = aa.angle() * aa.axis()[0];
      corr[t][4] = aa.angle() * aa.axis()[1];
      corr[t][5] = aa.angle() * aa.axis()[2];
    }
  }

  // The ultimate quantity we are solving for
  std::map<ros::Time, double[6]> wTb;

  // We are going to estimate the pose of each slave lighthouse in the frame
  // of the master lighthouse (vive frame) using PNP. We can think of the
  // two lighthouses as a stereo pair that are looking at a set of corresp-
  // ondences (photosensors). We want to calibrate this stereo pair.
  {
    ROS_INFO("Using P3P to estimate tracker pose in light frame.");
    // Create a new ceres problem to solve
    ceres::Problem problem;
    // Various lighjthouse parameters
    double fov = 2.0944;                          // 120deg FOV
    double w = 1.0;                               // 1m synthetic image plane
    double z = w / (2.0 * std::tan(fov / 2.0));   // Principle distance
    uint32_t count = 0;                           // Track num transforms
    // This recursively calculates the mean, std dev for a variable
    Statistic height;
    // Iterate over lighthouses
    LighthouseMap::iterator lt;
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
      // Iterate over trackers
      TrackerMap::iterator tt;
      for (tt = trackers_.begin(); tt != trackers_.end(); tt++) {
        ROS_INFO_STREAM("- Slave " << lt->first << " and tracker " << tt->first);
        // Iterate over time epochs
        Bundle::iterator bt;
        for (bt = bundle[tt->first][lt->first].begin();
          bt != bundle[tt->first][lt->first].end(); bt++) {
          // One for each time instance
          std::vector<cv::Point3f> obj;
          std::vector<cv::Point2f> img;
          Group group;
          // Try and find correspondences for every possible sensor
          for (uint8_t s = 0; s < NUM_SENSORS; s++) {
            // Mean angles for the <lighthouse, axis>
            double angles[2];
            // Check that we have azimuth/elevation for both lighthouses
            if (!Mean(bundle[tt->first][lt->first][bt->first][s][0], angles[0]) ||
                !Mean(bundle[tt->first][lt->first][bt->first][s][1], angles[1]))
              continue;
            // Add the pre-corrected angles to the light group
            group[std::pair<uint16_t, uint8_t>(s, 0)] = angles[0];
            group[std::pair<uint16_t, uint8_t>(s, 1)] = angles[1];
            // Correct the angles using the lighthouse parameters
            Correct(lt->second.params, angles, correct_);
            // Push on the correct world sensor position
            obj.push_back(cv::Point3f(
              trackers_[tt->first].sensors[s * 6 + 0],
              trackers_[tt->first].sensors[s * 6 + 1],
              trackers_[tt->first].sensors[s * 6 + 2]));
            // Push on the coordinate in the slave image plane
            img.push_back(cv::Point2f(z * tan(angles[0]), z * tan(angles[1])));
          }
          // In the case that we have 4 or more measurements, then we can try
          // and estimate the trackers location in the lighthouse frame.
          if (obj.size() > 3) {
            cv::Mat cam = cv::Mat::eye(3, 3, cv::DataType<double>::type);
            cv::Mat dist;
            cam.at<double>(0, 0) = z;
            cam.at<double>(1, 1) = z;
            cv::Mat R(3, 1, cv::DataType<double>::type);
            cv::Mat T(3, 1, cv::DataType<double>::type);
            cv::Mat C(3, 3, cv::DataType<double>::type);
            if (cv::solvePnPRansac(obj, img, cam, dist, R, T,  false,
              100, 8.0, 0.99, cv::noArray(), cv::SOLVEPNP_UPNP)) {
              cv::Rodrigues(R, C);
              Eigen::Matrix3d rot;
              for (size_t r = 0; r < 3; r++)
                for (size_t c = 0; c < 3; c++)
                  rot(r, c) = C.at<double>(r, c);
              // Get the transform from the trackng to lighthouse frame
              Eigen::Affine3d lTt;
              lTt.translation()[0] = T.at<double>(0, 0);
              lTt.translation()[1] = T.at<double>(1, 0);
              lTt.translation()[2] = T.at<double>(2, 0);
              lTt.linear() = rot;
              // This is a great initial estimate of the true location
              Eigen::Affine3d obs;
              obs = CeresToEigen(wTv_)                  // vive -> world
                  * CeresToEigen(lt->second.vTl)        // lighthouse -> vive
                  * lTt                                 // tracking -> lighthouse
                  * CeresToEigen(tt->second.tTh)        // head -> tracking
                  * CeresToEigen(tt->second.bTh, true); // body -> head
              // Set the initial estimate to this pose
              Eigen::AngleAxisd aa(obs.linear());
              wTb[bt->first][0] = obs.translation()[0];
              wTb[bt->first][1] = obs.translation()[1];
              wTb[bt->first][2] = obs.translation()[2];
              wTb[bt->first][3] = aa.angle() * aa.axis()[0];
              wTb[bt->first][4] = aa.angle() * aa.axis()[1];
              wTb[bt->first][5] = aa.angle() * aa.axis()[2];
              // Recursive calculation of mean
              height.Feed(wTb[bt->first][2]);
              // Add the cost function
              ceres::CostFunction* cost = new ceres::AutoDiffCostFunction<GroupCost,
                ceres::DYNAMIC, 6, 6, 2, 1, 2, 1, 6, 6, NUM_SENSORS * 6,
                  NUM_PARAMS * 2>(new GroupCost(group), group.size());
              // Add the residual block
              problem.AddResidualBlock(cost, new ceres::HuberLoss(1.0),
                reinterpret_cast<double*>(wTv_),
                reinterpret_cast<double*>(lt->second.vTl),
                reinterpret_cast<double*>(&wTb[bt->first][0]),
                reinterpret_cast<double*>(&wTb[bt->first][2]),
                reinterpret_cast<double*>(&wTb[bt->first][3]),
                reinterpret_cast<double*>(&wTb[bt->first][5]),
                reinterpret_cast<double*>(tt->second.bTh),
                reinterpret_cast<double*>(tt->second.tTh),
                reinterpret_cast<double*>(tt->second.sensors),
                reinterpret_cast<double*>(lt->second.params));
              if (force2d_) {
                wTb[bt->first][3] = 0.0;    // Pitch
                wTb[bt->first][4] = 0.0;    // Roll
                problem.SetParameterBlockConstant(&wTb[bt->first][2]);
                problem.SetParameterBlockConstant(&wTb[bt->first][3]);
              }
              // If we have a previous node, then link with a motion cost
              if (smoothing_ > 0) {
                std::map<ros::Time, double[6]>::iterator c = wTb.find(bt->first);
                std::map<ros::Time, double[6]>::iterator p = std::prev(c);
                if (c != wTb.end() && p != c) {
                  // Create a cost function to represent motion
                  ceres::CostFunction* cost = new ceres::AutoDiffCostFunction
                        <MotionCost, 6, 2, 1, 2, 1, 2, 1, 2, 1>(new MotionCost());
                  // Add a residual block for error
                  problem.AddResidualBlock(cost, new ceres::HuberLoss(1.0),
                    reinterpret_cast<double*>(&p->second[0]),  // pos: xy
                    reinterpret_cast<double*>(&p->second[2]),  // pos: z
                    reinterpret_cast<double*>(&p->second[3]),  // rot: xy
                    reinterpret_cast<double*>(&p->second[5]),  // rot: z
                    reinterpret_cast<double*>(&c->second[0]),  // pos: xy
                    reinterpret_cast<double*>(&c->second[2]),  // pos: z
                    reinterpret_cast<double*>(&c->second[3]),  // rot: xy
                    reinterpret_cast<double*>(&c->second[5])); // rot: z
                }
              }
            }
            count++;
          }
        }
        // Fix tracker parameters 
        if (!refine_extrinsics_)
          problem.SetParameterBlockConstant(tt->second.bTh);
        if (!refine_head_)
          problem.SetParameterBlockConstant(tt->second.tTh);
        if (!refine_sensors_)
          problem.SetParameterBlockConstant(tt->second.sensors);
      }
       // Fix lighthouse parameters
      if (!refine_lighthouses_ || lt == lighthouses_.begin())
        problem.SetParameterBlockConstant(lt->second.vTl);
      if (!refine_params_)
        problem.SetParameterBlockConstant(lt->second.params);
    }
    if (!refine_registration_)
      problem.SetParameterBlockConstant(wTv_);



    // If we have a fixed the height use the mean height estimate
    if (force2d_) {
      std::map<ros::Time, double[6]>::iterator it;
      for (it = wTb.begin(); it != wTb.end(); it++)
        it->second[2] = height.Mean();
    }

    // Parameters before solving
    {
      LighthouseMap::iterator lt;
      for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
        ROS_INFO_STREAM("Parameters BEFORE solving: " << lt->first);
        for (uint8_t a = 0; a < 2; a++) {
          ROS_INFO_STREAM("AXIS " << a);
          ROS_INFO_STREAM("- phase: " << lt->second.params[a*NUM_PARAMS + PARAM_PHASE]);
          ROS_INFO_STREAM("- tilt: " << lt->second.params[a*NUM_PARAMS + PARAM_TILT]);
          ROS_INFO_STREAM("- gib phase: " << lt->second.params[a*NUM_PARAMS + PARAM_GIB_PHASE]);
          ROS_INFO_STREAM("- git mag: " << lt->second.params[a*NUM_PARAMS + PARAM_GIB_MAG]);
          ROS_INFO_STREAM("- curve: " << lt->second.params[a*NUM_PARAMS + PARAM_CURVE]);
        }
      }
    }

    // Now solve the problem
    ROS_INFO_STREAM("Solving optimization problem with " << count << " obs");
    ceres::Solver::Summary summary;
    ceres::Solve(options_, &problem, &summary);
    if (summary.IsSolutionUsable()) {
      ROS_INFO("Usable solution found.");
      if (visualize_) {
        ROS_INFO("- Visualizing");
        nav_msgs::Path msg;
        msg.header.stamp = ros::Time::now();
        msg.header.frame_id = frame_world_;
        std::map<ros::Time, double[6]>::iterator it;
        for (it = wTb.begin(); it != wTb.end(); it++) {
          geometry_msgs::PoseStamped ps;
          Eigen::Vector3d v(it->second[3], it->second[4], it->second[5]);
          Eigen::AngleAxisd aa;
          if (v.norm() > 0) {
            aa.angle() = v.norm();
            aa.axis() = v.normalized();
          }
          Eigen::Quaterniond q(aa);
          ps.header.stamp = it->first;
          ps.header.frame_id = frame_world_;
          ps.pose.position.x = it->second[0];
          ps.pose.position.y = it->second[1];
          ps.pose.position.z = it->second[2];
          ps.pose.orientation.w = q.w();
          ps.pose.orientation.x = q.x();
          ps.pose.orientation.y = q.y();
          ps.pose.orientation.z = q.z();
          msg.poses.push_back(ps);
        }
        pub_path_.publish(msg);
      }
      ROS_INFO("- Writing performance to file");
      std::ofstream outfile(perfile_);
      if (outfile.is_open()) {
        nav_msgs::Path msg;
        msg.header.stamp = ros::Time::now();
        msg.header.frame_id = frame_world_;
        std::map<ros::Time, double[6]>::iterator it, ct;
        for (it = wTb.begin(); it != wTb.end(); it++) {
          ct = corr.find(it->first);
          if (ct == corr.end())
            continue;
          outfile << (it->first.toSec() - wTb.begin()->first.toSec()) << ","
                  << it->second[0] << ","
                  << it->second[1] << ","
                  << it->second[2] << ","
                  << it->second[3] << ","
                  << it->second[4] << ","
                  << it->second[5] << ","
                  << ct->second[0] << ","
                  << ct->second[1] << ","
                  << ct->second[2] << ","
                  << ct->second[3] << ","
                  << ct->second[4] << ","
                  << ct->second[5] << std::endl;
          // Add the pose
          geometry_msgs::PoseStamped ps;
          Eigen::Vector3d v(ct->second[3], ct->second[4], ct->second[5]);
          Eigen::AngleAxisd aa;
          if (v.norm() > 0) {
            aa.angle() = v.norm();
            aa.axis() = v.normalized();
          }
          Eigen::Quaterniond q(aa);
          ps.header.stamp = ct->first;
          ps.header.frame_id = frame_world_;
          ps.pose.position.x = ct->second[0];
          ps.pose.position.y = ct->second[1];
          ps.pose.position.z = ct->second[2];
          ps.pose.orientation.w = q.w();
          ps.pose.orientation.x = q.x();
          ps.pose.orientation.y = q.y();
          ps.pose.orientation.z = q.z();
          msg.poses.push_back(ps);
        }
        outfile.close();
        pub_ekf_.publish(msg);
      }
      // Update transforms so we can see the solution iun rviz
      SendTransforms(frame_world_, frame_vive_, frame_body_,
        wTv_, lighthouses_, trackers_);
    } else {
      ROS_WARN("Solution is not usable.");
      return false;
    }
  }

  // Parameters before solving
  {
    LighthouseMap::iterator lt;
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
      ROS_INFO_STREAM("Parameters AFTER solving: " << lt->first);
      for (uint8_t a = 0; a < 2; a++) {
        ROS_INFO_STREAM("AXIS " << a);
        ROS_INFO_STREAM("- phase: " << lt->second.params[a*NUM_PARAMS + PARAM_PHASE]);
        ROS_INFO_STREAM("- tilt: " << lt->second.params[a*NUM_PARAMS + PARAM_TILT]);
        ROS_INFO_STREAM("- gib phase: " << lt->second.params[a*NUM_PARAMS + PARAM_GIB_PHASE]);
        ROS_INFO_STREAM("- git mag: " << lt->second.params[a*NUM_PARAMS + PARAM_GIB_MAG]);
        ROS_INFO_STREAM("- curve: " << lt->second.params[a*NUM_PARAMS + PARAM_CURVE]);
      }
    }
  }


  return true;
}

// MESSAGE CALLBACKS

void LightCallback(deepdive_ros::Light::ConstPtr const& msg) {
  // Reset the timer use din offline mode to determine the end of experiment
  timer_.stop();
  timer_.start();
  // Check that we are recording and that the tracker/lighthouse is ready
  if (!recording_ ||
    trackers_.find(msg->header.frame_id) == trackers_.end() ||
    lighthouses_.find(msg->lighthouse) == lighthouses_.end() ||
    !trackers_[msg->header.frame_id].ready ||
    !lighthouses_[msg->lighthouse].ready) return;
  // Copy over the data
  size_t deleted = 0;
  deepdive_ros::Light data = *msg;
  std::vector<deepdive_ros::Pulse>::iterator it = data.pulses.end();
  while (it-- > data.pulses.begin()) {
    if (it->angle > thresh_angle_ / 57.2958 &&    // Check angle
        it->duration < thresh_duration_ / 1e-6) {  // Check duration
      it = data.pulses.erase(it);
      deleted++;
    }
  }
  if (data.pulses.size() < thresh_count_)
    return; 
  // Add the data
  measurements_[ros::Time::now()].light = data;
}

void CorrectionCallback(tf2_msgs::TFMessage::ConstPtr const& msg) {
  // Check that we are recording and that the tracker/lighthouse is ready
  if (!recording_)
    return;
  std::vector<geometry_msgs::TransformStamped>::const_iterator it;
  for (it = msg->transforms.begin(); it != msg->transforms.end(); it++) {
    if (it->header.frame_id == frame_world_ &&
        it->child_frame_id == frame_body_) {
      corrections_[ros::Time::now()] = *it;
    }
  }
}

bool TriggerCallback(std_srvs::Trigger::Request  &req,
                     std_srvs::Trigger::Response &res)
{
  if (!recording_) {
    res.success = true;
    res.message = "Recording started.";
  }
  if (recording_) {
    // Solve the problem
    res.success = Solve();
    if (res.success)
      res.message = "Recording stopped. Solution found.";
    else
      res.message = "Recording stopped. Solution not found.";
    // Clear all the data and corrections
    measurements_.clear();
    corrections_.clear();
  }
  // Toggle recording state
  recording_ = !recording_;
  // Success
  return true;
}

// Fake a trigger when the timer expires
void TimerCallback(ros::TimerEvent const& event) {
  std_srvs::Trigger::Request req;
  std_srvs::Trigger::Response res;
  TriggerCallback(req, res);
}

// Called when a new lighthouse appears
void NewLighthouseCallback(LighthouseMap::iterator lighthouse) {
  ROS_INFO_STREAM("Found lighthouse " << lighthouse->first);
}

// Called when a new tracker appears
void NewTrackerCallback(TrackerMap::iterator tracker) {
  ROS_INFO_STREAM("Found tracker " << tracker->first);
  if (visualize_) {
    visualization_msgs::MarkerArray msg;
    std::map<std::string, Tracker>::iterator jt;
    size_t n = 0;
    for (jt = trackers_.begin(); jt != trackers_.end(); jt++, n++)  {
      for (uint16_t i = 0; i < NUM_SENSORS; i++) {
        // All this code just to convert a normal to a quaternion
        Eigen::Vector3d vfwd(jt->second.sensors[6*i+3],
          jt->second.sensors[6*i+4], jt->second.sensors[6*i+5]);
        if (vfwd.norm() > 0) {
          Eigen::Vector3d vdown(0.0, 0.0, 1.0);
          Eigen::Vector3d vright = vdown.cross(vfwd);
          vfwd = vfwd.normalized();
          vright = vright.normalized();
          vdown = vdown.normalized();
          Eigen::Matrix3d dcm;
          dcm << vfwd.x(), vright.x(), vdown.x(),
                 vfwd.y(), vright.y(), vdown.y(),
                 vfwd.z(), vright.z(), vdown.z();
          Eigen::Quaterniond q(dcm);
          // Now plot an arrow representing the normal
          static visualization_msgs::Marker marker;
          marker.header.frame_id = jt->first + "/light";
          marker.header.stamp = ros::Time::now();
          marker.ns = "sensors";
          marker.id = NUM_SENSORS * n + i;
          marker.type = visualization_msgs::Marker::ARROW;
          marker.action = visualization_msgs::Marker::ADD;
          marker.pose.position.x = jt->second.sensors[6*i+0];
          marker.pose.position.y = jt->second.sensors[6*i+1];
          marker.pose.position.z = jt->second.sensors[6*i+2];
          marker.pose.orientation.w = q.w();
          marker.pose.orientation.x = q.x();
          marker.pose.orientation.y = q.y();
          marker.pose.orientation.z = q.z();
          marker.scale.x = 0.010;
          marker.scale.y = 0.001;
          marker.scale.z = 0.001;
          marker.color.a = 1.0;
          marker.color.r = 1.0;
          marker.color.g = 0.0;
          marker.color.b = 0.0;
          msg.markers.push_back(marker);
        }
      }
    }
    pub_sensors_.publish(msg);
  }
}

// MAIN ENTRY POINT

int main(int argc, char **argv) {
  // Initialize ROS and create node handle
  ros::init(argc, argv, "deepdive_registration");
  ros::NodeHandle nh("~");

  // If we are in offline mode when we will replay the data back at 10x the
  // speed, using it all to find a calibration solution for both the body
  // as a function of  time and the lighthouse positions.
  if (!nh.getParam("offline", offline_))
    ROS_FATAL("Failed to get if we are running in offline mode.");
  if (offline_) {
    ROS_INFO("We are in offline mode. Speeding up bag replay.");
    recording_ = true;
  }

  // Get the calibration file
  if (!nh.getParam("calfile", calfile_))
    ROS_FATAL("Failed to get the calfile file.");

  // Get the performance file
  if (!nh.getParam("perfile", perfile_))
    ROS_FATAL("Failed to get the perfile file.");

  // Get some global information
  if (!nh.getParam("frames/world", frame_world_))
    ROS_FATAL("Failed to get frames/world parameter.");
  if (!nh.getParam("frames/vive", frame_vive_))
    ROS_FATAL("Failed to get frames/vive parameter.");
  if (!nh.getParam("frames/body", frame_body_))
    ROS_FATAL("Failed to get frames/body parameter.");
  if (!nh.getParam("frames/truth", frame_truth_))
    ROS_FATAL("Failed to get frames/truth parameter.");

  // Get the pose graph resolution
  if (!nh.getParam("resolution", res_))
    ROS_FATAL("Failed to get resolution parameter.");

  // Get the thresholds
  if (!nh.getParam("thresholds/count", thresh_count_))
    ROS_FATAL("Failed to get threshods/count parameter.");
  if (!nh.getParam("thresholds/angle", thresh_angle_))
    ROS_FATAL("Failed to get thresholds/angle parameter.");
  if (!nh.getParam("thresholds/duration", thresh_duration_))
    ROS_FATAL("Failed to get thresholds/duration parameter.");

  // Whether to apply light corrections
  if (!nh.getParam("correct", correct_))
    ROS_FATAL("Failed to get correct parameter.");

  // Whether to apply light corrections
  if (!nh.getParam("force2d", force2d_))
    ROS_FATAL("Failed to get force2d parameter.");

  // Whether to apply light corrections
  if (!nh.getParam("smoothing", smoothing_))
    ROS_FATAL("Failed to get smoothing parameter.");

  // What to refine
  if (!nh.getParam("refine/registration", refine_registration_))
    ROS_FATAL("Failed to get refine/registration parameter.");
  if (!nh.getParam("refine/lighthouses", refine_lighthouses_))
    ROS_FATAL("Failed to get refine/lighthouses parameter.");
  if (!nh.getParam("refine/extrinsics", refine_extrinsics_))
    ROS_FATAL("Failed to get refine/extrinsics parameter.");
  if (!nh.getParam("refine/sensors", refine_sensors_))
    ROS_FATAL("Failed to get refine/sensors parameter.");
  if (!nh.getParam("refine/head", refine_head_))
    ROS_FATAL("Failed to get refine/head parameter.");
  if (!nh.getParam("refine/params", refine_params_))
    ROS_FATAL("Failed to get refine/params parameter.");

  // Define the ceres problem
  options_.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  if (!nh.getParam("solver/max_time", options_.max_solver_time_in_seconds))
    ROS_FATAL("Failed to get the solver/max_time parameter.");
  if (!nh.getParam("solver/max_iterations", options_.max_num_iterations))
    ROS_FATAL("Failed to get the solver/max_iterations parameter.");
  if (!nh.getParam("solver/threads", options_.num_threads))
    ROS_FATAL("Failed to get the solver/threads parameter.");
  if (!nh.getParam("solver/debug", options_.minimizer_progress_to_stdout))
    ROS_FATAL("Failed to get the solver/debug parameter.");

  // Visualization option
  if (!nh.getParam("visualize", visualize_))
    ROS_FATAL("Failed to get the visualize parameter.");

  // Get the parent information
  std::vector<std::string> lighthouses;
  if (!nh.getParam("lighthouses", lighthouses))
    ROS_FATAL("Failed to get the lighthouse list.");
  std::vector<std::string>::iterator it;
  for (it = lighthouses.begin(); it != lighthouses.end(); it++) {
    std::string serial;
    if (!nh.getParam(*it + "/serial", serial))
      ROS_FATAL("Failed to get the lighthouse serial.");
    std::vector<double> transform;
    if (!nh.getParam(*it + "/transform", transform))
      ROS_FATAL("Failed to get the lighthouse transform.");
    if (transform.size() != 7) {
      ROS_FATAL("Failed to parse lighthouse transform.");
      continue;
    }
    Eigen::Quaterniond q(
      transform[6],  // qw
      transform[3],  // qx
      transform[4],  // qy
      transform[5]); // qz
    Eigen::AngleAxisd aa(q);
    lighthouses_[serial].vTl[0] = transform[0];
    lighthouses_[serial].vTl[1] = transform[1];
    lighthouses_[serial].vTl[2] = transform[2];
    lighthouses_[serial].vTl[3] = aa.angle() * aa.axis()[0];
    lighthouses_[serial].vTl[4] = aa.angle() * aa.axis()[1];
    lighthouses_[serial].vTl[5] = aa.angle() * aa.axis()[2];
    lighthouses_[serial].ready = false;
  }

  // Get the parent information
  std::vector<std::string> trackers;
  if (!nh.getParam("trackers", trackers))
    ROS_FATAL("Failed to get the tracker list.");
  std::vector<std::string>::iterator jt;
  for (jt = trackers.begin(); jt != trackers.end(); jt++) {
    std::string serial;
    if (!nh.getParam(*jt + "/serial", serial))
      ROS_FATAL("Failed to get the tracker serial.");
    std::vector<double> extrinsics;
    if (!nh.getParam(*jt + "/extrinsics", extrinsics))
      ROS_FATAL("Failed to get the tracker extrinsics.");
    if (extrinsics.size() != 7) {
      ROS_FATAL("Failed to parse tracker extrinsics.");
      continue;
    }
    Eigen::Quaterniond q(
      extrinsics[6],  // qw
      extrinsics[3],  // qx
      extrinsics[4],  // qy
      extrinsics[5]); // qz
    Eigen::AngleAxisd aa(q);
    trackers_[serial].bTh[0] = extrinsics[0];
    trackers_[serial].bTh[1] = extrinsics[1];
    trackers_[serial].bTh[2] = extrinsics[2];
    trackers_[serial].bTh[3] = aa.angle() * aa.axis()[0];
    trackers_[serial].bTh[4] = aa.angle() * aa.axis()[1];
    trackers_[serial].bTh[5] = aa.angle() * aa.axis()[2];
    trackers_[serial].ready = false;
  }

  // If reading the configuration file results in inserting the correct
  // number of static transforms into the problem, then we can publish
  // the solution for use by other entities in the system.
  if (ReadConfig(calfile_, frame_world_, frame_vive_, frame_body_,
    wTv_, lighthouses_, trackers_)) {
    ROS_INFO("Read transforms from calibration");
  } else {
    ROS_INFO("Could not read calibration file");
  }
  SendTransforms(frame_world_, frame_vive_, frame_body_,
    wTv_, lighthouses_, trackers_);

  // Subscribe to tracker and lighthouse updates
  ros::Subscriber sub_tracker  = 
    nh.subscribe<deepdive_ros::Trackers>("/trackers", 1000, std::bind(
      TrackerCallback, std::placeholders::_1, std::ref(trackers_),
        NewTrackerCallback));
  ros::Subscriber sub_lighthouse = 
    nh.subscribe<deepdive_ros::Lighthouses>("/lighthouses", 1000, std::bind(
      LighthouseCallback, std::placeholders::_1, std::ref(lighthouses_),
        NewLighthouseCallback));
  ros::Subscriber sub_light =
    nh.subscribe("/light", 1000, LightCallback);
  ros::Subscriber sub_corrections =
    nh.subscribe("/tf", 1000, CorrectionCallback);
  ros::ServiceServer service =
    nh.advertiseService("/trigger", TriggerCallback);

  // Publish sensor location and body trajectory 
  pub_sensors_ =
    nh.advertise<visualization_msgs::MarkerArray>("/sensors", 10, true);
  pub_path_ =
    nh.advertise<nav_msgs::Path>("/path", 10, true);
  pub_ekf_ =
    nh.advertise<nav_msgs::Path>("/truth", 10, true);

  // Setup a timer to automatically trigger solution on end of experiment
  timer_ = nh.createTimer(ros::Duration(1.0), TimerCallback, true, false);

  // Block until safe shutdown
  ros::spin();

  // Success!
  return 0;
}