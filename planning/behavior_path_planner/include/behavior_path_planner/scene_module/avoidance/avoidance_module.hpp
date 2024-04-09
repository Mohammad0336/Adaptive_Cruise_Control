// Copyright 2021 Tier IV, Inc.
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

#ifndef BEHAVIOR_PATH_PLANNER__SCENE_MODULE__AVOIDANCE__AVOIDANCE_MODULE_HPP_
#define BEHAVIOR_PATH_PLANNER__SCENE_MODULE__AVOIDANCE__AVOIDANCE_MODULE_HPP_

#include "behavior_path_planner/scene_module/scene_module_interface.hpp"
#include "behavior_path_planner/scene_module/scene_module_visitor.hpp"
#include "behavior_path_planner/utils/avoidance/avoidance_module_data.hpp"
#include "behavior_path_planner/utils/avoidance/helper.hpp"
#include "behavior_path_planner/utils/path_safety_checker/safety_check.hpp"

#include <rclcpp/rclcpp.hpp>

#include <autoware_auto_perception_msgs/msg/predicted_object.hpp>
#include <autoware_auto_planning_msgs/msg/path_with_lane_id.hpp>
#include <autoware_auto_vehicle_msgs/msg/turn_indicators_command.hpp>
#include <tier4_planning_msgs/msg/avoidance_debug_msg.hpp>
#include <tier4_planning_msgs/msg/avoidance_debug_msg_array.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace behavior_path_planner
{

using motion_utils::calcSignedArcLength;
using motion_utils::findNearestIndex;

using tier4_planning_msgs::msg::AvoidanceDebugMsg;

class AvoidanceModule : public SceneModuleInterface
{
public:
  AvoidanceModule(
    const std::string & name, rclcpp::Node & node, std::shared_ptr<AvoidanceParameters> parameters,
    const std::unordered_map<std::string, std::shared_ptr<RTCInterface>> & rtc_interface_ptr_map);

  CandidateOutput planCandidate() const override;
  BehaviorModuleOutput plan() override;
  BehaviorModuleOutput planWaitingApproval() override;
  bool isExecutionRequested() const override;
  bool isExecutionReady() const override;
  void processOnEntry() override;
  void processOnExit() override;
  void updateData() override;
  void acceptVisitor(const std::shared_ptr<SceneModuleVisitor> & visitor) const override;

  void updateModuleParams(const std::any & parameters) override
  {
    parameters_ = std::any_cast<std::shared_ptr<AvoidanceParameters>>(parameters);
  }
  std::shared_ptr<AvoidanceDebugMsgArray> get_debug_msg_array() const;

private:
  bool canTransitSuccessState() override;

  bool canTransitFailureState() override { return false; }

  bool canTransitIdleToRunningState() override { return true; }

  /**
   * @brief update RTC status for candidate shift line.
   * @param candidate path.
   */
  void updateCandidateRTCStatus(const CandidateOutput & candidate)
  {
    if (candidate.lateral_shift > 0.0) {
      rtc_interface_ptr_map_.at("left")->updateCooperateStatus(
        uuid_map_.at("left"), isExecutionReady(), candidate.start_distance_to_path_change,
        candidate.finish_distance_to_path_change, clock_->now());
      candidate_uuid_ = uuid_map_.at("left");
      return;
    }
    if (candidate.lateral_shift < 0.0) {
      rtc_interface_ptr_map_.at("right")->updateCooperateStatus(
        uuid_map_.at("right"), isExecutionReady(), candidate.start_distance_to_path_change,
        candidate.finish_distance_to_path_change, clock_->now());
      candidate_uuid_ = uuid_map_.at("right");
      return;
    }

    RCLCPP_WARN_STREAM(
      getLogger(),
      "Direction is UNKNOWN, start_distance = " << candidate.start_distance_to_path_change);
  }

  /**
   * @brief update RTC status for approved shift line.
   * @param approved avoidance path.
   */
  void updateRegisteredRTCStatus(const PathWithLaneId & path)
  {
    const Point ego_position = planner_data_->self_odometry->pose.pose.position;

    for (const auto & left_shift : left_shift_array_) {
      const double start_distance =
        calcSignedArcLength(path.points, ego_position, left_shift.start_pose.position);
      const double finish_distance =
        calcSignedArcLength(path.points, ego_position, left_shift.finish_pose.position);
      rtc_interface_ptr_map_.at("left")->updateCooperateStatus(
        left_shift.uuid, true, start_distance, finish_distance, clock_->now());
      if (finish_distance > -1.0e-03) {
        steering_factor_interface_ptr_->updateSteeringFactor(
          {left_shift.start_pose, left_shift.finish_pose}, {start_distance, finish_distance},
          SteeringFactor::AVOIDANCE_PATH_CHANGE, SteeringFactor::LEFT, SteeringFactor::TURNING, "");
      }
    }

    for (const auto & right_shift : right_shift_array_) {
      const double start_distance =
        calcSignedArcLength(path.points, ego_position, right_shift.start_pose.position);
      const double finish_distance =
        calcSignedArcLength(path.points, ego_position, right_shift.finish_pose.position);
      rtc_interface_ptr_map_.at("right")->updateCooperateStatus(
        right_shift.uuid, true, start_distance, finish_distance, clock_->now());
      if (finish_distance > -1.0e-03) {
        steering_factor_interface_ptr_->updateSteeringFactor(
          {right_shift.start_pose, right_shift.finish_pose}, {start_distance, finish_distance},
          SteeringFactor::AVOIDANCE_PATH_CHANGE, SteeringFactor::RIGHT, SteeringFactor::TURNING,
          "");
      }
    }
  }

  /**
   * @brief remove RTC status for candidate path.
   */
  void removeCandidateRTCStatus()
  {
    if (rtc_interface_ptr_map_.at("left")->isRegistered(candidate_uuid_)) {
      rtc_interface_ptr_map_.at("left")->removeCooperateStatus(candidate_uuid_);
    } else if (rtc_interface_ptr_map_.at("right")->isRegistered(candidate_uuid_)) {
      rtc_interface_ptr_map_.at("right")->removeCooperateStatus(candidate_uuid_);
    }
  }

  /**
   * @brief remove RTC status for left approved path.
   */
  void removePreviousRTCStatusLeft()
  {
    if (rtc_interface_ptr_map_.at("left")->isRegistered(uuid_map_.at("left"))) {
      rtc_interface_ptr_map_.at("left")->removeCooperateStatus(uuid_map_.at("left"));
    }
  }

  /**
   * @brief remove RTC status for right approved path.
   */
  void removePreviousRTCStatusRight()
  {
    if (rtc_interface_ptr_map_.at("right")->isRegistered(uuid_map_.at("right"))) {
      rtc_interface_ptr_map_.at("right")->removeCooperateStatus(uuid_map_.at("right"));
    }
  }

  // initializer

  /**
   * @brief init member variables.
   */
  void initVariables();

  /**
   * @brief init RTC status.
   */
  void initRTCStatus();

  /**
   * @brief update RTC status.
   */
  void updateRTCData();

  // ego state check

  /**
   * @brief update ego status based on avoidance path and surround condition.
   * @param ego status. (NOT_AVOID, AVOID, YIELD, AVOID_EXECUTE, AVOID_PATH_NOT_READY)
   */
  AvoidanceState updateEgoState(const AvoidancePlanningData & data) const;

  // ego behavior update

  /**
   * @brief insert stop/decel point in output path.
   * @param avoidance data.
   * @param target path.
   */
  void updateEgoBehavior(const AvoidancePlanningData & data, ShiftedPath & path);

  /**
   * @brief insert stop point in output path.
   * @param flag. if it is true, the ego decelerates within accel/jerk constraints.
   * @param target path.
   */
  void insertWaitPoint(const bool use_constraints_for_decel, ShiftedPath & shifted_path) const;

  /**
   * @brief insert stop point to yield. (stop in the lane if possible, even if the shift has
   * initiated.)
   * @param flag. if it is true, the ego decelerates within accel/jerk constraints.
   * @param target path.
   */
  void insertStopPoint(const bool use_constraints_for_decel, ShiftedPath & shifted_path) const;

  void insertReturnDeadLine(const bool use_constraints_for_decel, ShiftedPath & shifted_path) const;

  /**
   * @brief insert stop point in output path.
   * @param target path.
   */
  void insertPrepareVelocity(ShiftedPath & shifted_path) const;

  /**
   * @brief insert decel point in output path in order to yield. the ego decelerates within
   * accel/jerk constraints.
   * @param target path.
   */
  void insertYieldVelocity(ShiftedPath & shifted_path) const;

  /**
   * @brief calculate stop distance based on object's overhang.
   * @param stop distance.
   */
  double calcDistanceToStopLine(const ObjectData & object) const;

  // avoidance data preparation

  /**
   * @brief update main avoidance data for avoidance path generation based on latest planner data.
   */
  void fillFundamentalData(AvoidancePlanningData & data, DebugData & debug);

  /**
   * @brief fill additional data so that the module judges target objects.
   * @return object that has additional data.
   */
  ObjectData createObjectData(
    const AvoidancePlanningData & data, const PredictedObject & object) const;

  /**
   * @brief fill additional data so that the module judges target objects.
   * @param avoidance data.
   * @param debug data.
   */
  void fillAvoidanceTargetObjects(AvoidancePlanningData & data, DebugData & debug) const;

  /**
   * @brief fill candidate shift lines.
   * @param avoidance data.
   * @param debug data.
   * @details in this function, following two shift line arrays are generated.
   * - unapproved raw shift lines.
   * - unapproved new shift lines.
   * and check whether the new shift lines are safe or not.
   */
  void fillShiftLine(AvoidancePlanningData & data, DebugData & debug) const;

  /**
   * @brief fill ego status based on the candidate path safety check result.
   * @param avoidance data.
   * @param debug data.
   */
  void fillEgoStatus(AvoidancePlanningData & data, DebugData & debug) const;

  /**
   * @brief fill debug data.
   * @param avoidance data.
   * @param debug data.
   */
  void fillDebugData(const AvoidancePlanningData & data, DebugData & debug) const;

  /**
   * @brief update registered shift lines.
   * @param current shift lines.
   */
  void registerRawShiftLines(const AvoidLineArray & future_registered);

  /**
   * @brief update path index of the registered objects. remove old objects whose end point is
   * behind ego pose.
   */
  void updateRegisteredRawShiftLines();

  /**
   * @brief check whether the ego can transit yield maneuver.
   * @param avoidance data.
   */
  bool canYieldManeuver(const AvoidancePlanningData & data) const;

  // shift line generation

  /**
   * @brief Calculate the shift points (start/end point, shift length) from the object lateral
   * and longitudinal positions in the Frenet coordinate. The jerk limit is also considered here.
   * @param avoidance data.
   * @param debug data.
   * @return processed shift lines.
   */
  AvoidOutlines generateAvoidOutline(AvoidancePlanningData & data, DebugData & debug) const;

  /*
   * @brief merge avoid outlines.
   * @param original shift lines.
   * @param debug data.
   * @return processed shift lines.
   */
  AvoidOutlines applyMergeProcess(const AvoidOutlines & outlines, DebugData & debug) const;

  /*
   * @brief fill gap between two shift lines.
   * @param original shift lines.
   * @param debug data.
   * @return processed shift lines.
   */
  AvoidOutlines applyFillGapProcess(const AvoidOutlines & outlines, DebugData & debug) const;

  /*
   * @brief generate candidate shift lines.
   * @param one-shot shift lines.
   * @param debug data.
   */
  AvoidLineArray generateCandidateShiftLine(
    const AvoidLineArray & shift_lines, DebugData & debug) const;

  /**
   * @brief clean up raw shift lines.
   * @param target shift lines.
   * @param debug data.
   * @return processed shift lines.
   * @details process flow:
   * 1. combine raw shirt lines and previous registered shift lines.
   * 2. add return shift line.
   * 3. merge raw shirt lines.
   * 4. trim unnecessary shift lines.
   */
  AvoidLineArray applyPreProcess(const AvoidOutlines & outlines, DebugData & debug) const;

  /*
   * @brief fill gap among shift lines.
   * @param original shift lines.
   * @param debug data.
   * @return processed shift lines.
   */
  AvoidLineArray applyFillGapProcess(const AvoidLineArray & shift_lines, DebugData & debug) const;

  /*
   * @brief merge negative & positive shift lines.
   * @param original shift lines.
   * @param debug data.
   * @return processed shift lines.
   */
  AvoidLineArray applyMergeProcess(const AvoidLineArray & shift_lines, DebugData & debug) const;

  /*
   * @brief add return shift line from ego position.
   * @param current raw shift line.
   * @param current registered shift line.
   * @param debug data.
   */
  AvoidLineArray applyCombineProcess(
    const AvoidLineArray & shift_lines, const AvoidLineArray & registered_lines,
    [[maybe_unused]] DebugData & debug) const;

  /*
   * @brief add return shift line from ego position.
   * @param shift lines which the return shift is added.
   * Pick up the last shift point, which is the most farthest from ego, from the current candidate
   * avoidance points and registered points in the shifter. If the last shift length of the point is
   * non-zero, add a return-shift to center line from the point. If there is no shift point in
   * candidate avoidance points nor registered points, and base_shift > 0, add a return-shift to
   * center line from ego.
   */
  AvoidLineArray addReturnShiftLine(const AvoidLineArray & shift_lines, DebugData & debug) const;

  /*
   * @brief extract shift lines from total shift lines based on their gradient.
   * @param shift length data.
   * @return extracted shift lines.
   */
  AvoidLineArray extractShiftLinesFromLine(ShiftLineData & shift_line_data) const;

  /*
   * @brief remove unnecessary avoid points
   * @param original shift lines.
   * @param debug data.
   * @return processed shift lines.
   * @details
   * - Combine avoid points that have almost same gradient
   * - Quantize the shift length to reduce the shift point noise
   * - Change the shift length to the previous one if the deviation is small.
   * - Remove unnecessary return shift (back to the center line).
   */
  AvoidLineArray applyTrimProcess(const AvoidLineArray & shift_lines, DebugData & debug) const;

  /*
   * @brief extract new shift lines based on current shifted path. the module makes a RTC request
   * for those new shift lines.
   * @param candidate shift lines.
   * @return new shift lines.
   */
  AvoidLineArray findNewShiftLine(const AvoidLineArray & shift_lines, DebugData & debug) const;

  /*
   * @brief generate total shift line. total shift line has shift length and gradient array.
   * @param raw shift lines.
   * @param total shift line.
   */
  void generateTotalShiftLine(
    const AvoidLineArray & avoid_points, ShiftLineData & shift_line_data) const;

  /*
   * @brief quantize shift line length.
   * @param target shift lines.
   * @param threshold. shift length is quantized by this value. (if it is 0.3[m], the output shift
   * length is 0.0, 0.3, 0.6...)
   */
  void applyQuantizeProcess(AvoidLineArray & shift_lines, const double threshold) const;

  /*
   * @brief trim shift line whose relative longitudinal distance is less than threshold.
   * @param target shift lines.
   * @param threshold.
   */
  void applySmallShiftFilter(AvoidLineArray & shift_lines, const double threshold) const;

  /*
   * @brief merge multiple shift lines whose relative gradient is less than threshold.
   * @param target shift lines.
   * @param threshold.
   */
  void applySimilarGradFilter(AvoidLineArray & shift_lines, const double threshold) const;

  /**
   * @brief add new shift line to path shifter if the RTC status is activated.
   * @param new shift lines.
   */
  void updatePathShifter(const AvoidLineArray & point);

  /**
   * @brief add new shift line to path shifter.
   * @param new shift lines.
   */
  void addNewShiftLines(PathShifter & path_shifter, const AvoidLineArray & shift_lines) const;

  /**
   * @brief once generate avoidance path from new shift lines, and calculate lateral offset between
   * ego and the path.
   * @param new shift lines.
   * @param path shifter.
   * @return result. if there is huge gap between the ego position and candidate path, return false.
   */
  bool isValidShiftLine(const AvoidLineArray & shift_lines, const PathShifter & shifter) const;

  // generate output data

  /**
   * @brief calculate turn signal infomation.
   * @param avoidance path.
   * @return turn signal command.
   */
  TurnSignalInfo calcTurnSignalInfo(const ShiftedPath & path) const;

  /**
   * @brief fill debug markers.
   */
  void updateDebugMarker(
    const AvoidancePlanningData & data, const PathShifter & shifter, const DebugData & debug) const;

  /**
   * @brief fill information markers that are shown in Rviz by default.
   */
  void updateInfoMarker(const AvoidancePlanningData & data) const;

  /**
   * @brief fill debug msg that are published as a topic.
   */
  void updateAvoidanceDebugData(std::vector<AvoidanceDebugMsg> & avoidance_debug_msg_array) const;

  // safety check

  /**
   * @brief check avoidance path safety for surround moving objects.
   * @param avoidance path.
   * @param debug data.
   * @return result.
   */
  bool isSafePath(ShiftedPath & shifted_path, DebugData & debug) const;

  bool isComfortable(const AvoidLineArray & shift_lines) const
  {
    return std::all_of(shift_lines.begin(), shift_lines.end(), [&](const auto & line) {
      return PathShifter::calcJerkFromLatLonDistance(
               line.getRelativeLength(), line.getRelativeLongitudinal(),
               helper_.getAvoidanceEgoSpeed()) < helper_.getLateralMaxJerkLimit();
    });
  }

  // post process

  /**
   * @brief extend backward length so that path shift inserts behind shift lines.
   * @param current output path.
   * @return extended path.
   */
  PathWithLaneId extendBackwardLength(const PathWithLaneId & original_path) const;

  /**
   * @brief reset registered shift lines.
   * @details reset only when the base offset is zero. Otherwise, sudden steering will be caused;
   */
  void removeRegisteredShiftLines()
  {
    constexpr double THRESHOLD = 0.1;
    if (std::abs(path_shifter_.getBaseOffset()) > THRESHOLD) {
      RCLCPP_INFO(getLogger(), "base offset is not zero. can't reset registered shift lines.");
      return;
    }

    unlockNewModuleLaunch();

    if (!path_shifter_.getShiftLines().empty()) {
      left_shift_array_.clear();
      right_shift_array_.clear();
      removeRTCStatus();
    }

    current_raw_shift_lines_.clear();
    registered_raw_shift_lines_.clear();
    path_shifter_.setShiftLines(ShiftLineArray{});
  }

  /**
   * @brief remove behind shift lines.
   * @param path shifter.
   */
  void postProcess()
  {
    const size_t idx = planner_data_->findEgoIndex(path_shifter_.getReferencePath().points);
    path_shifter_.removeBehindShiftLineAndSetBaseOffset(idx);
  }

  // misc functions

  double getCurrentBaseShift() const { return path_shifter_.getBaseOffset(); }

  // TODO(Horibe): think later.
  // for unique ID
  mutable uint64_t original_unique_id = 0;  // TODO(Horibe) remove mutable
  uint64_t getOriginalShiftLineUniqueId() const { return original_unique_id++; }

  struct RegisteredShiftLine
  {
    UUID uuid;
    Pose start_pose;
    Pose finish_pose;
  };

  using RegisteredShiftLineArray = std::vector<RegisteredShiftLine>;

  bool is_avoidance_maneuver_starts;

  bool arrived_path_end_{false};

  bool safe_{true};

  std::shared_ptr<AvoidanceParameters> parameters_;

  helper::avoidance::AvoidanceHelper helper_;

  AvoidancePlanningData avoid_data_;

  PathShifter path_shifter_;

  RegisteredShiftLineArray left_shift_array_;

  RegisteredShiftLineArray right_shift_array_;

  AvoidLineArray registered_raw_shift_lines_;

  AvoidLineArray current_raw_shift_lines_;

  UUID candidate_uuid_;

  ObjectDataArray registered_objects_;

  mutable size_t safe_count_{0};

  mutable ObjectDataArray ego_stopped_objects_;

  mutable ObjectDataArray stopped_objects_;

  mutable DebugData debug_data_;

  mutable std::shared_ptr<AvoidanceDebugMsgArray> debug_msg_ptr_;

  mutable std::vector<AvoidanceDebugMsg> debug_avoidance_initializer_for_shift_line_;

  mutable rclcpp::Time debug_avoidance_initializer_for_shift_line_time_;
};

}  // namespace behavior_path_planner

#endif  // BEHAVIOR_PATH_PLANNER__SCENE_MODULE__AVOIDANCE__AVOIDANCE_MODULE_HPP_
