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

#include "velox/common/base/Portability.h"
#include "velox/dwio/common/ColumnVisitors.h"
#include "velox/dwio/common/DirectDecoder.h"
#include "velox/dwio/common/SelectiveColumnReader.h"
#include "velox/dwio/common/TypeUtils.h"
#include "velox/exec/AggregationHook.h"
#include "velox/type/Timestamp.h"
#include "velox/vector/ConstantVector.h"
#include "velox/vector/DictionaryVector.h"
#include "velox/vector/FlatVector.h"

#include <numeric>

namespace facebook::velox::dwio::common {

template <typename T>
void SelectiveColumnReader::ensureValuesCapacity(
    vector_size_t numRows,
    bool preserveData) {
  if (values_ && (isFlatMapValue_ || values_->unique()) &&
      values_->capacity() >=
          BaseVector::byteSize<T>(numRows) + simd::kPadding) {
    return;
  }
  auto newValues = AlignedBuffer::allocate<T>(
      numRows + simd::kPadding / sizeof(T), memoryPool_);
  if (preserveData) {
    std::memcpy(
        newValues->template asMutable<char>(), rawValues_, values_->capacity());
  }
  values_ = std::move(newValues);
  rawValues_ = values_->asMutable<char>();
}

template <typename T>
void SelectiveColumnReader::prepareRead(
    int64_t offset,
    const RowSet& rows,
    const uint64_t* incomingNulls) {
  const vector_size_t numRows = rows.back() + 1;
  readNulls(offset, numRows, incomingNulls);

  // We check for all nulls and no nulls. We expect both calls to
  // bits::isAllSet to fail early in the common case. We could do a
  // single traversal of null bits counting the bits and then compare
  // this to 0 and the total number of rows but this would end up
  // reading more in the mixed case and would not be better in the all
  // (non)-null case.
  allNull_ = nullsInReadRange_ &&
      bits::isAllSet(
                 nullsInReadRange_->as<uint64_t>(), 0, numRows, bits::kNull);
  if (nullsInReadRange_ &&
      bits::isAllSet(
          nullsInReadRange_->as<uint64_t>(), 0, numRows, bits::kNotNull)) {
    nullsInReadRange_ = nullptr;
  }

  innerNonNullRows_.clear();
  outerNonNullRows_.clear();
  outputRows_.clear();
  // Is part of read() and after read returns getValues may be called.
  mayGetValues_ = true;
  numValues_ = 0;
  valueSize_ = sizeof(T);
  inputRows_ = rows;
  if (scanSpec_->filter() || hasDeletion()) {
    outputRows_.reserve(rows.size());
  }

  ensureValuesCapacity<T>(rows.size());
  if (scanSpec_->keepValues() && !scanSpec_->valueHook()) {
    valueRows_.clear();
    prepareNulls(rows, nullsInReadRange_ != nullptr);
  }
}

template <typename T, typename TVector>
void SelectiveColumnReader::getFlatValues(
    const RowSet& rows,
    VectorPtr* result,
    const TypePtr& type,
    bool isFinal) {
  VELOX_CHECK_NE(valueSize_, kNoValueSize);
  VELOX_CHECK(mayGetValues_);
  if (isFinal) {
    mayGetValues_ = false;
  }

  if (allNull_) {
    if (isFlatMapValue_) {
      if (flatMapValueConstantNullValues_) {
        flatMapValueConstantNullValues_->resize(rows.size());
      } else {
        flatMapValueConstantNullValues_ =
            std::make_shared<ConstantVector<TVector>>(
                memoryPool_, rows.size(), true, type, T());
      }
      *result = flatMapValueConstantNullValues_;
    } else {
      *result = std::make_shared<ConstantVector<TVector>>(
          memoryPool_, rows.size(), true, type, T());
    }
    return;
  }

  if (valueSize_ == sizeof(TVector)) {
    compactScalarValues<TVector, TVector>(rows, isFinal);
  } else if (sizeof(T) >= sizeof(TVector)) {
    compactScalarValues<T, TVector>(rows, isFinal);
  } else {
    upcastScalarValues<T, TVector>(rows);
  }
  valueSize_ = sizeof(TVector);
  if (isFlatMapValue_) {
    if (flatMapValueFlatValues_) {
      auto* flat = flatMapValueFlatValues_->asUnchecked<FlatVector<TVector>>();
      flat->unsafeSetSize(numValues_);
      flat->setNulls(resultNulls());
      flat->unsafeSetValues(values_);
      flat->setStringBuffers(std::move(stringBuffers_));
    } else {
      flatMapValueFlatValues_ = std::make_shared<FlatVector<TVector>>(
          memoryPool_,
          type,
          resultNulls(),
          numValues_,
          values_,
          std::move(stringBuffers_));
    }
    *result = flatMapValueFlatValues_;
  } else {
    *result = std::make_shared<FlatVector<TVector>>(
        memoryPool_,
        type,
        resultNulls(),
        numValues_,
        values_,
        std::move(stringBuffers_));
  }
}

template <>
void SelectiveColumnReader::getFlatValues<int8_t, bool>(
    const RowSet& rows,
    VectorPtr* result,
    const TypePtr& type,
    bool isFinal);

template <typename T, typename TVector>
void SelectiveColumnReader::upcastScalarValues(const RowSet& rows) {
  VELOX_CHECK_LE(rows.size(), numValues_);
  VELOX_CHECK(!rows.empty());
  if (!values_) {
    return;
  }
  VELOX_CHECK_GT(sizeof(TVector), sizeof(T));
  // Since upcast is not going to be a common path, allocate buffer to copy
  // upcasted values to and then copy back to the values buffer.
  std::vector<TVector> buf;
  buf.resize(rows.size());
  T* typedSourceValues = reinterpret_cast<T*>(rawValues_);
  RowSet sourceRows;
  // The row numbers corresponding to elements in 'values_' are in
  // 'valueRows_' if values have been accessed before. Otherwise
  // they are in 'outputRows_' if these are non-empty (there is a
  // filter) and in 'inputRows_' otherwise.
  if (!valueRows_.empty()) {
    sourceRows = valueRows_;
  } else if (!outputRows_.empty()) {
    sourceRows = outputRows_;
  } else {
    sourceRows = inputRows_;
  }
  if (valueRows_.empty()) {
    valueRows_.resize(rows.size());
  }
  vector_size_t rowIndex = 0;
  auto nextRow = rows[rowIndex];
  auto* moveNullsFrom = shouldMoveNulls(rows);
  for (size_t i = 0; i < numValues_; i++) {
    if (sourceRows[i] < nextRow) {
      continue;
    }

    VELOX_DCHECK(sourceRows[i] == nextRow);
    buf[rowIndex] = typedSourceValues[i];
    if (moveNullsFrom && rowIndex != i) {
      bits::setBit(rawResultNulls_, rowIndex, bits::isBitSet(moveNullsFrom, i));
    }
    valueRows_[rowIndex] = nextRow;
    rowIndex++;
    if (rowIndex >= rows.size()) {
      break;
    }
    nextRow = rows[rowIndex];
  }
  ensureValuesCapacity<TVector>(rows.size());
  std::memcpy(rawValues_, buf.data(), rows.size() * sizeof(TVector));
  numValues_ = rows.size();
  valueRows_.resize(numValues_);
  values_->setSize(numValues_ * sizeof(TVector));
}

template <typename T, typename TVector>
void SelectiveColumnReader::compactScalarValues(
    const RowSet& rows,
    bool isFinal) {
  VELOX_CHECK_LE(rows.size(), numValues_);
  VELOX_CHECK(!rows.empty());
  if (!values_ || (rows.size() == numValues_ && sizeof(T) == sizeof(TVector))) {
    if (values_) {
      values_->setSize(numValues_ * sizeof(T));
    }
    return;
  }

  VELOX_CHECK_LE(sizeof(TVector), sizeof(T));
  T* typedSourceValues = reinterpret_cast<T*>(rawValues_);
  TVector* typedDestValues = reinterpret_cast<TVector*>(rawValues_);
  RowSet sourceRows;
  // The row numbers corresponding to elements in 'values_' are in
  // 'valueRows_' if values have been accessed before. Otherwise
  // they are in 'outputRows_' if these are non-empty (there is a
  // filter) and in 'inputRows_' otherwise.
  if (!valueRows_.empty()) {
    sourceRows = valueRows_;
  } else if (!outputRows_.empty()) {
    sourceRows = outputRows_;
  } else {
    sourceRows = inputRows_;
  }
  if (valueRows_.empty()) {
    valueRows_.resize(rows.size());
  }

  vector_size_t rowIndex = 0;
  auto nextRow = rows[rowIndex];
  const auto* moveNullsFrom = shouldMoveNulls(rows);
  for (size_t i = 0; i < numValues_; ++i) {
    if (sourceRows[i] < nextRow) {
      continue;
    }

    VELOX_DCHECK_EQ(sourceRows[i], nextRow);
    typedDestValues[rowIndex] = typedSourceValues[i];
    if (moveNullsFrom && rowIndex != i) {
      bits::setBit(rawResultNulls_, rowIndex, bits::isBitSet(moveNullsFrom, i));
    }
    if (!isFinal) {
      valueRows_[rowIndex] = nextRow;
    }
    ++rowIndex;
    if (rowIndex >= rows.size()) {
      break;
    }
    nextRow = rows[rowIndex];
  }

  numValues_ = rows.size();
  valueRows_.resize(numValues_);
  values_->setSize(numValues_ * sizeof(TVector));
}

template <>
void SelectiveColumnReader::compactScalarValues<bool, bool>(
    const RowSet& rows,
    bool isFinal);

inline int32_t sizeOfIntKind(TypeKind kind) {
  switch (kind) {
    case TypeKind::SMALLINT:
      return 2;
    case TypeKind::INTEGER:
      return 4;
    case TypeKind::BIGINT:
      return 8;
    default:
      VELOX_FAIL("Not an integer TypeKind: {}", static_cast<int>(kind));
  }
}

template <typename T>
void SelectiveColumnReader::filterNulls(
    const RowSet& rows,
    bool isNull,
    bool extractValues) {
  const bool isDense = rows.back() == rows.size() - 1;
  // We decide is (not) null based on 'nullsInReadRange_'. This may be
  // set due to nulls in enclosing structs even if the column itself
  // does not add nulls.
  auto* rawNulls =
      nullsInReadRange_ ? nullsInReadRange_->as<uint64_t>() : nullptr;
  if (isNull) {
    if (!rawNulls) {
      // The stripe has nulls but the current range does not. Nothing matches.
    } else if (isDense) {
      bits::forEachUnsetBit(
          rawNulls, 0, rows.back() + 1, [&](vector_size_t row) {
            addOutputRow(row);
            if (extractValues) {
              addNull<T>();
            }
          });
    } else {
      for (auto row : rows) {
        if (bits::isBitNull(rawNulls, row)) {
          addOutputRow(row);
          if (extractValues) {
            addNull<T>();
          }
        }
      }
    }
    return;
  }

  VELOX_CHECK(
      !extractValues,
      "filterNulls for not null only applies to filter-only case");
  if (!rawNulls) {
    // All pass.
    for (auto row : rows) {
      addOutputRow(row);
    }
  } else if (isDense) {
    bits::forEachSetBit(rawNulls, 0, rows.back() + 1, [&](vector_size_t row) {
      addOutputRow(row);
    });
  } else {
    for (auto row : rows) {
      if (!bits::isBitNull(rawNulls, row)) {
        addOutputRow(row);
      }
    }
  }
}

} // namespace facebook::velox::dwio::common
