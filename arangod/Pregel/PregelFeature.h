////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_PREGEL_FEATURE_H
#define ARANGODB_PREGEL_FEATURE_H 1

#include <cstdint>
#include "ApplicationFeatures/ApplicationFeature.h"
#include "Basics/Common.h"
#include "Basics/Mutex.h"

namespace arangodb {
namespace pregel {

class Conductor;
class IWorker;

class PregelFeature final : public application_features::ApplicationFeature {
 public:
  explicit PregelFeature(application_features::ApplicationServer* server);
  ~PregelFeature();

  static PregelFeature* instance();

  void beginShutdown() override final;

  uint64_t createExecutionNumber();
  void addExecution(Conductor* const exec, uint64_t executionNumber);
  Conductor* conductor(uint64_t executionNumber);
  void notifyConductorOutage();

  void addWorker(IWorker* const worker, uint64_t executionNumber);
  IWorker* worker(uint64_t executionNumber);

  void cleanup(uint64_t executionNumber);
  void cleanupAll();

 private:
  std::unordered_map<uint64_t, Conductor*> _conductors;
  std::unordered_map<uint64_t, IWorker*> _workers;
  Mutex _mutex;
};
}
}

#endif
