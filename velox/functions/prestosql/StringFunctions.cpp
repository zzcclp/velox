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

#include "velox/expression/EvalCtx.h"
#include "velox/expression/Expr.h"
#include "velox/expression/StringWriter.h"
#include "velox/expression/VectorFunction.h"
#include "velox/functions/lib/StringEncodingUtils.h"
#include "velox/functions/lib/string/StringImpl.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::functions {

using namespace stringCore;

namespace {

/**
 * concat(string1, ..., stringN) → varchar
 * Returns the concatenation of string1, string2, ..., stringN. This function
 * provides the same functionality as the SQL-standard concatenation operator
 * (||).
 * */
class ConcatFunction : public exec::VectorFunction {
 public:
  ConcatFunction(
      const std::string& /* name */,
      const std::vector<exec::VectorFunctionArg>& inputArgs) {
    auto numArgs = inputArgs.size();

    // Save constant values to constantStrings_.
    // Identify and combine consecutive constant inputs.
    argMapping_.reserve(numArgs);
    constantStrings_.reserve(numArgs);

    for (auto i = 0; i < numArgs; ++i) {
      argMapping_.push_back(i);

      const auto& arg = inputArgs[i];
      if (arg.constantValue) {
        std::string value = arg.constantValue->as<ConstantVector<StringView>>()
                                ->valueAt(0)
                                .str();

        column_index_t j = i + 1;
        for (; j < inputArgs.size(); ++j) {
          if (!inputArgs[j].constantValue) {
            break;
          }

          value += inputArgs[j]
                       .constantValue->as<ConstantVector<StringView>>()
                       ->valueAt(0)
                       .str();
        }

        constantStrings_.push_back(std::string(value.data(), value.size()));

        i = j - 1;
      } else {
        constantStrings_.push_back(std::string());
      }
    }

    // Create StringViews for constant strings.
    constantStringViews_.reserve(numArgs);
    for (const auto& constantString : constantStrings_) {
      constantStringViews_.push_back(
          StringView(constantString.data(), constantString.size()));
    }
  }

  bool propagateStringEncodingFromAllInputs() const override {
    return true;
  }

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const override {
    context.ensureWritable(rows, outputType, result);
    auto flatResult = result->asFlatVector<StringView>();

    auto numArgs = argMapping_.size();

    std::vector<exec::LocalDecodedVector> decodedArgs;
    decodedArgs.reserve(numArgs);

    for (auto i = 0; i < numArgs; ++i) {
      auto index = argMapping_[i];
      if (constantStringViews_[i].empty()) {
        decodedArgs.emplace_back(context, *args[index], rows);
      } else {
        // Do not decode constant inputs.
        decodedArgs.emplace_back(context);
      }
    }

    // Calculate the combined size of the result strings.
    size_t totalResultBytes = 0;
    rows.applyToSelected([&](int row) {
      for (int i = 0; i < numArgs; i++) {
        if (constantStringViews_[i].empty()) {
          auto value = decodedArgs[i]->valueAt<StringView>(row);
          totalResultBytes += value.size();
        } else {
          totalResultBytes += constantStringViews_[i].size();
        }
      }
    });

    // Allocate a string buffer.
    auto rawBuffer = flatResult->getRawStringBufferWithSpace(totalResultBytes);
    size_t offset = 0;
    rows.applyToSelected([&](int row) {
      const char* start = rawBuffer + offset;
      size_t combinedSize = 0;
      for (int i = 0; i < numArgs; i++) {
        StringView value;
        if (constantStringViews_[i].empty()) {
          value = decodedArgs[i]->valueAt<StringView>(row);
        } else {
          value = constantStringViews_[i];
        }
        auto size = value.size();
        if (size > 0) {
          memcpy(rawBuffer + offset, value.data(), size);
          combinedSize += size;
          offset += size;
        }
      }
      flatResult->setNoCopy(row, StringView(start, combinedSize));
    });
  }

  static std::vector<std::shared_ptr<exec::FunctionSignature>> signatures() {
    return {
        // varchar, varchar,.. -> varchar
        exec::FunctionSignatureBuilder()
            .returnType("varchar")
            .argumentType("varchar")
            .argumentType("varchar")
            .variableArity("varchar")
            .build(),
        // varbinary, varbinary,.. -> varbinary
        exec::FunctionSignatureBuilder()
            .returnType("varbinary")
            .argumentType("varbinary")
            .argumentType("varbinary")
            .variableArity("varbinary")
            .build(),
    };
  }

  static exec::VectorFunctionMetadata metadata() {
    return {true /* supportsFlattening */};
  }

 private:
  std::vector<column_index_t> argMapping_;
  std::vector<std::string> constantStrings_;
  std::vector<StringView> constantStringViews_;
};

/**
 * replace(string, search) → varchar
 * Removes all instances of search from string.
 *
 * replace(string, search, replace) → varchar
 * Replaces all instances of search with replace in string.
 * If search is an empty string, inserts replace in front of every character
 * and at the end of the string.
 *
 * If replaceFirst=true.
 * replace_first(string, search, replace) -> varchar
 * Replaces the first instances of ``search`` with ``replace`` in ``string``.
 * If search is an empty string, it inserts replace at the beginning of the
 * string.
 **/
class Replace : public exec::VectorFunction {
 private:
  template <
      typename StringReader,
      typename SearchReader,
      typename ReplaceReader>
  void applyInternal(
      StringReader stringReader,
      SearchReader searchReader,
      ReplaceReader replaceReader,
      const SelectivityVector& rows,
      FlatVector<StringView>* results) const {
    rows.applyToSelected([&](int row) {
      auto proxy = exec::StringWriter(results, row);
      stringImpl::replace(
          proxy,
          stringReader(row),
          searchReader(row),
          replaceReader(row),
          replaceFirst_);
      proxy.finalize();
    });
  }

  const bool replaceFirst_;

 public:
  explicit Replace(bool replaceFirst) : replaceFirst_(replaceFirst) {}

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      const TypePtr& /* outputType */,
      exec::EvalCtx& context,
      VectorPtr& result) const override {
    // Read string input
    exec::LocalDecodedVector decodedStringHolder(context, *args[0], rows);
    auto decodedStringInput = decodedStringHolder.get();

    // Read search argument
    exec::LocalDecodedVector decodedSearchHolder(context, *args[1], rows);
    auto decodedSearchInput = decodedSearchHolder.get();

    std::optional<StringView> searchArgValue;
    if (decodedSearchInput->isConstantMapping()) {
      searchArgValue = decodedSearchInput->valueAt<StringView>(0);
    }

    // Read replace argument
    exec::LocalDecodedVector decodedReplaceHolder(context);
    auto decodedReplaceInput = decodedReplaceHolder.get();
    std::optional<StringView> replaceArgValue;

    if (args.size() <= 2) {
      replaceArgValue = StringView("");
    } else {
      decodedReplaceInput->decode(*args.at(2), rows);
      if (decodedReplaceInput->isConstantMapping()) {
        replaceArgValue = decodedReplaceInput->valueAt<StringView>(0);
      }
    }

    auto stringReader = [&](const vector_size_t row) {
      return decodedStringInput->valueAt<StringView>(row);
    };

    auto searchReader = [&](const vector_size_t row) {
      return decodedSearchInput->valueAt<StringView>(row);
    };

    auto replaceReader = [&](const vector_size_t row) {
      if (replaceArgValue.has_value()) {
        return replaceArgValue.value();
      } else {
        return decodedReplaceInput->valueAt<StringView>(row);
      }
    };

    // Not in place path
    VectorPtr emptyVectorPtr;
    prepareFlatResultsVector(result, rows, context, emptyVectorPtr);
    auto* resultFlatVector = result->as<FlatVector<StringView>>();

    applyInternal(
        stringReader, searchReader, replaceReader, rows, resultFlatVector);
  }

  static std::vector<std::shared_ptr<exec::FunctionSignature>> signatures(
      bool replaceFirst) {
    return replaceFirst ? std::vector<std::shared_ptr<exec::FunctionSignature>>{
        // varchar, varchar, varchar -> varchar
        exec::FunctionSignatureBuilder()
            .returnType("varchar")
            .argumentType("varchar")
            .argumentType("varchar")
            .argumentType("varchar")
            .build(),
    } : std::vector<std::shared_ptr<exec::FunctionSignature>>{
        // varchar, varchar -> varchar
        exec::FunctionSignatureBuilder()
            .returnType("varchar")
            .argumentType("varchar")
            .argumentType("varchar")
            .build(),
        // varchar, varchar, varchar -> varchar
        exec::FunctionSignatureBuilder()
            .returnType("varchar")
            .argumentType("varchar")
            .argumentType("varchar")
            .argumentType("varchar")
            .build(),
    };
  }

  // Only the original string and the replacement are relevant to the result
  // encoding.
  // TODO: The propagation is a safe approximation here, it might be better
  // for some cases to keep it unset and then rescan.
  std::optional<std::vector<size_t>> propagateStringEncodingFrom()
      const override {
    return {{0, 2}};
  }
};
} // namespace

VELOX_DECLARE_STATEFUL_VECTOR_FUNCTION_WITH_METADATA(
    udf_concat,
    ConcatFunction::signatures(),
    ConcatFunction::metadata(),
    [](const auto& name,
       const auto& inputs,
       const core::QueryConfig& /*config*/) {
      return std::make_unique<ConcatFunction>(name, inputs);
    });

VELOX_DECLARE_VECTOR_FUNCTION(
    udf_replaceFirst,
    Replace::signatures(/*replaceFirst*/ true),
    std::make_unique<Replace>(/*replaceFirst*/ true));

VELOX_DECLARE_VECTOR_FUNCTION(
    udf_replace,
    Replace::signatures(/*replaceFirst*/ false),
    std::make_unique<Replace>(/*replaceFirst*/ false));

} // namespace facebook::velox::functions
