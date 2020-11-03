/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_CORE_FRAMEWORK_USER_OP_ATTR_H_
#define ONEFLOW_CORE_FRAMEWORK_USER_OP_ATTR_H_

#include "oneflow/core/framework/user_op_attr.pb.h"
#include "oneflow/core/common/util.h"
#include "oneflow/core/common/shape.h"
#include "oneflow/core/common/data_type.h"

namespace oneflow {

namespace user_op {

// SEQ
#define BASIC_ATTR_SEQ                                               \
  OF_PP_MAKE_TUPLE_SEQ(at_int32, int32_t, UserOpAttrType::kAtInt32)  \
  OF_PP_MAKE_TUPLE_SEQ(at_int64, int64_t, UserOpAttrType::kAtInt64)  \
  OF_PP_MAKE_TUPLE_SEQ(at_bool, bool, UserOpAttrType::kAtBool)       \
  OF_PP_MAKE_TUPLE_SEQ(at_float, float, UserOpAttrType::kAtFloat)    \
  OF_PP_MAKE_TUPLE_SEQ(at_double, double, UserOpAttrType::kAtDouble) \
  OF_PP_MAKE_TUPLE_SEQ(at_string, std::string, UserOpAttrType::kAtString)

#define ENUM_ATTR_SEQ OF_PP_MAKE_TUPLE_SEQ(at_data_type, DataType, UserOpAttrType::kAtDataType)

#define MESSAGE_ATTR_SEQ OF_PP_MAKE_TUPLE_SEQ(at_shape, Shape, UserOpAttrType::kAtShape)

#define LIST_BASIC_ATTR_SEQ                                                               \
  OF_PP_MAKE_TUPLE_SEQ(at_list_int32, std::vector<int32_t>, UserOpAttrType::kAtListInt32) \
  OF_PP_MAKE_TUPLE_SEQ(at_list_int64, std::vector<int64_t>, UserOpAttrType::kAtListInt64) \
  OF_PP_MAKE_TUPLE_SEQ(at_list_float, std::vector<float>, UserOpAttrType::kAtListFloat)

#define LIST_ENUM_ATTR_SEQ \
  OF_PP_MAKE_TUPLE_SEQ(at_list_data_type, std::vector<DataType>, UserOpAttrType::kAtListDataType)

#define LIST_MESSAGE_ATTR_SEQ \
  OF_PP_MAKE_TUPLE_SEQ(at_list_shape, std::vector<Shape>, UserOpAttrType::kAtListShape)

#define LIST_STRING_ATTR_SEQ \
  OF_PP_MAKE_TUPLE_SEQ(at_list_string, std::vector<std::string>, UserOpAttrType::kAtListString)

#define ATTR_SEQ        \
  BASIC_ATTR_SEQ        \
  ENUM_ATTR_SEQ         \
  MESSAGE_ATTR_SEQ      \
  LIST_BASIC_ATTR_SEQ   \
  LIST_ENUM_ATTR_SEQ    \
  LIST_MESSAGE_ATTR_SEQ \
  LIST_STRING_ATTR_SEQ

// Type Trait: GetAttrType, GetCppType

template<typename T>
struct GetAttrType;

template<UserOpAttrType AttrT>
struct GetCppType;

#define SPECIALIZE_GET_ATTR_TYPE(field, type_cpp, type_proto)                           \
  template<>                                                                            \
  struct GetAttrType<type_cpp> : std::integral_constant<UserOpAttrType, type_proto> {}; \
  template<>                                                                            \
  struct GetCppType<type_proto> {                                                       \
    typedef type_cpp type;                                                              \
  };
OF_PP_FOR_EACH_TUPLE(SPECIALIZE_GET_ATTR_TYPE, ATTR_SEQ);
#undef SPECIALIZE_GET_ATTR_TYPE

}  // namespace user_op

}  // namespace oneflow

#endif  // ONEFLOW_CORE_FRAMEWORK_USER_OP_ATTR_H_