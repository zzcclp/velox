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

#include "velox/exec/Driver.h"
#include "velox/experimental/wave/exec/Wave.h"
#include "velox/experimental/wave/vector/WaveVector.h"
#include "velox/type/Filter.h"

namespace facebook::velox::wave {

class CompileState;
class WaveDriver;

class WaveOperator {
 public:
  WaveOperator(
      CompileState& state,
      const RowTypePtr& outputType,
      const std::string& planNodeId);

  virtual ~WaveOperator() = default;

  virtual exec::BlockingReason isBlocked(
      WaveStream& stream,
      ContinueFuture* future) {
    return exec::BlockingReason::kNotBlocked;
  }

  const RowTypePtr& outputType() const {
    return outputType_;
  }

  /// True if may reduce cardinality without duplicating input rows.
  bool isFilter() {
    return isFilter_;
  }

  /// True if a single input can produce zero to multiple outputs.
  bool isExpanding() const {
    return isExpanding_;
  }

  virtual bool isSource() const {
    return false;
  }

  virtual bool isStreaming() const = 0;

  virtual void enqueue(WaveVectorPtr) {
    VELOX_FAIL("Override for blocking operator");
  }

  virtual void pipelineFinished(WaveStream& /*stream*/) {}

  /// Returns how many rows of output are available from 'this'. Source
  /// operators and cardinality increasing operators must return a correct
  /// answer if they are ready to produce data. Others should return 0.
  virtual std::vector<AdvanceResult> canAdvance(WaveStream& stream) {
    return {};
  }

  /// Adds processing for 'this' to 'stream'. If 'maxRows' is given,
  /// then this is the maximum number of intermediates/result rows
  /// this can produce. If not given, this defaults to the 'stream's
  /// current result row count. If the stream is pending and the
  /// count is not known, then this defaults to the max cardinality
  /// of the pending work. If the work has arrived, this can be the
  /// actual cardinality. The first schedule() of each 'stream '
  /// must specify this count. This is the number returned by
  /// canAdvance() for a source WaveOperator.
  virtual void schedule(WaveStream& stream, int32_t maxRows = 0) = 0;

  virtual bool isFinished() const {
    VELOX_FAIL("Override for source or blocking operator");
  }

  virtual bool isSink() const {
    return false;
  }

  virtual void callUpdateStatus(
      WaveStream& stream,
      const std::vector<WaveStream*>& otherStreams,
      AdvanceResult& advance) {
    VELOX_FAIL("Only Project supports callUpdateStatus()");
  }

  /// InstructionStatus that describes the extra statuses returned
  /// from device for the pipeline that begins with 'this'. Must be
  /// set for the head of each pipeline.
  const InstructionStatus& instructionStatus() const {
    VELOX_CHECK_NE(instructionStatus_.gridStateSize, 0);
    return instructionStatus_;
  }

  void setInstructionStatus(InstructionStatus status) {
    instructionStatus_ = status;
  }

  virtual std::string toString() const;

  AbstractOperand* definesSubfield(
      CompileState& state,
      const TypePtr& type,
      const std::string& parentPath = "",
      bool sourceNullable = false);

  /// Returns the operand if this is defined by 'this'.
  AbstractOperand* defines(Value value) {
    auto it = defines_.find(value);
    if (it == defines_.end()) {
      return nullptr;
    }
    return it->second;
  }

  /// Marks 'operand' as defined here.
  void defined(Value value, AbstractOperand* op) {
    defines_[value] = op;
  }

  void addSubfieldAndType(
      const common::Subfield* subfield,
      const TypePtr& type) {
    VELOX_UNSUPPORTED();
    // subfields_.push_back(subfield);
    // types_.push_back(type);
  }

  void setDriver(WaveDriver* driver) {
    driver_ = driver;
  }

  const OperandSet& outputIds() const {
    return outputIds_;
  }

  void addOutputId(OperandId id) {
    outputIds_.add(id);
  }

  // The set of output operands that must have arrived for there to be a result.
  virtual const OperandSet& syncSet() const {
    return outputIds_;
  }

  /// Called once on each Operator, first to last, after no more
  /// Operators will be added to the WaveDriver plan. Can be used for
  /// e.g. making executable images of Programs since their content
  /// and dependences will no longer change.
  virtual void finalize(CompileState& state) {}

  int32_t operatorId() const {
    return id_;
  }

  virtual bool canAddDynamicFilter() const {
    return false;
  }

  virtual void addDynamicFilter(
      const core::PlanNodeId& /*producer*/,
      const exec::PushdownFilters& /*filters*/) {
    VELOX_UNSUPPORTED();
  }

 protected:
  folly::Synchronized<exec::OperatorStats>& stats();

  // Sequence number in WaveOperator sequence inside WaveDriver. Used to label
  // states of different oprators in WaveStream.
  int32_t id_;

  WaveDriver* driver_{nullptr};

  // The Subfields that are produced. Different ones can arrive at
  // different times on different waves. In this list, ordered in
  // depth first preorder of outputType_. Top struct not listed,
  // struct columns have the parent before the children.
  // std::vector<const common::Subfield*> subfields_;

  // Pairwise type for each subfield.
  // std::vector<TypePtr> types_;

  // Id in original plan. Use for getting splits.
  std::string planNodeId_;

  // the execution time set of OperandIds.
  OperandSet outputIds_;

  bool isFilter_{false};

  bool isExpanding_{false};

  RowTypePtr outputType_;

  // The operands that are first defined here.
  DefinesMap defines_;

  // The operand for values that are projected through 'this'.
  DefinesMap projects_;

  std::vector<std::shared_ptr<Program>> programs_;

  // Executable instances of 'this'. A Driver may instantiate multiple
  // executable instances to processs consecutive input batches in parallel.
  // these are handed off to WaveStream for running, so reside here only when
  // not enqueued to run.
  std::vector<std::unique_ptr<Executable>> executables_;

  // Buffers containing unified memory for 'executables_' and all instructions,
  // operands etc. referenced from these.  This does not include buffers for
  // intermediate results.
  std::vector<WaveBufferPtr> executableMemory_;

  // The total size of grid and block level statuses for the pipeline. This must
  // be set for the first operator of any pipeline.
  InstructionStatus instructionStatus_;
};

class WaveSourceOperator : public WaveOperator {
 public:
  WaveSourceOperator(
      CompileState& state,
      const RowTypePtr& outputType,
      const std::string& planNodeId)
      : WaveOperator(state, outputType, planNodeId) {}
  bool isSource() const override {
    return true;
  }
};

} // namespace facebook::velox::wave
