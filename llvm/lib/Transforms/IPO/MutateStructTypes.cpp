//===- MutateStructTypes.cpp - Change struct defns --------------------------=//
//
// This pass is used to change structure accesses and type definitions in some
// way.  It can be used to arbitrarily permute structure fields, safely, without
// breaking code.  A transformation may only be done on a type if that type has
// been found to be "safe" by the 'FindUnsafePointerTypes' pass.  This pass will
// assert and die if you try to do an illegal transformation.
//
// This is an interprocedural pass that requires the entire program to do a
// transformation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/MutateStructTypes.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/SymbolTable.h"
#include "llvm/iPHINode.h"
#include "llvm/iMemory.h"
#include "llvm/iTerminators.h"
#include "llvm/iOther.h"
#include "Support/STLExtras.h"
#include <algorithm>
using std::map;
using std::vector;

//FIXME: These headers are only included because the analyses are killed!!!
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/FindUsedTypes.h"
#include "llvm/Analysis/FindUnsafePointerTypes.h"
//FIXME end

// To enable debugging, uncomment this...
//#define DEBUG_MST(x) x

#ifdef DEBUG_MST
#include "llvm/Assembly/Writer.h"
#else
#define DEBUG_MST(x)   // Disable debug code
#endif

// ValuePlaceHolder - A stupid little marker value.  It appears as an
// instruction of type Instruction::UserOp1.
//
struct ValuePlaceHolder : public Instruction {
  ValuePlaceHolder(const Type *Ty) : Instruction(Ty, UserOp1, "") {}

  virtual Instruction *clone() const { abort(); return 0; }
  virtual const char *getOpcodeName() const { return "placeholder"; }
};


// ConvertType - Convert from the old type system to the new one...
const Type *MutateStructTypes::ConvertType(const Type *Ty) {
  if (Ty->isPrimitiveType() ||
      isa<OpaqueType>(Ty)) return Ty;  // Don't convert primitives

  map<const Type *, PATypeHolder<Type> >::iterator I = TypeMap.find(Ty);
  if (I != TypeMap.end()) return I->second;

  const Type *DestTy = 0;

  PATypeHolder<Type> PlaceHolder = OpaqueType::get();
  TypeMap.insert(std::make_pair(Ty, PlaceHolder.get()));

  switch (Ty->getPrimitiveID()) {
  case Type::FunctionTyID: {
    const FunctionType *MT = cast<FunctionType>(Ty);
    const Type *RetTy = ConvertType(MT->getReturnType());
    vector<const Type*> ArgTypes;

    for (FunctionType::ParamTypes::const_iterator I = MT->getParamTypes().begin(),
           E = MT->getParamTypes().end(); I != E; ++I)
      ArgTypes.push_back(ConvertType(*I));
    
    DestTy = FunctionType::get(RetTy, ArgTypes, MT->isVarArg());
    break;
  }
  case Type::StructTyID: {
    const StructType *ST = cast<StructType>(Ty);
    const StructType::ElementTypes &El = ST->getElementTypes();
    vector<const Type *> Types;

    for (StructType::ElementTypes::const_iterator I = El.begin(), E = El.end();
         I != E; ++I)
      Types.push_back(ConvertType(*I));
    DestTy = StructType::get(Types);
    break;
  }
  case Type::ArrayTyID:
    DestTy = ArrayType::get(ConvertType(cast<ArrayType>(Ty)->getElementType()),
                            cast<ArrayType>(Ty)->getNumElements());
    break;

  case Type::PointerTyID:
    DestTy = PointerType::get(
                 ConvertType(cast<PointerType>(Ty)->getElementType()));
    break;
  default:
    assert(0 && "Unknown type!");
    return 0;
  }

  assert(DestTy && "Type didn't get created!?!?");

  // Refine our little placeholder value into a real type...
  cast<DerivedType>(PlaceHolder.get())->refineAbstractTypeTo(DestTy);
  TypeMap.insert(std::make_pair(Ty, PlaceHolder.get()));

  return PlaceHolder.get();
}


// AdjustIndices - Convert the indexes specifed by Idx to the new changed form
// using the specified OldTy as the base type being indexed into.
//
void MutateStructTypes::AdjustIndices(const CompositeType *OldTy,
                                      vector<Value*> &Idx,
                                      unsigned i = 0) {
  assert(i < Idx.size() && "i out of range!");
  const CompositeType *NewCT = cast<CompositeType>(ConvertType(OldTy));
  if (NewCT == OldTy) return;  // No adjustment unless type changes

  if (const StructType *OldST = dyn_cast<StructType>(OldTy)) {
    // Figure out what the current index is...
    unsigned ElNum = cast<ConstantUInt>(Idx[i])->getValue();
    assert(ElNum < OldST->getElementTypes().size());

    map<const StructType*, TransformType>::iterator I = Transforms.find(OldST);
    if (I != Transforms.end()) {
      assert(ElNum < I->second.second.size());
      // Apply the XForm specified by Transforms map...
      unsigned NewElNum = I->second.second[ElNum];
      Idx[i] = ConstantUInt::get(Type::UByteTy, NewElNum);
    }
  }

  // Recursively process subtypes...
  if (i+1 < Idx.size())
    AdjustIndices(cast<CompositeType>(OldTy->getTypeAtIndex(Idx[i])), Idx, i+1);
}


// ConvertValue - Convert from the old value in the old type system to the new
// type system.
//
Value *MutateStructTypes::ConvertValue(const Value *V) {
  // Ignore null values and simple constants..
  if (V == 0) return 0;

  if (Constant *CPV = dyn_cast<Constant>(V)) {
    if (V->getType()->isPrimitiveType())
      return CPV;

    if (isa<ConstantPointerNull>(CPV))
      return ConstantPointerNull::get(
                      cast<PointerType>(ConvertType(V->getType())));
    assert(0 && "Unable to convert constpool val of this type!");
  }

  // Check to see if this is an out of function reference first...
  if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
    // Check to see if the value is in the map...
    map<const GlobalValue*, GlobalValue*>::iterator I = GlobalMap.find(GV);
    if (I == GlobalMap.end())
      return GV;  // Not mapped, just return value itself
    return I->second;
  }
  
  map<const Value*, Value*>::iterator I = LocalValueMap.find(V);
  if (I != LocalValueMap.end()) return I->second;

  if (const BasicBlock *BB = dyn_cast<BasicBlock>(V)) {
    // Create placeholder block to represent the basic block we haven't seen yet
    // This will be used when the block gets created.
    //
    return LocalValueMap[V] = new BasicBlock(BB->getName());
  }

  DEBUG_MST(cerr << "NPH: " << V << endl);

  // Otherwise make a constant to represent it
  return LocalValueMap[V] = new ValuePlaceHolder(ConvertType(V->getType()));
}


// setTransforms - Take a map that specifies what transformation to do for each
// field of the specified structure types.  There is one element of the vector
// for each field of the structure.  The value specified indicates which slot of
// the destination structure the field should end up in.  A negative value 
// indicates that the field should be deleted entirely.
//
void MutateStructTypes::setTransforms(const TransformsType &XForm) {

  // Loop over the types and insert dummy entries into the type map so that 
  // recursive types are resolved properly...
  for (map<const StructType*, vector<int> >::const_iterator I = XForm.begin(),
         E = XForm.end(); I != E; ++I) {
    const StructType *OldTy = I->first;
    TypeMap.insert(std::make_pair(OldTy, OpaqueType::get()));
  }

  // Loop over the type specified and figure out what types they should become
  for (map<const StructType*, vector<int> >::const_iterator I = XForm.begin(),
         E = XForm.end(); I != E; ++I) {
    const StructType  *OldTy = I->first;
    const vector<int> &InVec = I->second;

    assert(OldTy->getElementTypes().size() == InVec.size() &&
           "Action not specified for every element of structure type!");

    vector<const Type *> NewType;

    // Convert the elements of the type over, including the new position mapping
    int Idx = 0;
    vector<int>::const_iterator TI = find(InVec.begin(), InVec.end(), Idx);
    while (TI != InVec.end()) {
      unsigned Offset = TI-InVec.begin();
      const Type *NewEl = ConvertType(OldTy->getContainedType(Offset));
      assert(NewEl && "Element not found!");
      NewType.push_back(NewEl);

      TI = find(InVec.begin(), InVec.end(), ++Idx);
    }

    // Create a new type that corresponds to the destination type
    PATypeHolder<StructType> NSTy = StructType::get(NewType);

    // Refine the old opaque type to the new type to properly handle recursive
    // types...
    //
    const Type *OldTypeStub = TypeMap.find(OldTy)->second.get();
    cast<DerivedType>(OldTypeStub)->refineAbstractTypeTo(NSTy);

    // Add the transformation to the Transforms map.
    Transforms.insert(std::make_pair(OldTy, std::make_pair(NSTy, InVec)));

    DEBUG_MST(cerr << "Mutate " << OldTy << "\nTo " << NSTy << endl);
  }
}

void MutateStructTypes::clearTransforms() {
  Transforms.clear();
  TypeMap.clear();
  GlobalMap.clear();
  assert(LocalValueMap.empty() &&
         "Local Value Map should always be empty between transformations!");
}

// doInitialization - This loops over global constants defined in the
// module, converting them to their new type.
//
void MutateStructTypes::processGlobals(Module *M) {
  // Loop through the functions in the module and create a new version of the
  // function to contained the transformed code.  Don't use an iterator, because
  // we will be adding values to the end of the vector, and it could be
  // reallocated.  Also, we don't want to process the values that we add.
  //
  unsigned NumFunctions = M->size();
  for (unsigned i = 0; i < NumFunctions; ++i) {
    Function *Meth = M->begin()[i];

    if (!Meth->isExternal()) {
      const FunctionType *NewMTy = 
        cast<FunctionType>(ConvertType(Meth->getFunctionType()));
      
      // Create a new function to put stuff into...
      Function *NewMeth = new Function(NewMTy, Meth->hasInternalLinkage(),
				   Meth->getName());
      if (Meth->hasName())
        Meth->setName("OLD."+Meth->getName());

      // Insert the new function into the method list... to be filled in later..
      M->getFunctionList().push_back(NewMeth);
      
      // Keep track of the association...
      GlobalMap[Meth] = NewMeth;
    }
  }

  // TODO: HANDLE GLOBAL VARIABLES

  // Remap the symbol table to refer to the types in a nice way
  //
  if (M->hasSymbolTable()) {
    SymbolTable *ST = M->getSymbolTable();
    SymbolTable::iterator I = ST->find(Type::TypeTy);
    if (I != ST->end()) {    // Get the type plane for Type's
      SymbolTable::VarMap &Plane = I->second;
      for (SymbolTable::type_iterator TI = Plane.begin(), TE = Plane.end();
           TI != TE; ++TI) {
        // This is gross, I'm reaching right into a symbol table and mucking
        // around with it's internals... but oh well.
        //
        TI->second = cast<Type>(ConvertType(cast<Type>(TI->second)));
      }
    }
  }
}


// removeDeadGlobals - For this pass, all this does is remove the old versions
// of the functions and global variables that we no longer need.
void MutateStructTypes::removeDeadGlobals(Module *M) {
  // Prepare for deletion of globals by dropping their interdependencies...
  for(Module::iterator I = M->begin(); I != M->end(); ++I) {
    if (GlobalMap.find(*I) != GlobalMap.end())
      (*I)->Function::dropAllReferences();
  }

  // Run through and delete the functions and global variables...
#if 0  // TODO: HANDLE GLOBAL VARIABLES
  M->getGlobalList().delete_span(M->gbegin(), M->gbegin()+NumGVars/2);
#endif
  for(Module::iterator I = M->begin(); I != M->end();) {
    if (GlobalMap.find(*I) != GlobalMap.end())
      delete M->getFunctionList().remove(I);
    else
      ++I;
  }
}



// transformMethod - This transforms the instructions of the function to use the
// new types.
//
void MutateStructTypes::transformMethod(Function *m) {
  const Function *M = m;
  map<const GlobalValue*, GlobalValue*>::iterator GMI = GlobalMap.find(M);
  if (GMI == GlobalMap.end())
    return;  // Do not affect one of our new functions that we are creating

  Function *NewMeth = cast<Function>(GMI->second);

  // Okay, first order of business, create the arguments...
  for (unsigned i = 0; i < M->getArgumentList().size(); ++i) {
    const FunctionArgument *OMA = M->getArgumentList()[i];
    FunctionArgument *NMA = new FunctionArgument(ConvertType(OMA->getType()),
                                                 OMA->getName());
    NewMeth->getArgumentList().push_back(NMA);
    LocalValueMap[OMA] = NMA; // Keep track of value mapping
  }


  // Loop over all of the basic blocks copying instructions over...
  for (Function::const_iterator BBI = M->begin(), BBE = M->end(); BBI != BBE;
       ++BBI) {

    // Create a new basic block and establish a mapping between the old and new
    const BasicBlock *BB = *BBI;
    BasicBlock *NewBB = cast<BasicBlock>(ConvertValue(BB));
    NewMeth->getBasicBlocks().push_back(NewBB);  // Add block to function

    // Copy over all of the instructions in the basic block...
    for (BasicBlock::const_iterator II = BB->begin(), IE = BB->end();
         II != IE; ++II) {

      const Instruction *I = *II;   // Get the current instruction...
      Instruction *NewI = 0;

      switch (I->getOpcode()) {
        // Terminator Instructions
      case Instruction::Ret:
        NewI = new ReturnInst(
                   ConvertValue(cast<ReturnInst>(I)->getReturnValue()));
        break;
      case Instruction::Br: {
        const BranchInst *BI = cast<BranchInst>(I);
        NewI = new BranchInst(
                           cast<BasicBlock>(ConvertValue(BI->getSuccessor(0))),
                    cast_or_null<BasicBlock>(ConvertValue(BI->getSuccessor(1))),
                              ConvertValue(BI->getCondition()));
        break;
      }
      case Instruction::Switch:
      case Instruction::Invoke:
        assert(0 && "Insn not implemented!");

        // Unary Instructions
      case Instruction::Not:
        NewI = UnaryOperator::create((Instruction::UnaryOps)I->getOpcode(),
                                     ConvertValue(I->getOperand(0)));
        break;

        // Binary Instructions
      case Instruction::Add:
      case Instruction::Sub:
      case Instruction::Mul:
      case Instruction::Div:
      case Instruction::Rem:
        // Logical Operations
      case Instruction::And:
      case Instruction::Or:
      case Instruction::Xor:

        // Binary Comparison Instructions
      case Instruction::SetEQ:
      case Instruction::SetNE:
      case Instruction::SetLE:
      case Instruction::SetGE:
      case Instruction::SetLT:
      case Instruction::SetGT:
        NewI = BinaryOperator::create((Instruction::BinaryOps)I->getOpcode(),
                                      ConvertValue(I->getOperand(0)),
                                      ConvertValue(I->getOperand(1)));
        break;

      case Instruction::Shr:
      case Instruction::Shl:
        NewI = new ShiftInst(cast<ShiftInst>(I)->getOpcode(),
                             ConvertValue(I->getOperand(0)),
                             ConvertValue(I->getOperand(1)));
        break;


        // Memory Instructions
      case Instruction::Alloca:
        NewI = 
          new AllocaInst(ConvertType(I->getType()),
                         I->getNumOperands()?ConvertValue(I->getOperand(0)):0);
        break;
      case Instruction::Malloc:
        NewI = 
          new MallocInst(ConvertType(I->getType()),
                         I->getNumOperands()?ConvertValue(I->getOperand(0)):0);
        break;

      case Instruction::Free:
        NewI = new FreeInst(ConvertValue(I->getOperand(0)));
        break;

      case Instruction::Load:
      case Instruction::Store:
      case Instruction::GetElementPtr: {
        const MemAccessInst *MAI = cast<MemAccessInst>(I);
        vector<Value*> Indices(MAI->idx_begin(), MAI->idx_end());
        const Value *Ptr = MAI->getPointerOperand();
        Value *NewPtr = ConvertValue(Ptr);
        if (!Indices.empty()) {
          const Type *PTy = cast<PointerType>(Ptr->getType())->getElementType();
          AdjustIndices(cast<CompositeType>(PTy), Indices);
        }

        if (isa<LoadInst>(I)) {
          NewI = new LoadInst(NewPtr, Indices);
        } else if (isa<StoreInst>(I)) {
          NewI = new StoreInst(ConvertValue(I->getOperand(0)), NewPtr, Indices);
        } else if (isa<GetElementPtrInst>(I)) {
          NewI = new GetElementPtrInst(NewPtr, Indices);
        } else {
          assert(0 && "Unknown memory access inst!!!");
        }
        break;
      }

        // Miscellaneous Instructions
      case Instruction::PHINode: {
        const PHINode *OldPN = cast<PHINode>(I);
        PHINode *PN = new PHINode(ConvertType(I->getType()));
        for (unsigned i = 0; i < OldPN->getNumIncomingValues(); ++i)
          PN->addIncoming(ConvertValue(OldPN->getIncomingValue(i)),
                    cast<BasicBlock>(ConvertValue(OldPN->getIncomingBlock(i))));
        NewI = PN;
        break;
      }
      case Instruction::Cast:
        NewI = new CastInst(ConvertValue(I->getOperand(0)),
                            ConvertType(I->getType()));
        break;
      case Instruction::Call: {
        Value *Meth = ConvertValue(I->getOperand(0));
        vector<Value*> Operands;
        for (unsigned i = 1; i < I->getNumOperands(); ++i)
          Operands.push_back(ConvertValue(I->getOperand(i)));
        NewI = new CallInst(Meth, Operands);
        break;
      }
        
      default:
        assert(0 && "UNKNOWN INSTRUCTION ENCOUNTERED!\n");
        break;
      }

      NewI->setName(I->getName());
      NewBB->getInstList().push_back(NewI);

      // Check to see if we had to make a placeholder for this value...
      map<const Value*,Value*>::iterator LVMI = LocalValueMap.find(I);
      if (LVMI != LocalValueMap.end()) {
        // Yup, make sure it's a placeholder...
        Instruction *I = cast<Instruction>(LVMI->second);
        assert(I->getOpcode() == Instruction::UserOp1 && "Not a placeholder!");

        // Replace all uses of the place holder with the real deal...
        I->replaceAllUsesWith(NewI);
        delete I;                    // And free the placeholder memory
      }

      // Keep track of the fact the the local implementation of this instruction
      // is NewI.
      LocalValueMap[I] = NewI;
    }
  }

  LocalValueMap.clear();
}


bool MutateStructTypes::run(Module *M) {
  processGlobals(M);

  for_each(M->begin(), M->end(),
           bind_obj(this, &MutateStructTypes::transformMethod));

  removeDeadGlobals(M);
  return true;
}

// getAnalysisUsageInfo - This function needs the results of the
// FindUsedTypes and FindUnsafePointerTypes analysis passes...
//
void MutateStructTypes::getAnalysisUsageInfo(Pass::AnalysisSet &Required,
                                             Pass::AnalysisSet &Destroyed,
                                             Pass::AnalysisSet &Provided) {
  Destroyed.push_back(FindUsedTypes::ID);
  Destroyed.push_back(FindUnsafePointerTypes::ID);
  Destroyed.push_back(CallGraph::ID);
}
