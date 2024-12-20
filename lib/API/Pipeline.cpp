//===- Pipeline.cpp - HLSL API Pipeline API -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "API/Pipeline.h"

using namespace hlsltest;

namespace llvm {
namespace yaml {
void MappingTraits<hlsltest::Pipeline>::mapping(IO &I, hlsltest::Pipeline &P) {
  MutableArrayRef<int> MutableDispatchSize(P.DispatchSize);
  I.mapRequired("DispatchSize", MutableDispatchSize);
  I.mapRequired("DescriptorSets", P.Sets);
}

void MappingTraits<hlsltest::DescriptorSet>::mapping(
    IO &I, hlsltest::DescriptorSet &D) {
  I.mapRequired("Resources", D.Resources);
}

void MappingTraits<hlsltest::Resource>::mapping(IO &I, hlsltest::Resource &R) {
  I.mapRequired("Access", R.Access);
  I.mapRequired("Format", R.Format);
  I.mapOptional("Channels", R.Channels, 1);
  I.mapOptional("RawSize", R.RawSize, 0);
  assert(R.RawSize >= 0 && "RawSize must be non-negative");
  switch (R.Format) {
#define DATA_CASE(Enum, Type)                                                  \
  case DataFormat::Enum: {                                                     \
    if (I.outputting()) {                                                      \
      llvm::MutableArrayRef<Type> Arr(reinterpret_cast<Type *>(R.Data.get()),  \
                                      R.Size / sizeof(Type));                  \
      I.mapRequired("Data", Arr);                                              \
    } else {                                                                   \
      int64_t ZeroInitSize;                                                    \
      I.mapOptional("ZeroInitSize", ZeroInitSize, 0);                          \
      if (ZeroInitSize > 0) {                                                  \
        R.Size = ZeroInitSize;                                                 \
        R.Data.reset(new char[R.Size]);                                        \
        memset(R.Data.get(), 0, R.Size);                                       \
        break;                                                                 \
      }                                                                        \
      llvm::SmallVector<Type, 64> Arr;                                         \
      I.mapRequired("Data", Arr);                                              \
      R.Size = Arr.size() * sizeof(Type);                                      \
      R.Data.reset(new char[R.Size]);                                          \
      memcpy(R.Data.get(), Arr.data(), R.Size);                                \
    }                                                                          \
    break;                                                                     \
  }
    DATA_CASE(Hex8, llvm::yaml::Hex8)
    DATA_CASE(Hex16, llvm::yaml::Hex16)
    DATA_CASE(Hex32, llvm::yaml::Hex32)
    DATA_CASE(Hex64, llvm::yaml::Hex64)
    DATA_CASE(UInt16, uint16_t)
    DATA_CASE(UInt32, uint32_t)
    DATA_CASE(UInt64, uint64_t)
    DATA_CASE(Int16, int16_t)
    DATA_CASE(Int32, int32_t)
    DATA_CASE(Int64, int64_t)
    DATA_CASE(Float32, float)
    DATA_CASE(Float64, double)
  }

  I.mapRequired("DirectXBinding", R.DXBinding);
  I.mapOptional("OutputProps", R.OutputProps);
}

void MappingTraits<hlsltest::DirectXBinding>::mapping(
    IO &I, hlsltest::DirectXBinding &B) {
  I.mapRequired("Register", B.Register);
  I.mapRequired("Space", B.Space);
}

void MappingTraits<hlsltest::OutputProperties>::mapping(
    IO &I, hlsltest::OutputProperties &P) {
  I.mapRequired("Name", P.Name);
  I.mapRequired("Height", P.Height);
  I.mapRequired("Width", P.Width);
  I.mapRequired("Depth", P.Depth);
}
} // namespace yaml
} // namespace llvm
