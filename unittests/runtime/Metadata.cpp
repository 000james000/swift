//===- swift/unittests/runtime/Metadata.cpp - Metadata tests --------------===//
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

#include "swift/Runtime/Metadata.h"
#include "gtest/gtest.h"
#include <vector>

using namespace swift;

/// Some unique global pointers.
char Global1 = 0;
char Global2 = 0;
char Global3 = 0;

/// The general structure of a generic metadata.
template <unsigned NumFields>
struct GenericMetadataTest {
  GenericMetadata Header;
  void *Fields[NumFields];
};

GenericMetadataTest<3> MetadataTest1 = {
  // Header
  {
    // allocation function
    [](GenericMetadata *pattern, const void *args) {
      auto metadata = swift_allocateGenericValueMetadata(pattern, args);
      auto metadataWords = reinterpret_cast<const void**>(metadata);
      auto argsWords = reinterpret_cast<const void* const*>(args);
      metadataWords[2] = argsWords[0];
      return metadata;
    },
    3 * sizeof(void*), // metadata size
    1, // num arguments
    0, // address point
    {} // private data
  },

  // Fields
  {
    (void*) MetadataKind::Struct,
    &Global1,
    nullptr
  }
};

TEST(MetadataTest, getGenericMetadata) {
  auto metadataTemplate = (GenericMetadata*) &MetadataTest1;

  void *args[] = { &Global2 };

  auto inst1a = swift_getGenericMetadata(metadataTemplate, args);
  auto inst1b = swift_getGenericMetadata(metadataTemplate, args);
  ASSERT_EQ(inst1a, inst1b);

  void * const *fields = reinterpret_cast<void * const *>(inst1a);
  ASSERT_EQ((void*) MetadataKind::Struct, fields[0]);
  ASSERT_EQ(&Global1, fields[1]);
  ASSERT_EQ(&Global2, fields[2]);

  args[0] = &Global3;
  auto inst2a = swift_getGenericMetadata(metadataTemplate, args);
  auto inst2b = swift_getGenericMetadata(metadataTemplate, args);
  ASSERT_EQ(inst2a, inst2b);
  ASSERT_NE(inst1a, inst2a);

  fields = reinterpret_cast<void * const *>(inst2a);
  ASSERT_EQ((void*) MetadataKind::Struct, fields[0]);
  ASSERT_EQ(&Global1, fields[1]);
  ASSERT_EQ(&Global3, fields[2]);
}

FullMetadata<ClassMetadata> MetadataTest2 = {
  { { nullptr }, { &_TWVBo } },
  { { { MetadataKind::Class } }, nullptr, 0, ClassFlags(), nullptr, 0, 0, 0, 0, 0 }
};

TEST(MetadataTest, getMetatypeMetadata) {
  auto inst1a = swift_getMetatypeMetadata(&_TMdBi64_.base);
  auto inst1b = swift_getMetatypeMetadata(&_TMdBi64_.base);
  ASSERT_EQ(inst1a, inst1b);

  auto inst2a = swift_getMetatypeMetadata(&_TMdBi32_.base);
  auto inst2b = swift_getMetatypeMetadata(&_TMdBi32_.base);
  ASSERT_EQ(inst2a, inst2b);

  // Both of these are trivial metatypes.
  ASSERT_EQ(size_t(0), inst1a->getValueWitnesses()->size);
  ASSERT_EQ(size_t(0), inst2a->getValueWitnesses()->size);

  auto inst3a = swift_getMetatypeMetadata(&MetadataTest2);
  auto inst3b = swift_getMetatypeMetadata(&MetadataTest2);
  ASSERT_EQ(inst3a, inst3b);

  // The representation here should be non-trivial.
  ASSERT_EQ(sizeof(void*), inst3a->getValueWitnesses()->size);

  // Going out another level of abstraction on the class metatype
  // should leave us with another non-trivial metatype.
  auto inst4a = swift_getMetatypeMetadata(inst3a);
  auto inst4b = swift_getMetatypeMetadata(inst3a);
  ASSERT_EQ(inst4a, inst4b);
  ASSERT_EQ(sizeof(void*), inst4a->getValueWitnesses()->size);

  // Similarly, going out a level of abstraction on a trivial
  // metatype should give us a trivial metatype.
  auto inst5a = swift_getMetatypeMetadata(inst1a);
  auto inst5b = swift_getMetatypeMetadata(inst1a);
  ASSERT_EQ(inst5a, inst5b);
  ASSERT_EQ(size_t(0), inst5a->getValueWitnesses()->size);

  // After all this, the instance type fields should still be valid.
  ASSERT_EQ(&_TMdBi64_.base, inst1a->InstanceType);
  ASSERT_EQ(&_TMdBi32_.base, inst2a->InstanceType);
  ASSERT_EQ(&MetadataTest2, inst3a->InstanceType);
  ASSERT_EQ(inst3a, inst4a->InstanceType);
  ASSERT_EQ(inst1a, inst5a->InstanceType);
}

ProtocolDescriptor ProtocolA{
  "_TMp8Metadata9ProtocolA",
  nullptr,
  ProtocolDescriptorFlags()
    .withSwift(true)
    .withClassConstraint(ProtocolClassConstraint::Any)
    .withNeedsWitnessTable(true)
};

ProtocolDescriptor ProtocolB{
  "_TMp8Metadata9ProtocolB",
  nullptr,
  ProtocolDescriptorFlags()
    .withSwift(true)
    .withClassConstraint(ProtocolClassConstraint::Any)
    .withNeedsWitnessTable(true)
};

ProtocolDescriptor ProtocolClassConstrained{
  "_TMp8Metadata24ProtocolClassConstrained",
  nullptr,
  ProtocolDescriptorFlags()
    .withSwift(true)
    .withClassConstraint(ProtocolClassConstraint::Class)
    .withNeedsWitnessTable(true)
};

ProtocolDescriptor ProtocolNoWitnessTable{
  "_TMp8Metadata22ProtocolNoWitnessTable",
  nullptr,
  ProtocolDescriptorFlags()
    .withSwift(true)
    .withClassConstraint(ProtocolClassConstraint::Class)
    .withNeedsWitnessTable(false)
};

static const ExistentialTypeMetadata *test_getExistentialMetadata(
                  std::initializer_list<const ProtocolDescriptor *> descriptors)
{
  std::vector<const ProtocolDescriptor *> mutDescriptors(descriptors);

  return swift_getExistentialTypeMetadata(mutDescriptors.size(),
                                          mutDescriptors.data());
}

TEST(MetadataTest, getExistentialMetadata) {
  {
    auto any1 = test_getExistentialMetadata({});
    auto any2 = test_getExistentialMetadata({});
    ASSERT_EQ(any1, any2);
    ASSERT_EQ(MetadataKind::Existential, any1->getKind());
    ASSERT_EQ(0U, any1->Flags.getNumWitnessTables());
    ASSERT_EQ(ProtocolClassConstraint::Any, any1->Flags.getClassConstraint());
    ASSERT_EQ(0U, any1->Protocols.NumProtocols);
  }
  {
    auto a1 = test_getExistentialMetadata({&ProtocolA});
    auto a2 = test_getExistentialMetadata({&ProtocolA});
    ASSERT_EQ(a1, a2);
    ASSERT_EQ(MetadataKind::Existential, a1->getKind());
    ASSERT_EQ(1U, a1->Flags.getNumWitnessTables());
    ASSERT_EQ(ProtocolClassConstraint::Any, a1->Flags.getClassConstraint());
    ASSERT_EQ(1U, a1->Protocols.NumProtocols);
    ASSERT_EQ(&ProtocolA, a1->Protocols[0]);
   
   
    auto b = test_getExistentialMetadata({&ProtocolB});
    ASSERT_NE(a1, b);
    ASSERT_EQ(MetadataKind::Existential, b->getKind());
    ASSERT_EQ(1U, b->Flags.getNumWitnessTables());
    ASSERT_EQ(ProtocolClassConstraint::Any, b->Flags.getClassConstraint());
    ASSERT_EQ(1U, b->Protocols.NumProtocols);
    ASSERT_EQ(&ProtocolB, b->Protocols[0]);
  }

  // protocol compositions are order-invariant
  {
    auto ab = test_getExistentialMetadata({&ProtocolA, &ProtocolB});
    auto ba = test_getExistentialMetadata({&ProtocolB, &ProtocolA});
    ASSERT_EQ(ab, ba);
    ASSERT_EQ(MetadataKind::Existential, ab->getKind());
    ASSERT_EQ(2U, ab->Flags.getNumWitnessTables());
    ASSERT_EQ(ProtocolClassConstraint::Any, ab->Flags.getClassConstraint());
    ASSERT_EQ(2U, ab->Protocols.NumProtocols);
    ASSERT_TRUE((ab->Protocols[0]==&ProtocolA && ab->Protocols[1]==&ProtocolB)
             || (ab->Protocols[0]==&ProtocolB && ab->Protocols[1]==&ProtocolA));
  }

  {
    auto classConstrained
      = test_getExistentialMetadata({&ProtocolClassConstrained});
    ASSERT_EQ(MetadataKind::Existential, classConstrained->getKind());
    ASSERT_EQ(1U, classConstrained->Flags.getNumWitnessTables());
    ASSERT_EQ(ProtocolClassConstraint::Class,
              classConstrained->Flags.getClassConstraint());
    ASSERT_EQ(1U, classConstrained->Protocols.NumProtocols);
    ASSERT_EQ(&ProtocolClassConstrained, classConstrained->Protocols[0]);
  }
  {
    auto noWitnessTable
      = test_getExistentialMetadata({&ProtocolNoWitnessTable});
    ASSERT_EQ(MetadataKind::Existential, noWitnessTable->getKind());
    ASSERT_EQ(0U, noWitnessTable->Flags.getNumWitnessTables());
    ASSERT_EQ(ProtocolClassConstraint::Class,
              noWitnessTable->Flags.getClassConstraint());
    ASSERT_EQ(1U, noWitnessTable->Protocols.NumProtocols);
    ASSERT_EQ(&ProtocolNoWitnessTable, noWitnessTable->Protocols[0]);
  }

  {
    auto mixedWitnessTable
      = test_getExistentialMetadata({&ProtocolNoWitnessTable,
                                     &ProtocolA, &ProtocolB});
    ASSERT_EQ(MetadataKind::Existential, mixedWitnessTable->getKind());
    ASSERT_EQ(2U, mixedWitnessTable->Flags.getNumWitnessTables());
    ASSERT_EQ(ProtocolClassConstraint::Class,
              mixedWitnessTable->Flags.getClassConstraint());
    ASSERT_EQ(3U, mixedWitnessTable->Protocols.NumProtocols);
  }
}
