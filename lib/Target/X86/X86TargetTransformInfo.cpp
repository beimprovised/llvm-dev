//===-- X86TargetTransformInfo.cpp - X86 specific TTI pass ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements a TargetTransformInfo analysis pass specific to the
/// X86 target machine. It uses the target's detailed information to provide
/// more precise answers to certain TTI queries, while letting the target
/// independent and default TTI implementations handle the rest.
///
//===----------------------------------------------------------------------===//
/// About Cost Model numbers used below it's necessary to say the following:
/// the numbers correspond to some "generic" X86 CPU instead of usage of
/// concrete CPU model. Usually the numbers correspond to CPU where the feature
/// apeared at the first time. For example, if we do Subtarget.hasSSE42() in
/// the lookups below the cost is based on Nehalem as that was the first CPU
/// to support that feature level and thus has most likely the worst case cost.
/// Some examples of other technologies/CPUs:
///   SSE 3   - Pentium4 / Athlon64
///   SSE 4.1 - Penryn
///   SSE 4.2 - Nehalem
///   AVX     - Sandy Bridge
///   AVX2    - Haswell
///   AVX-512 - Xeon Phi / Skylake
/// And some examples of instruction target dependent costs (latency)
///                   divss     sqrtss          rsqrtss
///   AMD K7            11-16     19              3
///   Piledriver        9-24      13-15           5
///   Jaguar            14        16              2
///   Pentium II,III    18        30              2
///   Nehalem           7-14      7-18            3
///   Haswell           10-13     11              5
/// TODO: Develop and implement  the target dependent cost model and
/// specialize cost numbers for different Cost Model Targets such as throughput,
/// code size, latency and uop count.
//===----------------------------------------------------------------------===//

#include "X86TargetTransformInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/CostTable.h"
#include "llvm/Target/TargetLowering.h"

using namespace llvm;

#define DEBUG_TYPE "x86tti"

//===----------------------------------------------------------------------===//
//
// X86 cost model.
//
//===----------------------------------------------------------------------===//

TargetTransformInfo::PopcntSupportKind
X86TTIImpl::getPopcntSupport(unsigned TyWidth) {
  assert(isPowerOf2_32(TyWidth) && "Ty width must be power of 2");
  // TODO: Currently the __builtin_popcount() implementation using SSE3
  //   instructions is inefficient. Once the problem is fixed, we should
  //   call ST->hasSSE3() instead of ST->hasPOPCNT().
  return ST->hasPOPCNT() ? TTI::PSK_FastHardware : TTI::PSK_Software;
}

unsigned X86TTIImpl::getNumberOfRegisters(bool Vector) {
  if (Vector && !ST->hasSSE1())
    return 0;

  if (ST->is64Bit()) {
    if (Vector && ST->hasAVX512())
      return 32;
    return 16;
  }
  return 8;
}

unsigned X86TTIImpl::getRegisterBitWidth(bool Vector) {
  if (Vector) {
    if (ST->hasAVX512()) return 512;
    if (ST->hasAVX()) return 256;
    if (ST->hasSSE1()) return 128;
    return 0;
  }

  if (ST->is64Bit())
    return 64;

  return 32;
}

unsigned X86TTIImpl::getMaxInterleaveFactor(unsigned VF) {
  // If the loop will not be vectorized, don't interleave the loop.
  // Let regular unroll to unroll the loop, which saves the overflow
  // check and memory check cost.
  if (VF == 1)
    return 1;

  if (ST->isAtom())
    return 1;

  // Sandybridge and Haswell have multiple execution ports and pipelined
  // vector units.
  if (ST->hasAVX())
    return 4;

  return 2;
}

int X86TTIImpl::getArithmeticInstrCost(
    unsigned Opcode, Type *Ty, TTI::OperandValueKind Op1Info,
    TTI::OperandValueKind Op2Info, TTI::OperandValueProperties Opd1PropInfo,
    TTI::OperandValueProperties Opd2PropInfo) {
  // Legalize the type.
  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Ty);

  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  if (ISD == ISD::SDIV &&
      Op2Info == TargetTransformInfo::OK_UniformConstantValue &&
      Opd2PropInfo == TargetTransformInfo::OP_PowerOf2) {
    // On X86, vector signed division by constants power-of-two are
    // normally expanded to the sequence SRA + SRL + ADD + SRA.
    // The OperandValue properties many not be same as that of previous
    // operation;conservatively assume OP_None.
    int Cost = 2 * getArithmeticInstrCost(Instruction::AShr, Ty, Op1Info,
                                          Op2Info, TargetTransformInfo::OP_None,
                                          TargetTransformInfo::OP_None);
    Cost += getArithmeticInstrCost(Instruction::LShr, Ty, Op1Info, Op2Info,
                                   TargetTransformInfo::OP_None,
                                   TargetTransformInfo::OP_None);
    Cost += getArithmeticInstrCost(Instruction::Add, Ty, Op1Info, Op2Info,
                                   TargetTransformInfo::OP_None,
                                   TargetTransformInfo::OP_None);

    return Cost;
  }

  static const CostTblEntry AVX512BWUniformConstCostTable[] = {
    { ISD::SDIV, MVT::v32i16,  6 }, // vpmulhw sequence
    { ISD::UDIV, MVT::v32i16,  6 }, // vpmulhuw sequence
  };

  if (Op2Info == TargetTransformInfo::OK_UniformConstantValue &&
      ST->hasBWI()) {
    if (const auto *Entry = CostTableLookup(AVX512BWUniformConstCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX512UniformConstCostTable[] = {
    { ISD::SDIV, MVT::v16i32, 15 }, // vpmuldq sequence
    { ISD::UDIV, MVT::v16i32, 15 }, // vpmuludq sequence
  };

  if (Op2Info == TargetTransformInfo::OK_UniformConstantValue &&
      ST->hasAVX512()) {
    if (const auto *Entry = CostTableLookup(AVX512UniformConstCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX2UniformConstCostTable[] = {
    { ISD::SRA,  MVT::v4i64,   4 }, // 2 x psrad + shuffle.

    { ISD::SDIV, MVT::v16i16,  6 }, // vpmulhw sequence
    { ISD::UDIV, MVT::v16i16,  6 }, // vpmulhuw sequence
    { ISD::SDIV, MVT::v8i32,  15 }, // vpmuldq sequence
    { ISD::UDIV, MVT::v8i32,  15 }, // vpmuludq sequence
  };

  if (Op2Info == TargetTransformInfo::OK_UniformConstantValue &&
      ST->hasAVX2()) {
    if (const auto *Entry = CostTableLookup(AVX2UniformConstCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry SSE2UniformConstCostTable[] = {
    { ISD::SDIV, MVT::v16i16, 12 }, // pmulhw sequence
    { ISD::SDIV, MVT::v8i16,   6 }, // pmulhw sequence
    { ISD::UDIV, MVT::v16i16, 12 }, // pmulhuw sequence
    { ISD::UDIV, MVT::v8i16,   6 }, // pmulhuw sequence
    { ISD::SDIV, MVT::v8i32,  38 }, // pmuludq sequence
    { ISD::SDIV, MVT::v4i32,  19 }, // pmuludq sequence
    { ISD::UDIV, MVT::v8i32,  30 }, // pmuludq sequence
    { ISD::UDIV, MVT::v4i32,  15 }, // pmuludq sequence
  };

  if (Op2Info == TargetTransformInfo::OK_UniformConstantValue &&
      ST->hasSSE2()) {
    // pmuldq sequence.
    if (ISD == ISD::SDIV && LT.second == MVT::v8i32 && ST->hasAVX())
      return LT.first * 30;
    if (ISD == ISD::SDIV && LT.second == MVT::v4i32 && ST->hasSSE41())
      return LT.first * 15;

    if (const auto *Entry = CostTableLookup(SSE2UniformConstCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX512DQCostTable[] = {
    { ISD::MUL,  MVT::v2i64, 1 },
    { ISD::MUL,  MVT::v4i64, 1 },
    { ISD::MUL,  MVT::v8i64, 1 }
  };

  // Look for AVX512DQ lowering tricks for custom cases.
  if (ST->hasDQI()) {
    if (const auto *Entry = CostTableLookup(AVX512DQCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX512BWCostTable[] = {
    { ISD::MUL,   MVT::v64i8,     11 }, // extend/pmullw/trunc sequence.
    { ISD::MUL,   MVT::v32i8,      4 }, // extend/pmullw/trunc sequence.
    { ISD::MUL,   MVT::v16i8,      4 }, // extend/pmullw/trunc sequence.

    // Vectorizing division is a bad idea. See the SSE2 table for more comments.
    { ISD::SDIV,  MVT::v64i8,  64*20 },
    { ISD::SDIV,  MVT::v32i16, 32*20 },
    { ISD::SDIV,  MVT::v16i32, 16*20 },
    { ISD::SDIV,  MVT::v8i64,   8*20 },
    { ISD::UDIV,  MVT::v64i8,  64*20 },
    { ISD::UDIV,  MVT::v32i16, 32*20 },
    { ISD::UDIV,  MVT::v16i32, 16*20 },
    { ISD::UDIV,  MVT::v8i64,   8*20 },
  };

  // Look for AVX512BW lowering tricks for custom cases.
  if (ST->hasBWI()) {
    if (const auto *Entry = CostTableLookup(AVX512BWCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX512CostTable[] = {
    { ISD::SHL,     MVT::v16i32,    1 },
    { ISD::SRL,     MVT::v16i32,    1 },
    { ISD::SRA,     MVT::v16i32,    1 },
    { ISD::SHL,     MVT::v8i64,     1 },
    { ISD::SRL,     MVT::v8i64,     1 },
    { ISD::SRA,     MVT::v8i64,     1 },

    { ISD::MUL,     MVT::v32i8,    13 }, // extend/pmullw/trunc sequence.
    { ISD::MUL,     MVT::v16i8,     5 }, // extend/pmullw/trunc sequence.
  };

  if (ST->hasAVX512()) {
    if (const auto *Entry = CostTableLookup(AVX512CostTable, ISD, LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX2CostTable[] = {
    // Shifts on v4i64/v8i32 on AVX2 is legal even though we declare to
    // customize them to detect the cases where shift amount is a scalar one.
    { ISD::SHL,     MVT::v4i32,    1 },
    { ISD::SRL,     MVT::v4i32,    1 },
    { ISD::SRA,     MVT::v4i32,    1 },
    { ISD::SHL,     MVT::v8i32,    1 },
    { ISD::SRL,     MVT::v8i32,    1 },
    { ISD::SRA,     MVT::v8i32,    1 },
    { ISD::SHL,     MVT::v2i64,    1 },
    { ISD::SRL,     MVT::v2i64,    1 },
    { ISD::SHL,     MVT::v4i64,    1 },
    { ISD::SRL,     MVT::v4i64,    1 },
  };

  // Look for AVX2 lowering tricks.
  if (ST->hasAVX2()) {
    if (ISD == ISD::SHL && LT.second == MVT::v16i16 &&
        (Op2Info == TargetTransformInfo::OK_UniformConstantValue ||
         Op2Info == TargetTransformInfo::OK_NonUniformConstantValue))
      // On AVX2, a packed v16i16 shift left by a constant build_vector
      // is lowered into a vector multiply (vpmullw).
      return LT.first;

    if (const auto *Entry = CostTableLookup(AVX2CostTable, ISD, LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry XOPCostTable[] = {
    // 128bit shifts take 1cy, but right shifts require negation beforehand.
    { ISD::SHL,     MVT::v16i8,    1 },
    { ISD::SRL,     MVT::v16i8,    2 },
    { ISD::SRA,     MVT::v16i8,    2 },
    { ISD::SHL,     MVT::v8i16,    1 },
    { ISD::SRL,     MVT::v8i16,    2 },
    { ISD::SRA,     MVT::v8i16,    2 },
    { ISD::SHL,     MVT::v4i32,    1 },
    { ISD::SRL,     MVT::v4i32,    2 },
    { ISD::SRA,     MVT::v4i32,    2 },
    { ISD::SHL,     MVT::v2i64,    1 },
    { ISD::SRL,     MVT::v2i64,    2 },
    { ISD::SRA,     MVT::v2i64,    2 },
    // 256bit shifts require splitting if AVX2 didn't catch them above.
    { ISD::SHL,     MVT::v32i8,    2 },
    { ISD::SRL,     MVT::v32i8,    4 },
    { ISD::SRA,     MVT::v32i8,    4 },
    { ISD::SHL,     MVT::v16i16,   2 },
    { ISD::SRL,     MVT::v16i16,   4 },
    { ISD::SRA,     MVT::v16i16,   4 },
    { ISD::SHL,     MVT::v8i32,    2 },
    { ISD::SRL,     MVT::v8i32,    4 },
    { ISD::SRA,     MVT::v8i32,    4 },
    { ISD::SHL,     MVT::v4i64,    2 },
    { ISD::SRL,     MVT::v4i64,    4 },
    { ISD::SRA,     MVT::v4i64,    4 },
  };

  // Look for XOP lowering tricks.
  if (ST->hasXOP()) {
    if (const auto *Entry = CostTableLookup(XOPCostTable, ISD, LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX2CustomCostTable[] = {
    { ISD::SHL,  MVT::v32i8,      11 }, // vpblendvb sequence.
    { ISD::SHL,  MVT::v16i16,     10 }, // extend/vpsrlvd/pack sequence.

    { ISD::SRL,  MVT::v32i8,      11 }, // vpblendvb sequence.
    { ISD::SRL,  MVT::v16i16,     10 }, // extend/vpsrlvd/pack sequence.

    { ISD::SRA,  MVT::v32i8,      24 }, // vpblendvb sequence.
    { ISD::SRA,  MVT::v16i16,     10 }, // extend/vpsravd/pack sequence.
    { ISD::SRA,  MVT::v2i64,       4 }, // srl/xor/sub sequence.
    { ISD::SRA,  MVT::v4i64,       4 }, // srl/xor/sub sequence.

    { ISD::MUL,   MVT::v32i8,     17 }, // extend/pmullw/trunc sequence.
    { ISD::MUL,   MVT::v16i8,      7 }, // extend/pmullw/trunc sequence.

    { ISD::FDIV,  MVT::f32,        7 }, // Haswell from http://www.agner.org/
    { ISD::FDIV,  MVT::v4f32,      7 }, // Haswell from http://www.agner.org/
    { ISD::FDIV,  MVT::v8f32,     14 }, // Haswell from http://www.agner.org/
    { ISD::FDIV,  MVT::f64,       14 }, // Haswell from http://www.agner.org/
    { ISD::FDIV,  MVT::v2f64,     14 }, // Haswell from http://www.agner.org/
    { ISD::FDIV,  MVT::v4f64,     28 }, // Haswell from http://www.agner.org/
  };

  // Look for AVX2 lowering tricks for custom cases.
  if (ST->hasAVX2()) {
    if (const auto *Entry = CostTableLookup(AVX2CustomCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVXCustomCostTable[] = {
    { ISD::MUL,   MVT::v32i8,  26 }, // extend/pmullw/trunc sequence.

    { ISD::FDIV,  MVT::f32,    14 }, // SNB from http://www.agner.org/
    { ISD::FDIV,  MVT::v4f32,  14 }, // SNB from http://www.agner.org/
    { ISD::FDIV,  MVT::v8f32,  28 }, // SNB from http://www.agner.org/
    { ISD::FDIV,  MVT::f64,    22 }, // SNB from http://www.agner.org/
    { ISD::FDIV,  MVT::v2f64,  22 }, // SNB from http://www.agner.org/
    { ISD::FDIV,  MVT::v4f64,  44 }, // SNB from http://www.agner.org/

    // Vectorizing division is a bad idea. See the SSE2 table for more comments.
    { ISD::SDIV,  MVT::v32i8,  32*20 },
    { ISD::SDIV,  MVT::v16i16, 16*20 },
    { ISD::SDIV,  MVT::v8i32,  8*20 },
    { ISD::SDIV,  MVT::v4i64,  4*20 },
    { ISD::UDIV,  MVT::v32i8,  32*20 },
    { ISD::UDIV,  MVT::v16i16, 16*20 },
    { ISD::UDIV,  MVT::v8i32,  8*20 },
    { ISD::UDIV,  MVT::v4i64,  4*20 },
  };

  // Look for AVX2 lowering tricks for custom cases.
  if (ST->hasAVX()) {
    if (const auto *Entry = CostTableLookup(AVXCustomCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry SSE42FloatCostTable[] = {
    { ISD::FDIV,  MVT::f32,   14 }, // Nehalem from http://www.agner.org/
    { ISD::FDIV,  MVT::v4f32, 14 }, // Nehalem from http://www.agner.org/
    { ISD::FDIV,  MVT::f64,   22 }, // Nehalem from http://www.agner.org/
    { ISD::FDIV,  MVT::v2f64, 22 }, // Nehalem from http://www.agner.org/
  };

  if (ST->hasSSE42()) {
    if (const auto *Entry = CostTableLookup(SSE42FloatCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry
  SSE2UniformCostTable[] = {
    // Uniform splats are cheaper for the following instructions.
    { ISD::SHL,  MVT::v16i8,  1 }, // psllw.
    { ISD::SHL,  MVT::v32i8,  2 }, // psllw.
    { ISD::SHL,  MVT::v8i16,  1 }, // psllw.
    { ISD::SHL,  MVT::v16i16, 2 }, // psllw.
    { ISD::SHL,  MVT::v4i32,  1 }, // pslld
    { ISD::SHL,  MVT::v8i32,  2 }, // pslld
    { ISD::SHL,  MVT::v2i64,  1 }, // psllq.
    { ISD::SHL,  MVT::v4i64,  2 }, // psllq.

    { ISD::SRL,  MVT::v16i8,  1 }, // psrlw.
    { ISD::SRL,  MVT::v32i8,  2 }, // psrlw.
    { ISD::SRL,  MVT::v8i16,  1 }, // psrlw.
    { ISD::SRL,  MVT::v16i16, 2 }, // psrlw.
    { ISD::SRL,  MVT::v4i32,  1 }, // psrld.
    { ISD::SRL,  MVT::v8i32,  2 }, // psrld.
    { ISD::SRL,  MVT::v2i64,  1 }, // psrlq.
    { ISD::SRL,  MVT::v4i64,  2 }, // psrlq.

    { ISD::SRA,  MVT::v16i8,  4 }, // psrlw, pand, pxor, psubb.
    { ISD::SRA,  MVT::v32i8,  8 }, // psrlw, pand, pxor, psubb.
    { ISD::SRA,  MVT::v8i16,  1 }, // psraw.
    { ISD::SRA,  MVT::v16i16, 2 }, // psraw.
    { ISD::SRA,  MVT::v4i32,  1 }, // psrad.
    { ISD::SRA,  MVT::v8i32,  2 }, // psrad.
    { ISD::SRA,  MVT::v2i64,  4 }, // 2 x psrad + shuffle.
    { ISD::SRA,  MVT::v4i64,  8 }, // 2 x psrad + shuffle.
  };

  if (ST->hasSSE2() &&
      ((Op2Info == TargetTransformInfo::OK_UniformConstantValue) ||
       (Op2Info == TargetTransformInfo::OK_UniformValue))) {
    if (const auto *Entry =
            CostTableLookup(SSE2UniformCostTable, ISD, LT.second))
      return LT.first * Entry->Cost;
  }

  if (ISD == ISD::SHL &&
      Op2Info == TargetTransformInfo::OK_NonUniformConstantValue) {
    MVT VT = LT.second;
    // Vector shift left by non uniform constant can be lowered
    // into vector multiply (pmullw/pmulld).
    if ((VT == MVT::v8i16 && ST->hasSSE2()) ||
        (VT == MVT::v4i32 && ST->hasSSE41()))
      return LT.first;

    // v16i16 and v8i32 shifts by non-uniform constants are lowered into a
    // sequence of extract + two vector multiply + insert.
    if ((VT == MVT::v8i32 || VT == MVT::v16i16) &&
       (ST->hasAVX() && !ST->hasAVX2()))
      ISD = ISD::MUL;

    // A vector shift left by non uniform constant is converted
    // into a vector multiply; the new multiply is eventually
    // lowered into a sequence of shuffles and 2 x pmuludq.
    if (VT == MVT::v4i32 && ST->hasSSE2())
      ISD = ISD::MUL;
  }

  static const CostTblEntry SSE41CostTable[] = {
    { ISD::SHL,  MVT::v16i8,    11 }, // pblendvb sequence.
    { ISD::SHL,  MVT::v32i8,  2*11 }, // pblendvb sequence.
    { ISD::SHL,  MVT::v8i16,    14 }, // pblendvb sequence.
    { ISD::SHL,  MVT::v16i16, 2*14 }, // pblendvb sequence.

    { ISD::SRL,  MVT::v16i8,    12 }, // pblendvb sequence.
    { ISD::SRL,  MVT::v32i8,  2*12 }, // pblendvb sequence.
    { ISD::SRL,  MVT::v8i16,    14 }, // pblendvb sequence.
    { ISD::SRL,  MVT::v16i16, 2*14 }, // pblendvb sequence.
    { ISD::SRL,  MVT::v4i32,    11 }, // Shift each lane + blend.
    { ISD::SRL,  MVT::v8i32,  2*11 }, // Shift each lane + blend.

    { ISD::SRA,  MVT::v16i8,    24 }, // pblendvb sequence.
    { ISD::SRA,  MVT::v32i8,  2*24 }, // pblendvb sequence.
    { ISD::SRA,  MVT::v8i16,    14 }, // pblendvb sequence.
    { ISD::SRA,  MVT::v16i16, 2*14 }, // pblendvb sequence.
    { ISD::SRA,  MVT::v4i32,    12 }, // Shift each lane + blend.
    { ISD::SRA,  MVT::v8i32,  2*12 }, // Shift each lane + blend.
  };

  if (ST->hasSSE41()) {
    if (const auto *Entry = CostTableLookup(SSE41CostTable, ISD, LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry SSE2CostTable[] = {
    // We don't correctly identify costs of casts because they are marked as
    // custom.
    { ISD::SHL,  MVT::v16i8,    26 }, // cmpgtb sequence.
    { ISD::SHL,  MVT::v32i8,  2*26 }, // cmpgtb sequence.
    { ISD::SHL,  MVT::v8i16,    32 }, // cmpgtb sequence.
    { ISD::SHL,  MVT::v16i16, 2*32 }, // cmpgtb sequence.
    { ISD::SHL,  MVT::v4i32,   2*5 }, // We optimized this using mul.
    { ISD::SHL,  MVT::v8i32, 2*2*5 }, // We optimized this using mul.
    { ISD::SHL,  MVT::v2i64,     4 }, // splat+shuffle sequence.
    { ISD::SHL,  MVT::v4i64,   2*4 }, // splat+shuffle sequence.

    { ISD::SRL,  MVT::v16i8,    26 }, // cmpgtb sequence.
    { ISD::SRL,  MVT::v32i8,  2*26 }, // cmpgtb sequence.
    { ISD::SRL,  MVT::v8i16,    32 }, // cmpgtb sequence.
    { ISD::SRL,  MVT::v16i16, 2*32 }, // cmpgtb sequence.
    { ISD::SRL,  MVT::v4i32,    16 }, // Shift each lane + blend.
    { ISD::SRL,  MVT::v8i32,  2*16 }, // Shift each lane + blend.
    { ISD::SRL,  MVT::v2i64,     4 }, // splat+shuffle sequence.
    { ISD::SRL,  MVT::v4i64,   2*4 }, // splat+shuffle sequence.

    { ISD::SRA,  MVT::v16i8,    54 }, // unpacked cmpgtb sequence.
    { ISD::SRA,  MVT::v32i8,  2*54 }, // unpacked cmpgtb sequence.
    { ISD::SRA,  MVT::v8i16,    32 }, // cmpgtb sequence.
    { ISD::SRA,  MVT::v16i16, 2*32 }, // cmpgtb sequence.
    { ISD::SRA,  MVT::v4i32,    16 }, // Shift each lane + blend.
    { ISD::SRA,  MVT::v8i32,  2*16 }, // Shift each lane + blend.
    { ISD::SRA,  MVT::v2i64,    12 }, // srl/xor/sub sequence.
    { ISD::SRA,  MVT::v4i64,  2*12 }, // srl/xor/sub sequence.

    { ISD::MUL,  MVT::v16i8,    12 }, // extend/pmullw/trunc sequence.

    { ISD::FDIV, MVT::f32,      23 }, // Pentium IV from http://www.agner.org/
    { ISD::FDIV, MVT::v4f32,    39 }, // Pentium IV from http://www.agner.org/
    { ISD::FDIV, MVT::f64,      38 }, // Pentium IV from http://www.agner.org/
    { ISD::FDIV, MVT::v2f64,    69 }, // Pentium IV from http://www.agner.org/

    // It is not a good idea to vectorize division. We have to scalarize it and
    // in the process we will often end up having to spilling regular
    // registers. The overhead of division is going to dominate most kernels
    // anyways so try hard to prevent vectorization of division - it is
    // generally a bad idea. Assume somewhat arbitrarily that we have to be able
    // to hide "20 cycles" for each lane.
    { ISD::SDIV,  MVT::v16i8,  16*20 },
    { ISD::SDIV,  MVT::v8i16,  8*20 },
    { ISD::SDIV,  MVT::v4i32,  4*20 },
    { ISD::SDIV,  MVT::v2i64,  2*20 },
    { ISD::UDIV,  MVT::v16i8,  16*20 },
    { ISD::UDIV,  MVT::v8i16,  8*20 },
    { ISD::UDIV,  MVT::v4i32,  4*20 },
    { ISD::UDIV,  MVT::v2i64,  2*20 },
  };

  if (ST->hasSSE2()) {
    if (const auto *Entry = CostTableLookup(SSE2CostTable, ISD, LT.second))
      return LT.first * Entry->Cost;
  }

  static const CostTblEntry AVX1CostTable[] = {
    // We don't have to scalarize unsupported ops. We can issue two half-sized
    // operations and we only need to extract the upper YMM half.
    // Two ops + 1 extract + 1 insert = 4.
    { ISD::MUL,     MVT::v16i16,   4 },
    { ISD::MUL,     MVT::v8i32,    4 },
    { ISD::SUB,     MVT::v32i8,    4 },
    { ISD::ADD,     MVT::v32i8,    4 },
    { ISD::SUB,     MVT::v16i16,   4 },
    { ISD::ADD,     MVT::v16i16,   4 },
    { ISD::SUB,     MVT::v8i32,    4 },
    { ISD::ADD,     MVT::v8i32,    4 },
    { ISD::SUB,     MVT::v4i64,    4 },
    { ISD::ADD,     MVT::v4i64,    4 },
    // A v4i64 multiply is custom lowered as two split v2i64 vectors that then
    // are lowered as a series of long multiplies(3), shifts(4) and adds(2)
    // Because we believe v4i64 to be a legal type, we must also include the
    // split factor of two in the cost table. Therefore, the cost here is 18
    // instead of 9.
    { ISD::MUL,     MVT::v4i64,    18 },
  };

  // Look for AVX1 lowering tricks.
  if (ST->hasAVX() && !ST->hasAVX2()) {
    MVT VT = LT.second;

    if (const auto *Entry = CostTableLookup(AVX1CostTable, ISD, VT))
      return LT.first * Entry->Cost;
  }

  // Custom lowering of vectors.
  static const CostTblEntry CustomLowered[] = {
    // A v2i64/v4i64 and multiply is custom lowered as a series of long
    // multiplies(3), shifts(4) and adds(2).
    { ISD::MUL,     MVT::v2i64,    9 },
    { ISD::MUL,     MVT::v4i64,    9 },
    { ISD::MUL,     MVT::v8i64,    9 }
  };
  if (const auto *Entry = CostTableLookup(CustomLowered, ISD, LT.second))
    return LT.first * Entry->Cost;

  // Special lowering of v4i32 mul on sse2, sse3: Lower v4i32 mul as 2x shuffle,
  // 2x pmuludq, 2x shuffle.
  if (ISD == ISD::MUL && LT.second == MVT::v4i32 && ST->hasSSE2() &&
      !ST->hasSSE41())
    return LT.first * 6;

  static const CostTblEntry SSE1FloatCostTable[] = {
    { ISD::FDIV, MVT::f32,   17 }, // Pentium III from http://www.agner.org/
    { ISD::FDIV, MVT::v4f32, 34 }, // Pentium III from http://www.agner.org/
  };

  if (ST->hasSSE1())
    if (const auto *Entry = CostTableLookup(SSE1FloatCostTable, ISD,
                                            LT.second))
      return LT.first * Entry->Cost;
  // Fallback to the default implementation.
  return BaseT::getArithmeticInstrCost(Opcode, Ty, Op1Info, Op2Info);
}

int X86TTIImpl::getShuffleCost(TTI::ShuffleKind Kind, Type *Tp, int Index,
                               Type *SubTp) {
  // We only estimate the cost of reverse and alternate shuffles.
  if (Kind != TTI::SK_Reverse && Kind != TTI::SK_Alternate)
    return BaseT::getShuffleCost(Kind, Tp, Index, SubTp);

  if (Kind == TTI::SK_Reverse) {
    std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Tp);
    int Cost = 1;
    if (LT.second.getSizeInBits() > 128)
      Cost = 3; // Extract + insert + copy.

    // Multiple by the number of parts.
    return Cost * LT.first;
  }

  if (Kind == TTI::SK_Alternate) {
    // 64-bit packed float vectors (v2f32) are widened to type v4f32.
    // 64-bit packed integer vectors (v2i32) are promoted to type v2i64.
    std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Tp);

    // The backend knows how to generate a single VEX.256 version of
    // instruction VPBLENDW if the target supports AVX2.
    if (ST->hasAVX2() && LT.second == MVT::v16i16)
      return LT.first;

    static const CostTblEntry AVXAltShuffleTbl[] = {
      {ISD::VECTOR_SHUFFLE, MVT::v4i64, 1},  // vblendpd
      {ISD::VECTOR_SHUFFLE, MVT::v4f64, 1},  // vblendpd

      {ISD::VECTOR_SHUFFLE, MVT::v8i32, 1},  // vblendps
      {ISD::VECTOR_SHUFFLE, MVT::v8f32, 1},  // vblendps

      // This shuffle is custom lowered into a sequence of:
      //  2x  vextractf128 , 2x vpblendw , 1x vinsertf128
      {ISD::VECTOR_SHUFFLE, MVT::v16i16, 5},

      // This shuffle is custom lowered into a long sequence of:
      //  2x vextractf128 , 4x vpshufb , 2x vpor ,  1x vinsertf128
      {ISD::VECTOR_SHUFFLE, MVT::v32i8, 9}
    };

    if (ST->hasAVX())
      if (const auto *Entry = CostTableLookup(AVXAltShuffleTbl,
                                              ISD::VECTOR_SHUFFLE, LT.second))
        return LT.first * Entry->Cost;

    static const CostTblEntry SSE41AltShuffleTbl[] = {
      // These are lowered into movsd.
      {ISD::VECTOR_SHUFFLE, MVT::v2i64, 1},
      {ISD::VECTOR_SHUFFLE, MVT::v2f64, 1},

      // packed float vectors with four elements are lowered into BLENDI dag
      // nodes. A v4i32/v4f32 BLENDI generates a single 'blendps'/'blendpd'.
      {ISD::VECTOR_SHUFFLE, MVT::v4i32, 1},
      {ISD::VECTOR_SHUFFLE, MVT::v4f32, 1},

      // This shuffle generates a single pshufw.
      {ISD::VECTOR_SHUFFLE, MVT::v8i16, 1},

      // There is no instruction that matches a v16i8 alternate shuffle.
      // The backend will expand it into the sequence 'pshufb + pshufb + or'.
      {ISD::VECTOR_SHUFFLE, MVT::v16i8, 3}
    };

    if (ST->hasSSE41())
      if (const auto *Entry = CostTableLookup(SSE41AltShuffleTbl, ISD::VECTOR_SHUFFLE,
                                              LT.second))
        return LT.first * Entry->Cost;

    static const CostTblEntry SSSE3AltShuffleTbl[] = {
      {ISD::VECTOR_SHUFFLE, MVT::v2i64, 1},  // movsd
      {ISD::VECTOR_SHUFFLE, MVT::v2f64, 1},  // movsd

      // SSE3 doesn't have 'blendps'. The following shuffles are expanded into
      // the sequence 'shufps + pshufd'
      {ISD::VECTOR_SHUFFLE, MVT::v4i32, 2},
      {ISD::VECTOR_SHUFFLE, MVT::v4f32, 2},

      {ISD::VECTOR_SHUFFLE, MVT::v8i16, 3}, // pshufb + pshufb + or
      {ISD::VECTOR_SHUFFLE, MVT::v16i8, 3}  // pshufb + pshufb + or
    };

    if (ST->hasSSSE3())
      if (const auto *Entry = CostTableLookup(SSSE3AltShuffleTbl,
                                              ISD::VECTOR_SHUFFLE, LT.second))
        return LT.first * Entry->Cost;

    static const CostTblEntry SSEAltShuffleTbl[] = {
      {ISD::VECTOR_SHUFFLE, MVT::v2i64, 1},  // movsd
      {ISD::VECTOR_SHUFFLE, MVT::v2f64, 1},  // movsd

      {ISD::VECTOR_SHUFFLE, MVT::v4i32, 2}, // shufps + pshufd
      {ISD::VECTOR_SHUFFLE, MVT::v4f32, 2}, // shufps + pshufd

      // This is expanded into a long sequence of four extract + four insert.
      {ISD::VECTOR_SHUFFLE, MVT::v8i16, 8}, // 4 x pextrw + 4 pinsrw.

      // 8 x (pinsrw + pextrw + and + movb + movzb + or)
      {ISD::VECTOR_SHUFFLE, MVT::v16i8, 48}
    };

    // Fall-back (SSE3 and SSE2).
    if (const auto *Entry = CostTableLookup(SSEAltShuffleTbl,
                                            ISD::VECTOR_SHUFFLE, LT.second))
      return LT.first * Entry->Cost;
    return BaseT::getShuffleCost(Kind, Tp, Index, SubTp);
  }

  return BaseT::getShuffleCost(Kind, Tp, Index, SubTp);
}

int X86TTIImpl::getCastInstrCost(unsigned Opcode, Type *Dst, Type *Src) {
  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  // FIXME: Need a better design of the cost table to handle non-simple types of
  // potential massive combinations (elem_num x src_type x dst_type).

  static const TypeConversionCostTblEntry AVX512DQConversionTbl[] = {
    { ISD::UINT_TO_FP,  MVT::v2f32,  MVT::v2i64,  1 },
    { ISD::UINT_TO_FP,  MVT::v2f64,  MVT::v2i64,  1 },
    { ISD::UINT_TO_FP,  MVT::v4f32,  MVT::v4i64,  1 },
    { ISD::UINT_TO_FP,  MVT::v4f64,  MVT::v4i64,  1 },
    { ISD::UINT_TO_FP,  MVT::v8f32,  MVT::v8i64,  1 },
    { ISD::UINT_TO_FP,  MVT::v8f64,  MVT::v8i64,  1 },

    { ISD::FP_TO_UINT,  MVT::v2i64, MVT::v2f32, 1 },
    { ISD::FP_TO_UINT,  MVT::v4i64, MVT::v4f32, 1 },
    { ISD::FP_TO_UINT,  MVT::v8i64, MVT::v8f32, 1 },
    { ISD::FP_TO_UINT,  MVT::v2i64, MVT::v2f64, 1 },
    { ISD::FP_TO_UINT,  MVT::v4i64, MVT::v4f64, 1 },
    { ISD::FP_TO_UINT,  MVT::v8i64, MVT::v8f64, 1 },
  };

  // TODO: For AVX512DQ + AVX512VL, we also have cheap casts for 128-bit and
  // 256-bit wide vectors.

  static const TypeConversionCostTblEntry AVX512FConversionTbl[] = {
    { ISD::FP_EXTEND, MVT::v8f64,   MVT::v8f32,  1 },
    { ISD::FP_EXTEND, MVT::v8f64,   MVT::v16f32, 3 },
    { ISD::FP_ROUND,  MVT::v8f32,   MVT::v8f64,  1 },

    { ISD::TRUNCATE,  MVT::v16i8,   MVT::v16i32, 1 },
    { ISD::TRUNCATE,  MVT::v16i16,  MVT::v16i32, 1 },
    { ISD::TRUNCATE,  MVT::v8i16,   MVT::v8i64,  1 },
    { ISD::TRUNCATE,  MVT::v8i32,   MVT::v8i64,  1 },

    // v16i1 -> v16i32 - load + broadcast
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i1,  2 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i1,  2 },
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i8,  1 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i8,  1 },
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i16, 1 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i16, 1 },
    { ISD::ZERO_EXTEND, MVT::v8i64,  MVT::v8i16,  1 },
    { ISD::SIGN_EXTEND, MVT::v8i64,  MVT::v8i16,  1 },
    { ISD::SIGN_EXTEND, MVT::v8i64,  MVT::v8i32,  1 },
    { ISD::ZERO_EXTEND, MVT::v8i64,  MVT::v8i32,  1 },

    { ISD::SINT_TO_FP,  MVT::v8f64,  MVT::v8i1,   4 },
    { ISD::SINT_TO_FP,  MVT::v16f32, MVT::v16i1,  3 },
    { ISD::SINT_TO_FP,  MVT::v8f64,  MVT::v8i8,   2 },
    { ISD::SINT_TO_FP,  MVT::v16f32, MVT::v16i8,  2 },
    { ISD::SINT_TO_FP,  MVT::v8f64,  MVT::v8i16,  2 },
    { ISD::SINT_TO_FP,  MVT::v16f32, MVT::v16i16, 2 },
    { ISD::SINT_TO_FP,  MVT::v16f32, MVT::v16i32, 1 },
    { ISD::SINT_TO_FP,  MVT::v8f64,  MVT::v8i32,  1 },
    { ISD::UINT_TO_FP,  MVT::v8f32,  MVT::v8i64, 26 },
    { ISD::UINT_TO_FP,  MVT::v8f64,  MVT::v8i64, 26 },

    { ISD::UINT_TO_FP,  MVT::v8f64,  MVT::v8i1,   4 },
    { ISD::UINT_TO_FP,  MVT::v16f32, MVT::v16i1,  3 },
    { ISD::UINT_TO_FP,  MVT::v2f64,  MVT::v2i8,   2 },
    { ISD::UINT_TO_FP,  MVT::v4f64,  MVT::v4i8,   2 },
    { ISD::UINT_TO_FP,  MVT::v8f32,  MVT::v8i8,   2 },
    { ISD::UINT_TO_FP,  MVT::v8f64,  MVT::v8i8,   2 },
    { ISD::UINT_TO_FP,  MVT::v16f32, MVT::v16i8,  2 },
    { ISD::UINT_TO_FP,  MVT::v2f64,  MVT::v2i16,  5 },
    { ISD::UINT_TO_FP,  MVT::v4f64,  MVT::v4i16,  2 },
    { ISD::UINT_TO_FP,  MVT::v8f32,  MVT::v8i16,  2 },
    { ISD::UINT_TO_FP,  MVT::v8f64,  MVT::v8i16,  2 },
    { ISD::UINT_TO_FP,  MVT::v16f32, MVT::v16i16, 2 },
    { ISD::UINT_TO_FP,  MVT::v2f32,  MVT::v2i32,  2 },
    { ISD::UINT_TO_FP,  MVT::v2f64,  MVT::v2i32,  1 },
    { ISD::UINT_TO_FP,  MVT::v4f32,  MVT::v4i32,  1 },
    { ISD::UINT_TO_FP,  MVT::v4f64,  MVT::v4i32,  1 },
    { ISD::UINT_TO_FP,  MVT::v8f32,  MVT::v8i32,  1 },
    { ISD::UINT_TO_FP,  MVT::v8f64,  MVT::v8i32,  1 },
    { ISD::UINT_TO_FP,  MVT::v16f32, MVT::v16i32, 1 },
    { ISD::UINT_TO_FP,  MVT::v2f32,  MVT::v2i64,  5 },
    { ISD::UINT_TO_FP,  MVT::v2f64,  MVT::v2i64,  5 },
    { ISD::UINT_TO_FP,  MVT::v4f64,  MVT::v4i64, 12 },
    { ISD::UINT_TO_FP,  MVT::v8f64,  MVT::v8i64, 26 },

    { ISD::FP_TO_UINT,  MVT::v2i32,  MVT::v2f32,  1 },
    { ISD::FP_TO_UINT,  MVT::v4i32,  MVT::v4f32,  1 },
    { ISD::FP_TO_UINT,  MVT::v8i32,  MVT::v8f32,  1 },
    { ISD::FP_TO_UINT,  MVT::v16i32, MVT::v16f32, 1 },
  };

  static const TypeConversionCostTblEntry AVX2ConversionTbl[] = {
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i1,   3 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i1,   3 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i1,   3 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i1,   3 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i8,   3 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i8,   3 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i8,   3 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i8,   3 },
    { ISD::SIGN_EXTEND, MVT::v16i16, MVT::v16i8,  1 },
    { ISD::ZERO_EXTEND, MVT::v16i16, MVT::v16i8,  1 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i16,  3 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i16,  3 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i16,  1 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i16,  1 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i32,  1 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i32,  1 },

    { ISD::TRUNCATE,    MVT::v4i8,   MVT::v4i64,  2 },
    { ISD::TRUNCATE,    MVT::v4i16,  MVT::v4i64,  2 },
    { ISD::TRUNCATE,    MVT::v4i32,  MVT::v4i64,  2 },
    { ISD::TRUNCATE,    MVT::v8i8,   MVT::v8i32,  2 },
    { ISD::TRUNCATE,    MVT::v8i16,  MVT::v8i32,  2 },
    { ISD::TRUNCATE,    MVT::v8i32,  MVT::v8i64,  4 },

    { ISD::FP_EXTEND,   MVT::v8f64,  MVT::v8f32,  3 },
    { ISD::FP_ROUND,    MVT::v8f32,  MVT::v8f64,  3 },

    { ISD::UINT_TO_FP,  MVT::v8f32,  MVT::v8i32,  8 },
  };

  static const TypeConversionCostTblEntry AVXConversionTbl[] = {
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i1,  6 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i1,  4 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i1,  7 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i1,  4 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i8,  6 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i8,  4 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i8,  7 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i8,  4 },
    { ISD::SIGN_EXTEND, MVT::v16i16, MVT::v16i8, 4 },
    { ISD::ZERO_EXTEND, MVT::v16i16, MVT::v16i8, 4 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i16, 6 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i16, 3 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i16, 4 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i16, 4 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i32, 4 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i32, 4 },

    { ISD::TRUNCATE,    MVT::v16i8, MVT::v16i16, 4 },
    { ISD::TRUNCATE,    MVT::v8i8,  MVT::v8i32,  4 },
    { ISD::TRUNCATE,    MVT::v8i16, MVT::v8i32,  5 },
    { ISD::TRUNCATE,    MVT::v4i8,  MVT::v4i64,  4 },
    { ISD::TRUNCATE,    MVT::v4i16, MVT::v4i64,  4 },
    { ISD::TRUNCATE,    MVT::v4i32, MVT::v4i64,  4 },
    { ISD::TRUNCATE,    MVT::v8i32, MVT::v8i64,  9 },

    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i1,  3 },
    { ISD::SINT_TO_FP,  MVT::v4f64, MVT::v4i1,  3 },
    { ISD::SINT_TO_FP,  MVT::v8f32, MVT::v8i1,  8 },
    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i8,  3 },
    { ISD::SINT_TO_FP,  MVT::v4f64, MVT::v4i8,  3 },
    { ISD::SINT_TO_FP,  MVT::v8f32, MVT::v8i8,  8 },
    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i16, 3 },
    { ISD::SINT_TO_FP,  MVT::v4f64, MVT::v4i16, 3 },
    { ISD::SINT_TO_FP,  MVT::v8f32, MVT::v8i16, 5 },
    { ISD::SINT_TO_FP,  MVT::v4f32, MVT::v4i32, 1 },
    { ISD::SINT_TO_FP,  MVT::v4f64, MVT::v4i32, 1 },
    { ISD::SINT_TO_FP,  MVT::v8f32, MVT::v8i32, 1 },

    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i1,  7 },
    { ISD::UINT_TO_FP,  MVT::v4f64, MVT::v4i1,  7 },
    { ISD::UINT_TO_FP,  MVT::v8f32, MVT::v8i1,  6 },
    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i8,  2 },
    { ISD::UINT_TO_FP,  MVT::v4f64, MVT::v4i8,  2 },
    { ISD::UINT_TO_FP,  MVT::v8f32, MVT::v8i8,  5 },
    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i16, 2 },
    { ISD::UINT_TO_FP,  MVT::v4f64, MVT::v4i16, 2 },
    { ISD::UINT_TO_FP,  MVT::v8f32, MVT::v8i16, 5 },
    { ISD::UINT_TO_FP,  MVT::v2f64, MVT::v2i32, 6 },
    { ISD::UINT_TO_FP,  MVT::v4f32, MVT::v4i32, 6 },
    { ISD::UINT_TO_FP,  MVT::v4f64, MVT::v4i32, 6 },
    { ISD::UINT_TO_FP,  MVT::v8f32, MVT::v8i32, 9 },
    // The generic code to compute the scalar overhead is currently broken.
    // Workaround this limitation by estimating the scalarization overhead
    // here. We have roughly 10 instructions per scalar element.
    // Multiply that by the vector width.
    // FIXME: remove that when PR19268 is fixed.
    { ISD::UINT_TO_FP,  MVT::v2f64, MVT::v2i64, 10 },
    { ISD::UINT_TO_FP,  MVT::v4f64, MVT::v4i64, 20 },
    { ISD::SINT_TO_FP,  MVT::v4f64, MVT::v4i64, 13 },
    { ISD::SINT_TO_FP,  MVT::v4f64, MVT::v4i64, 13 },

    { ISD::FP_TO_SINT,  MVT::v4i8,  MVT::v4f32, 1 },
    { ISD::FP_TO_SINT,  MVT::v8i8,  MVT::v8f32, 7 },
    // This node is expanded into scalarized operations but BasicTTI is overly
    // optimistic estimating its cost.  It computes 3 per element (one
    // vector-extract, one scalar conversion and one vector-insert).  The
    // problem is that the inserts form a read-modify-write chain so latency
    // should be factored in too.  Inflating the cost per element by 1.
    { ISD::FP_TO_UINT,  MVT::v8i32, MVT::v8f32, 8*4 },
    { ISD::FP_TO_UINT,  MVT::v4i32, MVT::v4f64, 4*4 },

    { ISD::FP_EXTEND,   MVT::v4f64,  MVT::v4f32,  1 },
    { ISD::FP_ROUND,    MVT::v4f32,  MVT::v4f64,  1 },
  };

  static const TypeConversionCostTblEntry SSE41ConversionTbl[] = {
    { ISD::ZERO_EXTEND, MVT::v4i64, MVT::v4i8,    2 },
    { ISD::SIGN_EXTEND, MVT::v4i64, MVT::v4i8,    2 },
    { ISD::ZERO_EXTEND, MVT::v4i64, MVT::v4i16,   2 },
    { ISD::SIGN_EXTEND, MVT::v4i64, MVT::v4i16,   2 },
    { ISD::ZERO_EXTEND, MVT::v4i64, MVT::v4i32,   2 },
    { ISD::SIGN_EXTEND, MVT::v4i64, MVT::v4i32,   2 },

    { ISD::ZERO_EXTEND, MVT::v4i16,  MVT::v4i8,   1 },
    { ISD::SIGN_EXTEND, MVT::v4i16,  MVT::v4i8,   2 },
    { ISD::ZERO_EXTEND, MVT::v4i32,  MVT::v4i8,   1 },
    { ISD::SIGN_EXTEND, MVT::v4i32,  MVT::v4i8,   1 },
    { ISD::ZERO_EXTEND, MVT::v8i16,  MVT::v8i8,   1 },
    { ISD::SIGN_EXTEND, MVT::v8i16,  MVT::v8i8,   1 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i8,   2 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i8,   2 },
    { ISD::ZERO_EXTEND, MVT::v16i16, MVT::v16i8,  2 },
    { ISD::SIGN_EXTEND, MVT::v16i16, MVT::v16i8,  2 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i8,  4 },
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i8,  4 },
    { ISD::ZERO_EXTEND, MVT::v4i32,  MVT::v4i16,  1 },
    { ISD::SIGN_EXTEND, MVT::v4i32,  MVT::v4i16,  1 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i16,  2 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i16,  2 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i16, 4 },
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i16, 4 },

    { ISD::TRUNCATE,    MVT::v4i8,   MVT::v4i16,  2 },
    { ISD::TRUNCATE,    MVT::v8i8,   MVT::v8i16,  1 },
    { ISD::TRUNCATE,    MVT::v4i8,   MVT::v4i32,  1 },
    { ISD::TRUNCATE,    MVT::v4i16,  MVT::v4i32,  1 },
    { ISD::TRUNCATE,    MVT::v8i8,   MVT::v8i32,  3 },
    { ISD::TRUNCATE,    MVT::v8i16,  MVT::v8i32,  3 },
    { ISD::TRUNCATE,    MVT::v16i16, MVT::v16i32, 6 },

  };

  static const TypeConversionCostTblEntry SSE2ConversionTbl[] = {
    // These are somewhat magic numbers justified by looking at the output of
    // Intel's IACA, running some kernels and making sure when we take
    // legalization into account the throughput will be overestimated.
    { ISD::SINT_TO_FP, MVT::v4f32, MVT::v16i8, 8 },
    { ISD::SINT_TO_FP, MVT::v2f64, MVT::v16i8, 16*10 },
    { ISD::SINT_TO_FP, MVT::v4f32, MVT::v8i16, 15 },
    { ISD::SINT_TO_FP, MVT::v2f64, MVT::v8i16, 8*10 },
    { ISD::SINT_TO_FP, MVT::v4f32, MVT::v4i32, 5 },
    { ISD::SINT_TO_FP, MVT::v2f64, MVT::v4i32, 4*10 },
    { ISD::SINT_TO_FP, MVT::v4f32, MVT::v2i64, 15 },
    { ISD::SINT_TO_FP, MVT::v2f64, MVT::v2i64, 2*10 },

    { ISD::UINT_TO_FP, MVT::v2f64, MVT::v16i8, 16*10 },
    { ISD::UINT_TO_FP, MVT::v4f32, MVT::v16i8, 8 },
    { ISD::UINT_TO_FP, MVT::v4f32, MVT::v8i16, 15 },
    { ISD::UINT_TO_FP, MVT::v2f64, MVT::v8i16, 8*10 },
    { ISD::UINT_TO_FP, MVT::v2f64, MVT::v4i32, 4*10 },
    { ISD::UINT_TO_FP, MVT::v4f32, MVT::v4i32, 8 },
    { ISD::UINT_TO_FP, MVT::v2f64, MVT::v2i64, 2*10 },
    { ISD::UINT_TO_FP, MVT::v4f32, MVT::v2i64, 15 },

    { ISD::FP_TO_SINT,  MVT::v2i32,  MVT::v2f64,  3 },

    { ISD::ZERO_EXTEND, MVT::v4i16,  MVT::v4i8,   1 },
    { ISD::SIGN_EXTEND, MVT::v4i16,  MVT::v4i8,   6 },
    { ISD::ZERO_EXTEND, MVT::v4i32,  MVT::v4i8,   2 },
    { ISD::SIGN_EXTEND, MVT::v4i32,  MVT::v4i8,   3 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i8,   4 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i8,   8 },
    { ISD::ZERO_EXTEND, MVT::v8i16,  MVT::v8i8,   1 },
    { ISD::SIGN_EXTEND, MVT::v8i16,  MVT::v8i8,   2 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i8,   6 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i8,   6 },
    { ISD::ZERO_EXTEND, MVT::v16i16, MVT::v16i8,  3 },
    { ISD::SIGN_EXTEND, MVT::v16i16, MVT::v16i8,  4 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i8,  9 },
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i8,  12 },
    { ISD::ZERO_EXTEND, MVT::v4i32,  MVT::v4i16,  1 },
    { ISD::SIGN_EXTEND, MVT::v4i32,  MVT::v4i16,  2 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i16,  3 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i16,  10 },
    { ISD::ZERO_EXTEND, MVT::v8i32,  MVT::v8i16,  3 },
    { ISD::SIGN_EXTEND, MVT::v8i32,  MVT::v8i16,  4 },
    { ISD::ZERO_EXTEND, MVT::v16i32, MVT::v16i16, 6 },
    { ISD::SIGN_EXTEND, MVT::v16i32, MVT::v16i16, 8 },
    { ISD::ZERO_EXTEND, MVT::v4i64,  MVT::v4i32,  3 },
    { ISD::SIGN_EXTEND, MVT::v4i64,  MVT::v4i32,  5 },

    { ISD::TRUNCATE,    MVT::v4i8,   MVT::v4i16,  4 },
    { ISD::TRUNCATE,    MVT::v8i8,   MVT::v8i16,  2 },
    { ISD::TRUNCATE,    MVT::v16i8,  MVT::v16i16, 3 },
    { ISD::TRUNCATE,    MVT::v4i8,   MVT::v4i32,  3 },
    { ISD::TRUNCATE,    MVT::v4i16,  MVT::v4i32,  3 },
    { ISD::TRUNCATE,    MVT::v8i8,   MVT::v8i32,  4 },
    { ISD::TRUNCATE,    MVT::v16i8,  MVT::v16i32, 7 },
    { ISD::TRUNCATE,    MVT::v8i16,  MVT::v8i32,  5 },
    { ISD::TRUNCATE,    MVT::v16i16, MVT::v16i32, 10 },
  };

  std::pair<int, MVT> LTSrc = TLI->getTypeLegalizationCost(DL, Src);
  std::pair<int, MVT> LTDest = TLI->getTypeLegalizationCost(DL, Dst);

  if (ST->hasSSE2() && !ST->hasAVX()) {
    if (const auto *Entry = ConvertCostTableLookup(SSE2ConversionTbl, ISD,
                                                   LTDest.second, LTSrc.second))
      return LTSrc.first * Entry->Cost;
  }

  EVT SrcTy = TLI->getValueType(DL, Src);
  EVT DstTy = TLI->getValueType(DL, Dst);

  // The function getSimpleVT only handles simple value types.
  if (!SrcTy.isSimple() || !DstTy.isSimple())
    return BaseT::getCastInstrCost(Opcode, Dst, Src);

  if (ST->hasDQI())
    if (const auto *Entry = ConvertCostTableLookup(AVX512DQConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;

  if (ST->hasAVX512())
    if (const auto *Entry = ConvertCostTableLookup(AVX512FConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;

  if (ST->hasAVX2()) {
    if (const auto *Entry = ConvertCostTableLookup(AVX2ConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  if (ST->hasAVX()) {
    if (const auto *Entry = ConvertCostTableLookup(AVXConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  if (ST->hasSSE41()) {
    if (const auto *Entry = ConvertCostTableLookup(SSE41ConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  if (ST->hasSSE2()) {
    if (const auto *Entry = ConvertCostTableLookup(SSE2ConversionTbl, ISD,
                                                   DstTy.getSimpleVT(),
                                                   SrcTy.getSimpleVT()))
      return Entry->Cost;
  }

  return BaseT::getCastInstrCost(Opcode, Dst, Src);
}

int X86TTIImpl::getCmpSelInstrCost(unsigned Opcode, Type *ValTy, Type *CondTy) {
  // Legalize the type.
  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, ValTy);

  MVT MTy = LT.second;

  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  static const CostTblEntry SSE2CostTbl[] = {
    { ISD::SETCC,   MVT::v2i64,   8 },
    { ISD::SETCC,   MVT::v4i32,   1 },
    { ISD::SETCC,   MVT::v8i16,   1 },
    { ISD::SETCC,   MVT::v16i8,   1 },
  };

  static const CostTblEntry SSE42CostTbl[] = {
    { ISD::SETCC,   MVT::v2f64,   1 },
    { ISD::SETCC,   MVT::v4f32,   1 },
    { ISD::SETCC,   MVT::v2i64,   1 },
  };

  static const CostTblEntry AVX1CostTbl[] = {
    { ISD::SETCC,   MVT::v4f64,   1 },
    { ISD::SETCC,   MVT::v8f32,   1 },
    // AVX1 does not support 8-wide integer compare.
    { ISD::SETCC,   MVT::v4i64,   4 },
    { ISD::SETCC,   MVT::v8i32,   4 },
    { ISD::SETCC,   MVT::v16i16,  4 },
    { ISD::SETCC,   MVT::v32i8,   4 },
  };

  static const CostTblEntry AVX2CostTbl[] = {
    { ISD::SETCC,   MVT::v4i64,   1 },
    { ISD::SETCC,   MVT::v8i32,   1 },
    { ISD::SETCC,   MVT::v16i16,  1 },
    { ISD::SETCC,   MVT::v32i8,   1 },
  };

  static const CostTblEntry AVX512CostTbl[] = {
    { ISD::SETCC,   MVT::v8i64,   1 },
    { ISD::SETCC,   MVT::v16i32,  1 },
    { ISD::SETCC,   MVT::v8f64,   1 },
    { ISD::SETCC,   MVT::v16f32,  1 },
  };

  if (ST->hasAVX512())
    if (const auto *Entry = CostTableLookup(AVX512CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasAVX2())
    if (const auto *Entry = CostTableLookup(AVX2CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasAVX())
    if (const auto *Entry = CostTableLookup(AVX1CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasSSE42())
    if (const auto *Entry = CostTableLookup(SSE42CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasSSE2())
    if (const auto *Entry = CostTableLookup(SSE2CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  return BaseT::getCmpSelInstrCost(Opcode, ValTy, CondTy);
}

int X86TTIImpl::getIntrinsicInstrCost(Intrinsic::ID IID, Type *RetTy,
                                      ArrayRef<Type *> Tys, FastMathFlags FMF) {
  // Costs should match the codegen from:
  // BITREVERSE: llvm\test\CodeGen\X86\vector-bitreverse.ll
  // BSWAP: llvm\test\CodeGen\X86\bswap-vector.ll
  // CTLZ: llvm\test\CodeGen\X86\vector-lzcnt-*.ll
  // CTPOP: llvm\test\CodeGen\X86\vector-popcnt-*.ll
  // CTTZ: llvm\test\CodeGen\X86\vector-tzcnt-*.ll
  static const CostTblEntry XOPCostTbl[] = {
    { ISD::BITREVERSE, MVT::v4i64,   4 },
    { ISD::BITREVERSE, MVT::v8i32,   4 },
    { ISD::BITREVERSE, MVT::v16i16,  4 },
    { ISD::BITREVERSE, MVT::v32i8,   4 },
    { ISD::BITREVERSE, MVT::v2i64,   1 },
    { ISD::BITREVERSE, MVT::v4i32,   1 },
    { ISD::BITREVERSE, MVT::v8i16,   1 },
    { ISD::BITREVERSE, MVT::v16i8,   1 },
    { ISD::BITREVERSE, MVT::i64,     3 },
    { ISD::BITREVERSE, MVT::i32,     3 },
    { ISD::BITREVERSE, MVT::i16,     3 },
    { ISD::BITREVERSE, MVT::i8,      3 }
  };
  static const CostTblEntry AVX2CostTbl[] = {
    { ISD::BITREVERSE, MVT::v4i64,   5 },
    { ISD::BITREVERSE, MVT::v8i32,   5 },
    { ISD::BITREVERSE, MVT::v16i16,  5 },
    { ISD::BITREVERSE, MVT::v32i8,   5 },
    { ISD::BSWAP,      MVT::v4i64,   1 },
    { ISD::BSWAP,      MVT::v8i32,   1 },
    { ISD::BSWAP,      MVT::v16i16,  1 },
    { ISD::CTLZ,       MVT::v4i64,  23 },
    { ISD::CTLZ,       MVT::v8i32,  18 },
    { ISD::CTLZ,       MVT::v16i16, 14 },
    { ISD::CTLZ,       MVT::v32i8,   9 },
    { ISD::CTPOP,      MVT::v4i64,   7 },
    { ISD::CTPOP,      MVT::v8i32,  11 },
    { ISD::CTPOP,      MVT::v16i16,  9 },
    { ISD::CTPOP,      MVT::v32i8,   6 },
    { ISD::CTTZ,       MVT::v4i64,  10 },
    { ISD::CTTZ,       MVT::v8i32,  14 },
    { ISD::CTTZ,       MVT::v16i16, 12 },
    { ISD::CTTZ,       MVT::v32i8,   9 },
    { ISD::FSQRT,      MVT::f32,     7 }, // Haswell from http://www.agner.org/
    { ISD::FSQRT,      MVT::v4f32,   7 }, // Haswell from http://www.agner.org/
    { ISD::FSQRT,      MVT::v8f32,  14 }, // Haswell from http://www.agner.org/
    { ISD::FSQRT,      MVT::f64,    14 }, // Haswell from http://www.agner.org/
    { ISD::FSQRT,      MVT::v2f64,  14 }, // Haswell from http://www.agner.org/
    { ISD::FSQRT,      MVT::v4f64,  28 }, // Haswell from http://www.agner.org/
  };
  static const CostTblEntry AVX1CostTbl[] = {
    { ISD::BITREVERSE, MVT::v4i64,  10 },
    { ISD::BITREVERSE, MVT::v8i32,  10 },
    { ISD::BITREVERSE, MVT::v16i16, 10 },
    { ISD::BITREVERSE, MVT::v32i8,  10 },
    { ISD::BSWAP,      MVT::v4i64,   4 },
    { ISD::BSWAP,      MVT::v8i32,   4 },
    { ISD::BSWAP,      MVT::v16i16,  4 },
    { ISD::CTLZ,       MVT::v4i64,  46 },
    { ISD::CTLZ,       MVT::v8i32,  36 },
    { ISD::CTLZ,       MVT::v16i16, 28 },
    { ISD::CTLZ,       MVT::v32i8,  18 },
    { ISD::CTPOP,      MVT::v4i64,  14 },
    { ISD::CTPOP,      MVT::v8i32,  22 },
    { ISD::CTPOP,      MVT::v16i16, 18 },
    { ISD::CTPOP,      MVT::v32i8,  12 },
    { ISD::CTTZ,       MVT::v4i64,  20 },
    { ISD::CTTZ,       MVT::v8i32,  28 },
    { ISD::CTTZ,       MVT::v16i16, 24 },
    { ISD::CTTZ,       MVT::v32i8,  18 },
    { ISD::FSQRT,      MVT::f32,    14 }, // SNB from http://www.agner.org/
    { ISD::FSQRT,      MVT::v4f32,  14 }, // SNB from http://www.agner.org/
    { ISD::FSQRT,      MVT::v8f32,  28 }, // SNB from http://www.agner.org/
    { ISD::FSQRT,      MVT::f64,    21 }, // SNB from http://www.agner.org/
    { ISD::FSQRT,      MVT::v2f64,  21 }, // SNB from http://www.agner.org/
    { ISD::FSQRT,      MVT::v4f64,  43 }, // SNB from http://www.agner.org/
  };
  static const CostTblEntry SSE42CostTbl[] = {
    { ISD::FSQRT, MVT::f32,   18 }, // Nehalem from http://www.agner.org/
    { ISD::FSQRT, MVT::v4f32, 18 }, // Nehalem from http://www.agner.org/
  };
  static const CostTblEntry SSSE3CostTbl[] = {
    { ISD::BITREVERSE, MVT::v2i64,   5 },
    { ISD::BITREVERSE, MVT::v4i32,   5 },
    { ISD::BITREVERSE, MVT::v8i16,   5 },
    { ISD::BITREVERSE, MVT::v16i8,   5 },
    { ISD::BSWAP,      MVT::v2i64,   1 },
    { ISD::BSWAP,      MVT::v4i32,   1 },
    { ISD::BSWAP,      MVT::v8i16,   1 },
    { ISD::CTLZ,       MVT::v2i64,  23 },
    { ISD::CTLZ,       MVT::v4i32,  18 },
    { ISD::CTLZ,       MVT::v8i16,  14 },
    { ISD::CTLZ,       MVT::v16i8,   9 },
    { ISD::CTPOP,      MVT::v2i64,   7 },
    { ISD::CTPOP,      MVT::v4i32,  11 },
    { ISD::CTPOP,      MVT::v8i16,   9 },
    { ISD::CTPOP,      MVT::v16i8,   6 },
    { ISD::CTTZ,       MVT::v2i64,  10 },
    { ISD::CTTZ,       MVT::v4i32,  14 },
    { ISD::CTTZ,       MVT::v8i16,  12 },
    { ISD::CTTZ,       MVT::v16i8,   9 }
  };
  static const CostTblEntry SSE2CostTbl[] = {
    { ISD::BSWAP,      MVT::v2i64,   7 },
    { ISD::BSWAP,      MVT::v4i32,   7 },
    { ISD::BSWAP,      MVT::v8i16,   7 },
    { ISD::CTLZ,       MVT::v2i64,  25 },
    { ISD::CTLZ,       MVT::v4i32,  26 },
    { ISD::CTLZ,       MVT::v8i16,  20 },
    { ISD::CTLZ,       MVT::v16i8,  17 },
    { ISD::CTPOP,      MVT::v2i64,  12 },
    { ISD::CTPOP,      MVT::v4i32,  15 },
    { ISD::CTPOP,      MVT::v8i16,  13 },
    { ISD::CTPOP,      MVT::v16i8,  10 },
    { ISD::CTTZ,       MVT::v2i64,  14 },
    { ISD::CTTZ,       MVT::v4i32,  18 },
    { ISD::CTTZ,       MVT::v8i16,  16 },
    { ISD::CTTZ,       MVT::v16i8,  13 },
    { ISD::FSQRT,      MVT::f64,    32 }, // Nehalem from http://www.agner.org/
    { ISD::FSQRT,      MVT::v2f64,  32 }, // Nehalem from http://www.agner.org/
  };
  static const CostTblEntry SSE1CostTbl[] = {
    { ISD::FSQRT, MVT::f32,   28 }, // Pentium III from http://www.agner.org/
    { ISD::FSQRT, MVT::v4f32, 56 }, // Pentium III from http://www.agner.org/
  };

  unsigned ISD = ISD::DELETED_NODE;
  switch (IID) {
  default:
    break;
  case Intrinsic::bitreverse:
    ISD = ISD::BITREVERSE;
    break;
  case Intrinsic::bswap:
    ISD = ISD::BSWAP;
    break;
  case Intrinsic::ctlz:
    ISD = ISD::CTLZ;
    break;
  case Intrinsic::ctpop:
    ISD = ISD::CTPOP;
    break;
  case Intrinsic::cttz:
    ISD = ISD::CTTZ;
    break;
  case Intrinsic::sqrt:
    ISD = ISD::FSQRT;
    break;
  }

  // Legalize the type.
  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, RetTy);
  MVT MTy = LT.second;

  // Attempt to lookup cost.
  if (ST->hasXOP())
    if (const auto *Entry = CostTableLookup(XOPCostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasAVX2())
    if (const auto *Entry = CostTableLookup(AVX2CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasAVX())
    if (const auto *Entry = CostTableLookup(AVX1CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasSSE42())
    if (const auto *Entry = CostTableLookup(SSE42CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasSSSE3())
    if (const auto *Entry = CostTableLookup(SSSE3CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasSSE2())
    if (const auto *Entry = CostTableLookup(SSE2CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  if (ST->hasSSE1())
    if (const auto *Entry = CostTableLookup(SSE1CostTbl, ISD, MTy))
      return LT.first * Entry->Cost;

  return BaseT::getIntrinsicInstrCost(IID, RetTy, Tys, FMF);
}

int X86TTIImpl::getIntrinsicInstrCost(Intrinsic::ID IID, Type *RetTy,
                                      ArrayRef<Value *> Args, FastMathFlags FMF) {
  return BaseT::getIntrinsicInstrCost(IID, RetTy, Args, FMF);
}

int X86TTIImpl::getVectorInstrCost(unsigned Opcode, Type *Val, unsigned Index) {
  assert(Val->isVectorTy() && "This must be a vector type");

  Type *ScalarType = Val->getScalarType();

  if (Index != -1U) {
    // Legalize the type.
    std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Val);

    // This type is legalized to a scalar type.
    if (!LT.second.isVector())
      return 0;

    // The type may be split. Normalize the index to the new type.
    unsigned Width = LT.second.getVectorNumElements();
    Index = Index % Width;

    // Floating point scalars are already located in index #0.
    if (ScalarType->isFloatingPointTy() && Index == 0)
      return 0;
  }

  // Add to the base cost if we know that the extracted element of a vector is
  // destined to be moved to and used in the integer register file.
  int RegisterFileMoveCost = 0;
  if (Opcode == Instruction::ExtractElement && ScalarType->isPointerTy())
    RegisterFileMoveCost = 1;

  return BaseT::getVectorInstrCost(Opcode, Val, Index) + RegisterFileMoveCost;
}

int X86TTIImpl::getScalarizationOverhead(Type *Ty, bool Insert, bool Extract) {
  assert (Ty->isVectorTy() && "Can only scalarize vectors");
  int Cost = 0;

  for (int i = 0, e = Ty->getVectorNumElements(); i < e; ++i) {
    if (Insert)
      Cost += getVectorInstrCost(Instruction::InsertElement, Ty, i);
    if (Extract)
      Cost += getVectorInstrCost(Instruction::ExtractElement, Ty, i);
  }

  return Cost;
}

int X86TTIImpl::getMemoryOpCost(unsigned Opcode, Type *Src, unsigned Alignment,
                                unsigned AddressSpace) {
  // Handle non-power-of-two vectors such as <3 x float>
  if (VectorType *VTy = dyn_cast<VectorType>(Src)) {
    unsigned NumElem = VTy->getVectorNumElements();

    // Handle a few common cases:
    // <3 x float>
    if (NumElem == 3 && VTy->getScalarSizeInBits() == 32)
      // Cost = 64 bit store + extract + 32 bit store.
      return 3;

    // <3 x double>
    if (NumElem == 3 && VTy->getScalarSizeInBits() == 64)
      // Cost = 128 bit store + unpack + 64 bit store.
      return 3;

    // Assume that all other non-power-of-two numbers are scalarized.
    if (!isPowerOf2_32(NumElem)) {
      int Cost = BaseT::getMemoryOpCost(Opcode, VTy->getScalarType(), Alignment,
                                        AddressSpace);
      int SplitCost = getScalarizationOverhead(Src, Opcode == Instruction::Load,
                                               Opcode == Instruction::Store);
      return NumElem * Cost + SplitCost;
    }
  }

  // Legalize the type.
  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, Src);
  assert((Opcode == Instruction::Load || Opcode == Instruction::Store) &&
         "Invalid Opcode");

  // Each load/store unit costs 1.
  int Cost = LT.first * 1;

  // This isn't exactly right. We're using slow unaligned 32-byte accesses as a
  // proxy for a double-pumped AVX memory interface such as on Sandybridge.
  if (LT.second.getStoreSize() == 32 && ST->isUnalignedMem32Slow())
    Cost *= 2;

  return Cost;
}

int X86TTIImpl::getMaskedMemoryOpCost(unsigned Opcode, Type *SrcTy,
                                      unsigned Alignment,
                                      unsigned AddressSpace) {
  VectorType *SrcVTy = dyn_cast<VectorType>(SrcTy);
  if (!SrcVTy)
    // To calculate scalar take the regular cost, without mask
    return getMemoryOpCost(Opcode, SrcTy, Alignment, AddressSpace);

  unsigned NumElem = SrcVTy->getVectorNumElements();
  VectorType *MaskTy =
    VectorType::get(Type::getInt8Ty(SrcVTy->getContext()), NumElem);
  if ((Opcode == Instruction::Load && !isLegalMaskedLoad(SrcVTy)) ||
      (Opcode == Instruction::Store && !isLegalMaskedStore(SrcVTy)) ||
      !isPowerOf2_32(NumElem)) {
    // Scalarization
    int MaskSplitCost = getScalarizationOverhead(MaskTy, false, true);
    int ScalarCompareCost = getCmpSelInstrCost(
        Instruction::ICmp, Type::getInt8Ty(SrcVTy->getContext()), nullptr);
    int BranchCost = getCFInstrCost(Instruction::Br);
    int MaskCmpCost = NumElem * (BranchCost + ScalarCompareCost);

    int ValueSplitCost = getScalarizationOverhead(
        SrcVTy, Opcode == Instruction::Load, Opcode == Instruction::Store);
    int MemopCost =
        NumElem * BaseT::getMemoryOpCost(Opcode, SrcVTy->getScalarType(),
                                         Alignment, AddressSpace);
    return MemopCost + ValueSplitCost + MaskSplitCost + MaskCmpCost;
  }

  // Legalize the type.
  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, SrcVTy);
  auto VT = TLI->getValueType(DL, SrcVTy);
  int Cost = 0;
  if (VT.isSimple() && LT.second != VT.getSimpleVT() &&
      LT.second.getVectorNumElements() == NumElem)
    // Promotion requires expand/truncate for data and a shuffle for mask.
    Cost += getShuffleCost(TTI::SK_Alternate, SrcVTy, 0, nullptr) +
            getShuffleCost(TTI::SK_Alternate, MaskTy, 0, nullptr);

  else if (LT.second.getVectorNumElements() > NumElem) {
    VectorType *NewMaskTy = VectorType::get(MaskTy->getVectorElementType(),
                                            LT.second.getVectorNumElements());
    // Expanding requires fill mask with zeroes
    Cost += getShuffleCost(TTI::SK_InsertSubvector, NewMaskTy, 0, MaskTy);
  }
  if (!ST->hasAVX512())
    return Cost + LT.first*4; // Each maskmov costs 4

  // AVX-512 masked load/store is cheapper
  return Cost+LT.first;
}

int X86TTIImpl::getAddressComputationCost(Type *Ty, bool IsComplex) {
  // Address computations in vectorized code with non-consecutive addresses will
  // likely result in more instructions compared to scalar code where the
  // computation can more often be merged into the index mode. The resulting
  // extra micro-ops can significantly decrease throughput.
  unsigned NumVectorInstToHideOverhead = 10;

  if (Ty->isVectorTy() && IsComplex)
    return NumVectorInstToHideOverhead;

  return BaseT::getAddressComputationCost(Ty, IsComplex);
}

int X86TTIImpl::getReductionCost(unsigned Opcode, Type *ValTy,
                                 bool IsPairwise) {

  std::pair<int, MVT> LT = TLI->getTypeLegalizationCost(DL, ValTy);

  MVT MTy = LT.second;

  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  // We use the Intel Architecture Code Analyzer(IACA) to measure the throughput
  // and make it as the cost.

  static const CostTblEntry SSE42CostTblPairWise[] = {
    { ISD::FADD,  MVT::v2f64,   2 },
    { ISD::FADD,  MVT::v4f32,   4 },
    { ISD::ADD,   MVT::v2i64,   2 },      // The data reported by the IACA tool is "1.6".
    { ISD::ADD,   MVT::v4i32,   3 },      // The data reported by the IACA tool is "3.5".
    { ISD::ADD,   MVT::v8i16,   5 },
  };

  static const CostTblEntry AVX1CostTblPairWise[] = {
    { ISD::FADD,  MVT::v4f32,   4 },
    { ISD::FADD,  MVT::v4f64,   5 },
    { ISD::FADD,  MVT::v8f32,   7 },
    { ISD::ADD,   MVT::v2i64,   1 },      // The data reported by the IACA tool is "1.5".
    { ISD::ADD,   MVT::v4i32,   3 },      // The data reported by the IACA tool is "3.5".
    { ISD::ADD,   MVT::v4i64,   5 },      // The data reported by the IACA tool is "4.8".
    { ISD::ADD,   MVT::v8i16,   5 },
    { ISD::ADD,   MVT::v8i32,   5 },
  };

  static const CostTblEntry SSE42CostTblNoPairWise[] = {
    { ISD::FADD,  MVT::v2f64,   2 },
    { ISD::FADD,  MVT::v4f32,   4 },
    { ISD::ADD,   MVT::v2i64,   2 },      // The data reported by the IACA tool is "1.6".
    { ISD::ADD,   MVT::v4i32,   3 },      // The data reported by the IACA tool is "3.3".
    { ISD::ADD,   MVT::v8i16,   4 },      // The data reported by the IACA tool is "4.3".
  };

  static const CostTblEntry AVX1CostTblNoPairWise[] = {
    { ISD::FADD,  MVT::v4f32,   3 },
    { ISD::FADD,  MVT::v4f64,   3 },
    { ISD::FADD,  MVT::v8f32,   4 },
    { ISD::ADD,   MVT::v2i64,   1 },      // The data reported by the IACA tool is "1.5".
    { ISD::ADD,   MVT::v4i32,   3 },      // The data reported by the IACA tool is "2.8".
    { ISD::ADD,   MVT::v4i64,   3 },
    { ISD::ADD,   MVT::v8i16,   4 },
    { ISD::ADD,   MVT::v8i32,   5 },
  };

  if (IsPairwise) {
    if (ST->hasAVX())
      if (const auto *Entry = CostTableLookup(AVX1CostTblPairWise, ISD, MTy))
        return LT.first * Entry->Cost;

    if (ST->hasSSE42())
      if (const auto *Entry = CostTableLookup(SSE42CostTblPairWise, ISD, MTy))
        return LT.first * Entry->Cost;
  } else {
    if (ST->hasAVX())
      if (const auto *Entry = CostTableLookup(AVX1CostTblNoPairWise, ISD, MTy))
        return LT.first * Entry->Cost;

    if (ST->hasSSE42())
      if (const auto *Entry = CostTableLookup(SSE42CostTblNoPairWise, ISD, MTy))
        return LT.first * Entry->Cost;
  }

  return BaseT::getReductionCost(Opcode, ValTy, IsPairwise);
}

/// \brief Calculate the cost of materializing a 64-bit value. This helper
/// method might only calculate a fraction of a larger immediate. Therefore it
/// is valid to return a cost of ZERO.
int X86TTIImpl::getIntImmCost(int64_t Val) {
  if (Val == 0)
    return TTI::TCC_Free;

  if (isInt<32>(Val))
    return TTI::TCC_Basic;

  return 2 * TTI::TCC_Basic;
}

int X86TTIImpl::getIntImmCost(const APInt &Imm, Type *Ty) {
  assert(Ty->isIntegerTy());

  unsigned BitSize = Ty->getPrimitiveSizeInBits();
  if (BitSize == 0)
    return ~0U;

  // Never hoist constants larger than 128bit, because this might lead to
  // incorrect code generation or assertions in codegen.
  // Fixme: Create a cost model for types larger than i128 once the codegen
  // issues have been fixed.
  if (BitSize > 128)
    return TTI::TCC_Free;

  if (Imm == 0)
    return TTI::TCC_Free;

  // Sign-extend all constants to a multiple of 64-bit.
  APInt ImmVal = Imm;
  if (BitSize & 0x3f)
    ImmVal = Imm.sext((BitSize + 63) & ~0x3fU);

  // Split the constant into 64-bit chunks and calculate the cost for each
  // chunk.
  int Cost = 0;
  for (unsigned ShiftVal = 0; ShiftVal < BitSize; ShiftVal += 64) {
    APInt Tmp = ImmVal.ashr(ShiftVal).sextOrTrunc(64);
    int64_t Val = Tmp.getSExtValue();
    Cost += getIntImmCost(Val);
  }
  // We need at least one instruction to materialize the constant.
  return std::max(1, Cost);
}

int X86TTIImpl::getIntImmCost(unsigned Opcode, unsigned Idx, const APInt &Imm,
                              Type *Ty) {
  assert(Ty->isIntegerTy());

  unsigned BitSize = Ty->getPrimitiveSizeInBits();
  // There is no cost model for constants with a bit size of 0. Return TCC_Free
  // here, so that constant hoisting will ignore this constant.
  if (BitSize == 0)
    return TTI::TCC_Free;

  unsigned ImmIdx = ~0U;
  switch (Opcode) {
  default:
    return TTI::TCC_Free;
  case Instruction::GetElementPtr:
    // Always hoist the base address of a GetElementPtr. This prevents the
    // creation of new constants for every base constant that gets constant
    // folded with the offset.
    if (Idx == 0)
      return 2 * TTI::TCC_Basic;
    return TTI::TCC_Free;
  case Instruction::Store:
    ImmIdx = 0;
    break;
  case Instruction::ICmp:
    // This is an imperfect hack to prevent constant hoisting of
    // compares that might be trying to check if a 64-bit value fits in
    // 32-bits. The backend can optimize these cases using a right shift by 32.
    // Ideally we would check the compare predicate here. There also other
    // similar immediates the backend can use shifts for.
    if (Idx == 1 && Imm.getBitWidth() == 64) {
      uint64_t ImmVal = Imm.getZExtValue();
      if (ImmVal == 0x100000000ULL || ImmVal == 0xffffffff)
        return TTI::TCC_Free;
    }
    ImmIdx = 1;
    break;
  case Instruction::And:
    // We support 64-bit ANDs with immediates with 32-bits of leading zeroes
    // by using a 32-bit operation with implicit zero extension. Detect such
    // immediates here as the normal path expects bit 31 to be sign extended.
    if (Idx == 1 && Imm.getBitWidth() == 64 && isUInt<32>(Imm.getZExtValue()))
      return TTI::TCC_Free;
    LLVM_FALLTHROUGH;
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::Or:
  case Instruction::Xor:
    ImmIdx = 1;
    break;
  // Always return TCC_Free for the shift value of a shift instruction.
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    if (Idx == 1)
      return TTI::TCC_Free;
    break;
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::IntToPtr:
  case Instruction::PtrToInt:
  case Instruction::BitCast:
  case Instruction::PHI:
  case Instruction::Call:
  case Instruction::Select:
  case Instruction::Ret:
  case Instruction::Load:
    break;
  }

  if (Idx == ImmIdx) {
    int NumConstants = (BitSize + 63) / 64;
    int Cost = X86TTIImpl::getIntImmCost(Imm, Ty);
    return (Cost <= NumConstants * TTI::TCC_Basic)
               ? static_cast<int>(TTI::TCC_Free)
               : Cost;
  }

  return X86TTIImpl::getIntImmCost(Imm, Ty);
}

int X86TTIImpl::getIntImmCost(Intrinsic::ID IID, unsigned Idx, const APInt &Imm,
                              Type *Ty) {
  assert(Ty->isIntegerTy());

  unsigned BitSize = Ty->getPrimitiveSizeInBits();
  // There is no cost model for constants with a bit size of 0. Return TCC_Free
  // here, so that constant hoisting will ignore this constant.
  if (BitSize == 0)
    return TTI::TCC_Free;

  switch (IID) {
  default:
    return TTI::TCC_Free;
  case Intrinsic::sadd_with_overflow:
  case Intrinsic::uadd_with_overflow:
  case Intrinsic::ssub_with_overflow:
  case Intrinsic::usub_with_overflow:
  case Intrinsic::smul_with_overflow:
  case Intrinsic::umul_with_overflow:
    if ((Idx == 1) && Imm.getBitWidth() <= 64 && isInt<32>(Imm.getSExtValue()))
      return TTI::TCC_Free;
    break;
  case Intrinsic::experimental_stackmap:
    if ((Idx < 2) || (Imm.getBitWidth() <= 64 && isInt<64>(Imm.getSExtValue())))
      return TTI::TCC_Free;
    break;
  case Intrinsic::experimental_patchpoint_void:
  case Intrinsic::experimental_patchpoint_i64:
    if ((Idx < 4) || (Imm.getBitWidth() <= 64 && isInt<64>(Imm.getSExtValue())))
      return TTI::TCC_Free;
    break;
  }
  return X86TTIImpl::getIntImmCost(Imm, Ty);
}

// Return an average cost of Gather / Scatter instruction, maybe improved later
int X86TTIImpl::getGSVectorCost(unsigned Opcode, Type *SrcVTy, Value *Ptr,
                                unsigned Alignment, unsigned AddressSpace) {

  assert(isa<VectorType>(SrcVTy) && "Unexpected type in getGSVectorCost");
  unsigned VF = SrcVTy->getVectorNumElements();

  // Try to reduce index size from 64 bit (default for GEP)
  // to 32. It is essential for VF 16. If the index can't be reduced to 32, the
  // operation will use 16 x 64 indices which do not fit in a zmm and needs
  // to split. Also check that the base pointer is the same for all lanes,
  // and that there's at most one variable index.
  auto getIndexSizeInBits = [](Value *Ptr, const DataLayout& DL) {
    unsigned IndexSize = DL.getPointerSizeInBits();
    GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Ptr);
    if (IndexSize < 64 || !GEP)
      return IndexSize;

    unsigned NumOfVarIndices = 0;
    Value *Ptrs = GEP->getPointerOperand();
    if (Ptrs->getType()->isVectorTy() && !getSplatValue(Ptrs))
      return IndexSize;
    for (unsigned i = 1; i < GEP->getNumOperands(); ++i) {
      if (isa<Constant>(GEP->getOperand(i)))
        continue;
      Type *IndxTy = GEP->getOperand(i)->getType();
      if (IndxTy->isVectorTy())
        IndxTy = IndxTy->getVectorElementType();
      if ((IndxTy->getPrimitiveSizeInBits() == 64 &&
          !isa<SExtInst>(GEP->getOperand(i))) ||
         ++NumOfVarIndices > 1)
        return IndexSize; // 64
    }
    return (unsigned)32;
  };


  // Trying to reduce IndexSize to 32 bits for vector 16.
  // By default the IndexSize is equal to pointer size.
  unsigned IndexSize = (VF >= 16) ? getIndexSizeInBits(Ptr, DL) :
    DL.getPointerSizeInBits();

  Type *IndexVTy = VectorType::get(IntegerType::get(SrcVTy->getContext(),
                                                    IndexSize), VF);
  std::pair<int, MVT> IdxsLT = TLI->getTypeLegalizationCost(DL, IndexVTy);
  std::pair<int, MVT> SrcLT = TLI->getTypeLegalizationCost(DL, SrcVTy);
  int SplitFactor = std::max(IdxsLT.first, SrcLT.first);
  if (SplitFactor > 1) {
    // Handle splitting of vector of pointers
    Type *SplitSrcTy = VectorType::get(SrcVTy->getScalarType(), VF / SplitFactor);
    return SplitFactor * getGSVectorCost(Opcode, SplitSrcTy, Ptr, Alignment,
                                         AddressSpace);
  }

  // The gather / scatter cost is given by Intel architects. It is a rough
  // number since we are looking at one instruction in a time.
  const int GSOverhead = 2;
  return GSOverhead + VF * getMemoryOpCost(Opcode, SrcVTy->getScalarType(),
                                           Alignment, AddressSpace);
}

/// Return the cost of full scalarization of gather / scatter operation.
///
/// Opcode - Load or Store instruction.
/// SrcVTy - The type of the data vector that should be gathered or scattered.
/// VariableMask - The mask is non-constant at compile time.
/// Alignment - Alignment for one element.
/// AddressSpace - pointer[s] address space.
///
int X86TTIImpl::getGSScalarCost(unsigned Opcode, Type *SrcVTy,
                                bool VariableMask, unsigned Alignment,
                                unsigned AddressSpace) {
  unsigned VF = SrcVTy->getVectorNumElements();

  int MaskUnpackCost = 0;
  if (VariableMask) {
    VectorType *MaskTy =
      VectorType::get(Type::getInt1Ty(SrcVTy->getContext()), VF);
    MaskUnpackCost = getScalarizationOverhead(MaskTy, false, true);
    int ScalarCompareCost =
      getCmpSelInstrCost(Instruction::ICmp, Type::getInt1Ty(SrcVTy->getContext()),
                         nullptr);
    int BranchCost = getCFInstrCost(Instruction::Br);
    MaskUnpackCost += VF * (BranchCost + ScalarCompareCost);
  }

  // The cost of the scalar loads/stores.
  int MemoryOpCost = VF * getMemoryOpCost(Opcode, SrcVTy->getScalarType(),
                                          Alignment, AddressSpace);

  int InsertExtractCost = 0;
  if (Opcode == Instruction::Load)
    for (unsigned i = 0; i < VF; ++i)
      // Add the cost of inserting each scalar load into the vector
      InsertExtractCost +=
        getVectorInstrCost(Instruction::InsertElement, SrcVTy, i);
  else
    for (unsigned i = 0; i < VF; ++i)
      // Add the cost of extracting each element out of the data vector
      InsertExtractCost +=
        getVectorInstrCost(Instruction::ExtractElement, SrcVTy, i);

  return MemoryOpCost + MaskUnpackCost + InsertExtractCost;
}

/// Calculate the cost of Gather / Scatter operation
int X86TTIImpl::getGatherScatterOpCost(unsigned Opcode, Type *SrcVTy,
                                       Value *Ptr, bool VariableMask,
                                       unsigned Alignment) {
  assert(SrcVTy->isVectorTy() && "Unexpected data type for Gather/Scatter");
  unsigned VF = SrcVTy->getVectorNumElements();
  PointerType *PtrTy = dyn_cast<PointerType>(Ptr->getType());
  if (!PtrTy && Ptr->getType()->isVectorTy())
    PtrTy = dyn_cast<PointerType>(Ptr->getType()->getVectorElementType());
  assert(PtrTy && "Unexpected type for Ptr argument");
  unsigned AddressSpace = PtrTy->getAddressSpace();

  bool Scalarize = false;
  if ((Opcode == Instruction::Load && !isLegalMaskedGather(SrcVTy)) ||
      (Opcode == Instruction::Store && !isLegalMaskedScatter(SrcVTy)))
    Scalarize = true;
  // Gather / Scatter for vector 2 is not profitable on KNL / SKX
  // Vector-4 of gather/scatter instruction does not exist on KNL.
  // We can extend it to 8 elements, but zeroing upper bits of
  // the mask vector will add more instructions. Right now we give the scalar
  // cost of vector-4 for KNL. TODO: Check, maybe the gather/scatter instruction is
  // better in the VariableMask case.
  if (VF == 2 || (VF == 4 && !ST->hasVLX()))
    Scalarize = true;

  if (Scalarize)
    return getGSScalarCost(Opcode, SrcVTy, VariableMask, Alignment, AddressSpace);

  return getGSVectorCost(Opcode, SrcVTy, Ptr, Alignment, AddressSpace);
}

bool X86TTIImpl::isLegalMaskedLoad(Type *DataTy) {
  Type *ScalarTy = DataTy->getScalarType();
  int DataWidth = isa<PointerType>(ScalarTy) ?
    DL.getPointerSizeInBits() : ScalarTy->getPrimitiveSizeInBits();

  return ((DataWidth == 32 || DataWidth == 64) && ST->hasAVX()) ||
         ((DataWidth == 8 || DataWidth == 16) && ST->hasBWI());
}

bool X86TTIImpl::isLegalMaskedStore(Type *DataType) {
  return isLegalMaskedLoad(DataType);
}

bool X86TTIImpl::isLegalMaskedGather(Type *DataTy) {
  // This function is called now in two cases: from the Loop Vectorizer
  // and from the Scalarizer.
  // When the Loop Vectorizer asks about legality of the feature,
  // the vectorization factor is not calculated yet. The Loop Vectorizer
  // sends a scalar type and the decision is based on the width of the
  // scalar element.
  // Later on, the cost model will estimate usage this intrinsic based on
  // the vector type.
  // The Scalarizer asks again about legality. It sends a vector type.
  // In this case we can reject non-power-of-2 vectors.
  if (isa<VectorType>(DataTy) && !isPowerOf2_32(DataTy->getVectorNumElements()))
    return false;
  Type *ScalarTy = DataTy->getScalarType();
  int DataWidth = isa<PointerType>(ScalarTy) ?
    DL.getPointerSizeInBits() : ScalarTy->getPrimitiveSizeInBits();

  // AVX-512 allows gather and scatter
  return (DataWidth == 32 || DataWidth == 64) && ST->hasAVX512();
}

bool X86TTIImpl::isLegalMaskedScatter(Type *DataType) {
  return isLegalMaskedGather(DataType);
}

bool X86TTIImpl::areInlineCompatible(const Function *Caller,
                                     const Function *Callee) const {
  const TargetMachine &TM = getTLI()->getTargetMachine();

  // Work this as a subsetting of subtarget features.
  const FeatureBitset &CallerBits =
      TM.getSubtargetImpl(*Caller)->getFeatureBits();
  const FeatureBitset &CalleeBits =
      TM.getSubtargetImpl(*Callee)->getFeatureBits();

  // FIXME: This is likely too limiting as it will include subtarget features
  // that we might not care about for inlining, but it is conservatively
  // correct.
  return (CallerBits & CalleeBits) == CalleeBits;
}

bool X86TTIImpl::enableInterleavedAccessVectorization() {
  // TODO: We expect this to be beneficial regardless of arch,
  // but there are currently some unexplained performance artifacts on Atom.
  // As a temporary solution, disable on Atom.
  return !(ST->isAtom() || ST->isSLM());
}
