//===- llvm/Transforms/Utils/LoopUtils.h - Loop utilities -*- C++ -*-=========//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines some loop transformation utilities.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_LOOPUTILS_H
#define LLVM_TRANSFORMS_UTILS_LOOPUTILS_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/EHPersonalities.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"

namespace llvm {
class AliasSet;
class AliasSetTracker;
class AssumptionCache;
class BasicBlock;
class DataLayout;
class DominatorTree;
class Loop;
class LoopInfo;
class Pass;
class PredicatedScalarEvolution;
class PredIteratorCache;
class ScalarEvolution;
class SCEV;
class TargetLibraryInfo;

/// \brief Captures loop safety information.
/// It keep information for loop & its header may throw exception.
struct LoopSafetyInfo {
  bool MayThrow;       // The current loop contains an instruction which
                       // may throw.
  bool HeaderMayThrow; // Same as previous, but specific to loop header
  // Used to update funclet bundle operands.
  DenseMap<BasicBlock *, ColorVector> BlockColors;
  LoopSafetyInfo() : MayThrow(false), HeaderMayThrow(false) {}
};

/// The RecurrenceDescriptor is used to identify recurrences variables in a
/// loop. Reduction is a special case of recurrence that has uses of the
/// recurrence variable outside the loop. The method isReductionPHI identifies
/// reductions that are basic recurrences.
///
/// Basic recurrences are defined as the summation, product, OR, AND, XOR, min,
/// or max of a set of terms. For example: for(i=0; i<n; i++) { total +=
/// array[i]; } is a summation of array elements. Basic recurrences are a
/// special case of chains of recurrences (CR). See ScalarEvolution for CR
/// references.

/// This struct holds information about recurrence variables.
class RecurrenceDescriptor {

public:
  /// This enum represents the kinds of recurrences that we support.
  enum RecurrenceKind {
    RK_NoRecurrence,  ///< Not a recurrence.
    RK_IntegerAdd,    ///< Sum of integers.
    RK_IntegerMult,   ///< Product of integers.
    RK_IntegerOr,     ///< Bitwise or logical OR of numbers.
    RK_IntegerAnd,    ///< Bitwise or logical AND of numbers.
    RK_IntegerXor,    ///< Bitwise or logical XOR of numbers.
    RK_IntegerMinMax, ///< Min/max implemented in terms of select(cmp()).
    RK_FloatAdd,      ///< Sum of floats.
    RK_FloatMult,     ///< Product of floats.
    RK_FloatMinMax    ///< Min/max implemented in terms of select(cmp()).
  };

  // This enum represents the kind of minmax recurrence.
  enum MinMaxRecurrenceKind {
    MRK_Invalid,
    MRK_UIntMin,
    MRK_UIntMax,
    MRK_SIntMin,
    MRK_SIntMax,
    MRK_FloatMin,
    MRK_FloatMax
  };

  RecurrenceDescriptor()
      : StartValue(nullptr), LoopExitInstr(nullptr), Kind(RK_NoRecurrence),
        MinMaxKind(MRK_Invalid), UnsafeAlgebraInst(nullptr),
        RecurrenceType(nullptr), IsSigned(false) {}

  RecurrenceDescriptor(Value *Start, Instruction *Exit, RecurrenceKind K,
                       MinMaxRecurrenceKind MK, Instruction *UAI, Type *RT,
                       bool Signed, SmallPtrSetImpl<Instruction *> &CI)
      : StartValue(Start), LoopExitInstr(Exit), Kind(K), MinMaxKind(MK),
        UnsafeAlgebraInst(UAI), RecurrenceType(RT), IsSigned(Signed) {
    CastInsts.insert(CI.begin(), CI.end());
  }

  /// This POD struct holds information about a potential recurrence operation.
  class InstDesc {

  public:
    InstDesc(bool IsRecur, Instruction *I, Instruction *UAI = nullptr)
        : IsRecurrence(IsRecur), PatternLastInst(I), MinMaxKind(MRK_Invalid),
          UnsafeAlgebraInst(UAI) {}

    InstDesc(Instruction *I, MinMaxRecurrenceKind K, Instruction *UAI = nullptr)
        : IsRecurrence(true), PatternLastInst(I), MinMaxKind(K),
          UnsafeAlgebraInst(UAI) {}

    bool isRecurrence() { return IsRecurrence; }

    bool hasUnsafeAlgebra() { return UnsafeAlgebraInst != nullptr; }

    Instruction *getUnsafeAlgebraInst() { return UnsafeAlgebraInst; }

    MinMaxRecurrenceKind getMinMaxKind() { return MinMaxKind; }

    Instruction *getPatternInst() { return PatternLastInst; }

  private:
    // Is this instruction a recurrence candidate.
    bool IsRecurrence;
    // The last instruction in a min/max pattern (select of the select(icmp())
    // pattern), or the current recurrence instruction otherwise.
    Instruction *PatternLastInst;
    // If this is a min/max pattern the comparison predicate.
    MinMaxRecurrenceKind MinMaxKind;
    // Recurrence has unsafe algebra.
    Instruction *UnsafeAlgebraInst;
  };

  /// Returns a struct describing if the instruction 'I' can be a recurrence
  /// variable of type 'Kind'. If the recurrence is a min/max pattern of
  /// select(icmp()) this function advances the instruction pointer 'I' from the
  /// compare instruction to the select instruction and stores this pointer in
  /// 'PatternLastInst' member of the returned struct.
  static InstDesc isRecurrenceInstr(Instruction *I, RecurrenceKind Kind,
                                    InstDesc &Prev, bool HasFunNoNaNAttr);

  /// Returns true if instruction I has multiple uses in Insts
  static bool hasMultipleUsesOf(Instruction *I,
                                SmallPtrSetImpl<Instruction *> &Insts);

  /// Returns true if all uses of the instruction I is within the Set.
  static bool areAllUsesIn(Instruction *I, SmallPtrSetImpl<Instruction *> &Set);

  /// Returns a struct describing if the instruction if the instruction is a
  /// Select(ICmp(X, Y), X, Y) instruction pattern corresponding to a min(X, Y)
  /// or max(X, Y).
  static InstDesc isMinMaxSelectCmpPattern(Instruction *I, InstDesc &Prev);

  /// Returns identity corresponding to the RecurrenceKind.
  static Constant *getRecurrenceIdentity(RecurrenceKind K, Type *Tp);

  /// Returns the opcode of binary operation corresponding to the
  /// RecurrenceKind.
  static unsigned getRecurrenceBinOp(RecurrenceKind Kind);

  /// Returns a Min/Max operation corresponding to MinMaxRecurrenceKind.
  static Value *createMinMaxOp(IRBuilder<> &Builder, MinMaxRecurrenceKind RK,
                               Value *Left, Value *Right);

  /// Returns true if Phi is a reduction of type Kind and adds it to the
  /// RecurrenceDescriptor.
  static bool AddReductionVar(PHINode *Phi, RecurrenceKind Kind, Loop *TheLoop,
                              bool HasFunNoNaNAttr,
                              RecurrenceDescriptor &RedDes);

  /// Returns true if Phi is a reduction in TheLoop. The RecurrenceDescriptor is
  /// returned in RedDes.
  static bool isReductionPHI(PHINode *Phi, Loop *TheLoop,
                             RecurrenceDescriptor &RedDes);

  /// Returns true if Phi is a first-order recurrence. A first-order recurrence
  /// is a non-reduction recurrence relation in which the value of the
  /// recurrence in the current loop iteration equals a value defined in the
  /// previous iteration.
  static bool isFirstOrderRecurrence(PHINode *Phi, Loop *TheLoop,
                                     DominatorTree *DT);

  RecurrenceKind getRecurrenceKind() { return Kind; }

  MinMaxRecurrenceKind getMinMaxRecurrenceKind() { return MinMaxKind; }

  TrackingVH<Value> getRecurrenceStartValue() { return StartValue; }

  Instruction *getLoopExitInstr() { return LoopExitInstr; }

  /// Returns true if the recurrence has unsafe algebra which requires a relaxed
  /// floating-point model.
  bool hasUnsafeAlgebra() { return UnsafeAlgebraInst != nullptr; }

  /// Returns first unsafe algebra instruction in the PHI node's use-chain.
  Instruction *getUnsafeAlgebraInst() { return UnsafeAlgebraInst; }

  /// Returns true if the recurrence kind is an integer kind.
  static bool isIntegerRecurrenceKind(RecurrenceKind Kind);

  /// Returns true if the recurrence kind is a floating point kind.
  static bool isFloatingPointRecurrenceKind(RecurrenceKind Kind);

  /// Returns true if the recurrence kind is an arithmetic kind.
  static bool isArithmeticRecurrenceKind(RecurrenceKind Kind);

  /// Determines if Phi may have been type-promoted. If Phi has a single user
  /// that ANDs the Phi with a type mask, return the user. RT is updated to
  /// account for the narrower bit width represented by the mask, and the AND
  /// instruction is added to CI.
  static Instruction *lookThroughAnd(PHINode *Phi, Type *&RT,
                                     SmallPtrSetImpl<Instruction *> &Visited,
                                     SmallPtrSetImpl<Instruction *> &CI);

  /// Returns true if all the source operands of a recurrence are either
  /// SExtInsts or ZExtInsts. This function is intended to be used with
  /// lookThroughAnd to determine if the recurrence has been type-promoted. The
  /// source operands are added to CI, and IsSigned is updated to indicate if
  /// all source operands are SExtInsts.
  static bool getSourceExtensionKind(Instruction *Start, Instruction *Exit,
                                     Type *RT, bool &IsSigned,
                                     SmallPtrSetImpl<Instruction *> &Visited,
                                     SmallPtrSetImpl<Instruction *> &CI);

  /// Returns the type of the recurrence. This type can be narrower than the
  /// actual type of the Phi if the recurrence has been type-promoted.
  Type *getRecurrenceType() { return RecurrenceType; }

  /// Returns a reference to the instructions used for type-promoting the
  /// recurrence.
  SmallPtrSet<Instruction *, 8> &getCastInsts() { return CastInsts; }

  /// Returns true if all source operands of the recurrence are SExtInsts.
  bool isSigned() { return IsSigned; }

private:
  // The starting value of the recurrence.
  // It does not have to be zero!
  TrackingVH<Value> StartValue;
  // The instruction who's value is used outside the loop.
  Instruction *LoopExitInstr;
  // The kind of the recurrence.
  RecurrenceKind Kind;
  // If this a min/max recurrence the kind of recurrence.
  MinMaxRecurrenceKind MinMaxKind;
  // First occurance of unasfe algebra in the PHI's use-chain.
  Instruction *UnsafeAlgebraInst;
  // The type of the recurrence.
  Type *RecurrenceType;
  // True if all source operands of the recurrence are SExtInsts.
  bool IsSigned;
  // Instructions used for type-promoting the recurrence.
  SmallPtrSet<Instruction *, 8> CastInsts;
};

/// A struct for saving information about induction variables.
class InductionDescriptor {
public:
  /// This enum represents the kinds of inductions that we support.
  enum InductionKind {
    IK_NoInduction,  ///< Not an induction variable.
    IK_IntInduction, ///< Integer induction variable. Step = C.
    IK_PtrInduction  ///< Pointer induction var. Step = C / sizeof(elem).
  };

public:
  /// Default constructor - creates an invalid induction.
  InductionDescriptor()
      : StartValue(nullptr), IK(IK_NoInduction), Step(nullptr) {}

  /// Get the consecutive direction. Returns:
  ///   0 - unknown or non-consecutive.
  ///   1 - consecutive and increasing.
  ///  -1 - consecutive and decreasing.
  int getConsecutiveDirection() const;

  /// Compute the transformed value of Index at offset StartValue using step
  /// StepValue.
  /// For integer induction, returns StartValue + Index * StepValue.
  /// For pointer induction, returns StartValue[Index * StepValue].
  /// FIXME: The newly created binary instructions should contain nsw/nuw
  /// flags, which can be found from the original scalar operations.
  Value *transform(IRBuilder<> &B, Value *Index, ScalarEvolution *SE,
                   const DataLayout& DL) const;

  Value *getStartValue() const { return StartValue; }
  InductionKind getKind() const { return IK; }
  const SCEV *getStep() const { return Step; }
  ConstantInt *getConstIntStepValue() const;

  /// Returns true if \p Phi is an induction. If \p Phi is an induction,
  /// the induction descriptor \p D will contain the data describing this
  /// induction. If by some other means the caller has a better SCEV
  /// expression for \p Phi than the one returned by the ScalarEvolution
  /// analysis, it can be passed through \p Expr.
  static bool isInductionPHI(PHINode *Phi, ScalarEvolution *SE,
                             InductionDescriptor &D,
                             const SCEV *Expr = nullptr);

  /// Returns true if \p Phi is an induction, in the context associated with
  /// the run-time predicate of PSE. If \p Assume is true, this can add further
  /// SCEV predicates to \p PSE in order to prove that \p Phi is an induction.
  /// If \p Phi is an induction, \p D will contain the data describing this
  /// induction.
  static bool isInductionPHI(PHINode *Phi, PredicatedScalarEvolution &PSE,
                             InductionDescriptor &D, bool Assume = false);

private:
  /// Private constructor - used by \c isInductionPHI.
  InductionDescriptor(Value *Start, InductionKind K, const SCEV *Step);

  /// Start value.
  TrackingVH<Value> StartValue;
  /// Induction kind.
  InductionKind IK;
  /// Step value.
  const SCEV *Step;
};

BasicBlock *InsertPreheaderForLoop(Loop *L, DominatorTree *DT, LoopInfo *LI,
                                   bool PreserveLCSSA);

/// \brief Simplify each loop in a loop nest recursively.
///
/// This takes a potentially un-simplified loop L (and its children) and turns
/// it into a simplified loop nest with preheaders and single backedges. It will
/// update \c AliasAnalysis and \c ScalarEvolution analyses if they're non-null.
bool simplifyLoop(Loop *L, DominatorTree *DT, LoopInfo *LI, ScalarEvolution *SE,
                  AssumptionCache *AC, bool PreserveLCSSA);

/// \brief Put loop into LCSSA form.
///
/// Looks at all instructions in the loop which have uses outside of the
/// current loop. For each, an LCSSA PHI node is inserted and the uses outside
/// the loop are rewritten to use this node.
///
/// LoopInfo and DominatorTree are required and preserved.
///
/// If ScalarEvolution is passed in, it will be preserved.
///
/// Returns true if any modifications are made to the loop.
bool formLCSSA(Loop &L, DominatorTree &DT, LoopInfo *LI, ScalarEvolution *SE);

/// \brief Put a loop nest into LCSSA form.
///
/// This recursively forms LCSSA for a loop nest.
///
/// LoopInfo and DominatorTree are required and preserved.
///
/// If ScalarEvolution is passed in, it will be preserved.
///
/// Returns true if any modifications are made to the loop.
bool formLCSSARecursively(Loop &L, DominatorTree &DT, LoopInfo *LI,
                          ScalarEvolution *SE);

/// \brief Walk the specified region of the CFG (defined by all blocks
/// dominated by the specified block, and that are in the current loop) in
/// reverse depth first order w.r.t the DominatorTree. This allows us to visit
/// uses before definitions, allowing us to sink a loop body in one pass without
/// iteration. Takes DomTreeNode, AliasAnalysis, LoopInfo, DominatorTree,
/// DataLayout, TargetLibraryInfo, Loop, AliasSet information for all
/// instructions of the loop and loop safety information as arguments.
/// It returns changed status.
bool sinkRegion(DomTreeNode *, AliasAnalysis *, LoopInfo *, DominatorTree *,
                TargetLibraryInfo *, Loop *, AliasSetTracker *,
                LoopSafetyInfo *);

/// \brief Walk the specified region of the CFG (defined by all blocks
/// dominated by the specified block, and that are in the current loop) in depth
/// first order w.r.t the DominatorTree.  This allows us to visit definitions
/// before uses, allowing us to hoist a loop body in one pass without iteration.
/// Takes DomTreeNode, AliasAnalysis, LoopInfo, DominatorTree, DataLayout,
/// TargetLibraryInfo, Loop, AliasSet information for all instructions of the
/// loop and loop safety information as arguments. It returns changed status.
bool hoistRegion(DomTreeNode *, AliasAnalysis *, LoopInfo *, DominatorTree *,
                 TargetLibraryInfo *, Loop *, AliasSetTracker *,
                 LoopSafetyInfo *);

/// \brief Try to promote memory values to scalars by sinking stores out of
/// the loop and moving loads to before the loop.  We do this by looping over
/// the stores in the loop, looking for stores to Must pointers which are
/// loop invariant. It takes AliasSet, Loop exit blocks vector, loop exit blocks
/// insertion point vector, PredIteratorCache, LoopInfo, DominatorTree, Loop,
/// AliasSet information for all instructions of the loop and loop safety
/// information as arguments. It returns changed status.
bool promoteLoopAccessesToScalars(AliasSet &, SmallVectorImpl<BasicBlock *> &,
                                  SmallVectorImpl<Instruction *> &,
                                  PredIteratorCache &, LoopInfo *,
                                  DominatorTree *, const TargetLibraryInfo *,
                                  Loop *, AliasSetTracker *, LoopSafetyInfo *);

/// \brief Computes safety information for a loop
/// checks loop body & header for the possibility of may throw
/// exception, it takes LoopSafetyInfo and loop as argument.
/// Updates safety information in LoopSafetyInfo argument.
void computeLoopSafetyInfo(LoopSafetyInfo *, Loop *);

/// Returns true if the instruction in a loop is guaranteed to execute at least
/// once.
bool isGuaranteedToExecute(const Instruction &Inst, const DominatorTree *DT,
                           const Loop *CurLoop,
                           const LoopSafetyInfo *SafetyInfo);

/// \brief Returns the instructions that use values defined in the loop.
SmallVector<Instruction *, 8> findDefsUsedOutsideOfLoop(Loop *L);

/// \brief Find string metadata for loop
///
/// If it has a value (e.g. {"llvm.distribute", 1} return the value as an
/// operand or null otherwise.  If the string metadata is not found return
/// Optional's not-a-value.
Optional<const MDOperand *> findStringMetadataForLoop(Loop *TheLoop,
                                                      StringRef Name);

/// \brief Set input string into loop metadata by keeping other values intact.
void addStringMetadataToLoop(Loop *TheLoop, const char *MDString,
                             unsigned V = 0);

/// Helper to consistently add the set of standard passes to a loop pass's \c
/// AnalysisUsage.
///
/// All loop passes should call this as part of implementing their \c
/// getAnalysisUsage.
void getLoopAnalysisUsage(AnalysisUsage &AU);
}

#endif
