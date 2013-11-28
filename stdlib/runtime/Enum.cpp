//===--- Enum.cpp - Runtime declarations for enums -----------*- C++ -*--===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Swift runtime functions in support of enums.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MathExtras.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Enum.h"
#include <cstring>
#include <algorithm>

using namespace swift;

// FIXME: We should cache this in the enum's metadata.
static unsigned getNumTagBytes(size_t size, unsigned cases) {
  // We can use the payload area with a tag bit set somewhere outside of the
  // payload area to represent cases. See how many bytes we need to cover
  // all the empty cases.

  if (size >= 4)
    // Assume that one tag bit is enough if the precise calculation overflows
    // an int32.
    return 1;
  else {
    unsigned bits = size * 8U;
    unsigned casesPerTagBitValue = 1U << bits;
    unsigned numTagBitValues
      = 1 + ((cases + (casesPerTagBitValue-1U)) >> bits);
    return (numTagBitValues < 256 ? 1 :
            numTagBitValues < 65536 ? 2 : 4);
  }
}

void
swift::swift_initEnumValueWitnessTableSinglePayload(ValueWitnessTable *vwtable,
                                                     const Metadata *payload,
                                                     unsigned emptyCases) {
  auto *payloadWitnesses = payload->getValueWitnesses();
  size_t payloadSize = payloadWitnesses->getSize();
  unsigned payloadNumExtraInhabitants
    = payloadWitnesses->getNumExtraInhabitants();
  
  unsigned unusedExtraInhabitants = 0;
  
  // If there are enough extra inhabitants for all of the cases, then the size
  // of the enum is the same as its payload.
  size_t size;
  if (payloadNumExtraInhabitants >= emptyCases) {
    size = payloadSize;
    unusedExtraInhabitants = payloadNumExtraInhabitants - emptyCases;
  } else {
    size = payloadSize + getNumTagBytes(payloadSize,
                                      emptyCases - payloadNumExtraInhabitants);
  }

  vwtable->size = size;
  vwtable->flags = payloadWitnesses->flags
    .withExtraInhabitants(unusedExtraInhabitants > 0);
  vwtable->stride = llvm::RoundUpToAlignment(size,
                                             payloadWitnesses->getAlignment());
  
  // If the payload has extra inhabitants left over after the ones we used,
  // forward them as our own.
  if (unusedExtraInhabitants > 0) {
    auto xiVWTable = static_cast<ExtraInhabitantsValueWitnessTable*>(vwtable);
    xiVWTable->extraInhabitantFlags = ExtraInhabitantFlags()
      .withNumExtraInhabitants(unusedExtraInhabitants);
  }
}

int
swift::swift_getEnumCaseSinglePayload(const OpaqueValue *value,
                                       const Metadata *payload,
                                       unsigned emptyCases) {
  auto *payloadWitnesses = payload->getValueWitnesses();
  auto payloadSize = payloadWitnesses->getSize();
  auto payloadNumExtraInhabitants = payloadWitnesses->getNumExtraInhabitants();

  // If there are extra tag bits, check them.
  if (emptyCases > payloadNumExtraInhabitants) {
    auto *valueAddr = reinterpret_cast<const uint8_t*>(value);
    auto *extraTagBitAddr = valueAddr + payloadSize;
    unsigned extraTagBits = 0;
    // FIXME: endianness
    memcpy(&extraTagBits, extraTagBitAddr,
           getNumTagBytes(payloadSize, emptyCases-payloadNumExtraInhabitants));
    
    // If the extra tag bits are zero, we have a valid payload or
    // extra inhabitant (checked below). If nonzero, form the case index from
    // the extra tag value and the value stored in the payload.
    if (extraTagBits > 0) {
      unsigned caseIndexFromExtraTagBits = payloadSize >= 4
        ? 0 : (extraTagBits - 1U) << (payloadSize*8U);
      
      // In practice we should need no more than four bytes from the payload
      // area.
      // FIXME: endianness.
      unsigned caseIndexFromValue = 0;
      memcpy(&caseIndexFromValue, valueAddr,
             std::min(size_t(4), payloadSize));
      return (caseIndexFromExtraTagBits | caseIndexFromValue)
        + payloadNumExtraInhabitants;
    }
  }
  
  // If there are extra inhabitants, see whether the payload is valid.
  if (payloadNumExtraInhabitants > 0) {
    return
      static_cast<const ExtraInhabitantsValueWitnessTable*>(payloadWitnesses)
        ->getExtraInhabitantIndex(value, payload);
  }
  
  // Otherwise, we have always have a valid payload.
  return -1;
}

void
swift::swift_storeEnumTagSinglePayload(OpaqueValue *value,
                                        const Metadata *payload,
                                        int whichCase, unsigned emptyCases) {
  auto *payloadWitnesses = payload->getValueWitnesses();
  auto payloadSize = payloadWitnesses->getSize();
  unsigned payloadNumExtraInhabitants
    = payloadWitnesses->getNumExtraInhabitants();

  auto *valueAddr = reinterpret_cast<uint8_t*>(value);
  auto *extraTagBitAddr = valueAddr + payloadSize;
  unsigned numExtraTagBytes = emptyCases > payloadNumExtraInhabitants
    ? getNumTagBytes(payloadSize, emptyCases - payloadNumExtraInhabitants)
    : 0;

  // For payload or extra inhabitant cases, zero-initialize the extra tag bits,
  // if any.
  if (whichCase < (int)payloadNumExtraInhabitants) {
    memset(extraTagBitAddr, 0, numExtraTagBytes);
    
    // If this is the payload case, we're done.
    if (whichCase == -1)
      return;
    
    // Store the extra inhabitant.
    static_cast<const ExtraInhabitantsValueWitnessTable*>(payloadWitnesses)
      ->storeExtraInhabitant(value, whichCase, payload);
    return;
  }
  
  // Factor the case index into payload and extra tag parts.
  unsigned caseIndex = whichCase - payloadNumExtraInhabitants;
  unsigned payloadIndex, extraTagIndex;
  if (payloadSize >= 4) {
    extraTagIndex = 1;
    payloadIndex = caseIndex;
  } else {
    unsigned payloadBits = payloadSize * 8U;
    extraTagIndex = 1U + (caseIndex >> payloadBits);
    payloadIndex = caseIndex & ((1U << payloadBits) - 1U);
  }
  
  // Store into the value.
  // FIXME: endianness.
  memcpy(valueAddr, &payloadIndex, std::min(size_t(4), payloadSize));
  if (payloadSize > 4)
    memset(valueAddr + 4, 0, payloadSize - 4);
  memcpy(extraTagBitAddr, &extraTagIndex, numExtraTagBytes);
}
