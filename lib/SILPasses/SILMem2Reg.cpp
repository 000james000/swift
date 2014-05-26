//===--- SILMem2Reg.cpp - Promotes AllocStacks to registers ---------------===//
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
// This pass promotes AllocStack instructions into memory references. It only
// handles load, store and deallocation instructions. The algorithm is based on:
//
//  Sreedhar and Gao. A linear time algorithm for placing phi-nodes. POPL '95.
//
//===----------------------------------------------------------------------===//


#define DEBUG_TYPE "sil-mem2reg"
#include "swift/SILPasses/Passes.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SILAnalysis/DominanceAnalysis.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <queue>
using namespace swift;

STATISTIC(NumAllocStackFound,    "Number of AllocStack found");
STATISTIC(NumAllocStackCaptured, "Number of AllocStack captured");
STATISTIC(NumInstRemoved,        "Number of Instructions removed");
STATISTIC(NumPhiPlaced,          "Number of Phi blocks placed");

namespace {

/// Promotes a single AllocStackInst into registers..
class StackAllocationPromoter {
  typedef SmallVector<SILBasicBlock *, 16> BlockList;
  typedef llvm::DomTreeNodeBase<SILBasicBlock> DomTreeNode;
  typedef llvm::DenseSet<SILBasicBlock *> BlockSet;
  typedef llvm::DenseMap<SILBasicBlock *, SILInstruction *> BlockToInstMap;

  // Use a priority queue keyed on dominator tree level so that inserted nodes
  // are handled from the bottom of the dom tree upwards.
  typedef std::pair<DomTreeNode *, unsigned> DomTreeNodePair;
  typedef std::priority_queue<DomTreeNodePair, SmallVector<DomTreeNodePair, 32>,
                                 llvm::less_second> NodePriorityQueue;

  /// The AllocStackInst that we are handling.
  AllocStackInst *ASI;

  /// The deallocation Instruction. This value could be NULL if there are
  /// multiple deallocations.
  DeallocStackInst *DSI;

  /// Dominator info.
  DominanceInfo *DT;

  /// Records the last store instruction in each block for a specific
  /// AllocStackInst.
  BlockToInstMap LastStoreInBlock;
public:
  /// C'tor.
  StackAllocationPromoter(AllocStackInst *Asi, DominanceInfo *Di)
      : ASI(Asi), DSI(0), DT(Di) {
        // Scan the users in search of a deallocation instruction.
        for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI)
          if (DeallocStackInst *D = dyn_cast<DeallocStackInst>(UI->getUser())) {
            // Don't record multiple dealloc instructions.
            if (DSI) {
              DSI = 0;
              break;
            }
            // Record the deallocation instruction.
            DSI = D;
          }
      }

  /// Promote the Allocation.
  void run();

private:
  /// \brief Promote AllocStacks into SSA.
  void promoteAllocationToPhi();

  /// \brief Replace the dummy nodes with new block arguments.
  void addBlockArguments(BlockSet &PhiBlocks);

  /// \brief Fix all of the Br instructions and the loads to use the AllocStack
  /// definitions (which include stores and Phis).
  void fixBranchesAndLoads(BlockSet &Blocks);

  /// \brief update the branch instructions with the new Phi argument.
  /// The blocks in \p PhiBlocks are blocks that define a value, \p Dest is
  /// the branch destination, and \p Pred is the predecessors who's branch we
  /// modify.
  void fixPhiPredBlock(BlockSet &PhiBlocks, SILBasicBlock *Dest,
                       SILBasicBlock *Pred);

  /// \brief Get the definition for block.
  SILValue getDefinitionForValue(BlockSet &PhiBlocks, SILBasicBlock *StartBB);

  /// \brief Prune AllocStacks usage in the function. Scan the function
  /// and remove in-block usage of the AllocStack. Leave only the first
  /// load and the last store.
  void pruneAllocStackUsage();

  /// \brief Promote all of the AllocStacks in a single basic block in one
  /// linear scan. This function deletes all of the loads and stores except
  /// for the first load and the last store.
  /// \returns the last StoreInst found or zero if none found.
  StoreInst *promoteAllocationInBlock(SILBasicBlock *BB);
};

} // end of namespace

namespace {
/// Promote memory to registers
class MemoryToRegisters {
  /// The function that we are optimizing.
  SILFunction &F;

  /// Dominators.
  DominanceInfo *DT;

  /// \brief Check if the AllocStackInst \p ASI is captured by any of its users.
  bool isCaptured(AllocStackInst *ASI);

  /// \brief Check if the AllocStackInst \p ASI is only used within a single
  /// basic block.
  bool isSingleBlockUsage(AllocStackInst *ASI);

  /// \brief Check if the AllocStackInst \p ASI is only written into.
  bool isWriteOnlyAllocation(AllocStackInst *ASI);

  /// \brief Promote all of the AllocStacks in a single basic block in one
  /// linear scan. Note: This function deletes all of the users of the
  /// AllocStackInst, including the DeallocStackInst but it does not remove the
  /// AllocStackInst itself!
  void removeSingleBlockAllocation(AllocStackInst *ASI);

public:
  /// C'tor
  MemoryToRegisters(SILFunction &Func, DominanceInfo *Dt) : F(Func), DT(Dt) {}

  /// \brief Promote memory to registers. Return True on change.
  bool run();
};

} // end anonymous namespace.

/// Returns true if this AllocStacks is captured.
bool MemoryToRegisters::isCaptured(AllocStackInst *ASI) {
  // For all users of the AllocStack instruction.
  for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI) {
    SILInstruction *II = UI->getUser();

    // Loads are okay.
    if (isa<LoadInst>(II))
      continue;

    // We can store into an AllocStack (but not the pointer).
    if (StoreInst *SI = dyn_cast<StoreInst>(II))
      if (SI->getDest().getDef() == ASI)
        continue;

    // Deallocation is also okay.
    if (isa<DeallocStackInst>(II))
      continue;

    // Other instructions are assumed to capture the AllocStack.
    DEBUG(llvm::dbgs() << "*** AllocStack is captured by: " << *II);
    return true;
  }

  // None of the users capture the AllocStack.
  return false;
}

/// Returns true if the AllocStack is only stored into.
bool MemoryToRegisters::isWriteOnlyAllocation(AllocStackInst *ASI) {
  // For all users of the AllocStack:
  for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI) {
    SILInstruction *II = UI->getUser();

    // It is okay to store into this AllocStack.
    if (StoreInst *SI = dyn_cast<StoreInst>(II))
      if (!isa<AllocStackInst>(SI->getSrc()))
        continue;

    // It is also okay to deallocate.
    if (isa<DeallocStackInst>(II))
      continue;

    // Can't do anything else with it.
    DEBUG(llvm::dbgs() << "*** AllocStack is loaded by: " << *II);
    return false;
  }

  return true;
}

/// Returns true if this AllocStack is only used within a single basic block.
bool MemoryToRegisters::isSingleBlockUsage(AllocStackInst *ASI) {
  assert(!isCaptured(ASI) && "This AllocStack must not be captured");
  SILBasicBlock *BB = ASI->getParent();

  // All of the users of the AllocStack must be in the same block.
  for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI)
    if (UI->getUser()->getParent() != BB)
      return false;

  return true;
}

StoreInst *
StackAllocationPromoter::promoteAllocationInBlock(SILBasicBlock *BB) {
  DEBUG(llvm::dbgs() << "*** Promoting ASI in block: " << *ASI);

  // We don't know the value of the alloca until we find the first store.
  SILValue RunningVal = SILValue();
  // Keep track of the last StoreInst that we found.
  StoreInst *LastStore = 0;

  // For all instructions in the block.
  for (auto BBI = BB->begin(), E = BB->end(); BBI != E;) {
    SILInstruction *Inst = BBI++;
    if (LoadInst *LI = dyn_cast<LoadInst>(Inst)) {
      // Make sure we are loading from this ASI.
      if (LI->getOperand().getDef() != ASI)
        continue;

      if (RunningVal.isValid()) {
        // If we are loading from the AllocStackInst and we already know the
        // conent of the Alloca then use it.
        DEBUG(llvm::dbgs() << "*** Promoting load: " << *LI);
        SILValue(Inst, 0).replaceAllUsesWith(RunningVal);
        Inst->eraseFromParent();
        NumInstRemoved++;
      } else {
        // If we don't know the content of the AllocStack then the loaded
        // value *is* the new value;
        DEBUG(llvm::dbgs() << "*** First load: " << *LI);
        RunningVal = LI;
      }
      continue;
    }

    // Remove stores and record the value that we are saving as the running
    // value.
    if (StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
      if (SI->getDest().getDef() != ASI)
        continue;

      // The stored value is the new running value.
      RunningVal = SI->getSrc();

      // If we met a store before this one, delete it.
      if (LastStore) {
        NumInstRemoved++;
        DEBUG(llvm::dbgs() << "*** Removing redundant store: " << *LastStore);
        LastStore->eraseFromParent();
      }
      LastStore = SI;
      continue;
    }

    // Stop on deallocation.
    if (DeallocStackInst *DSI = dyn_cast<DeallocStackInst>(Inst)) {
      if (DSI->getOperand() == ASI)
        break;
    }
  }
  if (LastStore) {
    DEBUG(llvm::dbgs() << "*** Finished promotion. Last store: " << *LastStore);
  } else {
    DEBUG(llvm::dbgs() << "*** Finished promotion with no stores.\n");
  }
  return LastStore;
}

void MemoryToRegisters::removeSingleBlockAllocation(AllocStackInst *ASI) {
  DEBUG(llvm::dbgs() << "*** Promoting in-block: " << *ASI);

  SILBasicBlock *BB = ASI->getParent();

  // The default value of the AllocStack is NULL because we don't have
  // unilitialized variables in Swift.
  SILValue RunningVal = SILValue();

  // For all instructions in the block.
  for (auto BBI = BB->begin(), E = BB->end(); BBI != E;) {
    SILInstruction *Inst = BBI++;
    // Remove instructions that we are loading from. Replace the loaded value
    // with our running value.
    if (LoadInst *LI = dyn_cast<LoadInst>(Inst)) {
      if (LI->getOperand().getDef() == ASI) {
        assert(RunningVal.isValid() &&
               "The AllocStack must be initialized before usage.");
        SILValue(Inst, 0).replaceAllUsesWith(RunningVal);
        Inst->eraseFromParent();
        NumInstRemoved++;
        continue;
      }
    }

    // Remove stores and record the value that we are saving as the running
    // value.
    if (StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
      if (SI->getDest().getDef() == ASI) {
        RunningVal = SI->getSrc();
        Inst->eraseFromParent();
        NumInstRemoved++;
        continue;
      }
    }

    // Remove deallocation.
    if (DeallocStackInst *DSI = dyn_cast<DeallocStackInst>(Inst)) {
      if (DSI->getOperand() == ASI) {
        Inst->eraseFromParent();
        NumInstRemoved++;
        // No need to continue scanning after deallocation.
        break;
      }
    }
  }
}

void StackAllocationPromoter::addBlockArguments(BlockSet &PhiBlocks) {
  DEBUG(llvm::dbgs() << "*** Adding new block arguments.\n");

  SILModule &M = ASI->getModule();

  for (auto Block : PhiBlocks)
    new (M) SILArgument(ASI->getElementType(), Block);
}

SILValue
StackAllocationPromoter::getDefinitionForValue(BlockSet &PhiBlocks,
                                                  SILBasicBlock *StartBB) {
  DEBUG(llvm::dbgs() << "*** Searching for a value definition.\n");
  // Walk the Dom tree in search of a defining value:
  DomTreeNode *Node = DT->getNode(StartBB);
  while (true) {
    SILBasicBlock *BB = Node->getBlock();

    // If there is a store (that must comes after the Phi) use its value.
    BlockToInstMap::iterator it = LastStoreInBlock.find(BB);
    if (it != LastStoreInBlock.end())
      if (StoreInst *St = dyn_cast_or_null<StoreInst>(it->second)) {
        DEBUG(llvm::dbgs() << "*** Found Store def " << *St->getSrc());
        return St->getSrc();
      }

    // If there is a Phi definition in this block:
    if (PhiBlocks.count(BB)) {
      // Return the dummy instruction that represents the new value that we will
      // add to the basic block.
      SILValue Phi = BB->getBBArg(BB->getNumBBArg()-1);
      DEBUG(llvm::dbgs() << "*** Found a dummy Phi def " << *Phi);
      return Phi;
    }

    // Move to the next dominating block.
    Node = Node->getIDom();
    if (!Node) {
      DEBUG(llvm::dbgs() << "*** Could not find a Def. Using Undef.\n");
      return SILValue();
    }

    DEBUG(llvm::dbgs() << "*** Walking up the iDOM.\n");
  }

  llvm_unreachable("Could not find a definition");
}


/// \brief Add an argument, \p val, to the branch-edge that is pointing into
/// block \p Dest. Return a new instruction and do not erase the old
/// instruction.
static TermInst *addArgumentToBranch(SILValue Val, SILBasicBlock *Dest,
                                     TermInst *Branch) {
  SILBuilder Builder(Branch);

  if (CondBranchInst *CBI = dyn_cast<CondBranchInst>(Branch)) {
    DEBUG(llvm::dbgs() << "*** Fixing CondBranchInst.\n");

    SmallVector<SILValue, 8> TrueArgs;
    SmallVector<SILValue, 8> FalseArgs;

    for (auto A : CBI->getTrueArgs())
      TrueArgs.push_back(A);

    for (auto A : CBI->getFalseArgs())
      FalseArgs.push_back(A);

    if (Dest == CBI->getTrueBB()) {
      TrueArgs.push_back(Val);
      assert(TrueArgs.size() == Dest->getNumBBArg());
    } else {
      FalseArgs.push_back(Val);
      assert(FalseArgs.size() == Dest->getNumBBArg());
    }

    return Builder.createCondBranch(CBI->getLoc(), CBI->getCondition(),
                                    CBI->getTrueBB(), TrueArgs,
                                    CBI->getFalseBB(), FalseArgs);
  }

  if (BranchInst *BI = dyn_cast<BranchInst>(Branch)) {
    DEBUG(llvm::dbgs() << "*** Fixing BranchInst.\n");

    SmallVector<SILValue, 8> Args;

    for (auto A : BI->getArgs())
      Args.push_back(A);

    Args.push_back(Val);
    assert(Args.size() == Dest->getNumBBArg());
    return Builder.createBranch(BI->getLoc(), BI->getDestBB(), Args);
  }

  llvm_unreachable("unsupported terminator");
}

void StackAllocationPromoter::fixPhiPredBlock(BlockSet &PhiBlocks,
                                              SILBasicBlock *Dest,
                                              SILBasicBlock *Pred) {
  TermInst *TI = Pred->getTerminator();
  DEBUG(llvm::dbgs() << "*** Fixing the terminator " << TI << ".\n");

  SILValue Def = getDefinitionForValue(PhiBlocks, Pred);
  if (!Def)
    Def =  SILUndef::get(ASI->getElementType(), ASI->getModule());

  DEBUG(llvm::dbgs() << "*** Found the definition: " << *Def);

  addArgumentToBranch(Def, Dest, TI);
  TI->eraseFromParent();
}

void StackAllocationPromoter::fixBranchesAndLoads(BlockSet &PhiBlocks) {
  // Start by fixing loads:
  for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E;) {
    LoadInst *LI = dyn_cast<LoadInst>(UI->getUser());
    UI++;
    if (!LI)
      continue;

    SILBasicBlock *BB = LI->getParent();
    DomTreeNode *Node = DT->getNode(BB);

    // If this block has no predecessors then nothing dominates it and the load
    // is dead code. Replace the load value with Undef and move on.
    if (LI->getParent()->pred_empty() || !Node) {
      SILValue Def =  SILUndef::get(ASI->getElementType(), ASI->getModule());
      SILValue(LI, 0).replaceAllUsesWith(Def);
      LI->eraseFromParent();
      NumInstRemoved++;
      continue;
    }

    // First, check if there is a Phi value in the current block. We know that
    // our loads happen before stores, so we need to first check for Phi nodes
    // in the first block, but stores first in all other stores in the idom
    // chain.
    if (PhiBlocks.count(BB)) {
      DEBUG(llvm::dbgs() << "*** Found a local Phi definiton.\n");
      SILValue Phi = BB->getBBArg(BB->getNumBBArg()-1);
      // Replace the load with the last argument of the BB, which is our Phi.
      SILValue(LI, 0).replaceAllUsesWith(Phi);
      LI->eraseFromParent();
      NumInstRemoved++;
      // We are done with this Load. Move on to the next Load.
      continue;
    }

    // We know that the load definition is not in our block, so start the search
    // one level up the idom tree.
    Node = Node->getIDom();
    assert(Node && "Promoting a load in the entry block ?");
    BB = Node->getBlock();

    SILValue Def = getDefinitionForValue(PhiBlocks, BB);
    if (!Def)
      Def =  SILUndef::get(ASI->getElementType(), ASI->getModule());
    DEBUG(llvm::dbgs() << "*** Replacing " << *LI << " with Def " << *Def);

    // Replace the load with the definition that we found.
    SILValue(LI, 0).replaceAllUsesWith(Def);
    LI->eraseFromParent();
    NumInstRemoved++;
  } // End of LoadInst loop.

  // Now that all of the loads are fixed we can fix the branches that point
  // to the blocks with the added arguments.

  // For each Block with a new Phi argument:
  for (auto Block : PhiBlocks) {
    // Fix all predecessors.
    for (auto PBB : Block->getPreds()) {
      assert(PBB && "Invalid block!");
      fixPhiPredBlock(PhiBlocks, Block, PBB);
    }
  }
}

void StackAllocationPromoter::pruneAllocStackUsage() {
  DEBUG(llvm::dbgs() << "*** Pruning : " << *ASI);
  BlockSet Blocks;

  // Insert all of the blocks that ASI is live in.
  for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI)
    Blocks.insert(UI->getUser()->getParent());

  // Clear AllocStack state.
  LastStoreInBlock.clear();

  for (auto Block : Blocks) {
    StoreInst *SI = promoteAllocationInBlock(Block);
    LastStoreInBlock[Block] = SI;
  }

  DEBUG(llvm::dbgs() << "*** Finished pruning : " << *ASI);
}

void StackAllocationPromoter::promoteAllocationToPhi() {
  DEBUG(llvm::dbgs() << "*** Placing Phis for : " << *ASI);

  /// Maps dom tree nodes to their dom tree levels.
  llvm::DenseMap<DomTreeNode *, unsigned> DomTreeLevels;

  // Assign tree levels to dom tree nodes.
  // TODO: This should happen once per function.
  SmallVector<DomTreeNode *, 32> Worklist;
  DomTreeNode *Root = DT->getRootNode();
  DomTreeLevels[Root] = 0;
  Worklist.push_back(Root);
  while (!Worklist.empty()) {
    DomTreeNode *Node = Worklist.pop_back_val();
    unsigned ChildLevel = DomTreeLevels[Node] + 1;
    for (auto CI = Node->begin(), CE = Node->end(); CI != CE; ++CI) {
      DomTreeLevels[*CI] = ChildLevel;
      Worklist.push_back(*CI);
    }
  }

  // A list of blocks that will require new Phi values.
  BlockSet PhiBlocks;

  // The "piggy-bank" data-structure that we use for processing the dom-tree
  // bottom-up.
  NodePriorityQueue PQ;

  // Collect all of the stores into the AllocStack. We know that at this point
  // we have at most one store per block.
  for (auto UI = ASI->use_begin(), E = ASI->use_end(); UI != E; ++UI) {
    SILInstruction *II = UI->getUser();
    // We need to place Phis for this block.
    if (isa<StoreInst>(II)) {
      // If the block is in the dom tree (dominated by the entry block).
      if (DomTreeNode *Node = DT->getNode(II->getParent()))
        PQ.push(std::make_pair(Node, DomTreeLevels[Node]));
    }
  }

  DEBUG(llvm::dbgs() << "*** Found: " << PQ.size() << " Defs\n");

  // A list of nodes for which we already calculated the dominator frontier.
  llvm::SmallPtrSet<DomTreeNode *, 32> Visited;

  // Scan all of the definitions in the function bottom-up using the priority
  // queue.
  while (!PQ.empty()) {
    DomTreeNodePair RootPair = PQ.top();
    PQ.pop();
    DomTreeNode *Root = RootPair.first;
    unsigned RootLevel = RootPair.second;

    // Walk all dom tree children of Root, inspecting their successors. Only
    // J-edges, whose target level is at most Root's level are added to the
    // dominance frontier.
    Worklist.clear();
    Worklist.push_back(Root);

    while (!Worklist.empty()) {
      DomTreeNode *Node = Worklist.pop_back_val();
      SILBasicBlock *BB = Node->getBlock();

      // For all successors of the node:
      for (auto &Succ : BB->getSuccs()) {
        DomTreeNode *SuccNode = DT->getNode(Succ);

        // Skip D-edges (edges that are dom-tree edges).
        if (SuccNode->getIDom() == Node)
          continue;

        // Ignore J-edges that point to nodes that are not smaller or equal
        // to the root level.
        unsigned SuccLevel = DomTreeLevels[SuccNode];
        if (SuccLevel > RootLevel)
          continue;

        // Ignore visited nodes.
        if (!Visited.insert(SuccNode))
          continue;

        // If the new PHInode is not dominated by the allocation then it's dead.
        if (!DT->dominates(ASI->getParent(), SuccNode->getBlock()))
            continue;

        // If the new PHInode is properly dominated by the deallocation then it
        // is obviously a dead PHInode, so we don't need to insert it.
        if (DSI && DT->properlyDominates(DSI->getParent(),
                                         SuccNode->getBlock()))
          continue;

        // The successor node is a new PHINode. If this is a new PHI node
        // then it may require additional definitions, so add it to the PQ.
        if (PhiBlocks.insert(Succ).second)
          PQ.push(std::make_pair(SuccNode, SuccLevel));
      }

      // Add the children in the dom-tree to the worklist.
      for (auto CI = Node->begin(), CE = Node->end(); CI != CE; ++CI)
        if (!Visited.count(*CI))
          Worklist.push_back(*CI);
    }
  }

  DEBUG(llvm::dbgs() << "*** Found: " << PhiBlocks.size() << " new PHIs\n");
  NumPhiPlaced += PhiBlocks.size();

  // At this point we calculated the locations of all of the new Phi values.
  // Next, add the Phi values and promote all of the loads and stores into the
  // new locations.

  // Replace the dummy values with new block arguments.
  addBlockArguments(PhiBlocks);

  // Hook up the Phi nodes and the loads with storing values.
  fixBranchesAndLoads(PhiBlocks);

  DEBUG(llvm::dbgs() << "*** Finished placing Phis ***\n");
}

void StackAllocationPromoter::run() {
  // Reduce the number of load/stores in the function to minimum.
  // After this phase we are left with up to one load and store
  // per block and the last store is recorded.
  pruneAllocStackUsage();

  // Replace AllocStacks with Phi-nodes.
  promoteAllocationToPhi();
}

bool MemoryToRegisters::run() {
  bool Changed = false;
  for (auto &BB : F) {
    auto I = BB.begin(), E = BB.end();
    while (I != E) {
      SILInstruction *Inst = I;
      AllocStackInst *ASI = dyn_cast<AllocStackInst>(Inst);
      if (!ASI) {
        ++I;
        continue;
      }

      DEBUG(llvm::dbgs() << "*** Memory to register looking at: " << *I);
      NumAllocStackFound++;

      // Don't handle captured AllocStacks.
      if (isCaptured(ASI)) {
        NumAllocStackCaptured++;
        ++I;
        continue;
      }

      // For AllocStacks that are only used within a single basic blocks, use
      // the linear sweep to remove the AllocStack.
      if (isSingleBlockUsage(ASI)) {
        removeSingleBlockAllocation(ASI);

        DEBUG(llvm::dbgs() << "*** Deleting single block AllocStackInst: "
                           << *ASI);
        I++;
        ASI->eraseFromParent();
        NumInstRemoved++;
        Changed = true;
        continue;
      }

      // Remove write-only AllocStacks.
      if (isWriteOnlyAllocation(ASI)) {
        eraseUsesOfInstruction(ASI);

        DEBUG(llvm::dbgs() << "*** Deleting store-only AllocStack: " << *ASI);
        I++;
        ASI->eraseFromParent();
        Changed = true;
        NumInstRemoved++;
        continue;
      }

      DEBUG(llvm::dbgs() << "*** Need to insert Phis for " << *ASI);

      // Promote this allocation.
      StackAllocationPromoter(ASI, DT).run();

      // Make sure that all of the allocations were promoted into registers.
      assert(isWriteOnlyAllocation(ASI) && "Loads left behind");
      // ... and erase the allocation.
      eraseUsesOfInstruction(ASI);

      I++;
      ASI->eraseFromParent();
      NumInstRemoved++;
      Changed = true;
    }
  }
  return Changed;
}

namespace {
class SILMem2Reg : public SILFunctionTransform {

  void run() override {
    SILFunction *F = getFunction();
    DEBUG(llvm::dbgs() << "** Mem2Reg on function: " << F->getName() <<" **\n");

    DominanceAnalysis* DA = PM->getAnalysis<DominanceAnalysis>();

    bool Changed = MemoryToRegisters(*F, DA->getDomInfo(F)).run();

    if (Changed)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }

  StringRef getName() override { return "SIL Mem2Reg"; }
};
} // end anonymous namespace

SILTransform *swift::createMem2Reg() {
  return new SILMem2Reg();
}
