// Copyright 2023 TIER IV, Inc.
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

#ifndef TRACKING_OBJECT_MERGER__DATA_ASSOCIATION__SOLVER__MU_SUCCESSIVE_SHORTEST_PATH_HPP_
#define TRACKING_OBJECT_MERGER__DATA_ASSOCIATION__SOLVER__MU_SUCCESSIVE_SHORTEST_PATH_HPP_

#include "tracking_object_merger/data_association/solver/gnn_solver_interface.hpp"

#include <unordered_map>
#include <vector>

namespace gnn_solver
{
class MuSSP : public GnnSolverInterface
{
public:
  MuSSP() = default;
  ~MuSSP() = default;

  void maximizeLinearAssignment(
    const std::vector<std::vector<double>> & cost, std::unordered_map<int, int> * direct_assignment,
    std::unordered_map<int, int> * reverse_assignment) override;
};
}  // namespace gnn_solver

#endif  // TRACKING_OBJECT_MERGER__DATA_ASSOCIATION__SOLVER__MU_SUCCESSIVE_SHORTEST_PATH_HPP_
