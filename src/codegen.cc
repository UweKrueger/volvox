#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"
#include "../lib/str.h"

//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::DIBuilder> DBuilder;
bool inside_function = false;
static llvm::ExitOnError ExitOnErr;

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

static llvm::DISubroutineType *CreateFunctionType(volvox::FullType* RetType, std::vector<volvox::FullType*>& ArgTypes, llvm::DIFile *Unit) {
	llvm::SmallVector<llvm::Metadata *, 8> EltTys;

	// Add the result type.
	EltTys.push_back(type_table.get_diType(RetType->type, RetType->type_attr & A_signed));
	auto NumArgs = ArgTypes.size();
	for (unsigned i = 0; i < NumArgs; i++)
		EltTys.push_back(type_table.get_diType(ArgTypes[i]->type, ArgTypes[i]->type_attr & A_signed));

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

std::pair<llvm::Function*, PrototypeAST*> getFunction(std::string Name) {
	auto FI = FunctionProtos.find(Name);
	if (FI == FunctionProtos.end())
		return { nullptr, nullptr };
	// See if the function has already been added to the current module.
	if (auto F = TheModule->getFunction(Name)) {
		return { F, FI->second.get() };
	}
	
	// codegen the declaration from the existing prototype.
	auto F = FI->second->codegen();
	return { F, FI->second.get() };
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
	switch (ft->type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		return llvm::ConstantInt::get(*Context.getContext(), llvm::APInt(ft->type->getIntegerBitWidth(), Val.Uint, ft->type_attr & A_signed));
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
		eprt("Warning: 16 bit floats are not supported, yet\n");
		// passthrough to 32 bit float for now - but expect problems...
	case llvm::Type::FloatTyID:
		return llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat((float)Val.Float));
	case llvm::Type::DoubleTyID:
		return llvm::ConstantFP::get(*Context.getContext(), llvm::APFloat(Val.Float));
	case llvm::Type::PointerTyID:
		if (ft->type_attr & A_signed)
			return Builder->CreateIntToPtr(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Context.getContext()), Val.Uint, false), llvm::Type::getInt8PtrTy(*Context.getContext()));
		else
			return Builder->CreateGlobalStringPtr(Val.Str, "", 0, TheModule.get());
	default:
		eprt("internal compiler error: unhandled literal type %d\n", ft->type->getTypeID());
		return nullptr;
	}
}

llvm::Value *AggregateExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	std::vector<llvm::Constant*> Initializers;
	for (auto& e: Elements)
		if (is_compile_time_const) {
			auto conversion = getConv(e->ft->type, ft->elem_type->type, e->ft->type_attr, ft->elem_type->type_attr, Loc, false, false);
			if (!conversion) {
				eprt("Cannot convert array element\n");
				return nullptr;
			}
			Initializers.push_back(llvm::dyn_cast<llvm::Constant>(conversion(e->codegen())));
		} else {
			Initializers.push_back(llvm::Constant::getNullValue(ft->elem_type->type));
		}
	return llvm::ConstantArray::get(reinterpret_cast<llvm::ArrayType*>(ft->type), Initializers);
}

llvm::Value *VariableExprAST::codegen() {
	if (!full_var.first)
		return LogErrorV("Unknown variable name1 %s", Name.c_str());
	// if (full_var.first->ft.type->isFunctionTy()) {
	// 	auto theFunction = getFunction(Name);
	// 	return theFunction.first;
	// }
	auto V = codegen_ref();
	// Load the value.
	if (full_var.second) { // global variable
		return Builder->CreateLoad(full_var.first->storage_type, V, Name.c_str());
	} else {
		return Builder->CreateLoad(full_var.first->ft.type, V, Name.c_str());
	}
}

llvm::Value *VariableExprAST::codegen_ref() {
	llvm::Value* V;
	if (full_var.second) { // global variable
		V = TheModule->getGlobalVariable(Name, true);
		if (!V) {
			V = new llvm::GlobalVariable(*TheModule, full_var.first->storage_type,
			                             false, llvm::GlobalValue::ExternalLinkage,
			                             nullptr, Name, nullptr,
			                             llvm::GlobalVariable::GeneralDynamicTLSModel,
			                             0, true);
		}
	} else {
		V = full_var.first->val;
	}

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	return V;
}

llvm::Value* FunctionExprAST::codegen() {
	if (auto F = TheModule->getFunction(Name)) {
		return F;
	}
	return ft->proto->codegen();
}

llvm::Value *UnaryExprAST::codegen() {
	if (Opcode[0] == '&') {
		if (auto V = dynamic_cast<VariableExprAST*>(Operand.get())) {
			return V->codegen_ref();
		} else {
			llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
			llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
			                       TheFunction->getEntryBlock().begin());
			llvm::AllocaInst* Alloca = TmpB.CreateAlloca(Operand->ft->type);
			Builder->CreateStore(Operand->codegen(), Alloca);
			return Alloca;
		}
	}
	llvm::Value *OperandV = Operand->codegen();
	if (!OperandV)
		return nullptr;

	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	switch (OperandV->getType()->getTypeID()) {
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
	case llvm::Type::FloatTyID:
	case llvm::Type::DoubleTyID:
		switch (Opcode[0]) {
		case '+':
			return OperandV;
		case '-':
			return Builder->CreateFNeg(OperandV, "negftmp");
		// TODO: case '&'
		default:
			return LogErrorV("unary operator '%c' undefined for floats", Opcode[0]);
		}
	case llvm::Type::IntegerTyID:
		if (Opcode[0] != '!' && OperandV->getType()->getIntegerBitWidth() == 1)
			return LogErrorV("unary operator '%c' undefined for bool", Opcode[0]);
		switch (Opcode[0]) {
		case '+':
			return OperandV;
		case '-':
			return Builder->CreateNeg(OperandV, "negtmp");
		case '!':
			return Builder->CreateNot(OperandV, "nottmp");
		default:
			return LogErrorV("unary operator '%c' undefined for integers", Opcode[0]);
		}
	default:
		auto F = getFunction(std::string("unary") + Opcode);
		if (!F.first)
			return LogErrorV("Unknown unary operator");
		// TODO: operand types
		return Builder->CreateCall(F.first, OperandV, "unop");
	}
}

static const char* operr = "Op %s cannot create result for type ID %d";

enum TypeClass {
	is_unknown = 0,
	is_float,
	is_int,
	is_string,
	is_other
};

enum OpKind {
	other_op = 0,
	assign_op,
	decl_assign_op,
	modification_op,
};

std::nullptr_t HandleGlobalVariable(BinaryExprAST* expr) {
	if (auto Val = expr->RHS->codegen()) {
		VariableExprAST* LHSE = static_cast<VariableExprAST *>(expr->LHS.get());
		const char* varname = LHSE->getName().c_str();
		llvm::Type* val_type = Val->getType();
		auto type_descr = MakeType(expr->RHS->ft->type, expr->RHS->ft->type_attr & A_signed, expr->RHS->is_unknown_type);
		llvm::Type* type = std::get<0>(type_descr);
		auto conversion = std::get<1>(type_descr);
		bool is_signed = std::get<2>(type_descr);
		auto convertedVal = conversion(Val);
		if (auto initializer = llvm::dyn_cast<llvm::Constant>(convertedVal)) {
			llvm::GlobalVariable* GV;
			if (comp_mode == comp_dbg) {
				// Create a debug descriptor for the variable.
				DBuilder->createGlobalVariableExpression(
					SP, varname, varname, Unit, expr->Loc.Line, type_table.get_diType(type, is_signed), false);
			}
			GV = new llvm::GlobalVariable(*TheModule, initializer->getType(),
			                              false, llvm::GlobalValue::ExternalLinkage,
			                              initializer, varname, nullptr,
			                              llvm::GlobalVariable::GeneralDynamicTLSModel);
			if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
				ExitOnErr(TheJIT->addModule(
					          llvm::orc::ThreadSafeModule(std::move(TheModule), Context)));
#else
				TheJIT->addModule(std::move(TheModule));
#endif
				InitializeModuleAndPassManager();
			}
			volvox::FullType ft = *expr->RHS->ft;
			ft.type = type;
			ft.type_attr = is_signed ? 1U : 0U;
			FullVar fv = {
				.storage_type = initializer->getType(),
				.ft = ft,
			};
			globals_table.insert(varname, fv);
			return nullptr;
		} else {
			goto nonconst;
		}
	} else {
		eprt("Could not generate assigned expression\n");
		return nullptr;
	}
nonconst:
	eprt("global variable must be initialized with compile time const\n");
	return nullptr;
}

llvm::Value *BinaryExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	bool is_bool = desired_type == llvm::Type::getInt1Ty(*Context.getContext()) || ft->type == llvm::Type::getInt1Ty(*Context.getContext());
	OpKind kind;
	if (Op[0] == '=')
		kind = assign_op;
	else if (Op[1] == '=')
		switch (Op[0]) {
		case ':':
			kind = decl_assign_op;
			break;
		case '+':
		case '-':
		case '*':
		case '/':
		case '%':
		case '&':
		case '|':
		case '^':
			kind = modification_op;
			break;
		default:
			kind = other_op;
		}
	else
		kind = other_op;
			
	// Special assign-like ops because we don't want to emit the LHS as an expression.
	// assign op '=' is a comparison (not an assignment) when a boolean result is expected
	if (kind != other_op && !(kind == assign_op && false /*is_bool */)) {
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
		const char* varname = LHSE->getName().c_str();
		FullVar* full_var = LHSE->full_var.first;
		bool is_global = LHSE->full_var.second;
		if (!full_var)
			goto not_found;
		if (kind == decl_assign_op) {
			return LogErrorV("cannot initialize existing variable %s", LHSE->getName().c_str());
		} else {
			auto Variable = LHSE->codegen_ref();
			auto OldVal = Builder->CreateLoad(full_var->ft.type, Variable, varname);
			Builder->CreateStore(Val, Variable);
			return OldVal;
		}
	not_found:
		llvm::Value* Variable = nullptr;
		if (kind != decl_assign_op)
			return LogErrorV("unknown variable name %s", varname);
		// variable declaration
		if (inside_function) {
			llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
			auto type_descr = MakeType(Val->getType(), RHS->ft->type_attr & A_signed, RHS->is_unknown_type);
			llvm::Type* type = std::get<0>(type_descr);
			auto conversion = std::get<1>(type_descr);
			bool is_signed = std::get<2>(type_descr);
			auto convertedVal = conversion(Val);
			llvm::AllocaInst* Alloca = CreateEntryBlockAlloca(TheFunction, varname, type);
			// Entry has already been created by parser
			locals_table.back()[varname]->val = Alloca;
			if (comp_mode == comp_dbg) {
				// Create a debug descriptor for the variable.
				llvm::DILocalVariable *D = DBuilder->createAutoVariable(
					SP, varname, Unit, LHS->Loc.Line, type_table.get_diType(type, is_signed),
					true);

				DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
										llvm::DILocation::get(SP->getContext(), LHS->Loc.Line, 0, SP),
										Builder->GetInsertBlock());
			}
			Builder->CreateStore(convertedVal, Alloca);
			return convertedVal;
		} else {
			return Val;
		}
		return Val;
	}
	llvm::Value* result;
	if (!desired_type) {
		desired_type = ft->type;
		desired_type_attr = ft->type_attr;
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
		if (ana.first || (ana.second && !conv.compat.err_msg)) {
			if (conv.ideal.LHS)
				L = conv.ideal.LHS(L);
			if (conv.ideal.RHS)
				R = conv.ideal.RHS(R);
			goto conv_done;
		}
			
	} 
	if (conv.compat.err_msg)
		return AutoErr(Loc, LHS->ft->type, RHS->ft->type, LHS->ft->type_attr, RHS->ft->type_attr, conv.compat.err_msg);
	if (conv.compat.LHS)
		L = conv.compat.LHS(L);
	if (conv.compat.RHS)
		R = conv.compat.RHS(R);
conv_done:
	// for comparisons ExprAST.type is bool, but we have to look at the operands that are in desired
	llvm::Type* OperandType = conv.compat.res_type;
	bool OperandSigned = conv.compat.res_attr | A_signed;
	TypeClass typeclass = is_unknown;
	switch(OperandType->getTypeID()) {
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

	switch (Op[0]) {
	case '+':
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
		break;
	case '-':
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
		break;
	case '*':
	case '\0':
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
		break;
	case '/':
		switch(typeclass) {
		case is_int:
			if (OperandSigned)
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
		break;
	case '%':
		switch(typeclass) {
		case is_int:
			if (OperandSigned)
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
		break;
	case '&':
		switch(typeclass) {
		case is_int:
			result = Builder->CreateAnd(L, R, "andtmp");
			break;
		default:
			LogError(operr, Op);
		}
		break;
	case '|':
		switch(typeclass) {
		case is_int:
			result = Builder->CreateOr(L, R, "ortmp");
			break;
		default:
			LogError(operr, Op);
		}
		break;
	case '^':
		switch(typeclass) {
		case is_int:
			result = Builder->CreateXor(L, R, "xortmp");
			break;
		default:
			LogError(operr, Op);
		}
		break;
	case '!':
		switch(typeclass) {
		case is_int:
			result = Builder->CreateNot(Builder->CreateXor(L, R, "xortmp"), "nxortmp");
			break;
		default:
			LogError(operr, Op);
		}
		break;
	case '<':
		if (Op[1] == '=') {
			switch(typeclass) {
			case is_int:
				if (OperandSigned)
					result = Builder->CreateICmpSLE(L, R, "lesitmp");
				else
					result = Builder->CreateICmpULE(L, R, "leuitmp");
				break;
			case is_float:
				result = Builder->CreateFCmpOLE(L, R, "leftmp");
				break;
			default:
				LogError(operr, Op);
			}
		} else if (Op[1] == '<') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateShl(L, R, "remtmp");
				break;
			default:
				LogError(operr, Op);
			}
			break;
		} else {
			switch(typeclass) {
			case is_int:
				if (OperandSigned)
					result = Builder->CreateICmpSLT(L, R, "ltsitmp");
				else
					result = Builder->CreateICmpULT(L, R, "ltuitmp");
				break;
			case is_float:
				result = Builder->CreateFCmpOLT(L, R, "ltftmp");
				break;
			default:
				LogError(operr, Op);
			}
		}
		break;
	case '>':
		if (Op[1] == '=') {
			switch(typeclass) {
			case is_int:
				if (OperandSigned)
					result = Builder->CreateICmpSGE(L, R, "gesitmp");
				else
					result = Builder->CreateICmpUGE(L, R, "geuitmp");
				break;
			case is_float:
				result = Builder->CreateFCmpOGE(L, R, "geftmp");
				break;
			default:
				LogError(operr, Op);
			}
		} else if (Op[1] == '>') {
			switch(typeclass) {
			case is_int:
				if (OperandSigned)
					result = Builder->CreateAShr(L, R, "remtmp");
				else
					result = Builder->CreateLShr(L, R, "remtmp");
				break;
			default:
				LogError(operr, Op);
			}
			break;
		} else {
			switch(typeclass) {
			case is_int:
				if (OperandSigned)
					result = Builder->CreateICmpSGT(L, R, "gtsitmp");
				else
					result = Builder->CreateICmpUGT(L, R, "gtuitmp");
				break;
			case is_float:
				result = Builder->CreateFCmpOGT(L, R, "gtftmp");
				break;
			default:
				LogError(operr, Op);
			}
		}
		break;
	}
	if (result) {
		auto conv = getConv(ft->type, desired_type, ft->type_attr, desired_type_attr, Loc, true, is_unknown_type);
		if (conv) {
			// dprt("converted result of binop from %s to %s (%s)\n",
			//        type_table.get_name(ft->type, ft->type_attr & A_signed),
			//        type_table.get_name(desired_type, desired_type_attr & A_signed),
			//        is_unknown_type ? "literal" : "explicit type");
			result = conv(result);
		}
		return result;
	}
	// If it wasn't a builtin binary operator, it must be a user defined one. Emit
	// a call to it.
	auto F = getFunction(std::string("binary") + Op);
	assert(F.first && "binary operator not found!");

	llvm::Value *Ops[] = {L, R};
	return Builder->CreateCall(F.first, Ops, "binop");
}

llvm::Value *CallExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Look up the name in the global module table.
	PrototypeAST* Proto = Callee->ft->proto;
	llvm::Value* theFunction = Callee->codegen();
	auto FT = llvm::cast<llvm::FunctionType>(Callee->ft->type);
	// If argument mismatch error.
	if (FT->getNumParams() > Args.size() || FT->getNumParams() < Args.size() && !Proto->IsVarArgs || FT->getNumParams() != Proto->Args.size())
		return LogErrorV("Incorrect # arguments passed");

	std::vector<llvm::Value *> ArgsV;
	for (unsigned i = 0, e = Args.size(), v = Proto->Args.size(); i != e; ++i) {
		if (i < v && (Proto->ArgTypes[i]->type->isIntegerTy() || Proto->ArgTypes[i]->type->isFloatingPointTy())) {
			auto conversion = getConv(
				Args[i]->ft->type, Proto->ArgTypes[i]->type,
				Args[i]->ft->type_attr, Proto->ArgTypes[i]->type_attr,
				Args[i]->Loc, false, Args[i]->is_unknown_type);
			if (!conversion)
				return nullptr;
			ArgsV.push_back(conversion(Args[i]->codegen()));
		} else {
			if (i < v && Args[i]->ft->type->getTypeID() != Proto->ArgTypes[i]->type->getTypeID())
				// TODO: better check compatibility and make error message human readable
				return LogErrorV("Wrong type passed for function arg #%d %u %u", i, Args[i]->ft->type->getTypeID(), Proto->ArgTypes[i]->type->getTypeID());
			ArgsV.push_back(Args[i]->codegen());
		}
		if (!ArgsV.back())
			return nullptr;
	}
	if (auto F = llvm::dyn_cast<llvm::Function>(theFunction)) {
		// Callee was a function symbol like `sin`
		return Builder->CreateCall(F, ArgsV, "calltmp");
	} else {
		// theFunction is a function pointer, i.e. a function call address (e.g. loaded from a variable)
		return Builder->CreateCall(FT, theFunction, ArgsV, "callptrtmp");
	}
}

inline static llvm::Value* CheckTailCall(llvm::Value* V) {
	if (auto C = llvm::dyn_cast<llvm::CallInst>(V))
		C->setTailCall();
	return V;
}

llvm::Value *IfExprAST::codegen() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	llvm::Value *CondV = Cond->codegen();
	if (!CondV)
		return nullptr;

	// Convert condition to a bool by comparing non-equal to 0.0.
	if (CondV->getType() != llvm::Type::getInt1Ty(*Context.getContext()))
		return Error(Cond->Loc, "bool type expected as \"if\" condition");

	llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Create blocks for the then and else cases.  Insert the 'then' block at the
	// end of the function.
	llvm::BasicBlock *ThenBB = llvm::BasicBlock::Create(*Context.getContext(), "then", TheFunction);
	llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(*Context.getContext(), "else");
	llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(*Context.getContext(), "ifcont");

	Builder->CreateCondBr(CondV, ThenBB, ElseBB);

	// Emit then value.
	Builder->SetInsertPoint(ThenBB);

	llvm::Value* ThenV = nullptr;
	for (auto& expr : Then)
		ThenV = expr->codegen();
	if (!ThenV)
		return nullptr;
	if (ThenEndKind == tok_return) {
		Builder->CreateRet(CheckTailCall(ThenV));
	} else {
		Builder->CreateBr(MergeBB);
	}
	if (is_void)
		ThenV = llvm::ConstantInt::getTrue(*Context.getContext());
	
	// Codegen of 'Then' can change the current block, update ThenBB for the PHI.
	ThenBB = Builder->GetInsertBlock();

	// Emit else block.
	TheFunction->getBasicBlockList().push_back(ElseBB);
	Builder->SetInsertPoint(ElseBB);

	llvm::Value* ElseV = nullptr;
	for (auto& expr : Else)
		ElseV = expr->codegen();
	if (!ElseV)
		return nullptr;
	if (ElseEndKind == tok_return) {
		Builder->CreateRet(CheckTailCall(ElseV));
	} else {
		Builder->CreateBr(MergeBB);
	}
	if (is_void)
		ElseV = llvm::ConstantInt::getFalse(*Context.getContext());

	// Codegen of 'Else' can change the current block, update ElseBB for the PHI.
	ElseBB = Builder->GetInsertBlock();
	// eprt("IfType: %s\n",
	//         type_table.get_name((llvm::Type*)((uintptr_t)type | (type_attr & A_signed))));
	// Emit merge block.
	TheFunction->getBasicBlockList().push_back(MergeBB);
	Builder->SetInsertPoint(MergeBB);
	llvm::PHINode *PN = Builder->CreatePHI(ft->type, 2, "iftmp");
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
	FullVar* OldValPtr = locals_table.back()[VarName.c_str()];
	llvm::Value *OldVal = OldValPtr ? OldValPtr->val : nullptr;
	if (OldVal) {
		OldVal = Alloca;
	} else {
		FullVar fv = {
			.val = Alloca,
			.ft = {
				.type = AllocaT,
				.type_attr = AllocaF
			},
		};
		locals_table.back().insert(VarName.c_str(), fv);
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
		locals_table.back().erase(VarName.c_str());
	// for expr always returns 0.0.
	return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*Context.getContext()));
}

llvm::Function *PrototypeAST::codegen() {
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
	auto CalleeF = getFunction(P.getName());
	llvm::Function* TheFunction = CalleeF.first;
	if (!TheFunction) {
		for (auto& expr : Body)
			llvm::Value *RetVal = expr->codegen();
		return nullptr;
	}
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
			CreateFunctionType(P.RetType, P.ArgTypes, Unit), ScopeLine,
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
		llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Arg.getName(), P.LLVMArgTypes[ArgIdx]);
		// get reference to argument in symbol table
		FullVar* mapitem = locals_table.back()[Arg.getName().str().c_str()];
		if (!mapitem) {
			eprt("internal compiler error: arg not found in table");
			exit(1);
		}
		llvm::Type* type = mapitem->ft.type;
		if (comp_mode == comp_dbg) {
			// Create a debug descriptor for the variable.
			llvm::DILocalVariable *D = DBuilder->createParameterVariable(
				SP, Arg.getName(), ++ArgIdx, Unit, LineNo, type_table.get_diType(mapitem->ft.type, mapitem->ft.type_attr & A_signed),
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
	if (!P.RetType->type->isVoidTy()) {
		Body.back()->desired_type = P.RetType->type;
		Body.back()->desired_type_attr = P.RetType->type_attr;
	}
	llvm::Value* RetVal;
	for (auto& Expr : Body) {
		if ((RetVal = Expr->codegen())) {
			if (comp_mode == comp_dbg) {
				KSDbgInfo.emitLocation(Expr.get());
			}
		} else {
			// Error reading body, remove function.
			TheFunction->eraseFromParent();
			
			if (comp_mode == comp_dbg) {
				// Pop off the lexical block for the function since we added it
				// unconditionally.
				KSDbgInfo.LexicalBlocks.pop_back();
			}
			return nullptr;
		}
	}
	// Finish off the function.
	if (P.RetType->type->isVoidTy()) {
		Builder->CreateRetVoid();
	} else {
		auto ret_type = RetVal->getType();
		//type = ret_type; // TODO: hande conversion if != proto->type;
		Builder->CreateRet(CheckTailCall(RetVal));
	}		
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
