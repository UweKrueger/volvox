#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

CompModes comp_mode = comp_obj;

DebugInfo KSDbgInfo;

TypeTable type_table;

//===----------------------------------------------------------------------===//
// Code Generation Globals
//===----------------------------------------------------------------------===//
static std::unique_ptr<llvm::LLVMContext> TheContext;
llvm::orc::ThreadSafeContext Context;
static std::unique_ptr<llvm::Module> TheModule;
static std::unique_ptr<llvm::IRBuilder<>> Builder;
static llvm::ExitOnError ExitOnErr;

// static std::map<std::string, llvm::AllocaInst *> NamedValues;
static std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM;
static std::unique_ptr<llvm::orc::VolvoxJIT> TheJIT;
std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

llvm::raw_ostream &indent(llvm::raw_ostream &O, int size) {
	return O << std::string(size, ' ');
}
MapNode* globals_table;
MapNode* locals_table; // including function arguments

//===----------------------------------------------------------------------===//
// Built-in Types
//===----------------------------------------------------------------------===//

unsigned stringkey;

void init() {
	type_table.add("void", llvm::Type::getVoidTy(*Context.getContext()));
	type_table.add("bool", llvm::Type::getInt1Ty(*Context.getContext()));
	type_table.add("i8", llvm::Type::getInt8Ty(*Context.getContext()), true);
	type_table.add("i16", llvm::Type::getInt16Ty(*Context.getContext()), true);
	type_table.add("i32", llvm::Type::getInt32Ty(*Context.getContext()), true);
	type_table.add("i64", llvm::Type::getInt64Ty(*Context.getContext()), true);
	type_table.add("u8", llvm::Type::getInt8Ty(*Context.getContext()));
	type_table.add("u16", llvm::Type::getInt16Ty(*Context.getContext()));
	type_table.add("u32", llvm::Type::getInt32Ty(*Context.getContext()));
	type_table.add("u64", llvm::Type::getInt64Ty(*Context.getContext()));
	type_table.add("f32", llvm::Type::getFloatTy(*Context.getContext()));
	type_table.add("f64", llvm::Type::getDoubleTy(*Context.getContext()));
	type_table.add("string", llvm::Type::getInt8PtrTy(*Context.getContext()));

	globals_table = map_string_new_map();
	locals_table = map_string_new_map();
}

//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//

static std::unique_ptr<llvm::DIBuilder> DBuilder;

llvm::DIType *DebugInfo::getDoubleTy() {
	if (DblTy)
		return DblTy;

	DblTy = DBuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
	return DblTy;
}

void DebugInfo::emitLocation(ExprAST *AST) {
	if (!AST)
		return Builder->SetCurrentDebugLocation(llvm::DebugLoc());
	llvm::DIScope *Scope;
	if (LexicalBlocks.empty())
		Scope = TheCU;
	else
		Scope = LexicalBlocks.back();
	Builder->SetCurrentDebugLocation(llvm::DILocation::get(
										 Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}

static llvm::DISubroutineType *CreateFunctionType(unsigned NumArgs, llvm::DIFile *Unit) {
	llvm::SmallVector<llvm::Metadata *, 8> EltTys;
	llvm::DIType *DblTy = KSDbgInfo.getDoubleTy();

	// Add the result type.
	EltTys.push_back(DblTy);

	for (unsigned i = 0, e = NumArgs; i != e; ++i)
		EltTys.push_back(DblTy);

	return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(EltTys));
}

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

static llvm::DISubprogram *SP;
static llvm::DIFile *Unit;

llvm::Value *LogErrorV(const char *Str, ...) {
	va_list ap;
    va_start(ap, Str);
    LogErrorGen(Str, ap);
    va_end(ap);
	return nullptr;
}

llvm::Function *getFunction(std::string Name) {
	// First, see if the function has already been added to the current module.
	if (auto *F = TheModule->getFunction(Name))
		return F;

	// If not, check whether we can codegen the declaration from some existing
	// prototype.
	auto FI = FunctionProtos.find(Name);
	if (FI != FunctionProtos.end())
		return FI->second->codegen();

	// If no existing prototype exists, return null.
	return nullptr;
}

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
												llvm::StringRef VarName, llvm::Type* type) {
	llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
						   TheFunction->getEntryBlock().begin());
	return TmpB.CreateAlloca(type, nullptr, VarName);
}

llvm::Value *LiteralExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	switch (type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		return llvm::ConstantInt::get(*Context.getContext(), llvm::APInt(type->getIntegerBitWidth(), Val.Uint, type_attr & A_signed));
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
	case llvm::Type::FloatTyID:
	case llvm::Type::DoubleTyID:
		return llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat(Val.Float));
	case llvm::Type::PointerTyID:
		return Builder->CreateGlobalStringPtr(Val.Str);
	default:
		fprintf(stderr, "internal compiler error: unhandled literal type %d\n", type->getTypeID());
		return nullptr;
	}
}

llvm::Value *VariableExprAST::codegen() {
	// Look this variable up in the function.
	MapValue* full_type = map_string_get(locals_table, Name.c_str());
	if (!full_type)
		return LogErrorV("Unknown variable name1 %s", Name.c_str());
	llvm::Value *V = ((FullType*)((char*)full_type + full_type->offset))->val;

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Load the value.
	return Builder->CreateLoad(type, V, Name.c_str());
}

llvm::Value *UnaryExprAST::codegen() {
	llvm::Value *OperandV = Operand->codegen();
	if (!OperandV)
		return nullptr;

	llvm::Function *F = getFunction(std::string("unary") + Opcode);
	if (!F)
		return LogErrorV("Unknown unary operator");

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	return Builder->CreateCall(F, OperandV, "unop");
}

static const char* operr = "Op %s cannot create result for type ID %d";

llvm::Value *BinaryExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Special case '=' because we don't want to emit the LHS as an expression.
	if (!strcmp(Op, "=")) {
		// Assignment requires the LHS to be an identifier.
		// This assume we're building without RTTI because LLVM builds that way by
		// default.  If you build LLVM with RTTI this can be changed to a
		// dynamic_cast for automatic error checking.
		VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
		if (!LHSE)
			return LogErrorV("destination of '=' must be a variable");
		// Codegen the RHS.
		llvm::Value *Val = RHS->codegen();
		if (!Val)
			return nullptr;

		// Look up the name.
		MapValue* full_type = map_string_get(locals_table, LHSE->getName().c_str());
		if (!full_type)
			return LogErrorV("Unknown variable name2 %s", LHSE->getName().c_str());
		llvm::Value *Variable = ((FullType*)((char*)full_type + full_type->offset))->val;

		Builder->CreateStore(Val, Variable);
		return Val;
	}

	llvm::Value *L = LHS->codegen();
	llvm::Value *R = RHS->codegen();
	if (!L || !R)
		return nullptr;

	if (!strcmp(Op, "+")) {
		switch(type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			return Builder->CreateAdd(L, R, "addtmp");
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return Builder->CreateFAdd(L, R, "addtmp");
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "-")) {
		switch(type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			return Builder->CreateSub(L, R, "subtmp");
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return Builder->CreateFSub(L, R, "subtmp");
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "*")) {
		switch(type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			return Builder->CreateMul(L, R, "multmp");
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return Builder->CreateFMul(L, R, "multmp");
		default:
			LogError(operr, Op);
		}
		return Builder->CreateMul(L, R, "multmp");
	} else if (!strcmp(Op, "/")) {
		switch(type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			if (type_attr & A_signed)
				return Builder->CreateSDiv(L, R, "divtmp");
			else
				return Builder->CreateUDiv(L, R, "divtmp");
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return Builder->CreateFDiv(L, R, "divtmp");
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "%")) {
		switch(type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			if (type_attr & A_signed)
				return Builder->CreateSRem(L, R, "remtmp");
			else
				return Builder->CreateURem(L, R, "remtmp");
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return Builder->CreateFRem(L, R, "remtmp");
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "<")) {
		L = Builder->CreateFCmpULT(L, R, "cmptmp");
		// Convert bool 0/1 to double 0.0 or 1.0
		return Builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*Context.getContext()), "booltmp");
	}
	// If it wasn't a builtin binary operator, it must be a user defined one. Emit
	// a call to it.
	llvm::Function *F = getFunction(std::string("binary") + Op);
	assert(F && "binary operator not found!");

	llvm::Value *Ops[] = {L, R};
	return Builder->CreateCall(F, Ops, "binop");
}

llvm::Value *CallExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Look up the name in the global module table.
	llvm::Function *CalleeF = getFunction(Callee);
	if (!CalleeF)
		return LogErrorV("Unknown function referenced");

	// If argument mismatch error.
	if (CalleeF->arg_size() != Args.size())
		return LogErrorV("Incorrect # arguments passed");

	std::vector<llvm::Value *> ArgsV;
	for (unsigned i = 0, e = Args.size(); i != e; ++i) {
		ArgsV.push_back(Args[i]->codegen());
		if (!ArgsV.back())
			return nullptr;
	}

	return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

llvm::Value *IfExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	llvm::Value *CondV = Cond->codegen();
	if (!CondV)
		return nullptr;

	// Convert condition to a bool by comparing non-equal to 0.0.
	CondV = Builder->CreateFCmpONE(
		CondV, llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat(0.0)), "ifcond");

	llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Create blocks for the then and else cases.  Insert the 'then' block at the
	// end of the function.
	llvm::BasicBlock *ThenBB = llvm::BasicBlock::Create(*Context.getContext(), "then", TheFunction);
	llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(*Context.getContext(), "else");
	llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(*Context.getContext(), "ifcont");

	Builder->CreateCondBr(CondV, ThenBB, ElseBB);

	// Emit then value.
	Builder->SetInsertPoint(ThenBB);

	llvm::Value *ThenV = Then->codegen();
	if (!ThenV)
		return nullptr;

	Builder->CreateBr(MergeBB);
	// Codegen of 'Then' can change the current block, update ThenBB for the PHI.
	ThenBB = Builder->GetInsertBlock();

	// Emit else block.
	TheFunction->getBasicBlockList().push_back(ElseBB);
	Builder->SetInsertPoint(ElseBB);

	llvm::Value *ElseV = Else->codegen();
	if (!ElseV)
		return nullptr;

	Builder->CreateBr(MergeBB);
	// Codegen of 'Else' can change the current block, update ElseBB for the PHI.
	ElseBB = Builder->GetInsertBlock();

	// Emit merge block.
	TheFunction->getBasicBlockList().push_back(MergeBB);
	Builder->SetInsertPoint(MergeBB);
	llvm::PHINode *PN = Builder->CreatePHI(llvm::Type::getDoubleTy(*Context.getContext()), 2, "iftmp");

	PN->addIncoming(ThenV, ThenBB);
	PN->addIncoming(ElseV, ElseBB);
	return PN;
}

// Output for-loop as:
//   var = alloca double
//   ...
//   start = startexpr
//   store start -> var
//   goto loop
// loop:
//   ...
//   bodyexpr
//   ...
// loopend:
//   step = stepexpr
//   endcond = endexpr
//
//   curvar = load var
//   nextvar = curvar + step
//   store nextvar -> var
//   br endcond, loop, endloop
// outloop:
llvm::Value *ForExprAST::codegen() {
	llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Create an alloca for the variable in the entry block.
	llvm::Type* AllocaT = llvm::Type::getInt32Ty(*Context.getContext());
	unsigned AllocaF = A_signed;
	llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, AllocaT);

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Emit the start code first, without 'variable' in scope.
	llvm::Value *StartVal = Start->codegen();
	if (!StartVal)
		return nullptr;

	// Store the value into the alloca.
	Builder->CreateStore(StartVal, Alloca);

	// Make the new basic block for the loop header, inserting after current
	// block.
	llvm::BasicBlock *LoopBB = llvm::BasicBlock::Create(*Context.getContext(), "loop", TheFunction);

	// Insert an explicit fall through from the current block to the LoopBB.
	Builder->CreateBr(LoopBB);

	// Start insertion in LoopBB.
	Builder->SetInsertPoint(LoopBB);

	// Within the loop, the variable is defined equal to the PHI node.  If it
	// shadows an existing variable, we have to restore it, so save it now.
	MapValue* OldValPtr = map_string_get(locals_table, VarName.c_str());
	llvm::AllocaInst *OldVal = OldValPtr ? ((FullType*)((char*)OldValPtr + OldValPtr->offset))->val : nullptr;
	if (OldVal) {
		((FullType*)((char*)OldValPtr + OldValPtr->offset))->val = Alloca;
	} else {
		FullType ft = {
			.type = AllocaT,
			.val = Alloca,
			.type_attr = AllocaF
		};
		map_string_insert(&locals_table, VarName.c_str(), (MapValue){ .src_ptr = &ft }, sizeof(FullType));
	}
	// Emit the body of the loop.  This, like any other expr, can change the
	// current BB.  Note that we ignore the value computed by the body, but don't
	// allow an error.
	if (!Body->codegen())
		return nullptr;

	// Emit the step value.
	llvm::Value *StepVal = nullptr;
	if (Step) {
		StepVal = Step->codegen();
		if (!StepVal)
			return nullptr;
	} else {
		// If not specified, use 1.0.
		StepVal = llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat(1.0));
	}

	// Compute the end condition.
	llvm::Value *EndCond = End->codegen();
	if (!EndCond)
		return nullptr;

	// Reload, increment, and restore the alloca.  This handles the case where
	// the body of the loop mutates the variable.
	llvm::Value *CurVar = Builder->CreateLoad(llvm::Type::getDoubleTy(*Context.getContext()), Alloca,
											  VarName.c_str());
	llvm::Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
	Builder->CreateStore(NextVar, Alloca);

	// Convert condition to a bool by comparing non-equal to 0.0.
	EndCond = Builder->CreateFCmpONE(
		EndCond, llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat(0.0)), "loopcond");

	// Create the "after loop" block and insert it.
	llvm::BasicBlock *AfterBB =
		llvm::BasicBlock::Create(*Context.getContext(), "afterloop", TheFunction);

	// Insert the conditional branch into the end of LoopEndBB.
	Builder->CreateCondBr(EndCond, LoopBB, AfterBB);

	// Any new code will be inserted in AfterBB.
	Builder->SetInsertPoint(AfterBB);

	// Restore the unshadowed variable.
	if (OldVal)
		((FullType*)((char*)OldValPtr + OldValPtr->offset))->val = OldVal;
	else
		map_string_delete(&locals_table, VarName.c_str());
	// for expr always returns 0.0.
	return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*Context.getContext()));
}

llvm::Value *VarExprAST::codegen() {
	std::vector<llvm::AllocaInst *> OldBindings;

	llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Register all variables and emit their initializer.

	unsigned LineNo = CurLoc.Line;
	for (unsigned i = 0, e = VarNames.size(); i != e; ++i) {
		const std::string &VarName = VarNames[i].first;
		ExprAST *Init = VarNames[i].second.get();

		// Emit the initializer before adding the variable to scope, this prevents
		// the initializer from referencing the variable itself, and permits stuff
		// like this:
		//  var a = 1 in
		//    var a = a in ...   # refers to outer 'a'.
		llvm::Value *InitVal;
		if (Init) {
			InitVal = Init->codegen();
			if (!InitVal)
				return nullptr;
		} else { // If not specified, use 0.0.
			InitVal = llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat(0.0));
		}

		llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, llvm::Type::getInt32Ty(*Context.getContext()));
		if (comp_mode == comp_dbg) {
			// Create a debug descriptor for the variable.
			llvm::DILocalVariable *D = DBuilder->createAutoVariable(
				SP, VarName, Unit, LineNo, KSDbgInfo.getDoubleTy(),
				true);

			DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
									llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
									Builder->GetInsertBlock());
		}
		Builder->CreateStore(InitVal, Alloca);

		// Remember the old variable binding so that we can restore the binding when
		// we unrecurse.
		MapValue* OldValPtr = map_string_get(locals_table, VarName.c_str());
		OldBindings.push_back(((FullType*)((char*)OldValPtr + OldValPtr->offset))->val);

		// Remember this binding.
		((FullType*)((char*)OldValPtr + OldValPtr->offset))->val = Alloca;
	}

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Codegen the body, now that all vars are in scope.
	llvm::Value *BodyVal = Body->codegen();
	if (!BodyVal)
		return nullptr;

	// Pop all our variables from scope.
	for (unsigned i = 0, e = VarNames.size(); i != e; ++i) {
		MapValue* OldValPtr = map_string_get(locals_table, VarNames[i].first.c_str());
		((FullType*)((char*)OldValPtr + OldValPtr->offset))->val =  OldBindings[i];
	}
	// Return the body computation.
	return BodyVal;
}

llvm::Function *PrototypeAST::codegen() {
	// Make the function type:  double(double,double) etc.
	// TODO: support returning multiple objects
	auto RetType = RetTypes.size() == 1 ?
		RetTypes[0].first : llvm::Type::getVoidTy(*Context.getContext());
	llvm::FunctionType *FT =
		llvm::FunctionType::get(RetType, ArgTypes, false);

	llvm::Function *F =
		llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, TheModule.get());

	// Set names for all arguments.
	unsigned Idx = 0;
	for (auto &Arg : F->args())
		Arg.setName(Args[Idx++]);

	return F;
}

llvm::Function *FunctionAST::codegen() {
	// Transfer ownership of the prototype to the FunctionProtos map, but keep a
	// reference to it for use below.
	auto &P = *Proto;
	FunctionProtos[Proto->getName()] = std::move(Proto);
	llvm::Function *TheFunction = getFunction(P.getName());
	if (!TheFunction)
		return nullptr;

	// Create a new basic block to start insertion into.
	llvm::BasicBlock *BB = llvm::BasicBlock::Create(*Context.getContext(), "entry", TheFunction);
	Builder->SetInsertPoint(BB);
	// llvm::DISubprogram *SP; - make static
	// llvm::DIFile *Unit;
	unsigned LineNo;
	if (comp_mode == comp_dbg) {
		// Create a subprogram DIE for this function.
		Unit = DBuilder->createFile(KSDbgInfo.TheCU->getFilename(),
									KSDbgInfo.TheCU->getDirectory());
		llvm::DIScope *FContext = Unit;
		LineNo = P.getLine();
		unsigned ScopeLine = LineNo;
		SP = DBuilder->createFunction(
			FContext, P.getName(), llvm::StringRef(), Unit, LineNo,
			CreateFunctionType(TheFunction->arg_size(), Unit), ScopeLine,
			llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
		TheFunction->setSubprogram(SP);
	  
		// Push the current scope.
		KSDbgInfo.LexicalBlocks.push_back(SP);

		// Unset the location for the prologue emission (leading instructions with no
		// location in a function are considered part of the prologue and the debugger
		// will run past them when breaking on a function)
		KSDbgInfo.emitLocation(nullptr);
	}
	// Record the function arguments in the NamedValues map.
	unsigned ArgIdx = 0;
	for (auto &Arg : TheFunction->args()) {
		// Create an alloca for this variable.
		llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Arg.getName(), P.ArgTypes[ArgIdx]);
		// get reference to argument in symbol table
		MapValue* mapitem = map_string_get(locals_table, Arg.getName().str().c_str());
		if (!mapitem) {
			fprintf(stderr, "internal compiler error: arg not found in table");
			exit(1);
		}
		llvm::Type* type = ((FullType*)((char*)mapitem + mapitem->offset))->type;
		if (comp_mode == comp_dbg) {
			// Create a debug descriptor for the variable.
			llvm::DILocalVariable *D = DBuilder->createParameterVariable(
				SP, Arg.getName(), ++ArgIdx, Unit, LineNo, KSDbgInfo.getDoubleTy() /* FIXME */,
				true);

			DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
									llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
									Builder->GetInsertBlock());
		}
		// Store the initial value into the alloca.
		Builder->CreateStore(&Arg, Alloca);

		// Add storage to variable in symbol table.
		((FullType*)((char*)mapitem + mapitem->offset))->val = Alloca;
	}

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(Body.get());
	}
	if (llvm::Value *RetVal = Body->codegen()) {
		// Finish off the function.
		Builder->CreateRet(RetVal);
		if (comp_mode == comp_dbg) {
			// Pop off the lexical block for the function.
			KSDbgInfo.LexicalBlocks.pop_back();
		}
		// Validate the generated code, checking for consistency.
		verifyFunction(*TheFunction);
		// Run the optimizer on the function.
		if (comp_mode == comp_jit) {
			TheFPM->run(*TheFunction);
		}

		return TheFunction;
	}

	// Error reading body, remove function.
	TheFunction->eraseFromParent();

	if (comp_mode == comp_dbg) {
		// Pop off the lexical block for the function since we added it
		// unconditionally.
		KSDbgInfo.LexicalBlocks.pop_back();
	}
	return nullptr;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

static void InitializeModuleAndPassManager() {
	// Open a new module.
	static bool has_run = false;
	if (!has_run) {
		TheContext = std::make_unique<llvm::LLVMContext>();
		Context = llvm::orc::ThreadSafeContext(std::move(TheContext));
		init();
		has_run = true;
	}
	TheModule = std::make_unique<llvm::Module>("my cool jit", *Context.getContext());
	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		TheModule->setDataLayout(
#if LLVM_VERSION_MAJOR >= 12
			TheJIT->getDataLayout()
#else
			TheJIT->getTargetMachine().createDataLayout()
#endif
			);
	}
	
	// Create a new builder for the module.
	Builder = std::make_unique<llvm::IRBuilder<>>(*Context.getContext());

	if (comp_mode == comp_jit) {
		// Create a new pass manager attached to it.
		TheFPM = std::make_unique<llvm::legacy::FunctionPassManager>(TheModule.get());
	  
		// Promote allocas to registers.
		TheFPM->add(llvm::createPromoteMemoryToRegisterPass());
		// Do simple "peephole" optimizations and bit-twiddling optzns.
		TheFPM->add(llvm::createInstructionCombiningPass());
		// Reassociate expressions.
		TheFPM->add(llvm::createReassociatePass());
		// Eliminate Common SubExpressions.
		TheFPM->add(llvm::createGVNPass());
		// Simplify the control flow graph (deleting unreachable blocks, etc).
		TheFPM->add(llvm::createCFGSimplificationPass());

		TheFPM->doInitialization();
	}
}

static void HandleDefinition() {
	if (auto FnAST = ParseDefinition()) {
		if (auto *FnIR = FnAST->codegen()) {
			if (comp_mode != comp_dbg) {
				fprintf(stderr, "Read function definition:");
				FnIR->print(llvm::errs());
				fprintf(stderr, "\n");
				if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
					ExitOnErr(TheJIT->addModule(
								  llvm::orc::ThreadSafeModule(std::move(TheModule), Context)));
#else
					TheJIT->addModule(std::move(TheModule));
#endif
					InitializeModuleAndPassManager();
				}
			}
		} else {
			fprintf(stderr, "Error reading function definition:");
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
	map_destroy(locals_table);
	locals_table = map_string_new_map();

	fprintf(stderr, "definition successfully handled\n");
}

static void HandleExtern() {
	if (auto ProtoAST = ParseExtern()) {
		if (auto *FnIR = ProtoAST->codegen()) {
			if (comp_mode != comp_dbg) {
				fprintf(stderr, "Read extern: ");
				FnIR->print(llvm::errs());
				fprintf(stderr, "\n");
			}
			FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
		} else {
			fprintf(stderr, "Error reading extern");
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleTopLevelExpression() {
	// Evaluate a top-level expression into an anonymous function.
	if (auto FnAST = ParseTopLevelExpr()) {
		auto RetType = (FnAST->Proto->RetTypes.size() == 1 ?
						FnAST->Proto->RetTypes[0].first :
						llvm::Type::getVoidTy(*Context.getContext()))->getTypeID();
		unsigned ret_type_attr = (FnAST->Proto->RetTypes.size() == 1 ?
								  FnAST->Proto->RetTypes[0].second :
								  0);

		auto anon_expr = FnAST->codegen();
		if (anon_expr) {
			if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
				// Create a ResourceTracker to track JIT'd memory allocated to our
				// anonymous expression -- that way we can free it after executing.
				auto RT = TheJIT->getMainJITDylib().createResourceTracker();
				auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), Context);
				ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
#else
				// JIT the module containing the anonymous expression, keeping a handle so
				// we can free it later.
				auto H = TheJIT->addModule(std::move(TheModule));
#endif
				InitializeModuleAndPassManager();

				// Search the JIT for the __anon_expr symbol.
#if LLVM_VERSION_MAJOR >= 12
				auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
#define UNWRAP(x) (x)
#else
				auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
				assert(ExprSymbol && "Function not found");
#define UNWRAP(x) cantFail(x)
#endif
			  
				// Get the symbol's address and cast it to the right type (takes no
				// arguments, returns a double) so we can call it as a native function.
				//std::vector<llvm::GenericValue> Args;
				//llvm::GenericValue gv = TheJIT->runFunction(anon_expr, Args);
				switch (RetType) {
				case llvm::Type::DoubleTyID:
				{
					double (*FP)() = (double (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
					fprintf(stderr, "Evaluated to %f\n", FP());
				}
				break;
				case llvm::Type::IntegerTyID:
				{
					if (ret_type_attr & A_signed) {
						int (*INT)() = (int (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
						fprintf(stderr, "Evaluated to %d\n", INT());
					} else {
						unsigned (*UINT)() = (unsigned (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
						fprintf(stderr, "Evaluated to %u\n", UINT());
					}
				}
				break;
				case llvm::Type::PointerTyID: // should be more sophisticated
				{
					const char* (*SP)() = (const char* (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
					fprintf(stderr, "Evaluated to >%s<\n", SP());
				}
				break;
				default:
					fprintf(stderr, "unknown expression type %d\n", RetType);
				}

#if LLVM_VERSION_MAJOR >= 12
				// Delete the anonymous expression module from the JIT.
				ExitOnErr(RT->remove());
#else
				// Delete the anonymous expression module from the JIT.
				TheJIT->removeModule(H);
#endif
			}
		} else {
			fprintf(stderr, "Error generating code for top level expr");
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
	while (true) {
		if (comp_mode == comp_jit) {
			fprintf(stderr, CurTok.kind == tok_eof ? "\n" : "ready> ");
		}
		switch (CurTok.kind) {
		case tok_eof:
			return;
		case ';': // ignore top-level semicolons.
			getNextToken();
			break;
		case tok_fn:
			HandleDefinition();
			break;
		case tok_extern:
			HandleExtern();
			break;
		default:
			HandleTopLevelExpression();
			break;
		}
	}
}

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
	fputc((char)X, stderr);
	return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
	fprintf(stderr, "%f\n", X);
	return 0;
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main(int argc, char* argv[]) {
	FILE* input_file = stdin;
	const char* input_file_name = "a.vx";
	if (argc == 1) {
		comp_mode = comp_jit;
	} else {
		if ((argc == 3) && std::string(argv[1]) == "-g") {
			comp_mode = comp_dbg;
			input_file_name = argv[2];
		} else {
			input_file_name = argv[1];
		}
		input_file = fopen(input_file_name, "r");
		if (!input_file) {
			fprintf(stderr, "Cannot open input file \"%s\": %s\n", input_file_name, strerror(errno));
			exit(1);
		}
	}
	int len = strlen(input_file_name);
	auto output_file = (char*)malloc(len + 3);
	strcpy(output_file, input_file_name);
	if(output_file[len-3]=='.' && output_file[len-2]=='v' && output_file[len-1]=='x') {
		output_file[len-2] = 'o';
		output_file[len-1] = '\0';
	} else {
		output_file[len] = '.';
		output_file[len+1] = 'o';
		output_file[len+2] = '\0';
	}
	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		llvm::InitializeNativeTargetAsmParser();
	}

	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		TheJIT =
#if LLVM_VERSION_MAJOR >= 12
			ExitOnErr(llvm::orc::VolvoxJIT::Create());
#else
			std::make_unique<llvm::orc::VolvoxJIT>();
#endif
	}

	InitializeModuleAndPassManager();

	// Prime the first token.
	if (comp_mode == comp_jit) {
		fprintf(stderr, "ready> ");
	}
	getNextToken();

	if (comp_mode == comp_dbg) {
		// Add the current debug info version into the module.
		TheModule->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
								 llvm::DEBUG_METADATA_VERSION);
		// Darwin only supports dwarf2.
		if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
			TheModule->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);
	  
		// Construct the DIBuilder, we do this here because we need the module.
		DBuilder = std::make_unique<llvm::DIBuilder>(*TheModule);
		// Create the compile unit for the module.
		// Currently down as "fib.ks" as a filename since we're redirecting stdin
		// but we'd like actual source locations.
		KSDbgInfo.TheCU = DBuilder->createCompileUnit(
			llvm::dwarf::DW_LANG_C, DBuilder->createFile(input_file_name, "."),
			"Volvox Compiler", 0, "", 0);
	}
	// Run the main "interpreter loop" now.
	MainLoop();

	if (comp_mode == comp_obj) {
		// Initialize the target registry etc.
		llvm::InitializeAllTargetInfos();
		llvm::InitializeAllTargets();
		llvm::InitializeAllTargetMCs();
		llvm::InitializeAllAsmParsers();
		llvm::InitializeAllAsmPrinters();
		auto TargetTriple = llvm::sys::getDefaultTargetTriple();
		TheModule->setTargetTriple(TargetTriple);

		std::string Error;
		auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);
		// Print an error and exit if we couldn't find the requested target.
		// This generally occurs if we've forgotten to initialise the
		// TargetRegistry or we have a bogus target triple.
		if (!Target) {
			llvm::errs() << Error;
			return 1;
		}

		auto CPU = "generic";
		auto Features = "";
		llvm::TargetOptions opt;
		auto RM = llvm::Optional<llvm::Reloc::Model>();
		auto TheTargetMachine =
			Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);
	  
		TheModule->setDataLayout(TheTargetMachine->createDataLayout());
	  
		auto Filename = output_file;
		std::error_code EC;
		llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

		if (EC) {
			llvm::errs() << "Could not open file: " << EC.message();
			return 1;
		}
	  
		llvm::legacy::PassManager pass;
		auto FileType = llvm::CGFT_ObjectFile;
	  
		if (TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
			llvm::errs() << "TheTargetMachine can't emit a file of this type";
			return 1;
		}
	  
		pass.run(*TheModule);
		dest.flush();
	  
		llvm::outs() << "Wrote " << Filename << "\n";
	} else if (comp_mode == comp_dbg) {
	    // Finalize the debug info.
		DBuilder->finalize();
	  
		// Print out all of the generated code.
		TheModule->print(llvm::errs(), nullptr);
	}
	return 0;
}
