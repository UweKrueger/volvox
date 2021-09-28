#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::DIBuilder> DBuilder;

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
		fprintf(stderr, "Warning: 16 bit floats are not supported, yet\n");
		// passthrough to 32 bit float for now - but expect problems...
	case llvm::Type::FloatTyID:
		return llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat((float)Val.Float));
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
	FullType* full_type = locals_table[Name.c_str()];
	if (!full_type)
		return LogErrorV("Unknown variable name1 %s", Name.c_str());
	llvm::Value *V = full_type->val;

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

enum TypeClass {
	is_unknown = 0,
	is_float,
	is_int,
	is_string,
	is_other
};

llvm::Value *BinaryExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	bool is_bool = desired_type == llvm::Type::getInt1Ty(*Context.getContext()) || type == llvm::Type::getInt1Ty(*Context.getContext());
	// Special case '=' because we don't want to emit the LHS as an expression.
	if (!strcmp(Op, "=") && !is_bool) {
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
		FullType* full_type = locals_table[LHSE->getName().c_str()];
		if (!full_type)
			return LogErrorV("Unknown variable name2 %s", LHSE->getName().c_str());
		llvm::Value *Variable = full_type->val;

		Builder->CreateStore(Val, Variable);
		return Val;
	}
	llvm::Value* result;
	if (!desired_type) {
		desired_type = type;
		desired_type_attr = type_attr;
	}
	if (!is_bool) {
		if (auto BinL = dynamic_cast<BinaryExprAST*>(LHS.get())) {
			BinL->desired_type = desired_type;
			BinL->desired_type_attr = desired_type_attr;
		}
		if (auto BinR = dynamic_cast<BinaryExprAST*>(RHS.get())) {
			BinR->desired_type = desired_type;
			BinR->desired_type_attr = desired_type_attr;
		}
	}
	llvm::Value *L = LHS->codegen();
	llvm::Value *R = RHS->codegen();
	if (!L || !R)
		return nullptr;
	if (desired_type && !conv.ideal.err_msg) {
		auto ana = analyze_types({ conv.ideal.res_type, conv.ideal.res_attr }, { desired_type, desired_type_attr });
		// printf("desired_type: %s %d %d\n", type_table.get_name((llvm::Type*)((uintptr_t)desired_type | (desired_type_attr & A_signed))), ana.first, (ana.second && !conv.compat.err_msg));
		if (ana.first || (ana.second && !conv.compat.err_msg)) {
			if (conv.ideal.LHS)
				L = conv.ideal.LHS(L);
			if (conv.ideal.RHS)
				R = conv.ideal.RHS(R);
			goto conv_done;
		}
			
	} 
	if (conv.compat.err_msg)
		return AutoErr(Loc, LHS->type, RHS->type, LHS->type_attr, RHS->type_attr, conv.compat.err_msg);
	if (conv.compat.LHS)
		L = conv.compat.LHS(L);
	if (conv.compat.RHS)
		R = conv.compat.RHS(R);
conv_done:
	TypeClass typeclass = is_unknown;
	switch(type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		typeclass = is_int;
		break;
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
	case llvm::Type::FloatTyID:
	case llvm::Type::DoubleTyID:
		typeclass = is_float;
		break;
	default:
		typeclass = is_unknown;
	}

	if (!strcmp(Op, "+")) {
		switch(typeclass) {
		case is_int:
			result = Builder->CreateAdd(L, R, "addtmp");
			break;
		case is_float:
			result = Builder->CreateFAdd(L, R, "addtmp");
			break;
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "-")) {
		switch(typeclass) {
		case is_int:
			result = Builder->CreateSub(L, R, "subtmp");
			break;
		case is_float:
			result = Builder->CreateFSub(L, R, "subtmp");
			break;
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "*")) {
		switch(typeclass) {
		case is_int:
			result = Builder->CreateMul(L, R, "multmp");
			break;
		case is_float:
			result = Builder->CreateFMul(L, R, "multmp");
			break;
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "/")) {
		switch(typeclass) {
		case is_int:
			if (type_attr & A_signed)
				result = Builder->CreateSDiv(L, R, "divtmp");
			else
				result = Builder->CreateUDiv(L, R, "divtmp");
			break;
		case is_float:
			result = Builder->CreateFDiv(L, R, "divtmp");
			break;
		default:
			LogError(operr, Op);
		}
	} else if (!strcmp(Op, "%")) {
		switch(typeclass) {
		case is_int:
			if (type_attr & A_signed)
				result = Builder->CreateSRem(L, R, "remtmp");
			else
				result = Builder->CreateURem(L, R, "remtmp");
			break;
		case is_float:
			result = Builder->CreateFRem(L, R, "remtmp");
			break;
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
	if (result) {
		auto conv = getConv(type, desired_type, type_attr, desired_type_attr, Loc, true);
		if (conv)
			result = conv(result);
		return result;
	}
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
	FullType* OldValPtr = locals_table[VarName.c_str()];
	llvm::AllocaInst *OldVal = OldValPtr ? OldValPtr->val : nullptr;
	if (OldVal) {
		OldVal = Alloca;
	} else {
		FullType ft = {
			.type = AllocaT,
			.val = Alloca,
			.type_attr = AllocaF
		};
		locals_table.insert(VarName.c_str(), ft);
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
		OldValPtr->val = OldVal;
	else
		locals_table.erase(VarName.c_str());
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
		FullType* OldValPtr = locals_table[VarName.c_str()];
		OldBindings.push_back(OldValPtr->val);

		// Remember this binding.
		OldValPtr->val = Alloca;
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
		FullType* OldValPtr = locals_table[VarNames[i].first.c_str()];
		OldValPtr->val =  OldBindings[i];
	}
	// Return the body computation.
	return BodyVal;
}

llvm::Function *PrototypeAST::codegen() {
	// Make the function type:  double(double,double) etc.
	// TODO: support returning multiple objects
	auto RetType = RetTypes.size() == 1 ?
		RetTypes[0].first : llvm::Type::getVoidTy(*Context.getContext());
	if (!RetType) // RetTypes[0] exists but type could not be derived
		return nullptr;
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
		FullType* mapitem = locals_table[Arg.getName().str().c_str()];
		if (!mapitem) {
			fprintf(stderr, "internal compiler error: arg not found in table");
			exit(1);
		}
		llvm::Type* type = mapitem->type;
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
		mapitem->val = Alloca;
	}

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(Body.get());
	}
	Body->desired_type = P.RetTypes[0].first;
	Body->desired_type_attr = P.RetTypes[0].second;
	if (llvm::Value *RetVal = Body->codegen()) {
		// Finish off the function.
		auto ret_type = RetVal->getType();
		//type = ret_type; // TODO: hande conversion if != proto->type;
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
