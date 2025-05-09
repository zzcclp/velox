# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
velox_add_library(velox_link_libs INTERFACE)
velox_link_libraries(
  velox_link_libs
  INTERFACE
    velox_caching
    velox_dwio_catalog_fbhive
    velox_dwio_common
    velox_dwio_common_exception
    velox_encode
    velox_exception
    velox_memory
    velox_process
    velox_serialization
    velox_type
    velox_type_fbhive
    velox_vector
    Folly::folly
    fmt::fmt)

add_subdirectory(common)
add_subdirectory(catalog)
add_subdirectory(dwrf)
add_subdirectory(orc)
add_subdirectory(parquet)
add_subdirectory(text)
