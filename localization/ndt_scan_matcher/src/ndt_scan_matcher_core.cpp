// Copyright 2015-2019 Autoware Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ndt_scan_matcher/ndt_scan_matcher_core.hpp"

#include "ndt_scan_matcher/matrix_type.hpp"
#include "ndt_scan_matcher/particle.hpp"
#include "ndt_scan_matcher/pose_array_interpolator.hpp"
#include "ndt_scan_matcher/tree_structured_parzen_estimator.hpp"
#include "ndt_scan_matcher/util_func.hpp"

#include <tier4_autoware_utils/geometry/geometry.hpp>
#include <tier4_autoware_utils/transform/transforms.hpp>

#include <boost/math/special_functions/erf.hpp>

#include <pcl_conversions/pcl_conversions.h>

#ifdef ROS_DISTRO_GALACTIC
#include <tf2_eigen/tf2_eigen.h>
#else
#include <tf2_eigen/tf2_eigen.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <thread>

tier4_debug_msgs::msg::Float32Stamped make_float32_stamped(
  const builtin_interfaces::msg::Time & stamp, const float data)
{
  using T = tier4_debug_msgs::msg::Float32Stamped;
  return tier4_debug_msgs::build<T>().stamp(stamp).data(data);
}

tier4_debug_msgs::msg::Int32Stamped make_int32_stamped(
  const builtin_interfaces::msg::Time & stamp, const int32_t data)
{
  using T = tier4_debug_msgs::msg::Int32Stamped;
  return tier4_debug_msgs::build<T>().stamp(stamp).data(data);
}

bool validate_local_optimal_solution_oscillation(
  const std::vector<geometry_msgs::msg::Pose> & result_pose_msg_array,
  const float oscillation_threshold, const float inversion_vector_threshold)
{
  bool prev_oscillation = false;
  int oscillation_cnt = 0;

  for (size_t i = 2; i < result_pose_msg_array.size(); ++i) {
    const Eigen::Vector3d current_pose = point_to_vector3d(result_pose_msg_array.at(i).position);
    const Eigen::Vector3d prev_pose = point_to_vector3d(result_pose_msg_array.at(i - 1).position);
    const Eigen::Vector3d prev_prev_pose =
      point_to_vector3d(result_pose_msg_array.at(i - 2).position);
    const auto current_vec = current_pose - prev_pose;
    const auto prev_vec = (prev_pose - prev_prev_pose).normalized();
    const bool oscillation = prev_vec.dot(current_vec) < inversion_vector_threshold;
    if (prev_oscillation && oscillation) {
      if (static_cast<float>(oscillation_cnt) > oscillation_threshold) {
        return true;
      }
      ++oscillation_cnt;
    } else {
      oscillation_cnt = 0;
    }
    prev_oscillation = oscillation;
  }
  return false;
}

// cspell: ignore degrounded
NDTScanMatcher::NDTScanMatcher()
: Node("ndt_scan_matcher"),
  tf2_broadcaster_(*this),
  ndt_ptr_(new NormalDistributionsTransform),
  state_ptr_(new std::map<std::string, std::string>),
  inversion_vector_threshold_(-0.9),  // Not necessary to extract to ndt_scan_matcher.param.yaml
  oscillation_threshold_(10),         // Not necessary to extract to ndt_scan_matcher.param.yaml
  regularization_enabled_(declare_parameter<bool>("regularization_enabled"))
{
  (*state_ptr_)["state"] = "Initializing";
  is_activated_ = false;

  int points_queue_size = this->declare_parameter<int>("input_sensor_points_queue_size");
  points_queue_size = std::max(points_queue_size, 0);
  RCLCPP_INFO(get_logger(), "points_queue_size: %d", points_queue_size);

  base_frame_ = this->declare_parameter<std::string>("base_frame");
  RCLCPP_INFO(get_logger(), "base_frame_id: %s", base_frame_.c_str());

  ndt_base_frame_ = this->declare_parameter<std::string>("ndt_base_frame");
  RCLCPP_INFO(get_logger(), "ndt_base_frame_id: %s", ndt_base_frame_.c_str());

  map_frame_ = this->declare_parameter<std::string>("map_frame");
  RCLCPP_INFO(get_logger(), "map_frame_id: %s", map_frame_.c_str());

  pclomp::NdtParams ndt_params{};
  ndt_params.trans_epsilon = this->declare_parameter<double>("trans_epsilon");
  ndt_params.step_size = this->declare_parameter<double>("step_size");
  ndt_params.resolution = this->declare_parameter<double>("resolution");
  ndt_params.max_iterations = this->declare_parameter<int>("max_iterations");
  ndt_params.num_threads = this->declare_parameter<int>("num_threads");
  ndt_params.num_threads = std::max(ndt_params.num_threads, 1);
  ndt_params.regularization_scale_factor =
    static_cast<float>(this->declare_parameter<float>("regularization_scale_factor"));
  ndt_ptr_->setParams(ndt_params);

  RCLCPP_INFO(
    get_logger(), "trans_epsilon: %lf, step_size: %lf, resolution: %lf, max_iterations: %d",
    ndt_params.trans_epsilon, ndt_params.step_size, ndt_params.resolution,
    ndt_params.max_iterations);

  int converged_param_type_tmp = this->declare_parameter<int>("converged_param_type");
  converged_param_type_ = static_cast<ConvergedParamType>(converged_param_type_tmp);

  converged_param_transform_probability_ =
    this->declare_parameter<double>("converged_param_transform_probability");
  converged_param_nearest_voxel_transformation_likelihood_ =
    this->declare_parameter<double>("converged_param_nearest_voxel_transformation_likelihood");

  lidar_topic_timeout_sec_ = this->declare_parameter<double>("lidar_topic_timeout_sec");

  critical_upper_bound_exe_time_ms_ =
    this->declare_parameter<int>("critical_upper_bound_exe_time_ms");

  initial_pose_timeout_sec_ = this->declare_parameter<double>("initial_pose_timeout_sec");

  initial_pose_distance_tolerance_m_ =
    this->declare_parameter<double>("initial_pose_distance_tolerance_m");

  std::vector<double> output_pose_covariance =
    this->declare_parameter<std::vector<double>>("output_pose_covariance");
  for (std::size_t i = 0; i < output_pose_covariance.size(); ++i) {
    output_pose_covariance_[i] = output_pose_covariance[i];
  }

  initial_estimate_particles_num_ = this->declare_parameter<int>("initial_estimate_particles_num");
  n_startup_trials_ = this->declare_parameter<int>("n_startup_trials");

  estimate_scores_for_degrounded_scan_ =
    this->declare_parameter<bool>("estimate_scores_for_degrounded_scan");

  z_margin_for_ground_removal_ = this->declare_parameter<double>("z_margin_for_ground_removal");

  rclcpp::CallbackGroup::SharedPtr initial_pose_callback_group;
  initial_pose_callback_group =
    this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  rclcpp::CallbackGroup::SharedPtr main_callback_group;
  main_callback_group = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  auto initial_pose_sub_opt = rclcpp::SubscriptionOptions();
  initial_pose_sub_opt.callback_group = initial_pose_callback_group;

  auto main_sub_opt = rclcpp::SubscriptionOptions();
  main_sub_opt.callback_group = main_callback_group;

  initial_pose_sub_ = this->create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "ekf_pose_with_covariance", 100,
    std::bind(&NDTScanMatcher::callback_initial_pose, this, std::placeholders::_1),
    initial_pose_sub_opt);
  sensor_points_sub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
    "points_raw", rclcpp::SensorDataQoS().keep_last(points_queue_size),
    std::bind(&NDTScanMatcher::callback_sensor_points, this, std::placeholders::_1), main_sub_opt);
  regularization_pose_sub_ =
    this->create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
      "regularization_pose_with_covariance", 100,
      std::bind(&NDTScanMatcher::callback_regularization_pose, this, std::placeholders::_1));

  sensor_aligned_pose_pub_ =
    this->create_publisher<sensor_msgs::msg::PointCloud2>("points_aligned", 10);
  no_ground_points_aligned_pose_pub_ =
    this->create_publisher<sensor_msgs::msg::PointCloud2>("points_aligned_no_ground", 10);
  ndt_pose_pub_ = this->create_publisher<geometry_msgs::msg::PoseStamped>("ndt_pose", 10);
  ndt_pose_with_covariance_pub_ =
    this->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
      "ndt_pose_with_covariance", 10);
  initial_pose_with_covariance_pub_ =
    this->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
      "initial_pose_with_covariance", 10);
  exe_time_pub_ = this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>("exe_time_ms", 10);
  transform_probability_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>("transform_probability", 10);
  nearest_voxel_transformation_likelihood_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>(
      "nearest_voxel_transformation_likelihood", 10);
  no_ground_transform_probability_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>(
      "no_ground_transform_probability", 10);
  no_ground_nearest_voxel_transformation_likelihood_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>(
      "no_ground_nearest_voxel_transformation_likelihood", 10);
  iteration_num_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Int32Stamped>("iteration_num", 10);
  initial_to_result_relative_pose_pub_ =
    this->create_publisher<geometry_msgs::msg::PoseStamped>("initial_to_result_relative_pose", 10);
  initial_to_result_distance_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>("initial_to_result_distance", 10);
  initial_to_result_distance_old_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>(
      "initial_to_result_distance_old", 10);
  initial_to_result_distance_new_pub_ =
    this->create_publisher<tier4_debug_msgs::msg::Float32Stamped>(
      "initial_to_result_distance_new", 10);
  ndt_marker_pub_ = this->create_publisher<visualization_msgs::msg::MarkerArray>("ndt_marker", 10);
  diagnostics_pub_ =
    this->create_publisher<diagnostic_msgs::msg::DiagnosticArray>("/diagnostics", 10);
  ndt_monte_carlo_initial_pose_marker_pub_ =
    this->create_publisher<visualization_msgs::msg::MarkerArray>(
      "monte_carlo_initial_pose_marker", 10);

  service_ = this->create_service<tier4_localization_msgs::srv::PoseWithCovarianceStamped>(
    "ndt_align_srv",
    std::bind(
      &NDTScanMatcher::service_ndt_align, this, std::placeholders::_1, std::placeholders::_2),
    rclcpp::ServicesQoS().get_rmw_qos_profile(), main_callback_group);
  service_trigger_node_ = this->create_service<std_srvs::srv::SetBool>(
    "trigger_node_srv",
    std::bind(
      &NDTScanMatcher::service_trigger_node, this, std::placeholders::_1, std::placeholders::_2),
    rclcpp::ServicesQoS().get_rmw_qos_profile(), main_callback_group);

  diagnostic_thread_ = std::thread(&NDTScanMatcher::timer_diagnostic, this);
  diagnostic_thread_.detach();

  tf2_listener_module_ = std::make_shared<Tf2ListenerModule>(this);

  use_dynamic_map_loading_ = this->declare_parameter<bool>("use_dynamic_map_loading");
  if (use_dynamic_map_loading_) {
    map_update_module_ = std::make_unique<MapUpdateModule>(
      this, &ndt_ptr_mtx_, ndt_ptr_, tf2_listener_module_, map_frame_, main_callback_group,
      state_ptr_);
  } else {
    map_module_ = std::make_unique<MapModule>(this, &ndt_ptr_mtx_, ndt_ptr_, main_callback_group);
  }
}

void NDTScanMatcher::timer_diagnostic()
{
  rclcpp::Rate rate(100);
  while (rclcpp::ok()) {
    diagnostic_msgs::msg::DiagnosticStatus diag_status_msg;
    diag_status_msg.name = "ndt_scan_matcher";
    diag_status_msg.hardware_id = "";

    for (const auto & key_value : (*state_ptr_)) {
      diagnostic_msgs::msg::KeyValue key_value_msg;
      key_value_msg.key = key_value.first;
      key_value_msg.value = key_value.second;
      diag_status_msg.values.push_back(key_value_msg);
    }

    diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
    diag_status_msg.message = "";
    if (state_ptr_->count("state") && (*state_ptr_)["state"] == "Initializing") {
      diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
      diag_status_msg.message += "Initializing State. ";
    }
    if (
      state_ptr_->count("lidar_topic_delay_time_sec") &&
      std::stod((*state_ptr_)["lidar_topic_delay_time_sec"]) > lidar_topic_timeout_sec_) {
      diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
      diag_status_msg.message += "lidar_topic_delay_time_sec exceed limit. ";
    }
    if (
      state_ptr_->count("skipping_publish_num") &&
      std::stoi((*state_ptr_)["skipping_publish_num"]) > 1 &&
      std::stoi((*state_ptr_)["skipping_publish_num"]) < 5) {
      diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
      diag_status_msg.message += "skipping_publish_num > 1. ";
    }
    if (
      state_ptr_->count("skipping_publish_num") &&
      std::stoi((*state_ptr_)["skipping_publish_num"]) >= 5) {
      diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
      diag_status_msg.message += "skipping_publish_num exceed limit. ";
    }
    if (
      state_ptr_->count("nearest_voxel_transformation_likelihood") &&
      std::stod((*state_ptr_)["nearest_voxel_transformation_likelihood"]) <
        converged_param_nearest_voxel_transformation_likelihood_) {
      diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
      diag_status_msg.message += "NDT score is unreliably low. ";
    }
    if (
      state_ptr_->count("execution_time") &&
      std::stod((*state_ptr_)["execution_time"]) >= critical_upper_bound_exe_time_ms_) {
      diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
      diag_status_msg.message +=
        "NDT exe time is too long. (took " + (*state_ptr_)["execution_time"] + " [ms])";
    }
    // Ignore local optimal solution
    if (
      state_ptr_->count("is_local_optimal_solution_oscillation") &&
      std::stoi((*state_ptr_)["is_local_optimal_solution_oscillation"])) {
      diag_status_msg.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
      diag_status_msg.message = "local optimal solution oscillation occurred";
    }

    diagnostic_msgs::msg::DiagnosticArray diag_msg;
    diag_msg.header.stamp = this->now();
    diag_msg.status.push_back(diag_status_msg);

    diagnostics_pub_->publish(diag_msg);

    rate.sleep();
  }
}

void NDTScanMatcher::callback_initial_pose(
  const geometry_msgs::msg::PoseWithCovarianceStamped::ConstSharedPtr initial_pose_msg_ptr)
{
  if (!is_activated_) return;

  // lock mutex for initial pose
  std::lock_guard<std::mutex> initial_pose_array_lock(initial_pose_array_mtx_);
  // if rosbag restart, clear buffer
  if (!initial_pose_msg_ptr_array_.empty()) {
    const builtin_interfaces::msg::Time & t_front =
      initial_pose_msg_ptr_array_.front()->header.stamp;
    const builtin_interfaces::msg::Time & t_msg = initial_pose_msg_ptr->header.stamp;
    if (t_front.sec > t_msg.sec || (t_front.sec == t_msg.sec && t_front.nanosec > t_msg.nanosec)) {
      initial_pose_msg_ptr_array_.clear();
    }
  }

  if (initial_pose_msg_ptr->header.frame_id == map_frame_) {
    initial_pose_msg_ptr_array_.push_back(initial_pose_msg_ptr);
  } else {
    // get TF from pose_frame to map_frame
    auto tf_pose_to_map_ptr = std::make_shared<geometry_msgs::msg::TransformStamped>();
    tf2_listener_module_->get_transform(
      this->now(), map_frame_, initial_pose_msg_ptr->header.frame_id, tf_pose_to_map_ptr);

    // transform pose_frame to map_frame
    auto initial_pose_msg_in_map_ptr =
      std::make_shared<geometry_msgs::msg::PoseWithCovarianceStamped>();
    *initial_pose_msg_in_map_ptr = transform(*initial_pose_msg_ptr, *tf_pose_to_map_ptr);
    initial_pose_msg_in_map_ptr->header.stamp = initial_pose_msg_ptr->header.stamp;
    initial_pose_msg_ptr_array_.push_back(initial_pose_msg_in_map_ptr);
  }
}

void NDTScanMatcher::callback_regularization_pose(
  geometry_msgs::msg::PoseWithCovarianceStamped::ConstSharedPtr pose_conv_msg_ptr)
{
  regularization_pose_msg_ptr_array_.push_back(pose_conv_msg_ptr);
}

void NDTScanMatcher::callback_sensor_points(
  sensor_msgs::msg::PointCloud2::ConstSharedPtr sensor_points_msg_in_sensor_frame)
{
  if (sensor_points_msg_in_sensor_frame->data.empty()) {
    RCLCPP_WARN_STREAM_THROTTLE(this->get_logger(), *this->get_clock(), 1, "Empty sensor points!");
    return;
  }

  const rclcpp::Time sensor_ros_time = sensor_points_msg_in_sensor_frame->header.stamp;
  const double lidar_topic_delay_time_sec = (this->now() - sensor_ros_time).seconds();
  (*state_ptr_)["lidar_topic_delay_time_sec"] = std::to_string(lidar_topic_delay_time_sec);

  if (lidar_topic_delay_time_sec > lidar_topic_timeout_sec_) {
    RCLCPP_WARN(
      this->get_logger(),
      "The LiDAR topic is experiencing latency. The delay time is %lf[sec] (the tolerance is "
      "%lf[sec])",
      lidar_topic_delay_time_sec, lidar_topic_timeout_sec_);

    // If the delay time of the LiDAR topic exceeds the delay compensation time of ekf_localizer,
    // even if further processing continues, the estimated result will be rejected by ekf_localizer.
    // Therefore, it would be acceptable to exit the function here.
    // However, for now, we will continue the processing as it is.

    // return;
  }

  // mutex ndt_ptr_
  std::lock_guard<std::mutex> lock(ndt_ptr_mtx_);

  const auto exe_start_time = std::chrono::system_clock::now();

  // preprocess input pointcloud
  pcl::shared_ptr<pcl::PointCloud<PointSource>> sensor_points_in_sensor_frame(
    new pcl::PointCloud<PointSource>);
  pcl::shared_ptr<pcl::PointCloud<PointSource>> sensor_points_in_baselink_frame(
    new pcl::PointCloud<PointSource>);
  const std::string & sensor_frame = sensor_points_msg_in_sensor_frame->header.frame_id;

  pcl::fromROSMsg(*sensor_points_msg_in_sensor_frame, *sensor_points_in_sensor_frame);
  transform_sensor_measurement(
    sensor_frame, base_frame_, sensor_points_in_sensor_frame, sensor_points_in_baselink_frame);
  ndt_ptr_->setInputSource(sensor_points_in_baselink_frame);
  if (!is_activated_) return;

  // calculate initial pose
  std::unique_lock<std::mutex> initial_pose_array_lock(initial_pose_array_mtx_);
  if (initial_pose_msg_ptr_array_.size() <= 1) {
    RCLCPP_WARN_STREAM_THROTTLE(this->get_logger(), *this->get_clock(), 1, "No Pose!");
    return;
  }
  PoseArrayInterpolator interpolator(
    this, sensor_ros_time, initial_pose_msg_ptr_array_, initial_pose_timeout_sec_,
    initial_pose_distance_tolerance_m_);
  if (!interpolator.is_success()) return;
  pop_old_pose(initial_pose_msg_ptr_array_, sensor_ros_time);
  initial_pose_array_lock.unlock();

  // if regularization is enabled and available, set pose to NDT for regularization
  if (regularization_enabled_) add_regularization_pose(sensor_ros_time);

  if (ndt_ptr_->getInputTarget() == nullptr) {
    RCLCPP_WARN_STREAM_THROTTLE(this->get_logger(), *this->get_clock(), 1, "No MAP!");
    return;
  }

  // perform ndt scan matching
  (*state_ptr_)["state"] = "Aligning";
  const Eigen::Matrix4f initial_pose_matrix =
    pose_to_matrix4f(interpolator.get_current_pose().pose.pose);
  auto output_cloud = std::make_shared<pcl::PointCloud<PointSource>>();
  ndt_ptr_->align(*output_cloud, initial_pose_matrix);
  const pclomp::NdtResult ndt_result = ndt_ptr_->getResult();
  (*state_ptr_)["state"] = "Sleeping";

  const auto exe_end_time = std::chrono::system_clock::now();
  const auto duration_micro_sec =
    std::chrono::duration_cast<std::chrono::microseconds>(exe_end_time - exe_start_time).count();
  const auto exe_time = static_cast<float>(duration_micro_sec) / 1000.0f;

  const geometry_msgs::msg::Pose result_pose_msg = matrix4f_to_pose(ndt_result.pose);
  std::vector<geometry_msgs::msg::Pose> transformation_msg_array;
  for (const auto & pose_matrix : ndt_result.transformation_array) {
    geometry_msgs::msg::Pose pose_ros = matrix4f_to_pose(pose_matrix);
    transformation_msg_array.push_back(pose_ros);
  }

  // perform several validations
  bool is_ok_iteration_num =
    validate_num_iteration(ndt_result.iteration_num, ndt_ptr_->getMaximumIterations());
  bool is_local_optimal_solution_oscillation = false;
  if (!is_ok_iteration_num) {
    is_local_optimal_solution_oscillation = validate_local_optimal_solution_oscillation(
      transformation_msg_array, oscillation_threshold_, inversion_vector_threshold_);
  }
  bool is_ok_converged_param = validate_converged_param(
    ndt_result.transform_probability, ndt_result.nearest_voxel_transformation_likelihood);
  bool is_converged = is_ok_iteration_num && is_ok_converged_param;
  static size_t skipping_publish_num = 0;
  if (is_converged) {
    skipping_publish_num = 0;
  } else {
    ++skipping_publish_num;
    RCLCPP_WARN(get_logger(), "Not Converged");
  }

  // publish
  initial_pose_with_covariance_pub_->publish(interpolator.get_current_pose());
  exe_time_pub_->publish(make_float32_stamped(sensor_ros_time, exe_time));
  transform_probability_pub_->publish(
    make_float32_stamped(sensor_ros_time, ndt_result.transform_probability));
  nearest_voxel_transformation_likelihood_pub_->publish(
    make_float32_stamped(sensor_ros_time, ndt_result.nearest_voxel_transformation_likelihood));
  iteration_num_pub_->publish(make_int32_stamped(sensor_ros_time, ndt_result.iteration_num));
  publish_tf(sensor_ros_time, result_pose_msg);
  publish_pose(sensor_ros_time, result_pose_msg, is_converged);
  publish_marker(sensor_ros_time, transformation_msg_array);
  publish_initial_to_result(
    sensor_ros_time, result_pose_msg, interpolator.get_current_pose(), interpolator.get_old_pose(),
    interpolator.get_new_pose());

  pcl::shared_ptr<pcl::PointCloud<PointSource>> sensor_points_in_map_ptr(
    new pcl::PointCloud<PointSource>);
  tier4_autoware_utils::transformPointCloud(
    *sensor_points_in_baselink_frame, *sensor_points_in_map_ptr, ndt_result.pose);
  publish_point_cloud(sensor_ros_time, map_frame_, sensor_points_in_map_ptr);

  // whether use de-grounded points calculate score
  if (estimate_scores_for_degrounded_scan_) {
    // remove ground
    pcl::shared_ptr<pcl::PointCloud<PointSource>> no_ground_points_in_map_ptr(
      new pcl::PointCloud<PointSource>);
    for (std::size_t i = 0; i < sensor_points_in_map_ptr->size(); i++) {
      const float point_z = sensor_points_in_map_ptr->points[i].z;  // NOLINT
      if (point_z - matrix4f_to_pose(ndt_result.pose).position.z > z_margin_for_ground_removal_) {
        no_ground_points_in_map_ptr->points.push_back(sensor_points_in_map_ptr->points[i]);
      }
    }
    // pub remove-ground points
    sensor_msgs::msg::PointCloud2 no_ground_points_msg_in_map;
    pcl::toROSMsg(*no_ground_points_in_map_ptr, no_ground_points_msg_in_map);
    no_ground_points_msg_in_map.header.stamp = sensor_ros_time;
    no_ground_points_msg_in_map.header.frame_id = map_frame_;
    no_ground_points_aligned_pose_pub_->publish(no_ground_points_msg_in_map);
    // calculate score
    const auto no_ground_transform_probability = static_cast<float>(
      ndt_ptr_->calculateTransformationProbability(*no_ground_points_in_map_ptr));
    const auto no_ground_nearest_voxel_transformation_likelihood = static_cast<float>(
      ndt_ptr_->calculateNearestVoxelTransformationLikelihood(*no_ground_points_in_map_ptr));
    // pub score
    no_ground_transform_probability_pub_->publish(
      make_float32_stamped(sensor_ros_time, no_ground_transform_probability));
    no_ground_nearest_voxel_transformation_likelihood_pub_->publish(
      make_float32_stamped(sensor_ros_time, no_ground_nearest_voxel_transformation_likelihood));
  }

  (*state_ptr_)["transform_probability"] = std::to_string(ndt_result.transform_probability);
  (*state_ptr_)["nearest_voxel_transformation_likelihood"] =
    std::to_string(ndt_result.nearest_voxel_transformation_likelihood);
  (*state_ptr_)["iteration_num"] = std::to_string(ndt_result.iteration_num);
  (*state_ptr_)["skipping_publish_num"] = std::to_string(skipping_publish_num);
  if (is_local_optimal_solution_oscillation) {
    (*state_ptr_)["is_local_optimal_solution_oscillation"] = "1";
  } else {
    (*state_ptr_)["is_local_optimal_solution_oscillation"] = "0";
  }
  (*state_ptr_)["execution_time"] = std::to_string(exe_time);
}

void NDTScanMatcher::transform_sensor_measurement(
  const std::string & source_frame, const std::string & target_frame,
  const pcl::shared_ptr<pcl::PointCloud<PointSource>> & sensor_points_input_ptr,
  pcl::shared_ptr<pcl::PointCloud<PointSource>> & sensor_points_output_ptr)
{
  auto tf_target_to_source_ptr = std::make_shared<geometry_msgs::msg::TransformStamped>();
  tf2_listener_module_->get_transform(
    this->now(), target_frame, source_frame, tf_target_to_source_ptr);
  const geometry_msgs::msg::PoseStamped target_to_source_pose_stamped =
    tier4_autoware_utils::transform2pose(*tf_target_to_source_ptr);
  const Eigen::Matrix4f base_to_sensor_matrix =
    pose_to_matrix4f(target_to_source_pose_stamped.pose);
  tier4_autoware_utils::transformPointCloud(
    *sensor_points_input_ptr, *sensor_points_output_ptr, base_to_sensor_matrix);
}

void NDTScanMatcher::publish_tf(
  const rclcpp::Time & sensor_ros_time, const geometry_msgs::msg::Pose & result_pose_msg)
{
  geometry_msgs::msg::PoseStamped result_pose_stamped_msg;
  result_pose_stamped_msg.header.stamp = sensor_ros_time;
  result_pose_stamped_msg.header.frame_id = map_frame_;
  result_pose_stamped_msg.pose = result_pose_msg;
  tf2_broadcaster_.sendTransform(
    tier4_autoware_utils::pose2transform(result_pose_stamped_msg, ndt_base_frame_));
}

void NDTScanMatcher::publish_pose(
  const rclcpp::Time & sensor_ros_time, const geometry_msgs::msg::Pose & result_pose_msg,
  const bool is_converged)
{
  geometry_msgs::msg::PoseStamped result_pose_stamped_msg;
  result_pose_stamped_msg.header.stamp = sensor_ros_time;
  result_pose_stamped_msg.header.frame_id = map_frame_;
  result_pose_stamped_msg.pose = result_pose_msg;

  geometry_msgs::msg::PoseWithCovarianceStamped result_pose_with_cov_msg;
  result_pose_with_cov_msg.header.stamp = sensor_ros_time;
  result_pose_with_cov_msg.header.frame_id = map_frame_;
  result_pose_with_cov_msg.pose.pose = result_pose_msg;
  result_pose_with_cov_msg.pose.covariance = output_pose_covariance_;

  if (is_converged) {
    ndt_pose_pub_->publish(result_pose_stamped_msg);
    ndt_pose_with_covariance_pub_->publish(result_pose_with_cov_msg);
  }
}

void NDTScanMatcher::publish_point_cloud(
  const rclcpp::Time & sensor_ros_time, const std::string & frame_id,
  const pcl::shared_ptr<pcl::PointCloud<PointSource>> & sensor_points_in_map_ptr)
{
  sensor_msgs::msg::PointCloud2 sensor_points_msg_in_map;
  pcl::toROSMsg(*sensor_points_in_map_ptr, sensor_points_msg_in_map);
  sensor_points_msg_in_map.header.stamp = sensor_ros_time;
  sensor_points_msg_in_map.header.frame_id = frame_id;
  sensor_aligned_pose_pub_->publish(sensor_points_msg_in_map);
}

void NDTScanMatcher::publish_marker(
  const rclcpp::Time & sensor_ros_time, const std::vector<geometry_msgs::msg::Pose> & pose_array)
{
  visualization_msgs::msg::MarkerArray marker_array;
  visualization_msgs::msg::Marker marker;
  marker.header.stamp = sensor_ros_time;
  marker.header.frame_id = map_frame_;
  marker.type = visualization_msgs::msg::Marker::ARROW;
  marker.action = visualization_msgs::msg::Marker::ADD;
  marker.scale = tier4_autoware_utils::createMarkerScale(0.3, 0.1, 0.1);
  int i = 0;
  marker.ns = "result_pose_matrix_array";
  marker.action = visualization_msgs::msg::Marker::ADD;
  for (const auto & pose_msg : pose_array) {
    marker.id = i++;
    marker.pose = pose_msg;
    marker.color = exchange_color_crc((1.0 * i) / 15.0);
    marker_array.markers.push_back(marker);
  }

  // TODO(Tier IV): delete old marker
  for (; i < ndt_ptr_->getMaximumIterations() + 2;) {
    marker.id = i++;
    marker.pose = geometry_msgs::msg::Pose();
    marker.color = exchange_color_crc(0);
    marker_array.markers.push_back(marker);
  }
  ndt_marker_pub_->publish(marker_array);
}

void NDTScanMatcher::publish_initial_to_result(
  const rclcpp::Time & sensor_ros_time, const geometry_msgs::msg::Pose & result_pose_msg,
  const geometry_msgs::msg::PoseWithCovarianceStamped & initial_pose_cov_msg,
  const geometry_msgs::msg::PoseWithCovarianceStamped & initial_pose_old_msg,
  const geometry_msgs::msg::PoseWithCovarianceStamped & initial_pose_new_msg)
{
  geometry_msgs::msg::PoseStamped initial_to_result_relative_pose_stamped;
  initial_to_result_relative_pose_stamped.pose =
    tier4_autoware_utils::inverseTransformPose(result_pose_msg, initial_pose_cov_msg.pose.pose);
  initial_to_result_relative_pose_stamped.header.stamp = sensor_ros_time;
  initial_to_result_relative_pose_stamped.header.frame_id = map_frame_;
  initial_to_result_relative_pose_pub_->publish(initial_to_result_relative_pose_stamped);

  const auto initial_to_result_distance =
    static_cast<float>(norm(initial_pose_cov_msg.pose.pose.position, result_pose_msg.position));
  initial_to_result_distance_pub_->publish(
    make_float32_stamped(sensor_ros_time, initial_to_result_distance));

  const auto initial_to_result_distance_old =
    static_cast<float>(norm(initial_pose_old_msg.pose.pose.position, result_pose_msg.position));
  initial_to_result_distance_old_pub_->publish(
    make_float32_stamped(sensor_ros_time, initial_to_result_distance_old));

  const auto initial_to_result_distance_new =
    static_cast<float>(norm(initial_pose_new_msg.pose.pose.position, result_pose_msg.position));
  initial_to_result_distance_new_pub_->publish(
    make_float32_stamped(sensor_ros_time, initial_to_result_distance_new));
}

bool NDTScanMatcher::validate_num_iteration(const int iter_num, const int max_iter_num)
{
  bool is_ok_iter_num = iter_num < max_iter_num;
  if (!is_ok_iter_num) {
    RCLCPP_WARN(
      get_logger(),
      "The number of iterations has reached its upper limit. The number of iterations: %d, Limit: "
      "%d",
      iter_num, max_iter_num);
  }
  return is_ok_iter_num;
}

bool NDTScanMatcher::validate_score(
  const double score, const double score_threshold, const std::string & score_name)
{
  bool is_ok_score = score > score_threshold;
  if (!is_ok_score) {
    RCLCPP_WARN(
      get_logger(), "%s is below the threshold. Score: %lf, Threshold: %lf", score_name.c_str(),
      score, score_threshold);
  }
  return is_ok_score;
}

bool NDTScanMatcher::validate_converged_param(
  const double & transform_probability, const double & nearest_voxel_transformation_likelihood)
{
  bool is_ok_converged_param = false;
  if (converged_param_type_ == ConvergedParamType::TRANSFORM_PROBABILITY) {
    is_ok_converged_param = validate_score(
      transform_probability, converged_param_transform_probability_, "Transform Probability");
  } else if (converged_param_type_ == ConvergedParamType::NEAREST_VOXEL_TRANSFORMATION_LIKELIHOOD) {
    is_ok_converged_param = validate_score(
      nearest_voxel_transformation_likelihood,
      converged_param_nearest_voxel_transformation_likelihood_,
      "Nearest Voxel Transformation Likelihood");
  } else {
    is_ok_converged_param = false;
    RCLCPP_ERROR_STREAM_THROTTLE(
      this->get_logger(), *this->get_clock(), 1, "Unknown converged param type.");
  }
  return is_ok_converged_param;
}

std::optional<Eigen::Matrix4f> NDTScanMatcher::interpolate_regularization_pose(
  const rclcpp::Time & sensor_ros_time)
{
  if (regularization_pose_msg_ptr_array_.empty()) {
    return std::nullopt;
  }

  // synchronization
  PoseArrayInterpolator interpolator(this, sensor_ros_time, regularization_pose_msg_ptr_array_);

  pop_old_pose(regularization_pose_msg_ptr_array_, sensor_ros_time);

  // if the interpolate_pose fails, 0.0 is stored in the stamp
  if (rclcpp::Time(interpolator.get_current_pose().header.stamp).seconds() == 0.0) {
    return std::nullopt;
  }

  return pose_to_matrix4f(interpolator.get_current_pose().pose.pose);
}

void NDTScanMatcher::add_regularization_pose(const rclcpp::Time & sensor_ros_time)
{
  ndt_ptr_->unsetRegularizationPose();
  std::optional<Eigen::Matrix4f> pose_opt = interpolate_regularization_pose(sensor_ros_time);
  if (pose_opt.has_value()) {
    ndt_ptr_->setRegularizationPose(pose_opt.value());
    RCLCPP_DEBUG_STREAM(get_logger(), "Regularization pose is set to NDT");
  }
}

void NDTScanMatcher::service_trigger_node(
  const std_srvs::srv::SetBool::Request::SharedPtr req,
  std_srvs::srv::SetBool::Response::SharedPtr res)
{
  is_activated_ = req->data;
  if (is_activated_) {
    std::lock_guard<std::mutex> initial_pose_array_lock(initial_pose_array_mtx_);
    initial_pose_msg_ptr_array_.clear();
  } else {
    (*state_ptr_)["state"] = "Initializing";
  }
  res->success = true;
}

void NDTScanMatcher::service_ndt_align(
  const tier4_localization_msgs::srv::PoseWithCovarianceStamped::Request::SharedPtr req,
  tier4_localization_msgs::srv::PoseWithCovarianceStamped::Response::SharedPtr res)
{
  // get TF from pose_frame to map_frame
  auto tf_pose_to_map_ptr = std::make_shared<geometry_msgs::msg::TransformStamped>();
  tf2_listener_module_->get_transform(
    get_clock()->now(), map_frame_, req->pose_with_covariance.header.frame_id, tf_pose_to_map_ptr);

  // transform pose_frame to map_frame
  const auto initial_pose_msg_in_map_frame =
    transform(req->pose_with_covariance, *tf_pose_to_map_ptr);
  if (use_dynamic_map_loading_) {
    map_update_module_->update_map(initial_pose_msg_in_map_frame.pose.pose.position);
  }

  // mutex Map
  std::lock_guard<std::mutex> lock(ndt_ptr_mtx_);

  if (ndt_ptr_->getInputTarget() == nullptr) {
    res->success = false;
    RCLCPP_WARN(
      get_logger(), "No InputTarget. Please check the map file and the map_loader service");
    return;
  }

  if (ndt_ptr_->getInputSource() == nullptr) {
    res->success = false;
    RCLCPP_WARN(get_logger(), "No InputSource. Please check the input lidar topic");
    return;
  }

  (*state_ptr_)["state"] = "Aligning";
  res->pose_with_covariance = align_pose(initial_pose_msg_in_map_frame);
  (*state_ptr_)["state"] = "Sleeping";
  res->success = true;
  res->pose_with_covariance.pose.covariance = req->pose_with_covariance.pose.covariance;
}

geometry_msgs::msg::PoseWithCovarianceStamped NDTScanMatcher::align_pose(
  const geometry_msgs::msg::PoseWithCovarianceStamped & initial_pose_with_cov)
{
  output_pose_with_cov_to_log(get_logger(), "align_pose_input", initial_pose_with_cov);

  const auto base_rpy = get_rpy(initial_pose_with_cov);
  const Eigen::Map<const RowMatrixXd> covariance = {
    initial_pose_with_cov.pose.covariance.data(), 6, 6};
  const double stddev_x = std::sqrt(covariance(0, 0));
  const double stddev_y = std::sqrt(covariance(1, 1));
  const double stddev_z = std::sqrt(covariance(2, 2));
  const double stddev_roll = std::sqrt(covariance(3, 3));
  const double stddev_pitch = std::sqrt(covariance(4, 4));

  // Let phi be the cumulative distribution function of the standard normal distribution.
  // It has the following relationship with the error function (erf).
  //   phi(x) = 1/2 (1 + erf(x / sqrt(2)))
  // so, 2 * phi(x) - 1 = erf(x / sqrt(2)).
  // The range taken by 2 * phi(x) - 1 is [-1, 1], so it can be used as a uniform distribution in
  // TPE. Let u = 2 * phi(x) - 1, then x = sqrt(2) * erf_inv(u). Computationally, it is not a good
  // to give erf_inv -1 and 1, so it is rounded off at (-1 + eps, 1 - eps).
  const double SQRT2 = std::sqrt(2);
  auto uniform_to_normal = [&SQRT2](const double uniform) {
    assert(-1.0 <= uniform && uniform <= 1.0);
    constexpr double epsilon = 1.0e-6;
    const double clamped = std::clamp(uniform, -1.0 + epsilon, 1.0 - epsilon);
    return boost::math::erf_inv(clamped) * SQRT2;
  };

  auto normal_to_uniform = [&SQRT2](const double normal) {
    return boost::math::erf(normal / SQRT2);
  };

  // Optimizing (x, y, z, roll, pitch, yaw) 6 dimensions.
  // The last dimension (yaw) is a loop variable.
  // Although roll and pitch are also angles, they are considered non-looping variables that follow
  // a normal distribution with a small standard deviation. This assumes that the initial pose of
  // the ego vehicle is aligned with the ground to some extent about roll and pitch.
  const std::vector<bool> is_loop_variable = {false, false, false, false, false, true};
  TreeStructuredParzenEstimator tpe(
    TreeStructuredParzenEstimator::Direction::MAXIMIZE, n_startup_trials_, is_loop_variable);

  std::vector<Particle> particle_array;
  auto output_cloud = std::make_shared<pcl::PointCloud<PointSource>>();

  for (int i = 0; i < initial_estimate_particles_num_; i++) {
    const TreeStructuredParzenEstimator::Input input = tpe.get_next_input();

    geometry_msgs::msg::Pose initial_pose;
    initial_pose.position.x =
      initial_pose_with_cov.pose.pose.position.x + uniform_to_normal(input[0]) * stddev_x;
    initial_pose.position.y =
      initial_pose_with_cov.pose.pose.position.y + uniform_to_normal(input[1]) * stddev_y;
    initial_pose.position.z =
      initial_pose_with_cov.pose.pose.position.z + uniform_to_normal(input[2]) * stddev_z;
    geometry_msgs::msg::Vector3 init_rpy;
    init_rpy.x = base_rpy.x + uniform_to_normal(input[3]) * stddev_roll;
    init_rpy.y = base_rpy.y + uniform_to_normal(input[4]) * stddev_pitch;
    init_rpy.z = base_rpy.z + input[5] * M_PI;
    tf2::Quaternion tf_quaternion;
    tf_quaternion.setRPY(init_rpy.x, init_rpy.y, init_rpy.z);
    initial_pose.orientation = tf2::toMsg(tf_quaternion);

    const Eigen::Matrix4f initial_pose_matrix = pose_to_matrix4f(initial_pose);
    ndt_ptr_->align(*output_cloud, initial_pose_matrix);
    const pclomp::NdtResult ndt_result = ndt_ptr_->getResult();

    Particle particle(
      initial_pose, matrix4f_to_pose(ndt_result.pose), ndt_result.transform_probability,
      ndt_result.iteration_num);
    particle_array.push_back(particle);
    const auto marker_array = make_debug_markers(
      get_clock()->now(), map_frame_, tier4_autoware_utils::createMarkerScale(0.3, 0.1, 0.1),
      particle, i);
    ndt_monte_carlo_initial_pose_marker_pub_->publish(marker_array);

    const geometry_msgs::msg::Pose pose = matrix4f_to_pose(ndt_result.pose);
    const geometry_msgs::msg::Vector3 rpy = get_rpy(pose);

    const double diff_x = pose.position.x - initial_pose_with_cov.pose.pose.position.x;
    const double diff_y = pose.position.y - initial_pose_with_cov.pose.pose.position.y;
    const double diff_z = pose.position.z - initial_pose_with_cov.pose.pose.position.z;
    const double diff_roll = rpy.x - base_rpy.x;
    const double diff_pitch = rpy.y - base_rpy.y;
    const double diff_yaw = rpy.z - base_rpy.z;

    // Only yaw is a loop_variable, so only simple normalization is performed.
    // All other variables are converted from normal distribution to uniform distribution.
    TreeStructuredParzenEstimator::Input result(is_loop_variable.size());
    result[0] = normal_to_uniform(diff_x / stddev_x);
    result[1] = normal_to_uniform(diff_y / stddev_y);
    result[2] = normal_to_uniform(diff_z / stddev_z);
    result[3] = normal_to_uniform(diff_roll / stddev_roll);
    result[4] = normal_to_uniform(diff_pitch / stddev_pitch);
    result[5] = diff_yaw / M_PI;
    tpe.add_trial(TreeStructuredParzenEstimator::Trial{result, ndt_result.transform_probability});

    auto sensor_points_in_map_ptr = std::make_shared<pcl::PointCloud<PointSource>>();
    tier4_autoware_utils::transformPointCloud(
      *ndt_ptr_->getInputSource(), *sensor_points_in_map_ptr, ndt_result.pose);
    publish_point_cloud(initial_pose_with_cov.header.stamp, map_frame_, sensor_points_in_map_ptr);
  }

  auto best_particle_ptr = std::max_element(
    std::begin(particle_array), std::end(particle_array),
    [](const Particle & lhs, const Particle & rhs) { return lhs.score < rhs.score; });

  geometry_msgs::msg::PoseWithCovarianceStamped result_pose_with_cov_msg;
  result_pose_with_cov_msg.header.stamp = initial_pose_with_cov.header.stamp;
  result_pose_with_cov_msg.header.frame_id = map_frame_;
  result_pose_with_cov_msg.pose.pose = best_particle_ptr->result_pose;

  output_pose_with_cov_to_log(get_logger(), "align_pose_output", result_pose_with_cov_msg);
  RCLCPP_INFO_STREAM(get_logger(), "best_score," << best_particle_ptr->score);

  return result_pose_with_cov_msg;
}
