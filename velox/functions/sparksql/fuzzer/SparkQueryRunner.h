/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "grpc++/channel.h" // @manual
#include "grpc++/client_context.h" // @manual
#include "grpc++/create_channel.h" // @manual
#include "grpc++/security/credentials.h" // @manual
#include "velox/common/fuzzer/Utils.h"
#include "velox/common/memory/Memory.h"
#include "velox/exec/fuzzer/ReferenceQueryRunner.h"
#include "velox/functions/sparksql/fuzzer/spark/connect/base.grpc.pb.h" // @manual
#include "velox/vector/ComplexVector.h"

namespace facebook::velox::functions::sparksql::fuzzer {

using facebook::velox::fuzzer::DataSpec;

/// Query runner that uses Spark as a reference database. It converts Velox
/// query plan to Spark SQL and executes it in Spark. The results are returned
/// as Velox compatible format.
class SparkQueryRunner : public velox::exec::test::ReferenceQueryRunner {
 public:
  /// @param coordinatorUri Spark connect server endpoint, e.g. localhost:15002.
  SparkQueryRunner(
      memory::MemoryPool* pool,
      const std::string& coordinatorUri,
      const std::string& userId,
      const std::string& userName)
      : ReferenceQueryRunner(pool),
        userId_(userId),
        userName_(userName),
        sessionId_(generateUUID()),
        stub_(spark::connect::SparkConnectService::NewStub(grpc::CreateChannel(
            coordinatorUri,
            grpc::InsecureChannelCredentials()))) {
    pool_ = aggregatePool()->addLeafChild("leaf");
    copyPool_ = aggregatePool()->addLeafChild("copy");
  };

  RunnerType runnerType() const override {
    return RunnerType::kSparkQueryRunner;
  }

  const std::vector<TypePtr>& supportedScalarTypes() const override;

  const std::unordered_map<std::string, DataSpec>&
  aggregationFunctionDataSpecs() const override;

  /// Converts Velox query plan to Spark SQL. Supports Values -> Aggregation.
  /// Values node is converted into reading from 'tmp' table.
  /// @return std::nullopt for unsupported cases.
  std::optional<std::string> toSql(
      const velox::core::PlanNodePtr& plan) override;

  // Converts 'plan' into an SQL query and executes it. Result is returned as a
  // MaterializedRowMultiset with the ReferenceQueryErrorCode::kSuccess if
  // successful, or an std::nullopt with a ReferenceQueryErrorCode if the query
  // fails.
  std::pair<
      std::optional<std::multiset<std::vector<variant>>>,
      exec::test::ReferenceQueryErrorCode>
  execute(const core::PlanNodePtr& plan) override;

  /// Similar to 'execute' but returns results in RowVector format.
  /// Caller should ensure 'supportsVeloxVectorResults' returns true.
  std::pair<
      std::optional<std::vector<RowVectorPtr>>,
      exec::test::ReferenceQueryErrorCode>
  executeAndReturnVector(const core::PlanNodePtr& plan) override;

  /// Executes Spark SQL query and returns the results. Tables referenced by
  /// the query must already exist.
  std::vector<velox::RowVectorPtr> execute(const std::string& sql) override;

  bool supportsVeloxVectorResults() const override {
    return true;
  }

 private:
  using ReferenceQueryRunner::toSql;

  // Generates a random UUID string for Spark. It must be of the format
  // '00112233-4455-6677-8899-aabbccddeeff'.
  std::string generateUUID();

  velox::memory::MemoryPool* pool() {
    return pool_.get();
  }

  // Reads the arrow IPC-format string data with arrow IPC reader and convert
  // them into Velox RowVectors.
  std::vector<velox::RowVectorPtr> readArrowData(const std::string& data);

  google::protobuf::Arena arena_;
  const std::string userId_;
  const std::string userName_;
  const std::string sessionId_;
  // Used to make gRPC calls to the SparkConnectService.
  std::unique_ptr<spark::connect::SparkConnectService::Stub> stub_;
  std::shared_ptr<velox::memory::MemoryPool> pool_;
  std::shared_ptr<velox::memory::MemoryPool> copyPool_;
};
} // namespace facebook::velox::functions::sparksql::fuzzer
