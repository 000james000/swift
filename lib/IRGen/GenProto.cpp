//===--- GenProto.cpp - Swift IR Generation for Protocols -----------------===//
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
//  This file implements IR generation for protocols in Swift.
//
//  Protocols serve two masters: generic algorithms and existential
//  types.  In either case, the size and structure of a type is opaque
//  to the code manipulating a value.  Local values of the type must
//  be stored in fixed-size buffers (which can overflow to use heap
//  allocation), and basic operations on the type must be dynamically
//  delegated to a collection of information that "witnesses" the
//  truth that a particular type implements the protocol.
//
//  In the comments throughout this file, three type names are used:
//    'B' is the type of a fixed-size buffer
//    'T' is the type which implements a protocol
//    'W' is the type of a witness to the protocol
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILValue.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include "CallEmission.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "FunctionRef.h"
#include "GenClass.h"
#include "GenHeap.h"
#include "GenMeta.h"
#include "GenOpaque.h"
#include "GenPoly.h"
#include "GenType.h"
#include "HeapTypeInfo.h"
#include "IndirectTypeInfo.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Linking.h"
#include "NecessaryBindings.h"
#include "NonFixedTypeInfo.h"
#include "ProtocolInfo.h"
#include "TypeInfo.h"
#include "UnownedTypeInfo.h"
#include "WeakTypeInfo.h"

#include "GenProto.h"

using namespace swift;
using namespace irgen;

namespace {
  /// The layout of an existential buffer.  This is intended to be a
  /// small, easily-computed type that can be passed around by value.
  class OpaqueExistentialLayout {
  private:
    unsigned NumTables;
    // If you add anything to the layout computation, you might need
    // to update certain uses;  check the external uses of getNumTables().
    // For example, getAssignExistentialsFunction relies on being uniqued
    // for different layout kinds.

  public:
    explicit OpaqueExistentialLayout(unsigned numTables)
      : NumTables(numTables) {}

    unsigned getNumTables() const { return NumTables; }

    /*
    friend bool operator==(ExistentialLayout a, ExistentialLayout b) {
      return a.NumTables == b.NumTables;
    }*/

    /// Given the offset of the buffer within an existential type.
    Size getBufferOffset(IRGenModule &IGM) const {
      return IGM.getPointerSize() * (NumTables + 1);
    }

    /// Given the address of an existential object, drill down to the
    /// buffer.
    Address projectExistentialBuffer(IRGenFunction &IGF, Address addr) const {
      return IGF.Builder.CreateStructGEP(addr, getNumTables() + 1,
                                         getBufferOffset(IGF.IGM));
    }
    
    /// Given the address of an existential object, drill down to the
    /// witness-table field.
    Address projectWitnessTable(IRGenFunction &IGF, Address addr,
                                unsigned which) const {
      assert(which < getNumTables());
      return IGF.Builder.CreateStructGEP(addr, which + 1,
                                         IGF.IGM.getPointerSize() * (which + 1));
    }

    /// Given the address of an existential object, load its witness table.
    llvm::Value *loadWitnessTable(IRGenFunction &IGF, Address addr,
                                  unsigned which) const {
      return IGF.Builder.CreateLoad(projectWitnessTable(IGF, addr, which),
                                    "witness-table");
    }
    
    /// Given the address of an existential object, drill down to the
    /// metadata field.
    Address projectMetadataRef(IRGenFunction &IGF, Address addr) {
      return IGF.Builder.CreateStructGEP(addr, 0, Size(0));
    }

    /// Given the address of an existential object, load its metadata
    /// object.
    llvm::Value *loadMetadataRef(IRGenFunction &IGF, Address addr) {
      return IGF.Builder.CreateLoad(projectMetadataRef(IGF, addr),
                               addr.getAddress()->getName() + ".metadata");
    }
  };
  
  /// A concrete witness table, together with its known layout.
  class WitnessTable {
    llvm::Value *Table;
    const ProtocolInfo &Info;
  public:
    WitnessTable(llvm::Value *wtable, const ProtocolInfo &info)
      : Table(wtable), Info(info) {}

    llvm::Value *getTable() const { return Table; }
    const ProtocolInfo &getInfo() const { return Info; }
  };
}

/// Given the address of an existential object, destroy it.
static void emitDestroyExistential(IRGenFunction &IGF, Address addr,
                                   OpaqueExistentialLayout layout) {
  llvm::Value *metadata = layout.loadMetadataRef(IGF, addr);

  Address object = layout.projectExistentialBuffer(IGF, addr);
  emitDestroyBufferCall(IGF, metadata, object);
}

static llvm::Constant *getAssignExistentialsFunction(IRGenModule &IGM,
                                               llvm::Type *objectPtrTy,
                                               OpaqueExistentialLayout layout);

namespace {

  /// A CRTP class for visiting the witnesses of a protocol.
  ///
  /// The design here is that each entry (or small group of entries)
  /// gets turned into a call to the implementation class describing
  /// the exact variant of witness.  For example, for member
  /// variables, there should be separate callbacks for adding a
  /// getter/setter pair, for just adding a getter, and for adding a
  /// physical projection (if we decide to support that).
  template <class T> class WitnessVisitor {
  protected:
    IRGenModule &IGM;

    WitnessVisitor(IRGenModule &IGM) : IGM(IGM) {}

  public:
    void visit(ProtocolDecl *protocol) {
      // Visit inherited protocols.
      // TODO: We need to figure out all the guarantees we want here.
      // It would be abstractly good to allow conversion to a base
      // protocol to be trivial, but it's not clear that there's
      // really a structural guarantee we can rely on here.
      for (auto baseProto : protocol->getProtocols()) {
        // ObjC protocols do not have witnesses.
        if (!requiresProtocolWitnessTable(baseProto))
          continue;

        asDerived().addOutOfLineBaseProtocol(baseProto);
      }

      visitMembers(protocol->getMembers());
    }

  private:
    T &asDerived() { return *static_cast<T*>(this); }

    /// Visit the witnesses for the direct members of a protocol.
    void visitMembers(ArrayRef<Decl*> members) {
      for (Decl *member : members) {
        visitMember(member);
      }
    }

    void visitMember(Decl *member) {
      switch (member->getKind()) {
      case DeclKind::Import:
      case DeclKind::Extension:
      case DeclKind::TopLevelCode:
      case DeclKind::Enum:
      case DeclKind::Struct:
      case DeclKind::Class:
      case DeclKind::Protocol:
      case DeclKind::EnumCase:
      case DeclKind::EnumElement:
      case DeclKind::Constructor:
      case DeclKind::Destructor:
      case DeclKind::InfixOperator:
      case DeclKind::PrefixOperator:
      case DeclKind::PostfixOperator:
      case DeclKind::TypeAlias:
      case DeclKind::GenericTypeParam:
        llvm_unreachable("declaration not legal as a protocol member");

      case DeclKind::PatternBinding:
        // We only care about the var decls in the pattern binding.
        return;

      case DeclKind::Func:
        return visitFunc(cast<FuncDecl>(member));

      case DeclKind::Subscript:
      case DeclKind::Var:
        // FIXME: To be implemented.
        return;

      case DeclKind::AssociatedType:
        return visitAssociatedType(cast<AssociatedTypeDecl>(member));
      }
      llvm_unreachable("bad decl kind");
    }

    void visitFunc(FuncDecl *func) {
      if (func->isAccessor())
        // FIXME: To be implemented.
        return;
      
      if (func->isStatic()) {
        asDerived().addStaticMethod(func);
      } else {
        asDerived().addInstanceMethod(func);
      }
    }
    
    void visitAssociatedType(AssociatedTypeDecl *ty) {
      asDerived().addAssociatedType(ty);
    }
  };

  /// A class which lays out a witness table in the abstract.
  class WitnessTableLayout : public WitnessVisitor<WitnessTableLayout> {
    unsigned NumWitnesses;
    SmallVector<WitnessTableEntry, 16> Entries;

    WitnessIndex getNextIndex() {
      return WitnessIndex(NumWitnesses++, /*isPrefix=*/false);
    }

  public:
    WitnessTableLayout(IRGenModule &IGM)
      : WitnessVisitor(IGM), NumWitnesses(0) {}

    /// The next witness is an out-of-line base protocol.
    void addOutOfLineBaseProtocol(ProtocolDecl *baseProto) {
      Entries.push_back(
             WitnessTableEntry::forOutOfLineBase(baseProto, getNextIndex()));
    }

    void addStaticMethod(FuncDecl *func) {
      Entries.push_back(WitnessTableEntry::forFunction(func, getNextIndex()));
    }

    void addInstanceMethod(FuncDecl *func) {
      Entries.push_back(WitnessTableEntry::forFunction(func, getNextIndex()));
    }
    
    void addAssociatedType(AssociatedTypeDecl *ty) {
      // An associated type takes up a spot for the type metadata and for the
      // witnesses to all its conformances.
      Entries.push_back(
                      WitnessTableEntry::forAssociatedType(ty, getNextIndex()));
      NumWitnesses += ty->getProtocols().size();
    }

    unsigned getNumWitnesses() const { return NumWitnesses; }
    ArrayRef<WitnessTableEntry> getEntries() const { return Entries; }
  };

  /// A path through a protocol hierarchy.
  class ProtocolPath {
    IRGenModule &IGM;

    /// The destination protocol. 
    ProtocolDecl *Dest;

    /// The path from the selected origin down to the destination
    /// protocol.
    SmallVector<WitnessIndex, 8> ReversePath;

    /// The origin index to use.
    unsigned OriginIndex;

    /// The best path length we found.
    unsigned BestPathLength;

  public:
    /// Find a path from the given set of origins to the destination
    /// protocol.
    ///
    /// T needs to provide a couple of member functions:
    ///   ProtocolDecl *getProtocol() const;
    ///   const ProtocolInfo &getInfo() const;
    template <class T>
    ProtocolPath(IRGenModule &IGM, ArrayRef<T> origins, ProtocolDecl *dest)
      : IGM(IGM), Dest(dest), BestPathLength(~0U) {

      // Consider each of the origins in turn, breaking out if any of
      // them yields a zero-length path.
      for (unsigned i = 0, e = origins.size(); i != e; ++i) {
        auto &origin = origins[i];
        if (considerOrigin(origin.getProtocol(), origin.getInfo(), i))
          break;
      }

      // Sanity check that we actually found a path at all.
      assert(BestPathLength != ~0U);
      assert(BestPathLength == ReversePath.size());
    }

    /// Returns the index of the origin protocol we chose.
    unsigned getOriginIndex() const { return OriginIndex; }

    /// Apply the path to the given witness table.
    llvm::Value *apply(IRGenFunction &IGF, llvm::Value *wtable) const {
      for (unsigned i = ReversePath.size(); i != 0; --i) {
        wtable = emitLoadOfOpaqueWitness(IGF, wtable, ReversePath[i-1]);
        wtable = IGF.Builder.CreateBitCast(wtable, IGF.IGM.WitnessTablePtrTy);
      }
      return wtable;
    }

  private:
    /// Consider paths starting from a new origin protocol.
    /// Returns true if there's no point in considering other origins.
    bool considerOrigin(ProtocolDecl *origin, const ProtocolInfo &originInfo,
                        unsigned originIndex) {
      assert(BestPathLength != 0);

      // If the origin *is* the destination, we can stop here.
      if (origin == Dest) {
        OriginIndex = originIndex;
        BestPathLength = 0;
        ReversePath.clear();
        return true;
      }

      // Otherwise, if the origin gives rise to a better path, that's
      // also cool.
      if (findBetterPath(origin, originInfo, 0)) {
        OriginIndex = originIndex;
        return BestPathLength == 0;
      }

      return false;
    }

    /// Consider paths starting at the given protocol.
    bool findBetterPath(ProtocolDecl *proto, const ProtocolInfo &protoInfo,
                        unsigned lengthSoFar) {
      assert(lengthSoFar < BestPathLength);
      assert(proto != Dest);

      // Keep track of whether we found a better path than the
      // previous best.
      bool foundBetter = false;
      for (auto base : proto->getProtocols()) {
        auto &baseEntry = protoInfo.getWitnessEntry(base);
        assert(baseEntry.isBase());

        // Compute the length down to this base.
        unsigned lengthToBase = lengthSoFar;
        if (baseEntry.isOutOfLineBase()) {
          lengthToBase++;

          // Don't consider this path if we reach a length that can't
          // possibly be better than the best so far.
          if (lengthToBase == BestPathLength) continue;
        }
        assert(lengthToBase < BestPathLength);

        // If this base *is* the destination, go ahead and start
        // building the path into ReversePath.
        if (base == Dest) {
          // Reset the collected best-path information.
          BestPathLength = lengthToBase;
          ReversePath.clear();

        // Otherwise, if there isn't a better path through this base,
        // don't accumulate anything in the path.
        } else if (!findBetterPath(base, IGM.getProtocolInfo(base),
                                   lengthToBase)) {
          continue;
        }

        // Okay, we've found a better path, and ReversePath contains a
        // path leading from base to Dest.
        assert(BestPathLength >= lengthToBase);
        foundBetter = true;

        // Add the link from proto to base if necessary.
        if (baseEntry.isOutOfLineBase()) {
          ReversePath.push_back(baseEntry.getOutOfLineBaseIndex());

        // If it isn't necessary, then we might be able to
        // short-circuit considering the bases of this protocol.
        } else {
          if (lengthSoFar == BestPathLength)
            return true;
        }
      }

      return foundBetter;
    }
  };

  /// An entry in an existential type's list of known protocols.
  class ProtocolEntry {
    ProtocolDecl *Protocol;
    const ProtocolInfo &Impl;

  public:
    explicit ProtocolEntry(ProtocolDecl *proto, const ProtocolInfo &impl)
      : Protocol(proto), Impl(impl) {}

    ProtocolDecl *getProtocol() const { return Protocol; }
    const ProtocolInfo &getInfo() const { return Impl; }
  };
  
  /// A TypeInfo implementation for existential types, i.e., types like:
  ///   Printable
  ///   protocol<Printable, Serializable>
  /// with the semantic translation:
  ///   \exists t : Printable . t
  /// t here is an ArchetypeType.
  ///
  /// This is used for both ProtocolTypes and ProtocolCompositionTypes.
  class OpaqueExistentialTypeInfo :
      public IndirectTypeInfo<OpaqueExistentialTypeInfo, FixedTypeInfo> {
    unsigned NumProtocols;

    ProtocolEntry *getProtocolsBuffer() {
      return reinterpret_cast<ProtocolEntry *>(this + 1);
    }
    const ProtocolEntry *getProtocolsBuffer() const {
      return reinterpret_cast<const ProtocolEntry *>(this + 1);
    }

    // FIXME: We could get spare bits out of the metadata and/or witness
    // pointers.
    OpaqueExistentialTypeInfo(llvm::Type *ty, Size size, Alignment align,
                        ArrayRef<ProtocolEntry> protocols)
      : IndirectTypeInfo(ty, size, llvm::BitVector{}, align, IsNotPOD),
        NumProtocols(protocols.size()) {

      for (unsigned i = 0; i != NumProtocols; ++i) {
        new (&getProtocolsBuffer()[i]) ProtocolEntry(protocols[i]);
      }
    }

  public:
    OpaqueExistentialLayout getLayout() const {
      return OpaqueExistentialLayout(NumProtocols);
    }

    static const OpaqueExistentialTypeInfo *create(llvm::Type *ty, Size size,
                                          Alignment align,
                                          ArrayRef<ProtocolEntry> protocols) {
      void *buffer = operator new(sizeof(OpaqueExistentialTypeInfo) +
                                  protocols.size() * sizeof(ProtocolEntry));
      return new(buffer) OpaqueExistentialTypeInfo(ty, size, align, protocols);
    }

    /// Returns the protocols that values of this type are known to
    /// implement.  This can be empty, meaning that values of this
    /// type are not know to implement any protocols, although we do
    /// still know how to manipulate them.
    ArrayRef<ProtocolEntry> getProtocols() const {
      return ArrayRef<ProtocolEntry>(getProtocolsBuffer(), NumProtocols);
    }

    /// Given an existential object, find the witness table
    /// corresponding to the given protocol.
    llvm::Value *findWitnessTable(IRGenFunction &IGF, Address obj,
                                  ProtocolDecl *protocol) const {
      assert(NumProtocols != 0 &&
             "finding a witness table in a trivial existential");

      ProtocolPath path(IGF.IGM, getProtocols(), protocol);
      llvm::Value *originTable =
        getLayout().loadWitnessTable(IGF, obj, path.getOriginIndex());
      return path.apply(IGF, originTable);
    }

    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        CanType T) const {
      auto objPtrTy = dest.getAddress()->getType();
      auto fn = getAssignExistentialsFunction(IGF.IGM, objPtrTy, getLayout());
      auto call = IGF.Builder.CreateCall2(fn, dest.getAddress(),
                                          src.getAddress());
      call->setCallingConv(IGF.IGM.RuntimeCC);
      call->setDoesNotThrow();
    }

    void initializeWithCopy(IRGenFunction &IGF,
                            Address dest, Address src,
                            CanType T) const {
      auto layout = getLayout();

      llvm::Value *metadata = layout.loadMetadataRef(IGF, src);
      IGF.Builder.CreateStore(metadata, layout.projectMetadataRef(IGF, dest));

      // Load the witness tables and copy them into the new object.
      // Remember one of them for the copy later;  it doesn't matter which.
      llvm::Value *wtable = nullptr;
      for (unsigned i = 0, e = layout.getNumTables(); i != e; ++i) {
        llvm::Value *table = layout.loadWitnessTable(IGF, src, i);
        Address destSlot = layout.projectWitnessTable(IGF, dest, i);
        IGF.Builder.CreateStore(table, destSlot);

        if (i == 0) wtable = table;
      }

      // Project down to the buffers and ask the witnesses to do a
      // copy-initialize.
      Address srcBuffer = layout.projectExistentialBuffer(IGF, src);
      Address destBuffer = layout.projectExistentialBuffer(IGF, dest);
      emitInitializeBufferWithCopyOfBufferCall(IGF, metadata,
                                               destBuffer, srcBuffer);
    }

    void destroy(IRGenFunction &IGF, Address addr, CanType T) const {
      emitDestroyExistential(IGF, addr, getLayout());
    }
  };

  /// A type implementation for [weak] existential types.
  class WeakClassExistentialTypeInfo :
      public IndirectTypeInfo<WeakClassExistentialTypeInfo, WeakTypeInfo> {
    unsigned NumProtocols;

  public:
    WeakClassExistentialTypeInfo(unsigned numProtocols,
                                 llvm::Type *ty, Size size, Alignment align)
      : IndirectTypeInfo(ty, size, align), NumProtocols(numProtocols) {
    }

    void emitCopyOfTables(IRGenFunction &IGF, Address dest, Address src) const {
      if (NumProtocols == 0) return;
      IGF.emitMemCpy(dest, src, NumProtocols * IGF.IGM.getPointerSize());
    }

    void emitLoadOfTables(IRGenFunction &IGF, Address existential,
                          Explosion &out) const {
      for (unsigned i = 0; i != NumProtocols; ++i) {
        auto tableAddr = IGF.Builder.CreateStructGEP(existential, i,
                                                i * IGF.IGM.getPointerSize());
        out.add(IGF.Builder.CreateLoad(tableAddr));
      }
    }

    void emitStoreOfTables(IRGenFunction &IGF, Explosion &in,
                           Address existential) const {
      for (unsigned i = 0; i != NumProtocols; ++i) {
        auto tableAddr = IGF.Builder.CreateStructGEP(existential, i,
                                                i * IGF.IGM.getPointerSize());
        IGF.Builder.CreateStore(in.claimNext(), tableAddr);
      }
    }

    Address projectValue(IRGenFunction &IGF, Address existential) const {
      return IGF.Builder.CreateStructGEP(existential, NumProtocols,
                                       NumProtocols * IGF.IGM.getPointerSize(),
                            existential.getAddress()->getName() + ".weakref");
    }

    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        CanType T) const {
      Address destValue = projectValue(IGF, dest);
      Address srcValue = projectValue(IGF, dest);
      IGF.emitUnknownWeakCopyAssign(destValue, srcValue);
      emitCopyOfTables(IGF, dest, src);
    }

    void initializeWithCopy(IRGenFunction &IGF,
                            Address dest, Address src,
                            CanType T) const {
      Address destValue = projectValue(IGF, dest);
      Address srcValue = projectValue(IGF, dest);
      IGF.emitUnknownWeakCopyInit(destValue, srcValue);
      emitCopyOfTables(IGF, dest, src);
    }

    void assignWithTake(IRGenFunction &IGF,
                        Address dest, Address src,
                        CanType T) const {
      Address destValue = projectValue(IGF, dest);
      Address srcValue = projectValue(IGF, dest);
      IGF.emitUnknownWeakTakeAssign(destValue, srcValue);
      emitCopyOfTables(IGF, dest, src);
    }

    void initializeWithTake(IRGenFunction &IGF,
                            Address dest, Address src,
                            CanType T) const {
      Address destValue = projectValue(IGF, dest);
      Address srcValue = projectValue(IGF, dest);
      IGF.emitUnknownWeakTakeInit(destValue, srcValue);
      emitCopyOfTables(IGF, dest, src);
    }

    void destroy(IRGenFunction &IGF, Address existential,
                 CanType T) const {
      Address valueAddr = projectValue(IGF, existential);
      IGF.emitUnknownWeakDestroy(valueAddr);
    }

    // These explosions must follow the same schema as
    // ClassExistentialTypeInfo, i.e. first the tables, then the value.

    void weakLoadStrong(IRGenFunction &IGF, Address existential,
                        Explosion &out) const override {
      emitLoadOfTables(IGF, existential, out);
      Address valueAddr = projectValue(IGF, existential);
      out.add(IGF.emitUnknownWeakLoadStrong(valueAddr,
                                            IGF.IGM.UnknownRefCountedPtrTy));
    }

    void weakTakeStrong(IRGenFunction &IGF, Address existential,
                        Explosion &out) const override {
      emitLoadOfTables(IGF, existential, out);
      Address valueAddr = projectValue(IGF, existential);
      out.add(IGF.emitUnknownWeakTakeStrong(valueAddr,
                                            IGF.IGM.UnknownRefCountedPtrTy));
    }

    void weakInit(IRGenFunction &IGF, Explosion &in,
                  Address existential) const override {
      emitStoreOfTables(IGF, in, existential);
      llvm::Value *value = in.claimNext();
      assert(value->getType() == IGF.IGM.UnknownRefCountedPtrTy);
      Address valueAddr = projectValue(IGF, existential);
      IGF.emitUnknownWeakInit(value, valueAddr);
    }

    void weakAssign(IRGenFunction &IGF, Explosion &in,
                    Address existential) const override {
      emitStoreOfTables(IGF, in, existential);
      llvm::Value *value = in.claimNext();
      assert(value->getType() == IGF.IGM.UnknownRefCountedPtrTy);
      Address valueAddr = projectValue(IGF, existential);
      IGF.emitUnknownWeakAssign(value, valueAddr);
    }
  };

  /// A helper class for working with existential types that can be
  /// exploded into scalars.
  template <class Derived, class Base>
  class ScalarExistentialTypeInfoBase : public ScalarTypeInfo<Derived, Base> {
    typedef ScalarTypeInfo<Derived, Base> super;
    const Derived &asDerived() const {
      return *static_cast<const Derived*>(this);
    }

  protected:
    const unsigned NumProtocols;

    template <class... T>
    ScalarExistentialTypeInfoBase(unsigned numProtos, T &&...args)
      : super(std::forward<T>(args)...), NumProtocols(numProtos) {}

  public:
    /// The storage type of a class existential is a struct containing
    /// witness table pointers for each conformed-to protocol followed by a
    /// refcounted pointer to the class instance value. Unlike for opaque
    /// existentials, a class existential does not need to store type
    /// metadata as an additional element, since it can be derived from the
    /// class instance.
    llvm::StructType *getStorageType() const {
      return cast<llvm::StructType>(TypeInfo::getStorageType());
    }

    unsigned getExplosionSize(ResilienceExpansion kind) const override {
      return 1 + NumProtocols;
    }
    
    void getSchema(ExplosionSchema &schema) const override {
      llvm::StructType *ty = getStorageType();
      for (unsigned i = 0; i < 1 + NumProtocols; ++i)
        schema.add(ExplosionSchema::Element::forScalar(ty->getElementType(i)));
    }

    /// Given the address of a class existential container, returns
    /// the address of a witness table pointer.
    Address projectWitnessTable(IRGenFunction &IGF, Address address,
                                unsigned n) const {
      assert(n < NumProtocols && "witness table index out of bounds");
      return IGF.Builder.CreateStructGEP(address, n, Size(0));
    }
    
    /// Given the address of a class existential container, returns
    /// the address of its instance pointer.
    Address projectValue(IRGenFunction &IGF, Address address) const {
      return IGF.Builder.CreateStructGEP(address, NumProtocols, Size(0));
    }

    llvm::Value *loadValue(IRGenFunction &IGF, Address addr) const {
      return IGF.Builder.CreateLoad(projectValue(IGF, addr));
    }
    
    /// Given a class existential container, returns a witness table
    /// pointer out of the container, and the type metadata pointer for the
    /// value.
    llvm::Value *
    getWitnessTable(IRGenFunction &IGF, Explosion &container,
                    unsigned which) const {
      assert(which < NumProtocols && "witness table index out of bounds");
      ArrayRef<llvm::Value *> values = container.claimAll();
      return values[which];
    }

    /// Deconstruct an existential object into witness tables and instance
    /// pointer.
    std::pair<ArrayRef<llvm::Value*>, llvm::Value*>
    getWitnessTablesAndValue(Explosion &container) const {
      ArrayRef<llvm::Value*> witnesses = container.claim(NumProtocols);
      llvm::Value *instance = container.claimNext();
      return {witnesses, instance};
    }

    /// Given a class existential container, returns the instance
    /// pointer value.
    llvm::Value *getValue(IRGenFunction &IGF, Explosion &container) const {
      container.claim(NumProtocols);
      return container.claimNext();
    }

    void loadAsCopy(IRGenFunction &IGF, Address address,
                    Explosion &out) const override {
      // Load the witness table pointers.
      for (unsigned i = 0; i < NumProtocols; ++i)
        out.add(IGF.Builder.CreateLoad(projectWitnessTable(IGF, address, i)));
      // Load the instance pointer, which is unknown-refcounted.
      llvm::Value *instance
        = IGF.Builder.CreateLoad(projectValue(IGF, address));
      asDerived().emitPayloadRetain(IGF, instance);
      out.add(instance);
    }

    void loadAsTake(IRGenFunction &IGF, Address address,
                    Explosion &e) const override {
      // Load the witness table pointers.
      for (unsigned i = 0; i < NumProtocols; ++i)
        e.add(IGF.Builder.CreateLoad(projectWitnessTable(IGF, address, i)));
      // Load the instance pointer.
      e.add(IGF.Builder.CreateLoad(projectValue(IGF, address)));
    }
    
    void assign(IRGenFunction &IGF, Explosion &e, Address address)
    const override {
      // Store the witness table pointers.
      for (unsigned i = 0; i < NumProtocols; ++i) {
        IGF.Builder.CreateStore(e.claimNext(),
                                projectWitnessTable(IGF, address, i));
      }
      Address instanceAddr = projectValue(IGF, address);
      llvm::Value *old = IGF.Builder.CreateLoad(instanceAddr);
      IGF.Builder.CreateStore(e.claimNext(), instanceAddr);
      asDerived().emitPayloadRelease(IGF, old);
    }
    
    void initialize(IRGenFunction &IGF, Explosion &e, Address address)
    const override {
      // Store the witness table pointers.
      for (unsigned i = 0; i < NumProtocols; ++i) {
        IGF.Builder.CreateStore(e.claimNext(),
                                projectWitnessTable(IGF, address, i));
      }
      // Store the instance pointer.
      IGF.Builder.CreateStore(e.claimNext(),
                              projectValue(IGF, address));
    }
    
    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest)
    const override {
      // Transfer the witness table pointers.
      src.transferInto(dest, NumProtocols);

      // Copy the instance pointer.
      llvm::Value *value = src.claimNext();
      dest.add(value);
      asDerived().emitPayloadRetain(IGF, value);
    }

    void consume(IRGenFunction &IGF, Explosion &src)
    const override {
      // Throw out the witness table pointers.
      src.claim(NumProtocols);
      
      // Copy the instance pointer.
      llvm::Value *value = src.claimNext();
      asDerived().emitPayloadRelease(IGF, value);
    }
    
    void destroy(IRGenFunction &IGF, Address addr, CanType T) const override {
      llvm::Value *value = IGF.Builder.CreateLoad(projectValue(IGF, addr));
      asDerived().emitPayloadRelease(IGF, value);
    }
    
    llvm::Value *packEnumPayload(IRGenFunction &IGF,
                                  Explosion &src,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      PackEnumPayload pack(IGF, bitWidth);
      for (unsigned i = 0; i < NumProtocols; ++i)
        pack.addAtOffset(src.claimNext(), offset);
      pack.add(src.claimNext());
      return pack.get();
    }
    
    void unpackEnumPayload(IRGenFunction &IGF,
                            llvm::Value *payload,
                            Explosion &dest,
                            unsigned offset) const override {
      UnpackEnumPayload unpack(IGF, payload);
      for (unsigned i = 0; i < NumProtocols; ++i)
        dest.add(unpack.claimAtOffset(IGF.IGM.WitnessTablePtrTy,
                                      offset));
      dest.add(unpack.claim(IGF.IGM.UnknownRefCountedPtrTy));
    }
  };

  /// A type implementation for [unowned] class existential types.
  class UnownedClassExistentialTypeInfo
    : public ScalarExistentialTypeInfoBase<UnownedClassExistentialTypeInfo,
                                           UnownedTypeInfo> {
  public:
    UnownedClassExistentialTypeInfo(unsigned numTables,
                                    llvm::Type *ty, Size size, Alignment align)
      : ScalarExistentialTypeInfoBase(numTables, ty, size, align) {}

    void emitPayloadRetain(IRGenFunction &IGF, llvm::Value *value) const {
      IGF.emitUnknownUnownedRetain(value);
    }

    void emitPayloadRelease(IRGenFunction &IGF, llvm::Value *value) const {
      IGF.emitUnknownUnownedRelease(value);
    }
  };
  
  /// A type info implementation for class existential types, that is,
  /// an existential type known to conform to one or more class protocols.
  /// Class existentials can be represented directly as an aggregation
  /// of a refcounted pointer plus witness tables instead of using an indirect
  /// buffer.
  class ClassExistentialTypeInfo
    : public ScalarExistentialTypeInfoBase<ClassExistentialTypeInfo,
                                           ReferenceTypeInfo>
  {
    ProtocolEntry *getProtocolsBuffer() {
      return reinterpret_cast<ProtocolEntry *>(this + 1);
    }
    const ProtocolEntry *getProtocolsBuffer() const {
      return reinterpret_cast<const ProtocolEntry *>(this + 1);
    }
    
    ClassExistentialTypeInfo(llvm::Type *ty,
                             Size size,
                             llvm::BitVector spareBits,
                             Alignment align,
                             ArrayRef<ProtocolEntry> protocols)
      : ScalarExistentialTypeInfoBase(protocols.size(), ty, size,
                                      std::move(spareBits), align)
    {
      for (unsigned i = 0; i != NumProtocols; ++i) {
        new (&getProtocolsBuffer()[i]) ProtocolEntry(protocols[i]);
      }
    }
    
  public:    
    static const ClassExistentialTypeInfo *
    create(llvm::Type *ty, Size size, llvm::BitVector spareBits, Alignment align,
           ArrayRef<ProtocolEntry> protocols)
    {
      void *buffer = operator new(sizeof(ClassExistentialTypeInfo) +
                                  protocols.size() * sizeof(ProtocolEntry));
      return new (buffer) ClassExistentialTypeInfo(ty, size,
                                                   std::move(spareBits),
                                                   align,
                                                   protocols);
    }

    /// Class existentials are single refcounted pointers if they have no
    /// witness tables. Right now we have no way of constraining an existential
    /// to Swift-refcounted types.
    bool isSingleSwiftRetainablePointer(ResilienceScope scope) const override {
      return false;
    }
    bool isSingleUnknownRetainablePointer(ResilienceScope scope) const override{
      return NumProtocols == 0;
    }
    
    /// Returns the protocols that values of this type are known to
    /// implement.  This can be empty, meaning that values of this
    /// type are not know to implement any protocols, although we do
    /// still know how to manipulate them.
    ArrayRef<ProtocolEntry> getProtocols() const {
      return ArrayRef<ProtocolEntry>(getProtocolsBuffer(), NumProtocols);
    }
    
    /// Given an existential object, find the witness table
    /// corresponding to the given protocol.
    llvm::Value *findWitnessTable(IRGenFunction &IGF,
                                  Explosion &container,
                                  ProtocolDecl *protocol) const {
      assert(NumProtocols != 0 &&
             "finding a witness table in a trivial existential");
      
      ProtocolPath path(IGF.IGM, getProtocols(), protocol);
      llvm::Value *witness
        = getWitnessTable(IGF, container, path.getOriginIndex());
      return path.apply(IGF, witness);
    }
    
    /// Given the witness table vector from an existential object, find the
    /// witness table corresponding to the given protocol.
    llvm::Value *findWitnessTable(IRGenFunction &IGF,
                                  ArrayRef<llvm::Value *> witnesses,
                                  ProtocolDecl *protocol) const {
      ProtocolPath path(IGF.IGM, getProtocols(), protocol);
      return path.apply(IGF, witnesses[path.getOriginIndex()]);
    }
    
    void loadAsCopy(IRGenFunction &IGF, Address address,
                    Explosion &out) const override {
      // Load the witness table pointers.
      for (unsigned i = 0; i < NumProtocols; ++i)
        out.add(IGF.Builder.CreateLoad(projectWitnessTable(IGF, address, i)));
      // Load the instance pointer, which is unknown-refcounted.
      llvm::Value *instance
        = IGF.Builder.CreateLoad(projectValue(IGF, address));
      IGF.emitUnknownRetainCall(instance);
      out.add(instance);
    }

    void retain(IRGenFunction &IGF, Explosion &e) const override {
      e.claim(NumProtocols);
      // The instance is treated as unknown-refcounted.
      IGF.emitUnknownRetainCall(e.claimNext());
    }
    
    void release(IRGenFunction &IGF, Explosion &e) const override {
      e.claim(NumProtocols);
      // The instance is treated as unknown-refcounted.
      IGF.emitUnknownRelease(e.claimNext());
    }

    void retainUnowned(IRGenFunction &IGF, Explosion &e) const override {
      e.claim(NumProtocols);
      // The instance is treated as unknown-refcounted.
      IGF.emitUnknownRetainUnowned(e.claimNext());
    }

    void unownedRetain(IRGenFunction &IGF, Explosion &e) const override {
      e.claim(NumProtocols);
      // The instance is treated as unknown-refcounted.
      IGF.emitUnknownUnownedRetain(e.claimNext());
    }

    void unownedRelease(IRGenFunction &IGF, Explosion &e) const override {
      e.claim(NumProtocols);
      // The instance is treated as unknown-refcounted.
      IGF.emitUnknownUnownedRelease(e.claimNext());
    }

    void emitPayloadRetain(IRGenFunction &IGF, llvm::Value *value) const {
      IGF.emitUnknownRetainCall(value);
    }

    void emitPayloadRelease(IRGenFunction &IGF, llvm::Value *value) const {
      IGF.emitUnknownRelease(value);
    }

    const UnownedTypeInfo *
    createUnownedStorageType(TypeConverter &TC) const override {
      // We can just re-use the storage type for the [unowned] type.
      return new UnownedClassExistentialTypeInfo(NumProtocols,
                                                 getStorageType(),
                                                 getFixedSize(),
                                                 getFixedAlignment());
    }

    const WeakTypeInfo *
    createWeakStorageType(TypeConverter &TC) const override {
      Size size = TC.IGM.getWeakReferenceSize()
                + NumProtocols * TC.IGM.getPointerSize();

      Alignment align = TC.IGM.getWeakReferenceAlignment();
      assert(align == TC.IGM.getPointerAlignment() &&
             "[weak] alignment not pointer alignment; fix existential layout");
      (void)align;

      // We need to build a new struct for the [weak] type because the weak
      // component is not necessarily pointer-sized.
      SmallVector<llvm::Type*, 8> fieldTys;
      fieldTys.resize(NumProtocols, TC.IGM.WitnessTablePtrTy);
      fieldTys.push_back(TC.IGM.WeakReferencePtrTy->getElementType());
      auto storageTy = llvm::StructType::get(TC.IGM.getLLVMContext(), fieldTys);

      return new WeakClassExistentialTypeInfo(NumProtocols, storageTy, size,
                                              TC.IGM.getWeakReferenceAlignment());
    }
    
    // Extra inhabitants of class existential containers.
    // We use the heap object extra inhabitants over the class pointer value.
    // We could get even more extra inhabitants from the witness table
    // pointer(s), but it's unlikely we would ever need to.
    
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return true;
    }
    
    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return getHeapObjectExtraInhabitantCount(IGM);
    }
    
    llvm::ConstantInt *getFixedExtraInhabitantValue(IRGenModule &IGM,
                                              unsigned bits,
                                              unsigned index) const override {
      // We place the extra inhabitant in the class pointer slot.
      auto offset = IGM.getPointerSize().getValueInBits() * NumProtocols;
      return getHeapObjectFixedExtraInhabitantValue(IGM, bits, index,
                                                    offset);
    }
    
    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF, Address src,
                                         CanType T)
    const override {
      // NB: We assume that the witness table slots are zero if an extra
      // inhabitant is stored in the container.
      
      src = projectValue(IGF, src);
      return getHeapObjectExtraInhabitantIndex(IGF, src);
    }
    
    void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                              Address dest, CanType T) const override {
      for (unsigned i = 0; i < NumProtocols; ++i) {
        Address witnessDest = projectWitnessTable(IGF, dest, i);
        IGF.Builder.CreateStore(
                      llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy),
                      witnessDest);
      }
      
      Address valueDest = projectValue(IGF, dest);
      storeHeapObjectExtraInhabitant(IGF, index, valueDest);
    }
  };

  /// Common type implementation details for all archetypes.
  class ArchetypeTypeInfoBase {
  protected:
    unsigned NumProtocols;
    ProtocolEntry *ProtocolsBuffer;
    
    ArchetypeTypeInfoBase(void *protocolsBuffer,
                          ArrayRef<ProtocolEntry> protocols)
      : NumProtocols(protocols.size()),
        ProtocolsBuffer(reinterpret_cast<ProtocolEntry*>(protocolsBuffer))
    {
      for (unsigned i = 0, e = protocols.size(); i != e; ++i) {
        ::new (&ProtocolsBuffer[i]) ProtocolEntry(protocols[i]);
      }
    }
    
  public:
    unsigned getNumProtocols() const {
      return NumProtocols;
    }
    
    ArrayRef<ProtocolEntry> getProtocols() const {
      return llvm::makeArrayRef(ProtocolsBuffer, getNumProtocols());
    }
    
    /// Return the witness table that's been set for this type.
    llvm::Value *getWitnessTable(IRGenFunction &IGF,
                                 CanArchetypeType archetype,
                                 unsigned which) const {
      assert(which < getNumProtocols());
      return IGF.getLocalTypeData(archetype, LocalTypeData(which));
    }
  };
  
  /// A type implementation for an ArchetypeType, otherwise known as a
  /// type variable: for example, This in a protocol declaration, or T
  /// in a generic declaration like foo<T>(x : T) -> T.  The critical
  /// thing here is that performing an operation involving archetypes
  /// is dependent on the witness binding we can see.
  class OpaqueArchetypeTypeInfo
    : public IndirectTypeInfo<OpaqueArchetypeTypeInfo,
                              WitnessSizedTypeInfo<OpaqueArchetypeTypeInfo>>,
      public ArchetypeTypeInfoBase
  {
    OpaqueArchetypeTypeInfo(llvm::Type *type,
                            ArrayRef<ProtocolEntry> protocols)
      : IndirectTypeInfo(type, Alignment(1), IsNotPOD),
        ArchetypeTypeInfoBase(this + 1, protocols)
    {}

  public:
    static const OpaqueArchetypeTypeInfo *create(llvm::Type *type,
                                           ArrayRef<ProtocolEntry> protocols) {
      void *buffer = operator new(sizeof(OpaqueArchetypeTypeInfo)
                                  + protocols.size() * sizeof(ProtocolEntry));
      return ::new (buffer) OpaqueArchetypeTypeInfo(type, protocols);
    }

    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        CanType T) const {
      emitAssignWithCopyCall(IGF, IGF.emitTypeMetadataRef(T),
                             dest.getAddress(), src.getAddress());
    }

    void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                        CanType T) const {
      emitAssignWithTakeCall(IGF, IGF.emitTypeMetadataRef(T),
                             dest.getAddress(), src.getAddress());
    }

    void initializeWithCopy(IRGenFunction &IGF,
                            Address dest, Address src, CanType T) const {
      emitInitializeWithCopyCall(IGF, IGF.emitTypeMetadataRef(T),
                                 dest.getAddress(), src.getAddress());
    }

    void initializeWithTake(IRGenFunction &IGF,
                            Address dest, Address src, CanType T) const {
      emitInitializeWithTakeCall(IGF, IGF.emitTypeMetadataRef(T),
                                 dest.getAddress(), src.getAddress());
    }

    void destroy(IRGenFunction &IGF, Address addr, CanType T) const {
      emitDestroyCall(IGF, IGF.emitTypeMetadataRef(T), addr.getAddress());
    }

    std::pair<llvm::Value*,llvm::Value*>
    getSizeAndAlignment(IRGenFunction &IGF, CanType T) const {
      llvm::Value *wtable = getValueWitnessTable(IGF, T);
      auto size = emitLoadOfSize(IGF, wtable);
      auto align = emitLoadOfAlignmentMask(IGF, wtable);
      return std::make_pair(size, align);
    }

    llvm::Value *getSize(IRGenFunction &IGF, CanType T) const {
      llvm::Value *wtable = getValueWitnessTable(IGF, T);
      return emitLoadOfSize(IGF, wtable);
    }

    llvm::Value *getAlignment(IRGenFunction &IGF, CanType T) const {
      llvm::Value *wtable = getValueWitnessTable(IGF, T);
      return emitLoadOfAlignmentMask(IGF, wtable);
    }

    llvm::Value *getStride(IRGenFunction &IGF, CanType T) const {
      llvm::Value *wtable = getValueWitnessTable(IGF, T);
      return emitLoadOfStride(IGF, wtable);
    }

    llvm::Constant *getStaticSize(IRGenModule &IGM) const { return nullptr; }
    llvm::Constant *getStaticAlignment(IRGenModule &IGM) const { return nullptr; }
    llvm::Constant *getStaticStride(IRGenModule &IGM) const { return nullptr; }
    
    void initializeMetadata(IRGenFunction &IGF,
                            llvm::Value *metadata,
                            llvm::Value *vwtable,
                            CanType T) const override {
      // Archetypes always refer to an existing type. A witness table should
      // never be independently initialized for one.
      llvm_unreachable("initializing value witness table for archetype?!");
    }
    
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return true;
    }
    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src,
                                         CanType T) const override {
      auto metadata = IGF.emitTypeMetadataRef(T);
      return emitGetExtraInhabitantIndexCall(IGF, metadata, src.getAddress());
    }
    void storeExtraInhabitant(IRGenFunction &IGF,
                              llvm::Value *index,
                              Address dest,
                              CanType T) const override {
      auto metadata = IGF.emitTypeMetadataRef(T);
      emitStoreExtraInhabitantCall(IGF, metadata, index, dest.getAddress());
    }
  };
  
  /// A type implementation for a class archetype, that is, an archetype
  /// bounded by a class protocol constraint. These archetypes can be
  /// represented by a refcounted pointer instead of an opaque value buffer.
  /// We use an unknown-refcounted pointer in order to allow ObjC or Swift
  /// classes to conform to the type variable.
  class ClassArchetypeTypeInfo
    : public HeapTypeInfo<ClassArchetypeTypeInfo>,
      public ArchetypeTypeInfoBase
  {
    bool HasSwiftRefcount;
    
    ClassArchetypeTypeInfo(llvm::PointerType *storageType,
                           Size size, llvm::BitVector spareBits,
                           Alignment align,
                           ArrayRef<ProtocolEntry> protocols,
                           bool hasSwiftRefcount)
      : HeapTypeInfo(storageType, size, spareBits, align),
        ArchetypeTypeInfoBase(this + 1, protocols),
        HasSwiftRefcount(hasSwiftRefcount)
    {}
    
  public:
    static const ClassArchetypeTypeInfo *create(llvm::PointerType *storageType,
                                           Size size, llvm::BitVector spareBits,
                                           Alignment align,
                                           ArrayRef<ProtocolEntry> protocols,
                                           bool hasSwiftRefcount) {
      void *buffer = operator new(sizeof(ClassArchetypeTypeInfo)
                                    + protocols.size() * sizeof(ProtocolEntry));
      return ::new (buffer)
        ClassArchetypeTypeInfo(storageType, size, spareBits, align,
                               protocols, hasSwiftRefcount);
    }
    
    bool hasSwiftRefcount() const {
      return HasSwiftRefcount;
    }
  };
  
  /// Return the ArchetypeTypeInfoBase information from the TypeInfo for any
  /// archetype.
  static const ArchetypeTypeInfoBase &
  getArchetypeInfo(IRGenFunction &IGF, CanArchetypeType t, const TypeInfo &ti) {
    if (t->requiresClass())
      return ti.as<ClassArchetypeTypeInfo>();
    return ti.as<OpaqueArchetypeTypeInfo>();
  }
}

static void setMetadataRef(IRGenFunction &IGF,
                           ArchetypeType *archetype,
                           llvm::Value *metadata) {
  assert(metadata->getType() == IGF.IGM.TypeMetadataPtrTy);
  IGF.setUnscopedLocalTypeData(CanType(archetype),
                               LocalTypeData::Metatype,
                               metadata);

  // Create a shadow copy of the metadata in an alloca for the debug info.
  StringRef Name = metadata->getName();
  if (IGF.IGM.Opts.OptLevel == 0) {
    auto Alloca = IGF.createAlloca(metadata->getType(),
                                   IGF.IGM.getPointerAlignment(), Name);
    IGF.Builder.CreateAlignedStore(metadata, Alloca.getAddress(),
                                   IGF.IGM.getPointerAlignment().getValue());
    metadata = Alloca.getAddress();
  }

  // Emit debug info for the metadata.
  if (IGF.IGM.DebugInfo)
    IGF.IGM.DebugInfo->emitTypeMetadata(IGF, metadata, Name);
}

static void setWitnessTable(IRGenFunction &IGF,
                            ArchetypeType *archetype,
                            unsigned protocolIndex,
                            llvm::Value *wtable) {
  assert(wtable->getType() == IGF.IGM.WitnessTablePtrTy);
  assert(protocolIndex < archetype->getConformsTo().size());
  IGF.setUnscopedLocalTypeData(CanType(archetype),
                               LocalTypeData(protocolIndex),
                               wtable);
}

/// Detail about how an object conforms to a protocol.
class irgen::ConformanceInfo {
  friend class ProtocolInfo;
public:
  virtual ~ConformanceInfo() {}
  virtual llvm::Value *getTable(IRGenFunction &IGF) const = 0;
  /// Try to get this table as a constant pointer.  This might just
  /// not be supportable at all.
  virtual llvm::Constant *tryGetConstantTable(IRGenModule &IGM) const = 0;
};

namespace {
  
/// Conformance info for a witness table that can be directly generated.
class DirectConformanceInfo : public ConformanceInfo {
  friend class ProtocolInfo;
  
  const NormalProtocolConformance *RootConformance;
public:
  DirectConformanceInfo(const NormalProtocolConformance *C)
    : RootConformance(C) {}
  
  llvm::Value *getTable(IRGenFunction &IGF) const override {
    return IGF.IGM.getAddrOfWitnessTable(RootConformance);
  }

  llvm::Constant *tryGetConstantTable(IRGenModule &IGM) const override {
    return IGM.getAddrOfWitnessTable(RootConformance);
  }
};
  
} //end anonymous namespace

static bool isNeverAllocated(FixedPacking packing) {
  switch (packing) {
  case FixedPacking::OffsetZero: return true;
  case FixedPacking::Allocate: return false;
  case FixedPacking::Dynamic: return false;
  }
  llvm_unreachable("bad FixedPacking value");
}

namespace {
  /// An operation to be peformed for various kinds of packing.
  struct DynamicPackingOperation {
    virtual ~DynamicPackingOperation() = default;

    /// Emit the operation at a concrete packing kind.
    ///
    /// Immediately after this call, there will be an unconditional
    /// branch to the continuation block.
    virtual void emitForPacking(IRGenFunction &IGF,
                                CanType T,
                                const TypeInfo &type,
                                FixedPacking packing) = 0;

    /// Given that we are currently at the beginning of the
    /// continuation block, complete the operation.
    virtual void complete(IRGenFunction &IGF,
                          CanType T,
                          const TypeInfo &type) = 0;
  };

  /// A class for merging a particular kind of value across control flow.
  template <class T> class DynamicPackingPHIMapping;

  /// An implementation of DynamicPackingPHIMapping for a single LLVM value.
  template <> class DynamicPackingPHIMapping<llvm::Value*> {
    llvm::PHINode *PHI = nullptr;
  public:
    void collect(IRGenFunction &IGF, CanType T,
                 const TypeInfo &type, llvm::Value *value) {
      // Add the result to the phi, creating it (unparented) if necessary.
      if (!PHI) PHI = llvm::PHINode::Create(value->getType(), 2,
                                            "dynamic-packing.result");
      PHI->addIncoming(value, IGF.Builder.GetInsertBlock());
    }
    void complete(IRGenFunction &IGF, CanType T, const TypeInfo &type) {
      assert(PHI);
      IGF.Builder.Insert(PHI);
    }
    llvm::Value *get(IRGenFunction &IGF, CanType T, const TypeInfo &type) {
      assert(PHI);
      return PHI;
    }
  };

  /// An implementation of DynamicPackingPHIMapping for Addresses.
  template <> class DynamicPackingPHIMapping<Address>
      : private DynamicPackingPHIMapping<llvm::Value*> {
    typedef DynamicPackingPHIMapping<llvm::Value*> super;
  public:
    void collect(IRGenFunction &IGF, CanType T,
                 const TypeInfo &type, Address value) {
      super::collect(IGF, T, type, value.getAddress());
    }
    void complete(IRGenFunction &IGF, CanType T,
                  const TypeInfo &type) {
      super::complete(IGF, T, type);
    }
    Address get(IRGenFunction &IGF, CanType T, const TypeInfo &type) {
      return type.getAddressForPointer(super::get(IGF, T, type));
    }
  };

  /// An implementation of packing operations based around a lambda.
  template <class ResultTy, class FnTy>
  class LambdaDynamicPackingOperation : public DynamicPackingOperation {
    FnTy Fn;
    DynamicPackingPHIMapping<ResultTy> Mapping;
  public:
    explicit LambdaDynamicPackingOperation(FnTy &&fn) : Fn(fn) {}
    void emitForPacking(IRGenFunction &IGF, CanType T, const TypeInfo &type,
                        FixedPacking packing) override {
      Mapping.collect(IGF, T, type, Fn(IGF, T, type, packing));
    }

    void complete(IRGenFunction &IGF, CanType T,
                  const TypeInfo &type) override {
      Mapping.complete(IGF, T, type);
    }

    ResultTy get(IRGenFunction &IGF, CanType T, const TypeInfo &type) {
      return Mapping.get(IGF, T, type);
    }
  };

  /// A partial specialization for lambda-based packing operations
  /// that return 'void'.
  template <class FnTy>
  class LambdaDynamicPackingOperation<void, FnTy>
      : public DynamicPackingOperation {
    FnTy Fn;
  public:
    explicit LambdaDynamicPackingOperation(FnTy &&fn) : Fn(fn) {}
    void emitForPacking(IRGenFunction &IGF, CanType T, const TypeInfo &type,
                        FixedPacking packing) override {
      Fn(IGF, T, type, packing);
    }
    void complete(IRGenFunction &IGF, CanType T,
                  const TypeInfo &type) override {}
    void get(IRGenFunction &IGF, CanType T, const TypeInfo &type) {}
  };
}

/// Dynamic check for the enabling conditions of different kinds of
/// packing into a fixed-size buffer, and perform an operation at each
/// of them.
static void emitDynamicPackingOperation(IRGenFunction &IGF,
                                        CanType T,
                                        const TypeInfo &type,
                                        DynamicPackingOperation &operation) {
  llvm::Value *size = type.getSize(IGF, T);
  llvm::Value *alignMask = type.getAlignmentMask(IGF, T);

  auto indirectBB = IGF.createBasicBlock("dynamic-packing.indirect");
  auto directBB = IGF.createBasicBlock("dynamic-packing.direct");
  auto contBB = IGF.createBasicBlock("dynamic-packing.cont");

  // Check whether the type is either over-sized or over-aligned.
  // Note that, since alignof(FixedBuffer) is a power of 2 and
  // alignMask is one less than one, alignMask > alignof(FixedBuffer)
  // is equivalent to alignMask+1 > alignof(FixedBuffer).
  auto bufferSize = IGF.IGM.getSize(getFixedBufferSize(IGF.IGM));
  auto oversize = IGF.Builder.CreateICmpUGT(size, bufferSize, "oversized");
  auto bufferAlign = IGF.IGM.getSize(getFixedBufferAlignment(IGF.IGM).asSize());
  auto overalign = IGF.Builder.CreateICmpUGT(alignMask, bufferAlign, "overaligned");

  // Branch.
  llvm::Value *cond = IGF.Builder.CreateOr(oversize, overalign, "indirect");
  IGF.Builder.CreateCondBr(cond, indirectBB, directBB);

  // Emit the indirect path.
  IGF.Builder.emitBlock(indirectBB);
  operation.emitForPacking(IGF, T, type, FixedPacking::Allocate);
  IGF.Builder.CreateBr(contBB);

  // Emit the direct path.
  IGF.Builder.emitBlock(directBB);
  operation.emitForPacking(IGF, T, type, FixedPacking::OffsetZero);
  IGF.Builder.CreateBr(contBB);

  // Enter the continuation block and add the PHI if required.
  IGF.Builder.emitBlock(contBB);
  operation.complete(IGF, T, type);
}

/// A helper function for creating a lambda-based DynamicPackingOperation.
template <class ResultTy, class FnTy>
LambdaDynamicPackingOperation<ResultTy, FnTy>
makeLambdaDynamicPackingOperation(FnTy &&fn) {
  return LambdaDynamicPackingOperation<ResultTy, FnTy>(std::move(fn));
}

/// Perform an operation on a type that requires dynamic packing.
template <class ResultTy, class... ArgTys>
static ResultTy emitForDynamicPacking(IRGenFunction &IGF,
                                      ResultTy (*fn)(IRGenFunction &IGF,
                                                     CanType T,
                                                     const TypeInfo &type,
                                                     FixedPacking packing,
                                                     ArgTys... args),
                                      CanType T,
                                      const TypeInfo &type,
                        // using enable_if to block template argument deduction
                        typename std::enable_if<true,ArgTys>::type... args) {
  auto operation = makeLambdaDynamicPackingOperation<ResultTy>(
    [&](IRGenFunction &IGF, CanType T, const TypeInfo &type, FixedPacking packing) {
      return fn(IGF, T, type, packing, args...);
    });
  emitDynamicPackingOperation(IGF, T, type, operation);
  return operation.get(IGF, T, type);
}
                                             
/// Emit a 'projectBuffer' operation.  Always returns a T*.
static Address emitProjectBuffer(IRGenFunction &IGF,
                                 CanType T,
                                 const TypeInfo &type,
                                 FixedPacking packing,
                                 Address buffer) {
  llvm::PointerType *resultTy = type.getStorageType()->getPointerTo();
  switch (packing) {
  case FixedPacking::Allocate: {
    Address slot = IGF.Builder.CreateBitCast(buffer, resultTy->getPointerTo(),
                                             "storage-slot");
    llvm::Value *address = IGF.Builder.CreateLoad(slot);
    return type.getAddressForPointer(address);
  }

  case FixedPacking::OffsetZero: {
    return IGF.Builder.CreateBitCast(buffer, resultTy, "object");
  }

  case FixedPacking::Dynamic:
    return emitForDynamicPacking(IGF, &emitProjectBuffer, T, type, buffer);
    
  }
  llvm_unreachable("bad packing!");
  
}

/// Emit an 'allocateBuffer' operation.  Always returns a T*.
static Address emitAllocateBuffer(IRGenFunction &IGF,
                                  CanType T,
                                  const TypeInfo &type,
                                  FixedPacking packing,
                                  Address buffer) {
  switch (packing) {
  case FixedPacking::Allocate: {
    auto sizeAndAlign = type.getSizeAndAlignmentMask(IGF, T);
    llvm::Value *addr =
      IGF.emitAllocRawCall(sizeAndAlign.first, sizeAndAlign.second);
    buffer = IGF.Builder.CreateBitCast(buffer, IGF.IGM.Int8PtrPtrTy);
    IGF.Builder.CreateStore(addr, buffer);

    addr = IGF.Builder.CreateBitCast(addr,
                                     type.getStorageType()->getPointerTo());
    return type.getAddressForPointer(addr);
  }

  case FixedPacking::OffsetZero:
    return emitProjectBuffer(IGF, T, type, packing, buffer);

  case FixedPacking::Dynamic:
    return emitForDynamicPacking(IGF, &emitAllocateBuffer, T, type, buffer);
  }
  llvm_unreachable("bad packing!");
}

/// Emit a 'deallocateBuffer' operation.
static void emitDeallocateBuffer(IRGenFunction &IGF,
                                 CanType T,
                                 const TypeInfo &type,
                                 FixedPacking packing,
                                 Address buffer) {
  switch (packing) {
  case FixedPacking::Allocate: {
    Address slot =
      IGF.Builder.CreateBitCast(buffer, IGF.IGM.Int8PtrPtrTy);
    llvm::Value *addr = IGF.Builder.CreateLoad(slot, "storage");
    IGF.emitDeallocRawCall(addr, type.getSize(IGF, T));
    return;
  }

  case FixedPacking::OffsetZero:
    return;

  case FixedPacking::Dynamic:
    return emitForDynamicPacking(IGF, &emitDeallocateBuffer, T, type, buffer);
  }
  llvm_unreachable("bad packing!");
}

/// Emit a 'destroyBuffer' operation.
static void emitDestroyBuffer(IRGenFunction &IGF,
                              CanType T,
                              const TypeInfo &type,
                              FixedPacking packing,
                              Address buffer) {
  // Special-case dynamic packing in order to thread the jumps.
  if (packing == FixedPacking::Dynamic)
    return emitForDynamicPacking(IGF, &emitDestroyBuffer, T, type, buffer);

  Address object = emitProjectBuffer(IGF, T, type, packing, buffer);
  type.destroy(IGF, object, T);
  emitDeallocateBuffer(IGF, T, type, packing, buffer);
}

/// Emit an 'initializeWithCopy' operation.
static void emitInitializeWithCopy(IRGenFunction &IGF,
                                   CanType T,
                                   const TypeInfo &type,
                                   Address dest, Address src) {
  type.initializeWithCopy(IGF, dest, src, T);
}

/// Emit an 'initializeWithTake' operation.
static void emitInitializeWithTake(IRGenFunction &IGF,
                                   CanType T,
                                   const TypeInfo &type,
                                   Address dest, Address src) {
  type.initializeWithTake(IGF, dest, src, T);
}

/// Emit an 'initializeBufferWithCopyOfBuffer' operation.
/// Returns the address of the destination object.
static Address emitInitializeBufferWithCopyOfBuffer(IRGenFunction &IGF,
                                                    CanType T,
                                                    const TypeInfo &type,
                                                    FixedPacking packing,
                                                    Address dest,
                                                    Address src) {
  // Special-case dynamic packing in order to thread the jumps.
  if (packing == FixedPacking::Dynamic)
    return emitForDynamicPacking(IGF, &emitInitializeBufferWithCopyOfBuffer,
                                 T, type, dest, src);

  Address destObject = emitAllocateBuffer(IGF, T, type, packing, dest);
  Address srcObject = emitProjectBuffer(IGF, T, type, packing, src);
  emitInitializeWithCopy(IGF, T, type, destObject, srcObject);
  return destObject;
}

/// Emit an 'initializeBufferWithCopy' operation.
/// Returns the address of the destination object.
static Address emitInitializeBufferWithCopy(IRGenFunction &IGF,
                                            CanType T,
                                            const TypeInfo &type,
                                            FixedPacking packing,
                                            Address dest,
                                            Address srcObject) {
  Address destObject = emitAllocateBuffer(IGF, T, type, packing, dest);
  emitInitializeWithCopy(IGF, T, type, destObject, srcObject);
  return destObject;
}

/// Emit an 'initializeBufferWithTake' operation.
/// Returns the address of the destination object.
static Address emitInitializeBufferWithTake(IRGenFunction &IGF,
                                            CanType T,
                                            const TypeInfo &type,
                                            FixedPacking packing,
                                            Address dest,
                                            Address srcObject) {
  Address destObject = emitAllocateBuffer(IGF, T, type, packing, dest);
  emitInitializeWithTake(IGF, T, type, destObject, srcObject);
  return destObject;
}

static llvm::Value *getArg(llvm::Function::arg_iterator &it,
                           StringRef name) {
  llvm::Value *arg = it++;
  arg->setName(name);
  return arg;
}

/// Get the next argument as a pointer to the given storage type.
static Address getArgAs(IRGenFunction &IGF,
                        llvm::Function::arg_iterator &it,
                        const TypeInfo &type,
                        StringRef name) {
  llvm::Value *arg = getArg(it, name);
  llvm::Value *result =
    IGF.Builder.CreateBitCast(arg, type.getStorageType()->getPointerTo());
  return type.getAddressForPointer(result);
}

/// Get the next argument as a pointer to the given storage type.
static Address getArgAsBuffer(IRGenFunction &IGF,
                              llvm::Function::arg_iterator &it,
                              StringRef name) {
  llvm::Value *arg = getArg(it, name);
  return Address(arg, getFixedBufferAlignment(IGF.IGM));
}

/// Get the next argument and use it as the 'self' type metadata.
static void getArgAsLocalSelfTypeMetadata(IRGenFunction &IGF,
                                          llvm::Function::arg_iterator &it,
                                          CanType abstractType);

/// Build a specific value-witness function.
static void buildValueWitnessFunction(IRGenModule &IGM,
                                      llvm::Function *fn,
                                      ValueWitness index,
                                      FixedPacking packing,
                                      CanType abstractType,
                                      CanType concreteType,
                                      const TypeInfo &type) {
  assert(isValueWitnessFunction(index));

  IRGenFunction IGF(IGM, fn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, fn);

  auto argv = fn->arg_begin();
  switch (index) {
  case ValueWitness::AllocateBuffer: {
    Address buffer = getArgAsBuffer(IGF, argv, "buffer");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    Address result = emitAllocateBuffer(IGF, concreteType, type, packing, buffer);
    result = IGF.Builder.CreateBitCast(result, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(result.getAddress());
    return;
  }

  case ValueWitness::AssignWithCopy: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    type.assignWithCopy(IGF, dest, src, concreteType);
    dest = IGF.Builder.CreateBitCast(dest, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::AssignWithTake: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    type.assignWithTake(IGF, dest, src, concreteType);
    dest = IGF.Builder.CreateBitCast(dest, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::DeallocateBuffer: {
    Address buffer = getArgAsBuffer(IGF, argv, "buffer");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    emitDeallocateBuffer(IGF, concreteType, type, packing, buffer);
    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::Destroy: {
    Address object = getArgAs(IGF, argv, type, "object");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    type.destroy(IGF, object, concreteType);
    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::DestroyBuffer: {
    Address buffer = getArgAsBuffer(IGF, argv, "buffer");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    emitDestroyBuffer(IGF, concreteType, type, packing, buffer);
    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::InitializeBufferWithCopyOfBuffer: {
    Address dest = getArgAsBuffer(IGF, argv, "dest");
    Address src = getArgAsBuffer(IGF, argv, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    Address result =
      emitInitializeBufferWithCopyOfBuffer(IGF, concreteType,
                                           type, packing, dest, src);
    result = IGF.Builder.CreateBitCast(result, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(result.getAddress());
    return;
  }

  case ValueWitness::InitializeBufferWithCopy: {
    Address dest = getArgAsBuffer(IGF, argv, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    Address result =
      emitInitializeBufferWithCopy(IGF, concreteType, type, packing, dest, src);
    result = IGF.Builder.CreateBitCast(result, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(result.getAddress());
    return;
  }

  case ValueWitness::InitializeBufferWithTake: {
    Address dest = getArgAsBuffer(IGF, argv, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    Address result =
      emitInitializeBufferWithTake(IGF, concreteType, type, packing, dest, src);
    result = IGF.Builder.CreateBitCast(result, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(result.getAddress());
    return;
  }

  case ValueWitness::InitializeWithCopy: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    emitInitializeWithCopy(IGF, concreteType, type, dest, src);
    dest = IGF.Builder.CreateBitCast(dest, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::InitializeWithTake: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    emitInitializeWithTake(IGF, concreteType, type, dest, src);
    dest = IGF.Builder.CreateBitCast(dest, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(dest.getAddress());
    return;
  }

  case ValueWitness::ProjectBuffer: {
    Address buffer = getArgAsBuffer(IGF, argv, "buffer");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);

    Address result = emitProjectBuffer(IGF, concreteType, type, packing, buffer);
    result = IGF.Builder.CreateBitCast(result, IGF.IGM.OpaquePtrTy);
    IGF.Builder.CreateRet(result.getAddress());
    return;
  }
      
  case ValueWitness::TypeOf: {
    // Only existentials need bespoke typeof witnesses, which are instantiated
    // by the runtime.
    llvm_unreachable("should always be able to use a standard typeof witness "
                     "from the runtime");
  }
  
  case ValueWitness::StoreExtraInhabitant: {
    Address dest = getArgAs(IGF, argv, type, "dest");
    llvm::Value *index = getArg(argv, "index");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    
    type.storeExtraInhabitant(IGF, index, dest, concreteType);
    IGF.Builder.CreateRetVoid();
    return;
  }

  case ValueWitness::GetExtraInhabitantIndex: {
    Address src = getArgAs(IGF, argv, type, "src");
    getArgAsLocalSelfTypeMetadata(IGF, argv, abstractType);
    
    llvm::Value *idx = type.getExtraInhabitantIndex(IGF, src, concreteType);
    IGF.Builder.CreateRet(idx);
    return;
  }

  // TODO
  case ValueWitness::GetEnumTag:
  case ValueWitness::InplaceProjectEnumData: {
    IGF.Builder.CreateUnreachable();
    return;
  }

  case ValueWitness::Size:
  case ValueWitness::Flags:
  case ValueWitness::Stride:
  case ValueWitness::ExtraInhabitantFlags:
    llvm_unreachable("these value witnesses aren't functions");
  }
  llvm_unreachable("bad value witness kind!");
}

static llvm::Constant *asOpaquePtr(IRGenModule &IGM, llvm::Constant *in) {
  return llvm::ConstantExpr::getBitCast(in, IGM.Int8PtrTy);
}

/// Should we be defining the given helper function?
static llvm::Function *shouldDefineHelper(IRGenModule &IGM,
                                          llvm::Constant *fn) {
  llvm::Function *def = dyn_cast<llvm::Function>(fn);
  if (!def) return nullptr;
  if (!def->empty()) return nullptr;

  def->setLinkage(llvm::Function::LinkOnceODRLinkage);
  def->setVisibility(llvm::Function::HiddenVisibility);
  def->setDoesNotThrow();
  def->setCallingConv(IGM.RuntimeCC);
  return def;
}

/// Return a function which performs an assignment operation on two
/// existentials.
///
/// Existential types are nominal, so we potentially need to cast the
/// function to the appropriate object-pointer type.
static llvm::Constant *getAssignExistentialsFunction(IRGenModule &IGM,
                                                     llvm::Type *objectPtrTy,
                                                     OpaqueExistentialLayout layout) {
  llvm::Type *argTys[] = { objectPtrTy, objectPtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(IGM.VoidTy, argTys, false);

  // __swift_assign_existentials_N is the well-known function for
  // assigning existential types with N witness tables.
  llvm::SmallString<40> fnName;
  llvm::raw_svector_ostream(fnName)
    << "__swift_assign_existentials_" << layout.getNumTables();
  llvm::Constant *fn = IGM.Module.getOrInsertFunction(fnName, fnTy);

  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    IRGenFunction IGF(IGM, def);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, def);

    auto it = def->arg_begin();
    Address dest(it++, getFixedBufferAlignment(IGM));
    Address src(it++, getFixedBufferAlignment(IGM));

    // If doing a self-assignment, we're done.
    llvm::BasicBlock *doneBB = IGF.createBasicBlock("done");
    llvm::BasicBlock *contBB = IGF.createBasicBlock("cont");
    llvm::Value *isSelfAssign =
      IGF.Builder.CreateICmpEQ(dest.getAddress(), src.getAddress(),
                               "isSelfAssign");
    IGF.Builder.CreateCondBr(isSelfAssign, doneBB, contBB);

    // Project down to the buffers.
    IGF.Builder.emitBlock(contBB);
    Address destBuffer = layout.projectExistentialBuffer(IGF, dest);
    Address srcBuffer = layout.projectExistentialBuffer(IGF, src);

    // Load the metadata tables.
    Address destMetadataSlot = layout.projectMetadataRef(IGF, dest);
    llvm::Value *destMetadata = IGF.Builder.CreateLoad(destMetadataSlot);
    llvm::Value *srcMetadata = layout.loadMetadataRef(IGF, src);

    // Check whether the metadata match.
    llvm::BasicBlock *matchBB = IGF.createBasicBlock("match");
    llvm::BasicBlock *noMatchBB = IGF.createBasicBlock("no-match");
    llvm::Value *sameMetadata =
      IGF.Builder.CreateICmpEQ(destMetadata, srcMetadata, "sameMetadata");
    IGF.Builder.CreateCondBr(sameMetadata, matchBB, noMatchBB);

    { // (scope to avoid contaminating other branches with these values)

      // If so, do a direct assignment.
      IGF.Builder.emitBlock(matchBB);

      llvm::Value *destObject =
        emitProjectBufferCall(IGF, destMetadata, destBuffer);
      llvm::Value *srcObject =
        emitProjectBufferCall(IGF, destMetadata, srcBuffer);
      emitAssignWithCopyCall(IGF, destMetadata, destObject, srcObject);
      IGF.Builder.CreateBr(doneBB);
    }

    // Otherwise, destroy and copy-initialize.
    // TODO: should we copy-initialize and then destroy?  That's
    // possible if we copy aside, which is a small expense but
    // always safe.  Otherwise the destroy (which can invoke user code)
    // could see invalid memory at this address.  These are basically
    // the madnesses that boost::variant has to go through, with the
    // advantage of address-invariance.
    IGF.Builder.emitBlock(noMatchBB);

    // Store the metadata ref.
    IGF.Builder.CreateStore(srcMetadata, destMetadataSlot);

    // Store the protocol witness tables.
    unsigned numTables = layout.getNumTables();
    for (unsigned i = 0, e = numTables; i != e; ++i) {
      Address destTableSlot = layout.projectWitnessTable(IGF, dest, i);
      llvm::Value *srcTable = layout.loadWitnessTable(IGF, src, i);

      // Overwrite the old witness table.
      IGF.Builder.CreateStore(srcTable, destTableSlot);
    }

    // Destroy the old value.
    emitDestroyBufferCall(IGF, destMetadata, destBuffer);

    // Copy-initialize with the new value.  Again, pull a value
    // witness table from the source metadata if we can't use a
    // protocol witness table.
    emitInitializeBufferWithCopyOfBufferCall(IGF, srcMetadata,
                                             destBuffer, srcBuffer);
    IGF.Builder.CreateBr(doneBB);

    // All done.
    IGF.Builder.emitBlock(doneBB);
    IGF.Builder.CreateRetVoid();
  }
  return fn;
}

/// Return a function which takes two pointer arguments and returns
/// void immediately.
static llvm::Constant *getNoOpVoidFunction(IRGenModule &IGM) {
  llvm::Type *argTys[] = { IGM.Int8PtrTy, IGM.TypeMetadataPtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(IGM.VoidTy, argTys, false);
  llvm::Constant *fn =
    IGM.Module.getOrInsertFunction("__swift_noop_void_return", fnTy);

  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(IGM.getLLVMContext(), "entry", def);
    IRBuilder B(IGM.getLLVMContext());
    B.SetInsertPoint(entry);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(*IGM.SILMod, B, def);
    B.CreateRetVoid();
  }
  return fn;
}

/// Return a function which takes two pointer arguments and returns
/// the first one immediately.
static llvm::Constant *getReturnSelfFunction(IRGenModule &IGM) {
  llvm::Type *argTys[] = { IGM.Int8PtrTy, IGM.TypeMetadataPtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(IGM.Int8PtrTy, argTys, false);
  llvm::Constant *fn =
    IGM.Module.getOrInsertFunction("__swift_noop_self_return", fnTy);

  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(IGM.getLLVMContext(), "entry", def);
    IRBuilder B(IGM.getLLVMContext());
    B.SetInsertPoint(entry);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(*IGM.SILMod, B, def);
    B.CreateRet(def->arg_begin());
  }
  return fn;
}

/// Return a function which takes three pointer arguments and does a
/// retaining assignWithCopy on the first two: it loads a pointer from
/// the second, retains it, loads a pointer from the first, stores the
/// new pointer in the first, and releases the old pointer.
static llvm::Constant *getAssignWithCopyStrongFunction(IRGenModule &IGM) {
  llvm::Type *ptrPtrTy = IGM.RefCountedPtrTy->getPointerTo();
  llvm::Type *argTys[] = { ptrPtrTy, ptrPtrTy, IGM.WitnessTablePtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(ptrPtrTy, argTys, false);
  llvm::Constant *fn =
    IGM.Module.getOrInsertFunction("__swift_assignWithCopy_strong", fnTy);

  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    IRGenFunction IGF(IGM, def);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, def);
    auto it = def->arg_begin();
    Address dest(it++, IGM.getPointerAlignment());
    Address src(it++, IGM.getPointerAlignment());

    llvm::Value *newValue = IGF.Builder.CreateLoad(src, "new");
    IGF.emitRetainCall(newValue);
    llvm::Value *oldValue = IGF.Builder.CreateLoad(dest, "old");
    IGF.Builder.CreateStore(newValue, dest);
    IGF.emitRelease(oldValue);

    IGF.Builder.CreateRet(dest.getAddress());
  }
  return fn;
}

/// Return a function which takes three pointer arguments and does a
/// retaining assignWithTake on the first two: it loads a pointer from
/// the second, retains it, loads a pointer from the first, stores the
/// new pointer in the first, and releases the old pointer.
static llvm::Constant *getAssignWithTakeStrongFunction(IRGenModule &IGM) {
  llvm::Type *ptrPtrTy = IGM.RefCountedPtrTy->getPointerTo();
  llvm::Type *argTys[] = { ptrPtrTy, ptrPtrTy, IGM.WitnessTablePtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(ptrPtrTy, argTys, false);
  llvm::Constant *fn =
    IGM.Module.getOrInsertFunction("__swift_assignWithTake_strong", fnTy);

  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    IRGenFunction IGF(IGM, def);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, def);

    auto it = def->arg_begin();
    Address dest(it++, IGM.getPointerAlignment());
    Address src(it++, IGM.getPointerAlignment());

    llvm::Value *newValue = IGF.Builder.CreateLoad(src, "new");
    llvm::Value *oldValue = IGF.Builder.CreateLoad(dest, "old");
    IGF.Builder.CreateStore(newValue, dest);
    IGF.emitRelease(oldValue);

    IGF.Builder.CreateRet(dest.getAddress());
  }
  return fn;
}

/// Return a function which takes three pointer arguments and does a
/// retaining initWithCopy on the first two: it loads a pointer from
/// the second, retains it, and stores that in the first.
static llvm::Constant *getInitWithCopyStrongFunction(IRGenModule &IGM) {
  llvm::Type *ptrPtrTy = IGM.RefCountedPtrTy->getPointerTo();
  llvm::Type *argTys[] = { ptrPtrTy, ptrPtrTy, IGM.WitnessTablePtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(ptrPtrTy, argTys, false);
  llvm::Constant *fn =
    IGM.Module.getOrInsertFunction("__swift_initWithCopy_strong", fnTy);

  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    IRGenFunction IGF(IGM, def);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, def);
    auto it = def->arg_begin();
    Address dest(it++, IGM.getPointerAlignment());
    Address src(it++, IGM.getPointerAlignment());

    llvm::Value *newValue = IGF.Builder.CreateLoad(src, "new");
    IGF.emitRetainCall(newValue);
    IGF.Builder.CreateStore(newValue, dest);

    IGF.Builder.CreateRet(dest.getAddress());
  }
  return fn;
}

/// Return a function which takes two pointer arguments, loads a
/// pointer from the first, and calls swift_release on it immediately.
static llvm::Constant *getDestroyStrongFunction(IRGenModule &IGM) {
  llvm::Type *argTys[] = { IGM.Int8PtrPtrTy, IGM.WitnessTablePtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(IGM.VoidTy, argTys, false);
  llvm::Constant *fn =
    IGM.Module.getOrInsertFunction("__swift_destroy_strong", fnTy);

  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    IRGenFunction IGF(IGM, def);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, def);
    Address arg(def->arg_begin(), IGM.getPointerAlignment());
    IGF.emitRelease(IGF.Builder.CreateLoad(arg));
    IGF.Builder.CreateRetVoid();
  }
  return fn;
}

/// Return a function which takes three pointer arguments, memcpys
/// from the second to the first, and returns the first argument.
static llvm::Constant *getMemCpyFunction(IRGenModule &IGM,
                                         const TypeInfo &objectTI) {
  llvm::Type *argTys[] = { IGM.Int8PtrTy, IGM.Int8PtrTy, IGM.TypeMetadataPtrTy };
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(IGM.Int8PtrTy, argTys, false);

  // If we don't have a fixed type, use the standard copy-opaque-POD
  // routine.  It's not quite clear how in practice we'll be able to
  // conclude that something is known-POD without knowing its size,
  // but it's (1) conceivable and (2) needed as a general export anyway.
  auto *fixedTI = dyn_cast<FixedTypeInfo>(&objectTI);
  if (!fixedTI) return IGM.getCopyPODFn();

  // We need to unique by both size and alignment.  Note that we're
  // assuming that it's safe to call a function that returns a pointer
  // at a site that assumes the function returns void.
  llvm::SmallString<40> name;
  {
    llvm::raw_svector_ostream nameStream(name);
    nameStream << "__swift_memcpy";
    nameStream << fixedTI->getFixedSize().getValue();
    nameStream << '_';
    nameStream << fixedTI->getFixedAlignment().getValue();
  }

  llvm::Constant *fn = IGM.Module.getOrInsertFunction(name, fnTy);
  if (llvm::Function *def = shouldDefineHelper(IGM, fn)) {
    IRGenFunction IGF(IGM, def);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, def);

    auto it = def->arg_begin();
    Address dest(it++, fixedTI->getFixedAlignment());
    Address src(it++, fixedTI->getFixedAlignment());
    IGF.emitMemCpy(dest, src, fixedTI->getFixedSize());
    IGF.Builder.CreateRet(dest.getAddress());
  }
  return fn;
}

/// Find a witness to the fact that a type is a value type.
/// Always returns an i8*.
static llvm::Constant *getValueWitness(IRGenModule &IGM,
                                       ValueWitness index,
                                       FixedPacking packing,
                                       CanType abstractType,
                                       CanType concreteType,
                                       const TypeInfo &concreteTI) {
  // Try to use a standard function.
  switch (index) {
  case ValueWitness::DeallocateBuffer:
    if (isNeverAllocated(packing))
      return asOpaquePtr(IGM, getNoOpVoidFunction(IGM));
    goto standard;

  case ValueWitness::DestroyBuffer:
    if (concreteTI.isPOD(ResilienceScope::Local)) {
      if (isNeverAllocated(packing))
        return asOpaquePtr(IGM, getNoOpVoidFunction(IGM));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceScope::Local)) {
      assert(isNeverAllocated(packing));
      return asOpaquePtr(IGM, getDestroyStrongFunction(IGM));
    }
    goto standard;

  case ValueWitness::Destroy:
    if (concreteTI.isPOD(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getNoOpVoidFunction(IGM));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getDestroyStrongFunction(IGM));
    }
    goto standard;

  case ValueWitness::InitializeBufferWithCopyOfBuffer:
  case ValueWitness::InitializeBufferWithCopy:
    if (packing == FixedPacking::OffsetZero) {
      if (concreteTI.isPOD(ResilienceScope::Local)) {
        return asOpaquePtr(IGM, getMemCpyFunction(IGM, concreteTI));
      } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceScope::Local)) {
        return asOpaquePtr(IGM, getInitWithCopyStrongFunction(IGM));
      }
    }
    goto standard;

  case ValueWitness::InitializeBufferWithTake:
    if (packing == FixedPacking::OffsetZero)    
      return asOpaquePtr(IGM, getMemCpyFunction(IGM, concreteTI));
    goto standard;

  case ValueWitness::InitializeWithTake:
    return asOpaquePtr(IGM, getMemCpyFunction(IGM, concreteTI));

  case ValueWitness::AssignWithCopy:
    if (concreteTI.isPOD(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getMemCpyFunction(IGM, concreteTI));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getAssignWithCopyStrongFunction(IGM));
    }
    goto standard;

  case ValueWitness::AssignWithTake:
    if (concreteTI.isPOD(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getMemCpyFunction(IGM, concreteTI));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getAssignWithTakeStrongFunction(IGM));
    }
    goto standard;

  case ValueWitness::InitializeWithCopy:
    if (concreteTI.isPOD(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getMemCpyFunction(IGM, concreteTI));
    } else if (concreteTI.isSingleSwiftRetainablePointer(ResilienceScope::Local)) {
      return asOpaquePtr(IGM, getInitWithCopyStrongFunction(IGM));
    }
    goto standard;

  case ValueWitness::AllocateBuffer:
  case ValueWitness::ProjectBuffer:
    if (packing == FixedPacking::OffsetZero)
      return asOpaquePtr(IGM, getReturnSelfFunction(IGM));
    goto standard;

  case ValueWitness::TypeOf:
    /// Class types require dynamic type lookup.
    if (ClassDecl *cd = concreteType->getClassOrBoundGenericClass()) {
      if (hasKnownSwiftMetadata(IGM, cd))
        return asOpaquePtr(IGM, IGM.getObjectTypeofFn());
      return asOpaquePtr(IGM, IGM.getObjCTypeofFn());
    } else if (!concreteType->isExistentialType()) {
      // Other non-existential types have static metadata.
      return asOpaquePtr(IGM, IGM.getStaticTypeofFn());
    }
    goto standard;
      
  case ValueWitness::Size: {
    if (auto value = concreteTI.getStaticSize(IGM))
      return llvm::ConstantExpr::getIntToPtr(value, IGM.Int8PtrTy);

    // Just fill in null here if the type can't be statically laid out.
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }

  case ValueWitness::Flags: {
    // If we locally know that the type has fixed layout, we can emit
    // meaningful flags for it.
    if (auto *fixedTI = dyn_cast<FixedTypeInfo>(&concreteTI)) {
      uint64_t flags = fixedTI->getFixedAlignment().getValue() - 1;
      if (!fixedTI->isPOD(ResilienceScope::Local))
        flags |= ValueWitnessFlags::IsNonPOD;
      assert(packing == FixedPacking::OffsetZero ||
             packing == FixedPacking::Allocate);
      if (packing != FixedPacking::OffsetZero)
        flags |= ValueWitnessFlags::IsNonInline;
      
      if (fixedTI->getFixedExtraInhabitantCount(IGM) > 0)
        flags |= ValueWitnessFlags::Enum_HasExtraInhabitants;
      
      auto value = IGM.getSize(Size(flags));
      return llvm::ConstantExpr::getIntToPtr(value, IGM.Int8PtrTy);
    }

    // Just fill in null here if the type can't be statically laid out.
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }

  case ValueWitness::Stride: {
    if (auto value = concreteTI.getStaticStride(IGM))
      return llvm::ConstantExpr::getIntToPtr(value, IGM.Int8PtrTy);

    // Just fill in null here if the type can't be statically laid out.
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }
  
  case ValueWitness::StoreExtraInhabitant:
  case ValueWitness::GetExtraInhabitantIndex: {
    assert(concreteTI.mayHaveExtraInhabitants(IGM));
    
    goto standard;
  }
    
  case ValueWitness::ExtraInhabitantFlags: {
    assert(concreteTI.mayHaveExtraInhabitants(IGM));
    
    // If we locally know that the type has fixed layout, we can emit
    // meaningful flags for it.
    if (auto *fixedTI = dyn_cast<FixedTypeInfo>(&concreteTI)) {
      uint64_t numExtraInhabitants = fixedTI->getFixedExtraInhabitantCount(IGM);
      assert(numExtraInhabitants <= ExtraInhabitantFlags::NumExtraInhabitantsMask);
      auto value = IGM.getSize(Size(numExtraInhabitants));
      return llvm::ConstantExpr::getIntToPtr(value, IGM.Int8PtrTy);
    }
    
    // Otherwise, just fill in null here if the type can't be statically
    // queried for extra inhabitants.
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }
  
  /// TODO:
  case ValueWitness::GetEnumTag:
  case ValueWitness::InplaceProjectEnumData:
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }
  llvm_unreachable("bad value witness kind");

 standard:
  llvm::Function *fn =
    IGM.getAddrOfValueWitness(abstractType, index, ForDefinition);
  if (fn->empty())
    buildValueWitnessFunction(IGM, fn, index, packing, abstractType,
                              concreteType, concreteTI);
  return asOpaquePtr(IGM, fn);
}

static void emitPolymorphicArgumentsWithInput(IRGenFunction &IGF,
                                              CanSILFunctionType origFnType,
                                              CanType substInputType,
                                              ArrayRef<Substitution> subs,
                                              Explosion &out);

namespace {
  /// A class which lays out a specific conformance to a protocol.
  class WitnessTableBuilder : public WitnessVisitor<WitnessTableBuilder> {
    SmallVectorImpl<llvm::Constant*> &Table;
    CanType ConcreteType;
    GenericParamList *ConcreteGenerics = nullptr;
    const TypeInfo &ConcreteTI;
    const ProtocolConformance &Conformance;
    ArrayRef<Substitution> Substitutions;
    ArrayRef<SILWitnessTable::Entry> SILEntries;

    void computeSubstitutionsForType() {
      // FIXME: This is a bit of a hack; the AST doesn't directly encode
      // substitutions for the conformance of a generic type to a
      // protocol, so we have to dig them out.
      Type ty = ConcreteType;
      while (ty) {
        if (auto nomTy = ty->getAs<NominalType>())
          ty = nomTy->getParent();
        else
          break;
      }
      if (ty) {
        if (auto boundTy = ty->getAs<BoundGenericType>()) {
          ConcreteGenerics = boundTy->getDecl()->getGenericParams();
          Substitutions = boundTy->getSubstitutions(/*FIXME:*/nullptr, nullptr);
        } else {
          assert(!ty || !ty->isSpecialized());
        }
      }
    }

  public:
    WitnessTableBuilder(IRGenModule &IGM,
                        SmallVectorImpl<llvm::Constant*> &table,
                        SILWitnessTable *SILWT)
      : WitnessVisitor(IGM), Table(table),
        ConcreteType(SILWT->getConformance()->getType()->getCanonicalType()),
        ConcreteTI(
               IGM.getTypeInfoForUnlowered(SILWT->getConformance()->getType())),
        Conformance(*SILWT->getConformance()),
        SILEntries(SILWT->getEntries())
    {
      computeSubstitutionsForType();
    }

    /// A base protocol is witnessed by a pointer to the conformance
    /// of this type to that protocol.
    void addOutOfLineBaseProtocol(ProtocolDecl *baseProto) {
      auto &entry = SILEntries.front();
      (void)entry;
      assert(entry.getKind() == SILWitnessTable::BaseProtocol
             && "sil witness table does not match protocol");
      assert(entry.getBaseProtocolWitness().Requirement == baseProto
             && "sil witness table does not match protocol");
      SILEntries = SILEntries.slice(1);
      
      // TODO: Use the witness entry instead of falling through here.
      
      // Look for a protocol type info.
      const ProtocolInfo &basePI = IGM.getProtocolInfo(baseProto);
      const ProtocolConformance *astConf
        = Conformance.getInheritedConformance(baseProto);
      const ConformanceInfo &conf =
        basePI.getConformance(IGM, ConcreteType, ConcreteTI,
                              baseProto, *astConf);

      llvm::Constant *baseWitness = conf.tryGetConstantTable(IGM);
      assert(baseWitness && "couldn't get a constant table!");
      Table.push_back(asOpaquePtr(IGM, baseWitness));
    }

    void addMethodFromSILWitnessTable(FuncDecl *iface) {
      auto &entry = SILEntries.front();
      assert(entry.getKind() == SILWitnessTable::Method
             && "sil witness table does not match protocol");
      assert(entry.getMethodWitness().Requirement.getDecl() == iface
             && "sil witness table does not match protocol");
      
      llvm::Constant *witness
        = IGM.getAddrOfSILFunction(entry.getMethodWitness().Witness,
                                   ResilienceExpansion::Minimal, NotForDefinition);
      witness = llvm::ConstantExpr::getBitCast(witness, IGM.Int8PtrTy);
      Table.push_back(witness);
      
      SILEntries = SILEntries.slice(1);
      return;
    }
    
    void addStaticMethod(FuncDecl *iface) {
      return addMethodFromSILWitnessTable(iface);
    }

    void addInstanceMethod(FuncDecl *iface) {
      return addMethodFromSILWitnessTable(iface);
    }
    
    void addAssociatedType(AssociatedTypeDecl *ty) {
      auto &entry = SILEntries.front();
      (void)entry;
      assert(entry.getKind() == SILWitnessTable::AssociatedType
             && "sil witness table does not match protocol");
      assert(entry.getAssociatedTypeWitness().Requirement == ty
             && "sil witness table does not match protocol");
      
      SILEntries = SILEntries.slice(1);
      
      // FIXME: Use info from SILWitnessTable instead of falling through.
      
      // Determine whether the associated type has static metadata. If it
      // doesn't, then this witness table is a template that requires runtime
      // instantiation.
      
      // FIXME: Add static type metadata.
      Table.push_back(llvm::ConstantPointerNull::get(IGM.Int8PtrTy));

      // FIXME: Add static witness tables for type conformances.
      for (auto protocol : ty->getProtocols()) {
        (void)protocol;
        auto &entry = SILEntries.front();
        (void)entry;
        assert(entry.getKind() == SILWitnessTable::AssociatedTypeProtocol
               && "sil witness table does not match protocol");
        assert(entry.getAssociatedTypeProtocolWitness().Requirement == ty
               && "sil witness table does not match protocol");
        assert(entry.getAssociatedTypeProtocolWitness().Protocol == protocol
               && "sil witness table does not match protocol");
        
        SILEntries = SILEntries.slice(1);
      
        // FIXME: Use info from SILWitnessTable instead of falling through.
        // FIXME: Add static witness table reference.
        Table.push_back(llvm::ConstantPointerNull::get(IGM.Int8PtrTy));
      }
    }
  };
}

/// Collect the value witnesses for a particular type.
static void addValueWitnesses(IRGenModule &IGM, FixedPacking packing,
                              CanType abstractType,
                              CanType concreteType, const TypeInfo &concreteTI,
                              SmallVectorImpl<llvm::Constant*> &table) {
  for (unsigned i = 0; i != NumRequiredValueWitnesses; ++i) {
    table.push_back(getValueWitness(IGM, ValueWitness(i),
                                    packing, abstractType, concreteType,
                                    concreteTI));
  }
  if (concreteTI.mayHaveExtraInhabitants(IGM)) {
    for (auto i = unsigned(ValueWitness::First_ExtraInhabitantValueWitness);
         i <= unsigned(ValueWitness::Last_ExtraInhabitantValueWitness);
         ++i) {
      table.push_back(getValueWitness(IGM, ValueWitness(i), packing,
                                      abstractType, concreteType, concreteTI));
    }
  }
}

/// True if a type has a generic-parameter-dependent value witness table.
/// Currently, This is true if the size and/or alignment of the type is
/// dependent on its generic parameters.
bool irgen::hasDependentValueWitnessTable(IRGenModule &IGM, CanType ty) {
  if (auto ugt = dyn_cast<UnboundGenericType>(ty))
    ty = ugt->getDecl()->getDeclaredTypeInContext()->getCanonicalType();
  
  return !IGM.getTypeInfoForUnlowered(ty).isFixedSize();
}

static void addValueWitnessesForAbstractType(IRGenModule &IGM,
                                 CanType abstractType,
                                 SmallVectorImpl<llvm::Constant*> &witnesses) {
  // Instantiate unbound generic types on their context archetypes.
  CanType concreteType = abstractType;
  if (auto ugt = dyn_cast<UnboundGenericType>(abstractType)) {
    concreteType = ugt->getDecl()->getDeclaredTypeInContext()->getCanonicalType();
  }
  
  auto &concreteTI = IGM.getTypeInfoForUnlowered(concreteType);
  FixedPacking packing = concreteTI.getFixedPacking(IGM);
  
  addValueWitnesses(IGM, packing, abstractType,
                    concreteType, concreteTI, witnesses);
}

/// Emit a value-witness table for the given type, which is assumed to
/// be non-dependent.
llvm::Constant *irgen::emitValueWitnessTable(IRGenModule &IGM,
                                             CanType abstractType) {
  // We shouldn't emit global value witness tables for generic type instances.
  assert(!isa<BoundGenericType>(abstractType) &&
         "emitting VWT for generic instance");
  
  // We shouldn't emit global value witness tables for non-fixed-layout types.
  assert(!hasDependentValueWitnessTable(IGM, abstractType) &&
         "emitting global VWT for dynamic-layout type");
  
  SmallVector<llvm::Constant*, MaxNumValueWitnesses> witnesses;
  addValueWitnessesForAbstractType(IGM, abstractType, witnesses);

  auto tableTy = llvm::ArrayType::get(IGM.Int8PtrTy, witnesses.size());
  auto table = llvm::ConstantArray::get(tableTy, witnesses);

  auto addr = IGM.getAddrOfValueWitnessTable(abstractType, table->getType());
  auto global = cast<llvm::GlobalVariable>(addr);
  global->setConstant(true);
  global->setInitializer(table);

  return llvm::ConstantExpr::getBitCast(global, IGM.WitnessTablePtrTy);
}

/// Emit the elements of a dependent value witness table template into a
/// vector.
void irgen::emitDependentValueWitnessTablePattern(IRGenModule &IGM,
                                    CanType abstractType,
                                    SmallVectorImpl<llvm::Constant*> &fields) {
  // We shouldn't emit global value witness tables for generic type instances.
  assert(!isa<BoundGenericType>(abstractType) &&
         "emitting VWT for generic instance");
  
  // We shouldn't emit global value witness tables for fixed-layout types.
  assert(hasDependentValueWitnessTable(IGM, abstractType) &&
         "emitting VWT pattern for fixed-layout type");

  addValueWitnessesForAbstractType(IGM, abstractType, fields);
}

/// Do a memoized witness-table layout for a protocol.
const ProtocolInfo &IRGenModule::getProtocolInfo(ProtocolDecl *protocol) {
  return Types.getProtocolInfo(protocol);
}

/// Do a memoized witness-table layout for a protocol.
const ProtocolInfo &TypeConverter::getProtocolInfo(ProtocolDecl *protocol) {
  // Check whether we've already translated this protocol.
  auto it = Protocols.find(protocol);
  if (it != Protocols.end()) return *it->second;

  // If not, layout the protocol's witness table.
  WitnessTableLayout layout(IGM);
  layout.visit(protocol);

  // Create a ProtocolInfo object from the layout.
  ProtocolInfo *info = ProtocolInfo::create(layout.getNumWitnesses(),
                                            layout.getEntries());
  info->NextConverted = FirstProtocol;
  FirstProtocol = info;

  // Memoize.
  Protocols.insert(std::make_pair(protocol, info));

  // Done.
  return *info;
}

/// Allocate a new ProtocolInfo.
ProtocolInfo *ProtocolInfo::create(unsigned numWitnesses,
                                   ArrayRef<WitnessTableEntry> table) {
  unsigned numEntries = table.size();
  size_t bufferSize =
    sizeof(ProtocolInfo) + numEntries * sizeof(WitnessTableEntry);
  void *buffer = ::operator new(bufferSize);
  return new(buffer) ProtocolInfo(numWitnesses, table);
}

ProtocolInfo::~ProtocolInfo() {
  for (auto &conf : Conformances) {
    delete conf.second;
  }
}

/// Find the conformance information for a protocol.
const ConformanceInfo &
ProtocolInfo::getConformance(IRGenModule &IGM, CanType concreteType,
                             const TypeInfo &concreteTI,
                             ProtocolDecl *protocol,
                             const ProtocolConformance &conformance) const {
  // Check whether we've already cached this.
  auto it = Conformances.find(&conformance);
  if (it != Conformances.end()) return *it->second;

  // Drill down to the root normal conformance.
  auto normalConformance = conformance.getRootNormalConformance();
  
  // Emit a direct-referencing conformance.
  // FIXME: For some conformances we need to do lazy initialization or runtime
  // instantiation.
  ConformanceInfo *info = new DirectConformanceInfo(normalConformance);
  auto res = Conformances.insert(std::make_pair(&conformance, info));
  return *res.first->second;
}

void IRGenModule::emitSILWitnessTable(SILWitnessTable *wt) {
  // Build the witnesses.
  SmallVector<llvm::Constant*, 32> witnesses;
  WitnessTableBuilder(*this, witnesses, wt)
    .visit(wt->getConformance()->getProtocol());
  
  // Produce the initializer value.
  auto tableTy = llvm::ArrayType::get(FunctionPtrTy, witnesses.size());
  auto initializer = llvm::ConstantArray::get(tableTy, witnesses);
  
  auto global = cast<llvm::GlobalVariable>(
                         getAddrOfWitnessTable(wt->getConformance(), tableTy));
  global->setConstant(true);
  global->setInitializer(initializer);
  
  // TODO: We should record what access mode the witness table requires:
  // direct, lazily initialized, or runtime instantiated template.
}

static const TypeInfo *createExistentialTypeInfo(IRGenModule &IGM,
                                                 llvm::StructType *type,
                                        ArrayRef<ProtocolDecl*> protocols) {
  assert(type->isOpaque() && "creating existential type in concrete struct");

  SmallVector<llvm::Type*, 5> fields;
  SmallVector<ProtocolEntry, 4> entries;

  // The first field is the metadata reference.
  fields.push_back(IGM.TypeMetadataPtrTy);

  bool requiresClass = false;
  
  for (auto protocol : protocols) {
    // The existential container is class-constrained if any of its protocol
    // constraints are.
    requiresClass |= protocol->requiresClass();
    
    // ObjC protocols need no layout or witness table info. All dispatch is done
    // through objc_msgSend.
    if (!requiresProtocolWitnessTable(protocol))
      continue;
    
    // Find the protocol layout.
    const ProtocolInfo &impl = IGM.getProtocolInfo(protocol);
    entries.push_back(ProtocolEntry(protocol, impl));

    // Each protocol gets a witness table.
    fields.push_back(IGM.WitnessTablePtrTy);
  }
  
  // If the existential is class, lower it to a class
  // existential representation.
  if (requiresClass) {
    // Add the class instance pointer to the fields.
    fields.push_back(IGM.UnknownRefCountedPtrTy);
    // Drop the type metadata pointer. We can get it from the class instance.
    ArrayRef<llvm::Type*> ClassFields = fields;
    ClassFields = ClassFields.slice(1);
    
    type->setBody(ClassFields);
    
    Alignment align = IGM.getPointerAlignment();
    Size size = ClassFields.size() * IGM.getPointerSize();
    
    llvm::BitVector spareBits;
    // BitVector doesn't have an append method...
    auto append = [](llvm::BitVector &b, const llvm::BitVector &x) {
      auto bSize = b.size(), xSize = x.size();
      b.resize(bSize + xSize);
      
      for (unsigned i = 0; i < xSize; ++i) {
        b[bSize + i] = x[i];
      }
    };
    
    // The witness table fields are pointers and have pointer spare bits.
    for (unsigned i = 0, e = ClassFields.size() - 1; i < e; ++i) {
      append(spareBits, IGM.TargetInfo.PointerSpareBits);
    }
    // The class pointer is a heap object reference and has heap object spare
    // bits.
    append(spareBits, IGM.getHeapObjectSpareBits());
    
    return ClassExistentialTypeInfo::create(type,
                                            size, std::move(spareBits), align,
                                            entries);
  }

  OpaqueExistentialLayout layout(entries.size());

  // Add the value buffer to the fields.
  fields.push_back(IGM.getFixedBufferTy());
  type->setBody(fields);

  Alignment align = getFixedBufferAlignment(IGM);
  assert(align >= IGM.getPointerAlignment());

  Size size = layout.getBufferOffset(IGM);
  assert(size.roundUpToAlignment(align) == size);
  size += getFixedBufferSize(IGM);

  return OpaqueExistentialTypeInfo::create(type, size, align, entries);
}

const TypeInfo *TypeConverter::convertProtocolType(ProtocolType *T) {
  // Protocol types are nominal.
  llvm::StructType *type = IGM.createNominalType(T->getDecl());
  return createExistentialTypeInfo(IGM, type, T->getDecl());
}

const TypeInfo *
TypeConverter::convertProtocolCompositionType(ProtocolCompositionType *T) {
  // Protocol composition types are not nominal, but we name them anyway.
  llvm::StructType *type = IGM.createNominalType(T);

  // Find the canonical protocols.  There might not be any.
  SmallVector<ProtocolDecl*, 4> protocols;
  bool isExistential = T->isExistentialType(protocols);
  assert(isExistential); (void) isExistential;

  return createExistentialTypeInfo(IGM, type, protocols);
}

const TypeInfo *TypeConverter::convertArchetypeType(ArchetypeType *archetype) {
  assert(isExemplarArchetype(archetype) && "lowering non-exemplary archetype");
  
  // Compute layouts for the protocols we ascribe to.
  SmallVector<ProtocolEntry, 4> protocols;
  for (auto protocol : archetype->getConformsTo()) {
    const ProtocolInfo &impl = IGM.getProtocolInfo(protocol);
    protocols.push_back(ProtocolEntry(protocol, impl));
  }

  // If the archetype is class-constrained, use a class pointer
  // representation.
  if (archetype->requiresClass()) {
    // Fully general archetypes can't be assumed to have a Swift refcount.
    bool swiftRefcount = false;
    llvm::PointerType *reprTy = IGM.UnknownRefCountedPtrTy;
    
    // If the archetype has a superclass constraint, it has at least the
    // retain semantics of its superclass, and it can be represented with
    // the supertype's pointer type.
    if (Type super = archetype->getSuperclass()) {
      ClassDecl *superClass = super->getClassOrBoundGenericClass();
      swiftRefcount = hasSwiftRefcount(IGM, superClass);
      
      auto &superTI = IGM.getTypeInfoForUnlowered(super);
      reprTy = cast<llvm::PointerType>(superTI.StorageType);
    }
    
    return ClassArchetypeTypeInfo::create(reprTy,
                                      IGM.getPointerSize(),
                                      IGM.getHeapObjectSpareBits(),
                                      IGM.getPointerAlignment(),
                                      protocols, swiftRefcount);
  }
  
  // Otherwise, for now, always use an opaque indirect type.
  llvm::Type *storageType = IGM.OpaquePtrTy->getElementType();
  return OpaqueArchetypeTypeInfo::create(storageType, protocols);
}

/// Inform IRGenFunction that the given archetype has the given value
/// witness value within this scope.
void IRGenFunction::bindArchetype(ArchetypeType *archetype,
                                  llvm::Value *metadata,
                                  ArrayRef<llvm::Value*> wtables) {
  // Set the metadata pointer.
  metadata->setName(archetype->getFullName());
  setMetadataRef(*this, archetype, metadata);

  // Set the protocol witness tables.

  unsigned wtableI = 0;
  for (unsigned i = 0, e = wtables.size(); i != e; ++i) {
    auto proto = archetype->getConformsTo()[i];
    if (!requiresProtocolWitnessTable(proto)) continue;
    auto wtable = wtables[wtableI++];
    wtable->setName(Twine(archetype->getFullName()) + "." +
                      proto->getName().str());
    setWitnessTable(*this, archetype, i, wtable);
  }
  assert(wtableI == wtables.size());
}

/// True if a function's signature in LLVM carries polymorphic parameters.
/// Generic functions and protocol witnesses carry polymorphic parameters.
bool irgen::hasPolymorphicParameters(CanSILFunctionType ty) {
  switch (ty->getAbstractCC()) {
  case AbstractCC::C:
    // Should never be polymorphic.
    assert(!ty->isPolymorphic() && "polymorphic C function?!");
  case AbstractCC::ObjCMethod:
    // An ObjC archetype_method reference will notionally have polymorphic type
    // <Self: P> (...) -> (...), but there are no polymorphic parameters that
    // can't be solved from the usual ObjC metadata.
    return false;
      
  case AbstractCC::Freestanding:
  case AbstractCC::Method:
    return ty->isPolymorphic();
      
  case AbstractCC::WitnessMethod:
    // Always carries polymorphic parameters for the Self type.
    return true;
  }
}

namespace {
  struct Fulfillment {
    Fulfillment() = default;
    Fulfillment(unsigned depth, unsigned index) : Depth(depth), Index(index) {}

    /// The distance up the metadata chain.
    /// 0 is the origin metadata, 1 is the parent of that, etc.
    unsigned Depth;

    /// The generic argument index.
    unsigned Index;
  };
  typedef std::pair<ArchetypeType*, ProtocolDecl*> FulfillmentKey;

  /// A class for computing how to pass arguments to a polymorphic
  /// function.  The subclasses of this are the places which need to
  /// be updated if the convention changes.
  class PolymorphicConvention {
  public:
    enum class SourceKind {
      /// There is no source of additional information.
      None,

      /// The polymorphic arguments are derived from a source class
      /// pointer.
      ClassPointer,
      
      /// The polymorphic arguments are derived from a type metadata
      /// pointer.
      Metadata,

      /// The polymorphic arguments are passed from generic type
      /// metadata for the origin type.
      GenericLValueMetadata,
      
      /// The polymorphic arguments are derived from a Self type binding
      /// passed via the WitnessMethod convention.
      WitnessSelf,
      
      /// The polymorphic arguments are derived from a Self type binding
      /// embedded in a thick WitnessMethod function value.
      WitnessExtraData,
    };

  protected:
    CanSILFunctionType FnType;
    SourceKind TheSourceKind = SourceKind::None;
    SmallVector<NominalTypeDecl*, 4> TypesForDepths;

    llvm::DenseMap<FulfillmentKey, Fulfillment> Fulfillments;

  public:
    PolymorphicConvention(CanSILFunctionType fnType)
        : FnType(fnType) {
      assert(hasPolymorphicParameters(fnType));

      // Protocol witnesses always derive all polymorphic parameter information
      // from the Self argument. We also *cannot* consider other arguments;
      // doing so would potentially make the signature incompatible with other
      // witnesses for the same method.
      if (fnType->getAbstractCC() == AbstractCC::WitnessMethod) {
        // If the type is thick, the metadata is derived from the extra data
        // in the function value. Otherwise, it's provided from the type of the
        // self argument.
        if (fnType->isThin())
          TheSourceKind = SourceKind::WitnessSelf;
        else
          TheSourceKind = SourceKind::WitnessExtraData;
          
        // Testify to archetypes in the Self type.
        auto params = fnType->getParameters();
        CanType selfTy = params.back().getType();
        if (auto metaTy = dyn_cast<MetatypeType>(selfTy))
          selfTy = metaTy.getInstanceType();
        
        if (auto nomTy = dyn_cast<NominalType>(selfTy))
          considerNominalType(nomTy, 0);
        else if (auto bgTy = dyn_cast<BoundGenericType>(selfTy))
          considerBoundGenericType(bgTy, 0);
        else if (auto archeTy = dyn_cast<ArchetypeType>(selfTy))
          considerArchetype(archeTy, archeTy, 0, 0);
        else
          llvm_unreachable("witness for non-nominal type?!");
        
        return;
      }
      
      // We don't need to pass anything extra as long as all of the
      // archetypes (and their requirements) are producible from the
      // class-pointer argument.

      // Just consider the 'self' parameter for now.
      auto params = fnType->getParameters();
      if (params.empty()) return;
      SourceKind source = considerParameter(params.back());

      // If we didn't fulfill anything, there's no source.
      if (Fulfillments.empty()) return;

      TheSourceKind = source;
    }
    
    /// Extract archetype metadata for a value witness function of the given
    /// type.
    PolymorphicConvention(NominalTypeDecl *ntd)
      : FnType(getNotionalFunctionType(ntd))
    {
      TheSourceKind = SourceKind::Metadata;

      auto paramType = FnType->getParameters()[0].getType();
      considerBoundGenericType(cast<BoundGenericType>(paramType), 0);
    }
    
    SourceKind getSourceKind() const { return TheSourceKind; }

    Range<NestedArchetypeIterator> getAllArchetypes() const {
      if (auto gp = FnType->getGenericParams())
        return gp->getAllNestedArchetypes();
      return NestedArchetypeIterator::emptyRange();
    }
    
  private:
    static CanSILFunctionType getNotionalFunctionType(NominalTypeDecl *D) {
      ASTContext &ctx = D->getASTContext();
      SILFunctionType::ExtInfo extInfo(AbstractCC::Method,
                                       /*thin*/ true,
                                       /*noreturn*/ false);
      SILParameterInfo param(D->getDeclaredTypeInContext()->getCanonicalType(),
                             ParameterConvention::Direct_Owned);
      SILResultInfo result(TupleType::getEmpty(ctx),
                           ResultConvention::Unowned);
      SILParameterInfo iparam(D->getDeclaredInterfaceType()->getCanonicalType(),
                              ParameterConvention::Direct_Owned);
      
      GenericSignature *sig = nullptr;
      auto sigArrays = D->getGenericSignatureOfContext();
      if (!sigArrays.first.empty() || !sigArrays.second.empty())
        sig = GenericSignature::getCanonical(sigArrays.first,
                                             sigArrays.second,
                                             ctx);
      
      return SILFunctionType::get(D->getGenericParamsOfContext(),
                                  sig,
                                  extInfo,
                                  ParameterConvention::Direct_Unowned,
                                  param, result, iparam, result, ctx);
    }

    SourceKind considerParameter(SILParameterInfo param) {
      auto type = param.getType();
      switch (param.getConvention()) {
      // Out-parameters don't give us a value we can use.
      case ParameterConvention::Indirect_Out:
        return SourceKind::None;

      // In-parameters do, but right now we don't bother, for no good reason.
      case ParameterConvention::Indirect_In:
        return SourceKind::None;

      case ParameterConvention::Indirect_Inout:
        if (auto nomTy = dyn_cast<NominalType>(type)) {
          considerNominalType(nomTy, 0);
          return SourceKind::GenericLValueMetadata;
        } else if (auto boundTy = dyn_cast<BoundGenericType>(type)) {
          considerBoundGenericType(boundTy, 0);
          return SourceKind::GenericLValueMetadata;
        }
        return SourceKind::None;

      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Unowned:
      case ParameterConvention::Direct_Guaranteed:
        if (auto classTy = dyn_cast<ClassType>(type)) {
          considerNominalType(classTy, 0);
          return SourceKind::ClassPointer;
        } else if (auto boundTy = dyn_cast<BoundGenericClassType>(type)) {
          considerBoundGenericType(boundTy, 0);
          return SourceKind::ClassPointer;
        } else if (auto metatypeTy = dyn_cast<MetatypeType>(type)) {
          CanType objTy = metatypeTy.getInstanceType();
          if (auto nomTy = dyn_cast<ClassType>(objTy)) {
            considerNominalType(nomTy, 0);
            return SourceKind::Metadata;
          } else if (auto boundTy = dyn_cast<BoundGenericClassType>(objTy)) {
            considerBoundGenericType(boundTy, 0);
            return SourceKind::Metadata;
          }
        }
        return SourceKind::None;
      }
      llvm_unreachable("bad parameter convention");
    }
    
    void considerParentType(CanType parent, unsigned depth) {
      // We might not have a parent type.
      if (!parent) return;

      // If we do, it has to be nominal one way or another.
      depth++;
      if (auto nom = dyn_cast<NominalType>(parent))
        considerNominalType(nom, depth);
      else
        considerBoundGenericType(cast<BoundGenericType>(parent), depth);
    }

    void considerNominalType(NominalType *type, unsigned depth) {
      assert(TypesForDepths.size() == depth);
      TypesForDepths.push_back(type->getDecl());

      // Nominal types add no generic arguments themselves, but they
      // may have the arguments of their parents.
      considerParentType(CanType(type->getParent()), depth);
    }

    void considerBoundGenericType(BoundGenericType *type, unsigned depth) {
      assert(TypesForDepths.size() == depth);
      TypesForDepths.push_back(type->getDecl());

      auto params = type->getDecl()->getGenericParams()->getAllArchetypes();
      auto substitutions = type->getSubstitutions(/*FIXME:*/nullptr, nullptr);
      assert(params.size() >= substitutions.size() &&
             "generic decl archetypes should parallel generic type subs");

      for (unsigned i = 0, e = substitutions.size(); i != e; ++i) {
        auto sub = substitutions[i];
        assert(sub.Archetype == params[i] &&
               "substitution does not match archetype!");
        CanType arg = sub.Replacement->getCanonicalType();

        // Right now, we can only pull things out of the direct
        // arguments, not out of nested metadata.  For example, this
        // prevents us from realizing that we can rederive T and U in the
        // following:
        //   \forall T U . Vector<T->U> -> ()
        if (auto argArchetype = dyn_cast<ArchetypeType>(arg)) {
          // Find the archetype from the generic type.
          considerArchetype(argArchetype, params[i], depth, i);
        }
      }

      // Match against the parent first.  The polymorphic type
      // will start with any arguments from the parent.
      considerParentType(CanType(type->getParent()), depth);
    }

    /// We found a reference to the arg archetype at the given depth
    /// and index.  Add any fulfillments this gives us.
    void considerArchetype(ArchetypeType *arg, ArchetypeType *param,
                           unsigned depth, unsigned index) {
      // First, record that we can find this archetype at this point.
      addFulfillment(arg, nullptr, depth, index);

      // Now consider each of the protocols that the parameter guarantees.
      for (auto protocol : param->getConformsTo()) {
        // If arg == param, the second check is always true.  This is
        // a fast path for some common cases where we're defining a
        // method within the type we're matching against.
        if (arg == param || requiresFulfillment(arg, protocol))
          addFulfillment(arg, protocol, depth, index);
      }
    }

    /// Does the given archetype require the given protocol to be fulfilled?
    static bool requiresFulfillment(ArchetypeType *arg, ProtocolDecl *proto) {
      // TODO: protocol inheritance should be considered here somehow.
      for (auto argProto : arg->getConformsTo()) {
        if (argProto == proto)
          return true;
      }
      return false;
    }

    /// Testify that there's a fulfillment at the given depth and level.
    void addFulfillment(ArchetypeType *arg, ProtocolDecl *proto,
                        unsigned depth, unsigned index) {
      // Only add a fulfillment if it's not enough information otherwise.
      auto key = FulfillmentKey(arg, proto);
      if (!Fulfillments.count(key))
        Fulfillments.insert(std::make_pair(key, Fulfillment(depth, index)));
    }
  };

  /// A class for binding type parameters of a generic function.
  class EmitPolymorphicParameters : public PolymorphicConvention {
    IRGenFunction &IGF;
    SmallVector<llvm::Value*, 4> MetadataForDepths;

  public:
    EmitPolymorphicParameters(IRGenFunction &IGF,
                              CanSILFunctionType fnType)
      : PolymorphicConvention(fnType), IGF(IGF) {}

    void emit(Explosion &in);
    
    /// Emit polymorphic parameters for a generic value witness.
    EmitPolymorphicParameters(IRGenFunction &IGF, NominalTypeDecl *ntd)
      : PolymorphicConvention(ntd), IGF(IGF) {}
    
    void emitForGenericValueWitness(llvm::Value *selfMeta);

  private:
    // Emit metadata bindings after the source, if any, has been bound.
    void emitWithSourceBound(Explosion &in);
    
    CanType getArgType() const {
      return FnType->getParameters().back().getType();
    }

    /// Emit the source value for parameters.
    llvm::Value *emitSourceForParameters(Explosion &in) {
      switch (getSourceKind()) {
      case SourceKind::None:
        return nullptr;

      case SourceKind::Metadata:
        return in.getLastClaimed();
          
      case SourceKind::ClassPointer:
        return emitHeapMetadataRefForHeapObject(IGF, in.getLastClaimed(),
                                                getArgType(),
                                                /*suppress cast*/ true);

      case SourceKind::GenericLValueMetadata: {
        llvm::Value *metatype = in.claimNext();
        metatype->setName("Self");

        // Mark this as the cached metatype for the l-value's object type.
        CanType argTy = getArgType();
        IGF.setUnscopedLocalTypeData(argTy, LocalTypeData::Metatype, metatype);
        return metatype;
      }
          
      case SourceKind::WitnessSelf:
      case SourceKind::WitnessExtraData: {
        // The 'Self' parameter is provided last.
        // TODO: For default implementations, the witness table pointer for
        // the 'Self : P' conformance must be provided last along with the
        // metatype.
        llvm::Value *metatype = in.takeLast();
        metatype->setName("Self");
        return metatype;
      }
      }
      llvm_unreachable("bad source kind!");
    }

    /// Produce the metadata value for the given depth, using the
    /// given cache.
    llvm::Value *getMetadataForDepth(unsigned depth) {
      assert(!MetadataForDepths.empty());
      while (depth >= MetadataForDepths.size()) {
        auto child = MetadataForDepths.back();
        auto childDecl = TypesForDepths[MetadataForDepths.size()];
        auto parent = emitParentMetadataRef(IGF, childDecl, child);
        MetadataForDepths.push_back(parent);
      }
      return MetadataForDepths[depth];
    }
  };
};

/// Emit a polymorphic parameters clause, binding all the metadata necessary.
void EmitPolymorphicParameters::emit(Explosion &in) {
  // Compute the first source metadata.
  MetadataForDepths.push_back(emitSourceForParameters(in));

  emitWithSourceBound(in);
}

/// Emit a polymorphic parameters clause for a generic value witness, binding
/// all the metadata necessary.
void
EmitPolymorphicParameters::emitForGenericValueWitness(llvm::Value *selfMeta) {
  // We get the source metadata verbatim from the value witness signature.
  MetadataForDepths.push_back(selfMeta);

  // All our archetypes should be satisfiable from the source.
  Explosion empty(ResilienceExpansion::Minimal);
  emitWithSourceBound(empty);
}

void
EmitPolymorphicParameters::emitWithSourceBound(Explosion &in) {
  
  for (auto archetype : getAllArchetypes()) {
    // Derive the appropriate metadata reference.
    llvm::Value *metadata;

    // If the reference is fulfilled by the source, go for it.
    auto it = Fulfillments.find(FulfillmentKey(archetype, nullptr));
    if (it != Fulfillments.end()) {
      auto &fulfillment = it->second;
      auto ancestor = getMetadataForDepth(fulfillment.Depth);
      auto ancestorDecl = TypesForDepths[fulfillment.Depth];
      metadata = emitArgumentMetadataRef(IGF, ancestorDecl,
                                         fulfillment.Index, ancestor);

    // Otherwise, it's just next in line.
    } else {
      metadata = in.claimNext();
    }

    // Collect all the witness tables.
    SmallVector<llvm::Value *, 8> wtables;
    for (auto protocol : archetype->getConformsTo()) {
      if (!requiresProtocolWitnessTable(protocol))
        continue;
      
      llvm::Value *wtable;

      // If the protocol witness table is fulfilled by the source, go for it.
      auto it = Fulfillments.find(FulfillmentKey(archetype, protocol));
      if (it != Fulfillments.end()) {
        auto &fulfillment = it->second;
        auto ancestor = getMetadataForDepth(fulfillment.Depth);
        auto ancestorDecl = TypesForDepths[fulfillment.Depth];
        wtable = emitArgumentWitnessTableRef(IGF, ancestorDecl,
                                             fulfillment.Index, protocol,
                                             ancestor);

      // Otherwise, it's just next in line.
      } else {
        wtable = in.claimNext();
      }
      wtables.push_back(wtable);
    }

    IGF.bindArchetype(archetype, metadata, wtables);
  }
}

/// Perform all the bindings necessary to emit the given declaration.
void irgen::emitPolymorphicParameters(IRGenFunction &IGF,
                                      CanSILFunctionType type,
                                      Explosion &in) {
  EmitPolymorphicParameters(IGF, type).emit(in);
}

/// Perform the metadata bindings necessary to emit a generic value witness.
void irgen::emitPolymorphicParametersForGenericValueWitness(IRGenFunction &IGF,
                                                        NominalTypeDecl *ntd,
                                                        llvm::Value *selfMeta) {
  EmitPolymorphicParameters(IGF, ntd).emitForGenericValueWitness(selfMeta);
  // Register the 'Self' argument as generic metadata for the type.
  IGF.setUnscopedLocalTypeData(ntd->getDeclaredTypeInContext()->getCanonicalType(),
                               LocalTypeData::Metatype, selfMeta);
}

/// Get the next argument and use it as the 'self' type metadata.
static void getArgAsLocalSelfTypeMetadata(IRGenFunction &IGF,
                                          llvm::Function::arg_iterator &it,
                                          CanType abstractType) {
  llvm::Value *arg = getArg(it, "Self");
  assert(arg->getType() == IGF.IGM.TypeMetadataPtrTy &&
         "Self argument is not a type?!");
  if (auto ugt = dyn_cast<UnboundGenericType>(abstractType)) {
    emitPolymorphicParametersForGenericValueWitness(IGF, ugt->getDecl(), arg);
  }
}

namespace {
  /// A CRTP class for finding the archetypes we need to bind in order
  /// to perform value operations on the given type.
  struct FindArchetypesToBind : CanTypeVisitor<FindArchetypesToBind> {
    llvm::SetVector<ArchetypeType*> &Types;
  public:
    FindArchetypesToBind(llvm::SetVector<ArchetypeType*> &types)
      : Types(types) {}

    // We're collecting archetypes.
    void visitArchetypeType(CanArchetypeType type) {
      Types.insert(type);
    }

    // We need to walk into tuples.
    void visitTupleType(CanTupleType tuple) {
      for (auto eltType : tuple.getElementTypes()) {
        visit(eltType);
      }
    }

    // We need to walk into constant-sized arrays.
    void visitArrayType(CanArrayType type) {
      visit(type.getBaseType());
    }

    // We do not need to walk into any of these types, because their
    // value operations do not depend on the specifics of their
    // sub-structure (or they have none).
    void visitAnyFunctionType(CanAnyFunctionType fn) {}
    void visitSILFunctionType(CanSILFunctionType fn) {}
    void visitBuiltinType(CanBuiltinType type) {}
    void visitMetatypeType(CanMetatypeType type) {}
    void visitModuleType(CanModuleType type) {}
    void visitDynamicSelfType(CanDynamicSelfType type) {}
    void visitProtocolCompositionType(CanProtocolCompositionType type) {}
    void visitReferenceStorageType(CanReferenceStorageType type) {}

    // L-values are impossible.
    void visitLValueType(CanLValueType type) {
      llvm_unreachable("cannot store l-value type directly");
    }
    void visitInOutType(CanInOutType type) {
      llvm_unreachable("cannot store inout type directly");
    }

    // Bind archetypes from the parent of nominal types.
    void visitNominalType(CanNominalType type) {
      if (auto parent = CanType(type->getParent()))
        visit(parent);
    }
    // Bind archetypes from bound generic types and their parents.
    void visitBoundGenericType(CanBoundGenericType type) {
      if (auto parent = CanType(type->getParent()))
        visit(parent);
      for (auto arg : type->getGenericArgs())
        visit(CanType(arg));
    }

    // FIXME: Will need to bind the archetype that this eventually refers to.
    void visitGenericTypeParamType(CanGenericTypeParamType type) { }

    // FIXME: Will need to bind the archetype that this eventually refers to.
    void visitDependentMemberType(CanDependentMemberType type) { }
  };
}

/// Initialize this set of necessary bindings.
NecessaryBindings::NecessaryBindings(IRGenModule &IGM, CanType type) {
  FindArchetypesToBind(Types).visit(type);
}

Size NecessaryBindings::getBufferSize(IRGenModule &IGM) const {
  return IGM.getPointerSize() * Types.size();
}

void NecessaryBindings::restore(IRGenFunction &IGF, Address buffer) const {
  if (Types.empty()) return;

  // Cast the buffer to %type**.
  auto metatypePtrPtrTy = IGF.IGM.TypeMetadataPtrTy->getPointerTo();
  buffer = IGF.Builder.CreateBitCast(buffer, metatypePtrPtrTy);

  for (unsigned i = 0, e = Types.size(); i != e; ++i) {
    auto archetype = Types[i];

    // GEP to the appropriate slot.
    Address slot = buffer;
    if (i) slot = IGF.Builder.CreateConstArrayGEP(slot, i,
                                                  IGF.IGM.getPointerSize());

    // Load the archetype's metatype.
    llvm::Value *metatype = IGF.Builder.CreateLoad(slot);
    metatype->setName(archetype->getFullName());
    setMetadataRef(IGF, archetype, metatype);
  }
}

void NecessaryBindings::save(IRGenFunction &IGF, Address buffer) const {
  if (Types.empty()) return;

  // Cast the buffer to %type**.
  auto metatypePtrPtrTy = IGF.IGM.TypeMetadataPtrTy->getPointerTo();
  buffer = IGF.Builder.CreateBitCast(buffer, metatypePtrPtrTy);

  for (unsigned i = 0, e = Types.size(); i != e; ++i) {
    auto archetype = Types[i];

    // GEP to the appropriate slot.
    Address slot = buffer;
    if (i) slot = IGF.Builder.CreateConstArrayGEP(slot, i,
                                                  IGF.IGM.getPointerSize());

    // Find the metatype for the appropriate archetype and store it in
    // the slot.
    llvm::Value *metatype =
      IGF.getLocalTypeData(CanType(archetype), LocalTypeData::Metatype);
    IGF.Builder.CreateStore(metatype, slot);
  }
}

/// Emit the witness table references required for the given type
/// substitution.
void irgen::emitWitnessTableRefs(IRGenFunction &IGF,
                                 const Substitution &sub,
                                 SmallVectorImpl<llvm::Value*> &out) {
  // We don't need to do anything if we have no protocols to conform to.
  auto archetypeProtos = sub.Archetype->getConformsTo();
  if (archetypeProtos.empty()) return;

  // Look at the replacement type.
  CanType replType = sub.Replacement->getCanonicalType();

  // If it's an archetype, we'll need to grab from the local context.
  if (auto archetype = dyn_cast<ArchetypeType>(replType)) {
    auto &archTI = getArchetypeInfo(IGF, archetype,
                                    IGF.getTypeInfoForLowered(archetype));

    for (auto proto : archetypeProtos) {
      ProtocolPath path(IGF.IGM, archTI.getProtocols(), proto);
      auto wtable = archTI.getWitnessTable(IGF, archetype,
                                           path.getOriginIndex());
      wtable = path.apply(IGF, wtable);
      out.push_back(wtable);
    }
    return;
  }

  // Otherwise, we can construct the witnesses from the protocol
  // conformances.
  auto &replTI = IGF.getTypeInfoForUnlowered(replType);

  assert(archetypeProtos.size() == sub.Conformance.size());
  for (unsigned j = 0, je = archetypeProtos.size(); j != je; ++j) {
    auto proto = archetypeProtos[j];
    auto &protoI = IGF.IGM.getProtocolInfo(proto);
    auto &confI = protoI.getConformance(IGF.IGM, replType, replTI, proto,
                                        *sub.Conformance[j]);

    llvm::Value *wtable = confI.getTable(IGF);
    out.push_back(wtable);
  }
}

namespace {
  class EmitPolymorphicArguments : public PolymorphicConvention {
    IRGenFunction &IGF;
  public:
    EmitPolymorphicArguments(IRGenFunction &IGF,
                             CanSILFunctionType polyFn)
      : PolymorphicConvention(polyFn), IGF(IGF) {}

    void emit(CanType substInputType, ArrayRef<Substitution> subs,
              Explosion &out);

  private:
    void emitSource(CanType substInputType, Explosion &out) {
      switch (getSourceKind()) {
      case SourceKind::None: return;
      case SourceKind::ClassPointer: return;
      case SourceKind::Metadata: return;
      case SourceKind::GenericLValueMetadata: {
        out.add(IGF.emitTypeMetadataRef(substInputType));
        return;
      }
      case SourceKind::WitnessSelf:
        // The 'Self' argument(s) are added as a special case in
        // EmitPolymorphicArguments::emit.
        return;
      case SourceKind::WitnessExtraData:
        // The 'Self' argument(s) are added implicitly from ExtraData of the
        // function value.
        return;
      }
      llvm_unreachable("bad source kind!");
    }
  };
}

/// Pass all the arguments necessary for the given function.
void irgen::emitPolymorphicArguments(IRGenFunction &IGF,
                                     CanSILFunctionType origFnType,
                                     CanSILFunctionType substFnType,
                                     ArrayRef<Substitution> subs,
                                     Explosion &out) {
  // Grab the apparent 'self' type.  If there isn't a 'self' type,
  // we're not going to try to access this anyway.
  CanType substInputType;
  if (!substFnType->getParameters().empty()) {
    auto selfParam = substFnType->getParameters().back();
    substInputType = selfParam.getType();
    // If the parameter is a direct metatype parameter, this is a static method
    // of the instance type. We can assume this because:
    // - metatypes cannot directly conform to protocols
    // - even if they could, they would conform as a value type 'self' and thus
    //   be passed indirectly as an @in or @inout parameter.
    if (auto meta = dyn_cast<MetatypeType>(substInputType)) {
      if (!selfParam.isIndirect())
        substInputType = meta.getInstanceType();
    }
  }
  emitPolymorphicArgumentsWithInput(IGF, origFnType, substInputType, subs, out);
}

static void emitPolymorphicArgumentsWithInput(IRGenFunction &IGF,
                                              CanSILFunctionType origFnType,
                                              CanType substInputType,
                                              ArrayRef<Substitution> subs,
                                              Explosion &out) {
  EmitPolymorphicArguments(IGF, origFnType).emit(substInputType, subs, out);
}

void EmitPolymorphicArguments::emit(CanType substInputType,
                                    ArrayRef<Substitution> subs,
                                    Explosion &out) {
  emitSource(substInputType, out);
  
  // For now, treat all archetypes independently.
  // FIXME: Later, we'll want to emit only the minimal set of archetypes,
  // because non-primary archetypes (which correspond to associated types)
  // will have their witness tables embedded in the witness table corresponding
  // to their parent.
  for (auto *archetype : getAllArchetypes()) {
    // Find the substitution for the archetype.
    auto const *subp = std::find_if(subs.begin(), subs.end(),
                                    [&](Substitution const &sub) {
                                      return sub.Archetype == archetype;
                                    });
    assert(subp != subs.end() && "no substitution for generic param?");
    auto const &sub = *subp;
    
    CanType argType = sub.Replacement->getCanonicalType();

    // Add the metadata reference unelss it's fulfilled.
    if (!Fulfillments.count(FulfillmentKey(archetype, nullptr))) {
      out.add(IGF.emitTypeMetadataRef(argType));
    }

    // Nothing else to do if there aren't any protocols to witness.
    auto protocols = archetype->getConformsTo();
    if (protocols.empty())
      continue;

    auto &argTI = IGF.getTypeInfoForUnlowered(argType);

    // Add witness tables for each of the required protocols.
    for (unsigned i = 0, e = protocols.size(); i != e; ++i) {
      auto protocol = protocols[i];
      
      // Skip this if the protocol doesn't require a witness table.
      if (!requiresProtocolWitnessTable(protocol))
        continue;

      // Skip this if it's fulfilled by the source.
      if (Fulfillments.count(FulfillmentKey(archetype, protocol)))
        continue;

      // If the target is an archetype, go to the type info.
      if (auto archetype = dyn_cast<ArchetypeType>(argType)) {
        auto &archTI = getArchetypeInfo(IGF, archetype,
                                        IGF.getTypeInfoForLowered(archetype));

        ProtocolPath path(IGF.IGM, archTI.getProtocols(), protocol);
        auto wtable = archTI.getWitnessTable(IGF, archetype,
                                             path.getOriginIndex());
        wtable = path.apply(IGF, wtable);
        out.add(wtable);
        continue;
      }

      // Otherwise, go to the conformances.
      auto &protoI = IGF.IGM.getProtocolInfo(protocol);
      auto &confI = protoI.getConformance(IGF.IGM, argType, argTI, protocol,
                                          *sub.Conformance[i]);
      llvm::Value *wtable = confI.getTable(IGF);
      out.add(wtable);
    }
  }
  
  // For a witness call, add the Self argument metadata arguments last.
  if (getSourceKind() == SourceKind::WitnessSelf) {
    auto self = IGF.emitTypeMetadataRef(substInputType);
    out.add(self);
    // TODO: Should also provide the protocol witness table,
    // for default implementations.
  }
}

namespace {
  /// A class for expanding a polymorphic signature.
  class ExpandPolymorphicSignature : public PolymorphicConvention {
    IRGenModule &IGM;
  public:
    ExpandPolymorphicSignature(IRGenModule &IGM, CanSILFunctionType fn)
      : PolymorphicConvention(fn), IGM(IGM) {}

    void expand(SmallVectorImpl<llvm::Type*> &out) {
      addSource(out);

      for (auto archetype : getAllArchetypes()) {
        // Pass the type argument if not fulfilled.
        if (!Fulfillments.count(FulfillmentKey(archetype, nullptr)))
          out.push_back(IGM.TypeMetadataPtrTy);

        // Pass each signature requirement that needs a witness table
        // separately (unless fulfilled).
        for (auto protocol : archetype->getConformsTo()) {
          if (!requiresProtocolWitnessTable(protocol))
            continue;
          
          if (!Fulfillments.count(FulfillmentKey(archetype, protocol)))
            out.push_back(IGM.WitnessTablePtrTy);
        }
      }
      
      // For a witness method, add the 'self' parameter.
      if (getSourceKind() == SourceKind::WitnessSelf) {
        out.push_back(IGM.TypeMetadataPtrTy);
        // TODO: Should also provide the protocol witness table,
        // for default implementations.
      }
    }

  private:
    /// Add signature elements for the source metadata.
    void addSource(SmallVectorImpl<llvm::Type*> &out) {
      switch (getSourceKind()) {
      case SourceKind::None: return;
      case SourceKind::ClassPointer: return; // already accounted for
      case SourceKind::Metadata: return; // already accounted for
      case SourceKind::GenericLValueMetadata:
        return out.push_back(IGM.TypeMetadataPtrTy);
      case SourceKind::WitnessSelf:
        return; // handled as a special case in expand()
      case SourceKind::WitnessExtraData:
        return; // added implicitly as ExtraData
      }
      llvm_unreachable("bad source kind");
    }
  };
}

/// Given a generic signature, add the argument types required in order to call it.
void irgen::expandPolymorphicSignature(IRGenModule &IGM,
                                       CanSILFunctionType polyFn,
                                       SmallVectorImpl<llvm::Type*> &out) {
  ExpandPolymorphicSignature(IGM, polyFn).expand(out);
}

/// Retrieve the protocol witness table for a conformance.
static llvm::Value *getProtocolWitnessTable(IRGenFunction &IGF,
                                            SILType srcType,
                                            const TypeInfo &srcTI,
                                            ProtocolEntry protoEntry,
                                            ProtocolConformance *conformance) {
  auto proto = protoEntry.getProtocol();
  assert(requiresProtocolWitnessTable(proto)
         && "protocol does not have witness tables?!");
  
  // If the source type is an archetype, look at what's locally bound.
  if (auto archetype = srcType.getAs<ArchetypeType>()) {
    assert(!conformance
           && "should not have concrete conformance info for archetype");
    auto &archTI = getArchetypeInfo(IGF, archetype, srcTI);
    ProtocolPath path(IGF.IGM, archTI.getProtocols(), proto);
    llvm::Value *rootTable = archTI.getWitnessTable(IGF, archetype,
                                                    path.getOriginIndex());
    return path.apply(IGF, rootTable);
  }
  
  // All other source types should be concrete enough that we have conformance
  // info for them.
  assert(conformance && "no conformance for concrete type?!");
  auto &protoI = protoEntry.getInfo();
  const ConformanceInfo &conformanceI
    = protoI.getConformance(IGF.IGM, srcType.getSwiftRValueType(),
                            srcTI, proto, *conformance);
  return conformanceI.getTable(IGF);
}

/// Emit protocol witness table pointers for the given protocol conformances,
/// passing each emitted witness table index into the given function body.
static void forEachProtocolWitnessTable(IRGenFunction &IGF,
                          SILType srcType, SILType destType,
                          ArrayRef<ProtocolEntry> protocols,
                          ArrayRef<ProtocolConformance*> conformances,
                          std::function<void (unsigned, llvm::Value*)> body) {
  // Collect the conformances that need witness tables.
  SmallVector<ProtocolDecl*, 2> destProtocols;
  bool isExistential
    = destType.getSwiftRValueType()->isExistentialType(destProtocols);
  
  assert(isExistential);
  (void)isExistential;

  SmallVector<ProtocolConformance*, 2> witnessConformances;
  assert(destProtocols.size() == conformances.size() &&
         "mismatched protocol conformances");
  for (unsigned i = 0, size = destProtocols.size(); i < size; ++i)
    if (requiresProtocolWitnessTable(destProtocols[i]))
      witnessConformances.push_back(conformances[i]);

  assert(protocols.size() == witnessConformances.size() &&
         "mismatched protocol conformances");
  
  auto &srcTI = IGF.getTypeInfo(srcType);
  for (unsigned i = 0, e = protocols.size(); i < e; ++i) {
    auto table = getProtocolWitnessTable(IGF, srcType, srcTI,
                                         protocols[i], witnessConformances[i]);
    body(i, table);
  }
}

/// Emit an existential container initialization by copying the value and
/// witness tables from an existential container of a more specific type.
void irgen::emitOpaqueExistentialContainerUpcast(IRGenFunction &IGF,
                                 Address dest, SILType destType,
                                 Address src,  SILType srcType,
                                 bool isTakeOfSrc) {
  assert(destType.isExistentialType());
  assert(!destType.isClassExistentialType());
  assert(srcType.isExistentialType());
  assert(!srcType.isClassExistentialType());
  auto &destTI = IGF.getTypeInfo(destType).as<OpaqueExistentialTypeInfo>();
  auto &srcTI = IGF.getTypeInfo(srcType).as<OpaqueExistentialTypeInfo>();

  auto destLayout = destTI.getLayout();
  auto srcLayout = srcTI.getLayout();
  
  ArrayRef<ProtocolEntry> destEntries = destTI.getProtocols();

  // Take the data out of the other buffer.
  // UpcastExistential never implies a transformation of the *value*,
  // just of the *witnesses*.
  Address destBuffer = destLayout.projectExistentialBuffer(IGF, dest);
  Address srcBuffer = srcLayout.projectExistentialBuffer(IGF, src);
  llvm::Value *srcMetadata = srcLayout.loadMetadataRef(IGF, src);
  if (isTakeOfSrc) {
    // If we can take the source, we can just memcpy the buffer.
    IGF.emitMemCpy(destBuffer, srcBuffer, getFixedBufferSize(IGF.IGM));
  } else {
    // Otherwise, we have to do a copy-initialization of the buffer.
    emitInitializeBufferWithCopyOfBufferCall(IGF, srcMetadata,
                                             destBuffer, srcBuffer);
  }
  
  // Copy the metadata as well.
  Address destMetadataRef = destLayout.projectMetadataRef(IGF, dest);
  IGF.Builder.CreateStore(srcMetadata, destMetadataRef);
  
  // Okay, the buffer on dest has been meaningfully filled in.
  // Fill in the witnesses.
  
  // If we're erasing *all* protocols, we're done.
  if (destEntries.empty())
    return;
  
  // Okay, so we're erasing to a non-trivial set of protocols.
  
  // First, find all the destination tables.  We can't write these
  // into dest immediately because later fetches of protocols might
  // give us trouble.
  SmallVector<llvm::Value*, 4> destTables;
  for (auto &entry : destEntries) {
    auto table = srcTI.findWitnessTable(IGF, src, entry.getProtocol());
    destTables.push_back(table);
  }
  
  // Now write those into the destination.
  for (unsigned i = 0, e = destTables.size(); i != e; ++i) {
    Address destSlot = destLayout.projectWitnessTable(IGF, dest, i);
    IGF.Builder.CreateStore(destTables[i], destSlot);
  }
}

void irgen::emitClassExistentialContainerUpcast(IRGenFunction &IGF,
                                                       Explosion &dest,
                                                       SILType destType,
                                                       Explosion &src,
                                                       SILType srcType) {
  assert(destType.isClassExistentialType());
  assert(srcType.isClassExistentialType());
  auto &destTI = IGF.getTypeInfo(destType).as<ClassExistentialTypeInfo>();
  auto &srcTI = IGF.getTypeInfo(srcType).as<ClassExistentialTypeInfo>();

  ArrayRef<llvm::Value*> srcTables;
  llvm::Value *instance;
  std::tie(srcTables, instance) = srcTI.getWitnessTablesAndValue(src);

  // Find the destination tables and add them to the destination.
  ArrayRef<ProtocolEntry> destEntries = destTI.getProtocols();
  SmallVector<llvm::Value*, 4> destTables;
  for (auto &entry : destEntries) {
    auto table = srcTI.findWitnessTable(IGF, srcTables, entry.getProtocol());
    dest.add(table);
  }

  // Add the instance.
  dest.add(instance);
}

/// "Deinitialize" an existential container whose contained value is allocated
/// but uninitialized, by deallocating the buffer owned by the container if any.
void irgen::emitOpaqueExistentialContainerDeinit(IRGenFunction &IGF,
                                                 Address container,
                                                 SILType type) {
  assert(type.isExistentialType());
  assert(!type.isClassExistentialType());
  auto &ti = IGF.getTypeInfo(type).as<OpaqueExistentialTypeInfo>();
  auto layout = ti.getLayout();
  
  llvm::Value *metadata = layout.loadMetadataRef(IGF, container);
  Address buffer = layout.projectExistentialBuffer(IGF, container);
  emitDeallocateBufferCall(IGF, metadata, buffer);
}

/// Emit a class existential container from a class instance value
/// as an explosion.
void irgen::emitClassExistentialContainer(IRGenFunction &IGF,
                               Explosion &out,
                               SILType outType,
                               llvm::Value *instance,
                               SILType instanceType,
                               ArrayRef<ProtocolConformance*> conformances) {
  assert(outType.isClassExistentialType() &&
         "creating a non-class existential type");
  
  auto &destTI = IGF.getTypeInfo(outType).as<ClassExistentialTypeInfo>();
  
  // Emit the witness table pointers.
  forEachProtocolWitnessTable(IGF, instanceType, outType,
                              destTI.getProtocols(),
                              conformances,
                              [&](unsigned i, llvm::Value *ptable) {
    out.add(ptable);
  });
  
  // Cast the instance pointer to an opaque refcounted pointer.
  llvm::Value *opaqueInstance
    = IGF.Builder.CreateBitCast(instance, IGF.IGM.UnknownRefCountedPtrTy);
  out.add(opaqueInstance);
}

/// Emit an existential container initialization operation for a concrete type.
/// Returns the address of the uninitialized buffer for the concrete value.
Address irgen::emitOpaqueExistentialContainerInit(IRGenFunction &IGF,
                                  Address dest,
                                  SILType destType,
                                  SILType srcType,
                                  ArrayRef<ProtocolConformance*> conformances) {
  assert(!destType.isClassExistentialType() &&
         "initializing a class existential container as opaque");
  auto &destTI = IGF.getTypeInfo(destType).as<OpaqueExistentialTypeInfo>();
  auto &srcTI = IGF.getTypeInfo(srcType);
  OpaqueExistentialLayout destLayout = destTI.getLayout();
  assert(destTI.getProtocols().size() == conformances.size());
  
  assert(!srcType.isExistentialType() &&
         "existential-to-existential erasure should be done with "
         "upcast_existential");
  
  // First, write out the metadata.
  llvm::Value *metadata = IGF.emitTypeMetadataRef(srcType);
  IGF.Builder.CreateStore(metadata, destLayout.projectMetadataRef(IGF, dest));
  
  // Compute basic layout information about the type.  If we have a
  // concrete type, we need to know how it packs into a fixed-size
  // buffer.  If we don't, we need a value witness table.
  FixedPacking packing;
  bool needValueWitnessToAllocate;
  if (srcType.is<ArchetypeType>()) { // FIXME: tuples of archetypes?
    packing = (FixedPacking) -1;
    needValueWitnessToAllocate = true;
  } else {
    packing = srcTI.getFixedPacking(IGF.IGM);
    needValueWitnessToAllocate = false;
  }
  
  // Next, write the protocol witness tables.
  forEachProtocolWitnessTable(IGF, srcType, destType,
                              destTI.getProtocols(), conformances,
                              [&](unsigned i, llvm::Value *ptable) {
    Address ptableSlot = destLayout.projectWitnessTable(IGF, dest, i);
    IGF.Builder.CreateStore(ptable, ptableSlot);
  });
  
  // Finally, evaluate into the buffer.
  
  // Project down to the destination fixed-size buffer.
  Address buffer = destLayout.projectExistentialBuffer(IGF, dest);

  // If the type is provably empty, we're done.
  if (srcTI.isKnownEmpty()) {
    assert(packing == FixedPacking::OffsetZero);
    return buffer;
  }
  
  // Otherwise, allocate if necessary.
  
  if (needValueWitnessToAllocate) {
    // If we're using a witness-table to do this, we need to emit a
    // value-witness call to allocate the fixed-size buffer.
    return Address(emitAllocateBufferCall(IGF, metadata, buffer),
                   Alignment(1));
  } else {
    // Otherwise, allocate using what we know statically about the type.
    return emitAllocateBuffer(IGF, srcType.getSwiftRValueType(),
                              srcTI, packing, buffer);
  }
}

static void getWitnessMethodValue(IRGenFunction &IGF,
                                  FuncDecl *fn,
                                  ProtocolDecl *fnProto,
                                  llvm::Value *wtable,
                                  llvm::Value *metadata,
                                  Explosion &out) {
  // Find the actual witness.
  auto &fnProtoInfo = IGF.IGM.getProtocolInfo(fnProto);
  auto index = fnProtoInfo.getWitnessEntry(fn).getFunctionIndex();
  llvm::Value *witness = emitLoadOfOpaqueWitness(IGF, wtable, index);

  // Cast the witness pointer to i8*.
  witness = IGF.Builder.CreateBitCast(witness, IGF.IGM.Int8PtrTy);
  
  // Build the value.
  out.add(witness);
  if (metadata)
    out.add(metadata);
}

void
irgen::emitArchetypeMethodValue(IRGenFunction &IGF,
                                SILType baseTy,
                                SILDeclRef member,
                                ProtocolConformance *conformance,
                                Explosion &out) {
  // The function we're going to call.
  // FIXME: Support getters and setters (and curried entry points?)
  assert(member.kind == SILDeclRef::Kind::Func
         && "getters and setters not yet supported");
  ValueDecl *vd = member.getDecl();
  FuncDecl *fn = cast<FuncDecl>(vd);
  
  // The protocol we're calling on.
  ProtocolDecl *fnProto = cast<ProtocolDecl>(fn->getDeclContext());
  
  // Find the witness table.
  auto &baseTI = IGF.getTypeInfo(baseTy);
  llvm::Value *wtable = getProtocolWitnessTable(IGF, baseTy, baseTI,
                      ProtocolEntry(fnProto, IGF.IGM.getProtocolInfo(fnProto)),
                      conformance); // FIXME conformance for concrete type
  
  // Build the value.
  getWitnessMethodValue(IGF, fn, fnProto, wtable, nullptr, out);
}

llvm::Value *
irgen::emitTypeMetadataRefForArchetype(IRGenFunction &IGF,
                                       Address addr,
                                       SILType type) {
  auto archetype = type.castTo<ArchetypeType>();
  
  // Acquire the archetype's static metadata.
  llvm::Value *metadata = IGF.getLocalTypeData(archetype,
                                               LocalTypeData::Metatype);

  // Call the 'typeof' value witness.
  return emitTypeofCall(IGF, metadata, addr.getAddress());
}

/// Extract the method pointer and metadata from a protocol witness table
/// as a function value.
void
irgen::emitOpaqueProtocolMethodValue(IRGenFunction &IGF,
                                     Address existAddr,
                                     SILType baseTy,
                                     SILDeclRef member,
                                     Explosion &out) {
  assert(baseTy.isExistentialType());
  assert(!baseTy.isClassExistentialType() &&
         "emitting class existential as opaque existential");
  // The protocol we're calling on.
  // TODO: support protocol compositions here.
  auto &baseTI = IGF.getTypeInfo(baseTy).as<OpaqueExistentialTypeInfo>();
  
  // The function we're going to call.
  // FIXME: Support getters and setters (and curried entry points?)
  assert(member.kind == SILDeclRef::Kind::Func
         && "getters and setters not yet supported");
  ValueDecl *vd = member.getDecl();
  FuncDecl *fn = cast<FuncDecl>(vd);
  ProtocolDecl *fnProto = cast<ProtocolDecl>(fn->getDeclContext());

  // Load the witness table.
  llvm::Value *wtable = baseTI.findWitnessTable(IGF, existAddr, fnProto);
  
  // Load the metadata.
  auto existLayout = baseTI.getLayout();  
  llvm::Value *metadata = existLayout.loadMetadataRef(IGF, existAddr);

  // Build the value.
  getWitnessMethodValue(IGF, fn, fnProto, wtable, metadata, out);
}

/// Extract the method pointer and metadata from a class existential
/// container's protocol witness table as a function value.
void irgen::emitClassProtocolMethodValue(IRGenFunction &IGF,
                                         Explosion &in,
                                         SILType baseTy,
                                         SILDeclRef member,
                                         Explosion &out) {
  assert(baseTy.isClassExistentialType());

  // The protocol we're calling on.
  auto &baseTI = IGF.getTypeInfo(baseTy).as<ClassExistentialTypeInfo>();
  ArrayRef<llvm::Value *> witnesses;
  llvm::Value *object;
  std::tie(witnesses, object) = baseTI.getWitnessTablesAndValue(in);
  
  // The function we're going to call.
  // FIXME: Support getters and setters (and curried entry points?)
  assert(member.kind == SILDeclRef::Kind::Func
         && "getters and setters not yet supported");
  ValueDecl *vd = member.getDecl();
  FuncDecl *fn = cast<FuncDecl>(vd);
  ProtocolDecl *fnProto = cast<ProtocolDecl>(fn->getDeclContext());
  
  // Load the witness table.
  llvm::Value *wtable = baseTI.findWitnessTable(IGF, witnesses, fnProto);

  // TODO: Load the metadata from the class reference. This is redundant,
  // but for simplicity in bringing up @cc(witness_method) we always provide
  // a metadata argument.
  llvm::Value *metadata = emitTypeMetadataRefForOpaqueHeapObject(IGF, object);
  
  // Build the value.
  getWitnessMethodValue(IGF, fn, fnProto, wtable, metadata, out);
}

llvm::Value *
irgen::emitTypeMetadataRefForOpaqueExistential(IRGenFunction &IGF, Address addr,
                                               SILType type) {
  return emitTypeMetadataRefForOpaqueExistential(IGF, addr,
                                           type.getSwiftRValueType());
}

llvm::Value *
irgen::emitTypeMetadataRefForClassExistential(IRGenFunction &IGF,
                                                     Explosion &value,
                                                     SILType type) {
  return emitTypeMetadataRefForClassExistential(IGF, value,
                                                 type.getSwiftRValueType());
}

llvm::Value *
irgen::emitTypeMetadataRefForOpaqueExistential(IRGenFunction &IGF, Address addr,
                                               CanType type) {
  assert(type->isExistentialType());
  assert(!type->isClassExistentialType());
  auto &baseTI = IGF.getTypeInfoForLowered(type).as<OpaqueExistentialTypeInfo>();

  // Get the static metadata.
  auto existLayout = baseTI.getLayout();
  llvm::Value *metadata = existLayout.loadMetadataRef(IGF, addr);
  
  // Project the buffer and apply the 'typeof' value witness.
  Address buffer = existLayout.projectExistentialBuffer(IGF, addr);
  llvm::Value *object = emitProjectBufferCall(IGF, metadata, buffer);
  return emitTypeofCall(IGF, metadata, object);
}

llvm::Value *
irgen::emitTypeMetadataRefForClassExistential(IRGenFunction &IGF,
                                                     Explosion &value,
                                                     CanType type) {
  assert(type->isClassExistentialType());
  auto &baseTI = IGF.getTypeInfoForLowered(type).as<ClassExistentialTypeInfo>();
  
  // Extract the class instance pointer.
  llvm::Value *instance = baseTI.getValue(IGF, value);
  // Get the type metadata.
  return emitTypeMetadataRefForOpaqueHeapObject(IGF, instance);
}

/// Emit a projection from an existential container to its concrete value
/// buffer with the type metadata for the contained value.
static std::pair<Address, llvm::Value*>
emitIndirectExistentialProjectionWithMetadata(IRGenFunction &IGF,
                                            Address base,
                                            SILType baseTy) {
  assert(baseTy.isExistentialType());
  if (baseTy.isClassExistentialType()) {
    auto &baseTI = IGF.getTypeInfo(baseTy).as<ClassExistentialTypeInfo>();
    auto valueAddr = baseTI.projectValue(IGF, base);
    auto value = IGF.Builder.CreateLoad(valueAddr);
    auto metadata = emitTypeMetadataRefForOpaqueHeapObject(IGF, value);
    return {valueAddr, metadata};
  } else {
    auto &baseTI = IGF.getTypeInfo(baseTy).as<OpaqueExistentialTypeInfo>();
    auto layout = baseTI.getLayout();
    
    llvm::Value *metadata = layout.loadMetadataRef(IGF, base);
    Address buffer = layout.projectExistentialBuffer(IGF, base);
    llvm::Value *object = emitProjectBufferCall(IGF, metadata, buffer);
    return {Address(object, Alignment(1)), metadata};
  }
}

/// Emit a projection from an existential container to its concrete value
/// buffer.
Address irgen::emitOpaqueExistentialProjection(IRGenFunction &IGF,
                                               Address base,
                                               SILType baseTy) {
  return emitIndirectExistentialProjectionWithMetadata(IGF, base, baseTy)
    .first;
}

/// Extract the instance pointer from a class existential value.
llvm::Value *irgen::emitClassExistentialProjection(IRGenFunction &IGF,
                                                          Explosion &base,
                                                          SILType baseTy) {
  assert(baseTy.isClassExistentialType());
  auto &baseTI = IGF.getTypeInfo(baseTy).as<ClassExistentialTypeInfo>();
  
  return baseTI.getValue(IGF, base);
}

static Address
emitOpaqueDowncast(IRGenFunction &IGF,
                   Address value,
                   llvm::Value *srcMetadata,
                   SILType destType,
                   CheckedCastMode mode) {
  llvm::Value *addr = IGF.Builder.CreateBitCast(value.getAddress(),
                                                IGF.IGM.OpaquePtrTy);

  srcMetadata = IGF.Builder.CreateBitCast(srcMetadata, IGF.IGM.Int8PtrTy);
  llvm::Value *destMetadata = IGF.emitTypeMetadataRef(destType);
  destMetadata = IGF.Builder.CreateBitCast(destMetadata, IGF.IGM.Int8PtrTy);
  
  llvm::Value *castFn;
  switch (mode) {
  case CheckedCastMode::Unconditional:
    castFn = IGF.IGM.getDynamicCastIndirectUnconditionalFn();
    break;
  case CheckedCastMode::Conditional:
    castFn = IGF.IGM.getDynamicCastIndirectFn();
    break;
  }
  
  auto *call = IGF.Builder.CreateCall3(castFn, addr, srcMetadata, destMetadata);
  // FIXME: Eventually, we may want to throw.
  call->setDoesNotThrow();
  
  // Convert the cast address to the destination type.
  auto &destTI = IGF.getTypeInfo(destType);
  llvm::Value *ptr = IGF.Builder.CreateBitCast(call,
                                           destTI.StorageType->getPointerTo());
  return destTI.getAddressForPointer(ptr);
}

/// Emit a checked cast of an opaque archetype.
Address irgen::emitOpaqueArchetypeDowncast(IRGenFunction &IGF,
                                           Address value,
                                           SILType srcType,
                                           SILType destType,
                                           CheckedCastMode mode) {
  llvm::Value *srcMetadata = IGF.emitTypeMetadataRef(srcType);
  return emitOpaqueDowncast(IGF, value, srcMetadata, destType, mode);
}

/// Emit a checked unconditional cast of an opaque existential container's
/// contained value.
Address irgen::emitIndirectExistentialDowncast(IRGenFunction &IGF,
                                               Address container,
                                               SILType srcType,
                                               SILType destType,
                                               CheckedCastMode mode) {
  assert(srcType.isExistentialType());
  
  // Project the value pointer and source type metadata out of the existential
  // container.
  Address value;
  llvm::Value *srcMetadata;
  std::tie(value, srcMetadata)
    = emitIndirectExistentialProjectionWithMetadata(IGF, container, srcType);

  return emitOpaqueDowncast(IGF, value, srcMetadata, destType, mode);
}

/// Emit a Protocol* value referencing an ObjC protocol.
static llvm::Value *emitReferenceToObjCProtocol(IRGenFunction &IGF,
                                                ProtocolDecl *proto) {
  assert(proto->isObjC() && "not an objc protocol");
  
  // Get the address of the global variable the protocol reference gets
  // indirected through.
  llvm::Constant *protocolRefAddr
    = IGF.IGM.getAddrOfObjCProtocolRef(proto, NotForDefinition);
  
  // Load the protocol reference.
  Address addr(protocolRefAddr, IGF.IGM.getPointerAlignment());
  return IGF.Builder.CreateLoad(addr);
}

/// Emit a checked cast to an Objective-C protocol or protocol composition.
llvm::Value *irgen::emitObjCExistentialDowncast(IRGenFunction &IGF,
                                                llvm::Value *orig,
                                                SILType srcType,
                                                SILType destType,
                                                CheckedCastMode mode) {
  orig = IGF.Builder.CreateBitCast(orig, IGF.IGM.ObjCPtrTy);
  SmallVector<ProtocolDecl*, 4> protos;
  bool isProtocol = destType.getSwiftRValueType()->isExistentialType(protos);
  assert(isProtocol); (void)isProtocol;
  
  // Get references to the ObjC Protocol* values for each protocol.
  Address protoRefsBuf = IGF.createAlloca(llvm::ArrayType::get(IGF.IGM.Int8PtrTy,
                                                               protos.size()),
                                          IGF.IGM.getPointerAlignment(),
                                          "objc_protocols");
  protoRefsBuf = IGF.Builder.CreateBitCast(protoRefsBuf,
                                           IGF.IGM.Int8PtrPtrTy);
  
  unsigned index = 0;
  for (auto proto : protos) {
    Address protoRefSlot = IGF.Builder.CreateConstArrayGEP(protoRefsBuf, index,
                                                     IGF.IGM.getPointerSize());
    auto protoRef = emitReferenceToObjCProtocol(IGF, proto);
    IGF.Builder.CreateStore(protoRef, protoRefSlot);
    ++index;
  }
  
  // Perform the cast.
  llvm::Value *castFn;
  switch (mode) {
  case CheckedCastMode::Unconditional:
    castFn = IGF.IGM.getDynamicCastObjCProtocolUnconditionalFn();
    break;
  case CheckedCastMode::Conditional:
    castFn = IGF.IGM.getDynamicCastObjCProtocolConditionalFn();
    break;
  }
  
  return IGF.Builder.CreateCall3(castFn, orig,
                                 IGF.IGM.getSize(Size(protos.size())),
                                 protoRefsBuf.getAddress());
}

StringRef irgen::getObjCProtocolName(ProtocolDecl *proto) {
  // For a Clang protocol, use the name on the Clang AST node directly.
  if (auto clangProto = cast_or_null<clang::ObjCProtocolDecl>(
                          proto->getClangNode().getAsDecl()))
    return clangProto->getName();

  return proto->getName().str();
}

bool irgen::requiresProtocolWitnessTable(ProtocolDecl *protocol) {
  return !protocol->isObjC();
}
