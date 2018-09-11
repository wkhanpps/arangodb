////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "Aql/AllRowsFetcher.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/SortExecutor.h"

using namespace arangodb;
using namespace arangodb::aql;


template <class Executor>
std::pair<ExecutionState, AqlItemMatrix const*>
AllRowsFetcher<Executor>::fetchAllRows() {
  // TODO IMPLEMENT ME
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

template <class Executor>
AllRowsFetcher<Executor>::AllRowsFetcher(
  ExecutionBlockImpl<Executor>& executionBlock)
  : _executionBlock(&executionBlock) {}

template <class Executor>
std::pair<ExecutionState, std::unique_ptr<AqlItemBlock>>
AllRowsFetcher<Executor>::fetchBlock() {
  auto res = _executionBlock->fetchBlock();

  _upstreamState = res.first;

  return res;
}

template <class Executor>
RegisterId AllRowsFetcher<Executor>::getNrInputRegisters() const {
  return _executionBlock->getNrInputRegisters();
}

template class ::arangodb::aql::AllRowsFetcher<SortExecutor>;
