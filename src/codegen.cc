/*
 * Copyright © Uwe Krüger 2021, 2022
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"
#include "../lib/str.h"

//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//

bool inside_function = false;
const char* last_shadow_saver = nullptr;
const char* last_shadow_restorer = nullptr;
const char* last_thread_constructor_caller = nullptr;
// list of boolean values that indicate that this loop branch is run for the first time
// this is used to avoid multiple  allocations of variables that are declared inside a then/while/repaet loop
VarTable* IfWhileVarTable = nullptr;
llvm::Value* ret_ptr = nullptr; // for sret
// both in loop bodies and in 'else' blocks array allocation should *not* be done in the entry block
// since the array size might be run time determined in one or the other block. To ensure this we track
// the nesting level of 'if/while/repeat/else' blocks - so we can use "if (condnesting) { ..."
unsigned condnesting = 0;

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

llvm::Value* LiteralExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	switch (ft->type->getTypeID()) {
	case llvm::Type::IntegerTyID:
	{
		unsigned bw = 0;
		if (desired_type) {
			if (auto it = llvm::dyn_cast<llvm::IntegerType>(desired_type))
				bw = it->getBitWidth();
			else if (desired_type->isDoubleTy())
				return handle(target, llvm::ConstantFP::get(Context, llvm::APFloat(ft->type_attr & A_signed ? (double)Val.Int : (double)Val.Uint)));
		}
		if (!bw)
			bw = ft->type->getIntegerBitWidth();
		if (bw == 1) // bool - treat anything != 0 as 'true'
			if (Val.Uint)
				return handle(target, Builder->getTrue());
			else
				return handle(target, Builder->getFalse());
		else
			return handle(target, llvm::ConstantInt::get(Context, llvm::APInt(bw, Val.Uint, ft->type_attr & A_signed)));
	}
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
		errs() << "Sorry, 16 bit floats are not supported, yet\n";
		return nullptr;
		// passthrough to 32 bit float for now - but expect problems...
	case llvm::Type::FloatTyID:
		return handle(target, llvm::ConstantFP::get(Context, llvm::APFloat((float)Val.Float)));
	case llvm::Type::DoubleTyID:
		return handle(target, llvm::ConstantFP::get(Context, llvm::APFloat(Val.Float)));
	case llvm::Type::PointerTyID:
		if (ft->type_attr & A_signed)
			return handle(target, Builder->CreateIntToPtr(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), Val.Uint, false), llvm::Type::getInt8PtrTy(Context)));
		else
			return handle(target, Builder->CreateGlobalStringPtr(Val.Str, "", 0, TheModule.get()));
	default:
		errs() << "internal compiler error: unhandled literal type " << *ft->type << "\n";
		return nullptr;
	}
}

llvm::Value* ListExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	if (Elements.size()) {
		std::vector<llvm::Type*> types;
		types.reserve(Elements.size());
		for (auto& elem: Elements)
			types.push_back(elem->ft->type);
		llvm::Type* list_type = llvm::StructType::get(Context, types);
		llvm::Value *V = llvm::UndefValue::get(list_type);
		for (unsigned i = 0; i < Elements.size(); ++i)
			V = Builder->CreateInsertValue(V, Elements[i]->codegen(), i, "listpush");
		return handle(target, V);
	} else {
		return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	}
}

llvm::Value* MapExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	const char* inserter;
	if (ft->elem_type[0].type == llvm::Type::getInt8PtrTy(Context)) // string key type
		inserter = "_ZN6volvox3map13string_insertEPPNS0_4NodeEPKcNS0_5ValueEiRS2_";
	else {
		errs() << Loc << ": maps with key type " << ft->elem_type[0] << " not supported\n";
		return nullptr;
	}
	PrototypeAST* inserter_proto = (*lex.findProtos(std::string(inserter)))[0].get();
	if (!inserter_proto) {
		errs() << Loc << ": prototype " << inserter << "() not found\n";
		return nullptr;
	}
	auto inserter_fn = getFunction(inserter_proto);
	llvm::Value* ptr = target;
	if (!ptr)
		ptr = CreateEntryBlockAlloca(llvm::Type::getInt8PtrTy(Context));
	Builder->CreateStore(llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context)), ptr);
	llvm::Value* do_replace = CreateEntryBlockAlloca(llvm::Type::getInt8PtrTy(Context));
	for (unsigned i=0; i<keys.size(); i++) {
		keys[i]->desired_type = ft->elem_type[0].type;
		llvm::Value* Key = keys[i]->codegen();
		values[i]->desired_type = ft->elem_type[1].type;
		llvm::Value* Value = values[i]->codegen();
		Value = Builder->CreateZExtOrBitCast(Value, llvm::Type::getInt64Ty(Context));
		Builder->CreateStore(llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context)), do_replace);
		Builder->CreateCall(inserter_proto->FT, inserter_fn, std::vector<llvm::Value*>{
				ptr, Key, Value, Builder->getInt32(0), do_replace });
	}
	if (target)
		return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	return ptr;
}

llvm::Value* StructExprAST::codegen_raw(llvm::Value* target) {
	if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
		llvm::Value* V = llvm::UndefValue::get(ft->type);
		unsigned num_fields = struct_type->getNumElements();
		std::vector<std::unique_ptr<ExprAST>> initializers(num_fields);
		for (auto& [fname, ini]: Fields) {
			MapValue* mv = map_string_get(ft->fields, fname.c_str());
			auto node = StructFieldType((MapNode*)((uintptr_t)mv - ((uintptr_t)&ft->fields->value - (uintptr_t)ft->fields)));
			unsigned index = node.getIndex();
			auto field_ft = node.getFt();
			ini->desired_type = field_ft->type;
			initializers[(ft->type_attr & A_union) ? 0 : index] = std::move(ini);
		}
		for (unsigned i=0; i<initializers.size(); i++) {
			if (initializers[i]) {
				llvm::Value* ini = initializers[i]->codegen();
				if (auto ini_array_type = llvm::dyn_cast<llvm::ArrayType>(ini->getType()))
					if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(initializers[i]->ft->type))
						ini = expandArrayInitializer(ini, ini_array_type, array_type);
				if (ft->type_attr & A_union) {
					size_t unionsize = TheModule->getDataLayout().getTypeAllocSize(ft->type);
					size_t valsize = TheModule->getDataLayout().getTypeAllocSize(ini->getType());
					if (valsize > unionsize)
						abort();
					unsigned szdiff = unionsize - valsize;
					if (szdiff) {
						// fill up remaining bytes with 0x00
						std::vector<llvm::Type*> types(1 + szdiff, llvm::Type::getInt8Ty(Context));
						types[0] = ini->getType();
						auto struct_type = llvm::StructType::get(Context, types);
						V = llvm::UndefValue::get(struct_type);
						V = Builder->CreateInsertValue(V, ini, 0, "unioninit");
						auto char0 = Builder->getInt8(0);
						for (unsigned i = szdiff; i; i--)
							V = Builder->CreateInsertValue(V, char0, i, "unionpadding");
					} else {
						V = ini;
					}
				} else {
					V = Builder->CreateInsertValue(V, ini, i, "structinit");
				}
			} else
				V = Builder->CreateInsertValue(V, llvm::Constant::getNullValue(struct_type->getElementType(i)), i , "structzeroinit");
		}
		return handle(target, V);
	} else
		abort();
}

llvm::Value* LvalueExprAST::codegen_raw(llvm::Value* target) {
	auto V = codegen_ref();
	if (V.first && V.second)
		// Load the value.
		return handle(target, Builder->CreateLoad(V.first, V.second, Name.c_str()));
	return nullptr;
}

std::pair<llvm::Type*,llvm::Value*> VariableExprAST::codegen_ref(bool silent_fail) {
	if (!full_var) {
		errs() << Loc << ": unknown variable name '" << Name << "'\n";
		return { nullptr, nullptr };
	}
	llvm::GlobalVariable* V;
	llvm::Type* storage_type;
	if (full_var->ft.type_attr & A_mainvar && ((comp_mode == comp_jit && !do_test) || (full_var->ft.type_attr & A_global))) { // global variable
		if (!full_var->mangled_name) {
			errs() << Loc << ": no mangled name for " << Name << '\n';
			return { nullptr, nullptr };
		}
		storage_type = full_var->storage_type;
		V = TheModule->getGlobalVariable(full_var->mangled_name, true);
		if (!V)
			V = new llvm::GlobalVariable(*TheModule, full_var->storage_type,
			                             false, llvm::GlobalValue::ExternalLinkage,
			                             nullptr, full_var->mangled_name, nullptr,
			                             (full_var->ft.type_attr & A_global) ?
			                             llvm::GlobalVariable::GeneralDynamicTLSModel :
			                             llvm::GlobalVariable::NotThreadLocal,
			                             0, true);
		V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(full_var->storage_type));
	} else {
		V = (llvm::GlobalVariable*)full_var->val;
		storage_type = ft->type; // full_var.first->val->getType() - deprecated;
		if (storage_type->isFunctionTy() || (ft->type_attr & A_ptrref))
			storage_type = storage_type->getPointerTo();
	}
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	if (full_var->ft.type_attr & A_ptrref) {
		auto the_ref = Builder->CreateLoad(storage_type, V);
		return { full_var->ft.type, the_ref };
	}
	return { storage_type, V };
}

llvm::MaybeAlign getAlignment(size_t elem_size) {
	uint64_t align = 1;
	// MaybeAlign constructor only accepts powers of 2, so create one from elem_size
	do {
		if (align >= elem_size)
			break;
		align <<= 1;
	} while (align < 8);
	return llvm::MaybeAlign(align);
}

llvm::MaybeAlign getAlignment(llvm::Value* size) {
	if (auto Align = llvm::dyn_cast<llvm::ConstantInt>(size)) {
		size_t elem_size = Align->getZExtValue();
		return getAlignment(elem_size);
	} else {
		errs() << "alignment requires const int size\n";
		return llvm::MaybeAlign();
	}
}

llvm::Value* StoreValue(llvm::Value* val, volvoxc::FullType* ft, llvm::Type* expected_type, const llvm::Twine &Name) {
	if (!expected_type)
		expected_type = ft->type;
	llvm::Type* expected_elem_type = expected_type;
	llvm::Type* elem_type = ft->type;
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)) {
		if (auto expected_array_type = llvm::dyn_cast<llvm::ArrayType>(expected_elem_type))
			return getInterfaceArrayOrStoreValue(val, array_type, expected_array_type, true, Name);
		else {
			errs() << CurLoc << ": mismatch in array structure\n";
			return nullptr;
		}
	} else {
		llvm::Value* Alloca;
		FullVar* var_in_if_branch;
		// Entry block allocations should be done only once for each variable. So in an 'else' branch
		// the allocation from the if/while branch should be reused if existing
		if (IfWhileVarTable && (var_in_if_branch = (*IfWhileVarTable)[Name.str().c_str()])) {
			if (var_in_if_branch->val->getType() != val->getType()->getPointerTo()) {
				errs() << "incompatible types for pointers to variable '" << Name << "' (" << *var_in_if_branch->val->getType()
				       << " in 'if'/'while' branch vs. " << *val->getType()->getPointerTo() << " in 'else' branch\n";
				return nullptr;
			}
			Alloca = var_in_if_branch->val;
		} else
			Alloca = CreateEntryBlockAlloca(val->getType(), Name);
		Builder->CreateStore(val, Alloca);
		return Alloca;
	}
}

std::pair<llvm::Type*,llvm::Value*> SelectExprAST::codegen_ref(bool silent_fail) {
	if (!ft || !ft->type)
		return { nullptr, nullptr }; // error message was already generated in AST
	if (auto LV = dynamic_cast<LvalueExprAST*>(Struct.get())) {
		auto struct_ref = LV->codegen_ref(silent_fail);
		if (struct_ref.second) {
			if (Struct->ft->type_attr & A_union)
				return { ft->type, Builder->CreatePointerCast(struct_ref.second, ft->type->getPointerTo()) };
			else
				return { ft->type, Builder->CreateStructGEP(struct_ref.first, struct_ref.second, FieldIndex) };
		}
	}
	if (!silent_fail)
		errs() << Struct->Loc << ": LHS of '.' expression must be an lvalue\n";
	return { ft->type, nullptr };
}

llvm::Value* SelectExprAST::codegen_raw(llvm::Value* target) {
	auto V = codegen_ref(true);
	if (auto val = ref2val(V))
		return handle(target, val);
	if (V.first) {
		llvm::Value* struct_val = Struct->codegen_raw(target);
		if (struct_val) {
			llvm::Value* val;
			if (Struct->ft->type_attr & A_union) {
				auto Store = CreateEntryBlockAlloca(ft->type);
				Builder->CreateStore(struct_val, Store);
				val = Builder->CreateLoad(ft->type, Store);
			} else
				val = Builder->CreateExtractValue(struct_val, FieldIndex);
			return handle(target, val);
		}
	}
	errs() << Loc << ": cannot generate code for select expression\n";
	return nullptr;
}

llvm::Value* FunctionExprAST::codegen_raw(llvm::Value* target) {
	if (auto F = TheModule->getFunction((*ft->Protos)[selected_proto]->Name)) {
		return handle(target, F);
	}
	return handle(target, (*ft->Protos)[selected_proto]->codegen());
}

llvm::Value* InterfaceExprAST::codegen_raw(llvm::Value* target) {
	llvm::Value* val;
	llvm::Type* type;
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ft->type)) {
		// pass by reference
		if (auto LV = dynamic_cast<LvalueExprAST*>(expr.get())) {
			auto V = LV->codegen_ref();
			type = V.first;
			val = V.second;
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(LV->ft->type))
				val = getInterfaceArrayValue(val, array_type);
		} else {
			llvm::Value* array = expr->codegen_raw(target);
			if (!array) {
				errs() << Loc << ": cannot generate code for expression\n";
				return nullptr;
			}
			if (array->getType()->isVoidTy())
				return handle(target, array);
			// if it's an rvalue we have to store it on stack to get a reference
			if (!target)
				val = StoreValue(array, expr->ft, MakeInterfaceArrayType(array_type));
		}
	} else if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
		if (auto LV = dynamic_cast<LvalueExprAST*>(expr.get())) {
			auto V = LV->codegen_ref();
			type = V.first;
			val = V.second;
		} else {
			llvm::Value* stuct_val = expr->codegen_raw(target);
			if (stuct_val->getType()->isVoidTy())
				return handle(target, stuct_val);
			if (!target)
				val = StoreValue(stuct_val, expr->ft);
		}
	} else {
		// pass by value
		val = expr->codegen();
	}
	if (!val)
		return nullptr;
	llvm::Constant* rttype_ptr = getRtType(expr->ft);
	llvm::Type* real_type = expr->ft->type;
	std::vector<llvm::Type*> types = { rttype_ptr->getType(), val->getType() };
	llvm::Type* struct_type = llvm::StructType::get(Context, types);
	llvm::Value* the_struct = llvm::UndefValue::get(struct_type);
	the_struct = Builder->CreateInsertValue(the_struct, rttype_ptr, 0);
	the_struct = Builder->CreateInsertValue(the_struct, val, 1);
	return handle(target, the_struct);
}

llvm::Value* PostfixExprAST::codegen_raw(llvm::Value* target) {
	auto OperandV = Operand->codegen_ref();
	if (!OperandV.second) {
		errs() << Operand->Loc << ": cannot generate code for postfix operand reference\n";
		return nullptr;
	}
	if (auto int_ty = llvm::dyn_cast<llvm::IntegerType>(OperandV.first)) {
		auto One = llvm::ConstantInt::get(int_ty, 1);
		llvm::Value* oldVal = Builder->CreateLoad(int_ty, OperandV.second);
		llvm::Value* newVal;
		if (Opcode[0] == '+')
			newVal = Builder->CreateAdd(oldVal, One);
		else
			newVal = Builder->CreateSub(oldVal, One);
		Builder->CreateStore(newVal, OperandV.second);
		return handle(target, oldVal);
	} else if (OperandV.first->isFloatingPointTy()) {
		auto One = Builder->CreateUIToFP(Builder->getInt32(1), OperandV.first);
		llvm::Value* oldVal = Builder->CreateLoad(OperandV.first, OperandV.second);
		llvm::Value* newVal;
		if (Opcode[0] == '+')
			newVal = Builder->CreateFAdd(oldVal, One);
		else
			newVal = Builder->CreateFSub(oldVal, One);
		Builder->CreateStore(newVal, OperandV.second);
		return handle(target, oldVal);
	}
	errs() << Operand->Loc << ": postfix operator not supported for type '" << *OperandV.first << "'\n";
	return nullptr;
}

llvm::Value* UnaryExprAST::codegen_raw(llvm::Value* target) {
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
			return handle(target, OperandV);
		case '-':
			return handle(target, Builder->CreateFNeg(OperandV, "negftmp"));
		// TODO: case '&'
		default:
			errs() << "unary operator '" << Opcode[0] << "' undefined for floats";
			return nullptr;
		}
	case llvm::Type::IntegerTyID:
		if (Opcode[0] != '!' && OperandV->getType()->getIntegerBitWidth() == 1) {
			errs() << "unary operator '" << Opcode[0] << "' undefined for bool";
			return nullptr;
		}
		switch (Opcode[0]) {
		case '+':
			return handle(target, OperandV);
		case '-':
			return handle(target, Builder->CreateNeg(OperandV, "negtmp"));
		case '!':
			return handle(target, Builder->CreateNot(OperandV, "nottmp"));
		default:
			errs() << "unary operator '" << Opcode[0] << "' undefined for integers";
			return nullptr;
		}
	default:
		// std::vector<volvoxc::FullType*> ArgTypes = { Operand->ft };
		// auto F = getFunction(std::string("unary") + Opcode, &ArgTypes);
		// if (!F.first) {
		//	errs() << "Unknown unary operator";
		//	return nullptr;
		// }
		// TODO: operand types
		// return Builder->CreateCall(F.first, OperandV, "unop");
		return nullptr;
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

llvm::Value* DefaultConstructorCall::codegen_raw(llvm::Value* target) {
	auto C = getConstructorOrDestructor(Var->ft);
	if (!C) {
		errs() << Var->Loc << ": no constructor for " << Var->Name << " type " << *Var->ft << " found\n";
		return nullptr;
	}
	auto GV = Var->codegen_ref();
	return Builder->CreateCall(C, { GV.second });
}

std::nullptr_t HandleGlobalVariable(BinaryExprAST* expr, unsigned sym_kind) {
	if (comp_mode == comp_jit && (!(sym_kind & A_global) || (expr->RHS->ft->type_attr & A_constructor)) && !do_test) {
		// This might be a non-const initialized main var that needs a temporary
		// 'setter' function. So finish the current module to be able to remove
		// the setter after usage
		finishFunctionOrModule();
	}
	VariableExprAST* LHSE = dynamic_cast<VariableExprAST*>(expr->LHS.get());
	ReferenceExprAST* LREF;
	if (LHSE)
		LREF = nullptr;
	else
		if ((LREF = dynamic_cast<ReferenceExprAST*>(expr->LHS.get())))
			LHSE = dynamic_cast<VariableExprAST*>(LREF->Operand.get());
	if (!LHSE) {
		errs() << LHSE->Loc << ": LHS of declaration must be a variable name\n";
		return nullptr;
	}
	const std::string& unmangled_name = LHSE->getName();
	std::string varname;
	if (lex.module->import_path.empty()) {
		varname = unmangled_name;
	} else {
		llvm::SmallString<128> buf = llvm::StringRef("_Z");
		varname = std::string(MangleBase(buf, lex.module->import_path, unmangled_name));
	}
	// We do not know in advance if the RHS of the 'main' var  initialization is a compile
	// time const. In order to be able to run 'RHS->codegen()' in any case, a function
	// context is needed. If the initializer turns out to be a compile time const this
	// function is not needed and can be 'erased'
	auto setter_name = "__global_" + varname + "_setter";
	llvm::FunctionType* ptr_fn_t = llvm::FunctionType::get(llvm::Type::getInt8PtrTy(Context),
	                                                       { llvm::Type::getInt64Ty(Context)->getPointerTo() }, false);
	llvm::Function* tmpf = llvm::Function::Create(ptr_fn_t, llvm::Function::ExternalLinkage, setter_name, TheModule.get());
	auto BB = llvm::BasicBlock::Create(Context, "entry", tmpf);
	Builder->SetInsertPoint(BB);
	llvm::Value* Arg = tmpf->getArg(0);
	if (last_shadow_restorer && comp_mode == comp_jit && !do_test) {
		auto last_restorer_proto = (*lex.findProtos(last_shadow_restorer))[0].get();
		auto last_restorer = getFunction(last_restorer_proto);
		Builder->CreateCall(last_restorer_proto->FT, last_restorer, std::vector<llvm::Value*>(), "callrestorer");
	}
	llvm::Value* Val;
	llvm::Type* val_type;
	llvm::Type* type;
	llvm::Value* convertedVal;
	FullVar* is_referencing = nullptr;
	std::string* rname;
	std::function<llvm::Value*(llvm::Value*)> conversion;
	bool is_signed = false;
	unsigned is_union = expr->RHS->ft->type_attr & A_union;
	bool is_constructor_call = false;
	bool use_target = false;
	llvm::Value* target = nullptr;
	size_t allocsz = expr->RHS->ft->type->isSized() ? TheModule->getDataLayout().getTypeAllocSize(expr->RHS->ft->type) : 0;
	if (LREF) {
		if (auto refexpr = dynamic_cast<LvalueExprAST*>(expr->RHS.get())) {
			auto BaseVar = refexpr->getBase();
			if (BaseVar->ft->type_attr & A_global) {
				errs() << BaseVar->Loc << ": cannot create reference to global variable\n";
				tmpf->eraseFromParent();
				lex.module->globals_table.erase(unmangled_name.c_str());
				return nullptr;
			}
			auto t_v = refexpr->codegen_ref();
			val_type = type = t_v.first;
			convertedVal = Val = t_v.second;
			if (Val) {
				is_referencing = BaseVar->full_var;
				rname = &BaseVar->Name;
			}
			is_signed = expr->RHS->ft->type_attr & A_signed;
		} else {
			Val = nullptr;
		}
	} else {
		if (llvm::isa<llvm::StructType>(expr->RHS->ft->type)) {
			if (auto callexpr = dynamic_cast<CallExprAST*>(expr->RHS.get())) {
				if (auto type_expr = dynamic_cast<TypeExprAST*>(callexpr->Callee.get()))
					is_constructor_call = true;
			}
			if (is_constructor_call || allocsz > 16 && !(sym_kind & A_global))
				use_target = true;
		}
		if (!use_target)
			Val = expr->RHS->codegen();
	}
	if (!use_target && !Val) {
		errs() << expr->RHS->Loc << ": could not generate code for variable initialization\n";
		tmpf->eraseFromParent();
		lex.module->globals_table.erase(unmangled_name.c_str());
		return nullptr;
	}
	if (!LREF && !use_target) {
		val_type = Val->getType();
		auto type_descr = MakeType(expr->RHS->ft->type, expr->RHS->ft->type_attr & A_signed, expr->RHS->is_unknown_type);
		type = std::get<0>(type_descr);
		conversion = std::get<1>(type_descr);
		is_signed = std::get<2>(type_descr);
		convertedVal = conversion(Val);
	}
	llvm::GlobalValue::LinkageTypes link_type = ((sym_kind & A_pub) || comp_mode == comp_jit) ?
		llvm::GlobalValue::ExternalLinkage :
		llvm::GlobalValue::InternalLinkage;
	llvm::Constant* initializer = use_target ? nullptr : llvm::dyn_cast<llvm::Constant>(convertedVal);
	bool needs_store;
	bool needs_constructor = !is_constructor_call && (expr->RHS->ft->type_attr & A_constructor);
	if (initializer) {
		needs_store = false;
		if (!(needs_constructor && (comp_mode == comp_jit) && !do_test))
			tmpf->eraseFromParent();
	} else {
		if (needs_constructor) {
			errs() << expr->RHS->Loc << ": internal error - unsized type but constructor required\n";
			abort();
		}
		needs_store = true;
		if (allocsz > 0) {
			initializer = llvm::Constant::getNullValue(expr->RHS->ft->type);
		}
	}
	bool needs_call = (needs_store || needs_constructor) && (comp_mode == comp_jit) && !do_test;
	if (needs_store && (sym_kind & A_global)) {
		if (LREF)
			errs() << expr->LHS->Loc << ": references are not allowed to be global\n";
		else
			errs() << expr->RHS->Loc << ": initializer for global variable must be a compile time const\n";
		tmpf->eraseFromParent();
		// the parser should have added this global variable to lex.module - revert this
		lex.module->globals_table.erase(unmangled_name.c_str());
		return nullptr;
	} else {
		llvm::GlobalVariable* GV;
		if (initializer && !LREF)
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(expr->RHS->ft->type))
				if (auto ini_array_type = llvm::dyn_cast<llvm::ArrayType>(initializer->getType()))
					if (auto const_initializer = llvm::dyn_cast<llvm::Constant>(expandArrayInitializer(initializer, ini_array_type, array_type)))
						initializer = const_initializer;
		if (comp_mode == comp_dbg) {
			// Create a debug descriptor for the variable.
			DBuilder->createGlobalVariableExpression(
				SP, varname, varname, Unit, expr->Loc.Line, lex.get_diType(type, is_signed), false);
		}
		if (initializer) { // i.e. constant size type
			// If 'needs_store' this here is part of a module which is going to be
			// removed later. So in this case it's only a declaration and the 'real'
			// variable is defined below in a separate module that will stay.
			GV = new llvm::GlobalVariable(*TheModule, initializer->getType(),
			                              false, link_type,
			                              needs_call ? nullptr : initializer, varname, nullptr,
			                              (sym_kind & A_global) ?
			                              llvm::GlobalVariable::GeneralDynamicTLSModel :
			                              llvm::GlobalVariable::NotThreadLocal, 0, needs_call);
			GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(initializer->getType()));
		}
		FullVar* fv = lex.module->globals_table[unmangled_name.c_str()];
		if (!fv) {
			errs() << expr->RHS->Loc << ": internal error - variable '" << unmangled_name << "' not found in database\n";
			return nullptr;
		}
		fv->storage_type = initializer ? initializer->getType() : nullptr;
		fv->mangled_name = strdup(varname.c_str());
		fv->ft = *expr->RHS->ft;
		fv->ft.type = use_target ? expr->RHS->ft->type : type;
		fv->ft.type_attr = sym_kind | (is_signed ? A_signed : 0U) | is_union | (LREF ? A_ptrref : 0U) | A_mainvar;
		if (is_referencing)
			fv->mark_as_referencing(is_referencing);
		if (!needs_call) {
			if (needs_constructor) { // we are not in interactive JIT mode -> call constructor by main()
				auto varExpr = std::make_unique<VariableExprAST>(expr->LHS->Loc, unmangled_name, fv);
				auto constructor_call = std::make_unique<DefaultConstructorCall>(expr->Loc, std::move(varExpr));
				GlobalExprList.push_back(std::move(constructor_call));
			}
		} else {
			llvm::Type* array_ptr_ty = nullptr;
			llvm::Value* ptrRet = nullptr;
			unsigned ndim = 0;
			if (needs_store) { // no global
				if (comp_mode != comp_jit) {
					errs() << expr->Loc <<"internal error: non-global main variable '" << varname
					       << "' handled by HandleGlobalVariable() in non-JIT mode\n";
					abort();
				}
				if (initializer) { // constant size initializer
					if (use_target)
						expr->RHS->codegen_raw(GV);
					else
						Builder->CreateStore(convertedVal, GV);
				} else { // variable size array - no global
					auto retVal = StoreValue(convertedVal, expr->RHS->ft, nullptr, varname);
					if (auto struct_type = llvm::dyn_cast<llvm::StructType>(retVal->getType())) {
						ndim = struct_type->getNumElements() - 1;
						for (unsigned dim = 0; ; ) {
							Builder->CreateStore(Builder->CreateExtractValue(retVal, dim), Arg);
							if (++dim >= ndim)
								break;
							Arg = Builder->CreateIntToPtr(Builder->CreateAdd(Builder->CreatePtrToInt(Arg, llvm::Type::getInt64Ty(Context)),
							                                                 Builder->getInt64(sizeof(size_t))), Arg->getType());
						}
						ptrRet = Builder->CreateExtractValue(retVal, ndim);
						array_ptr_ty = ptrRet->getType();
					} else {
						errs() << expr->Loc << ": internal error; stuct expected\n";
						abort();
					}
				}
			}
			if (needs_constructor) { // no array - but maybe global
				auto C = getConstructorOrDestructor(&fv->ft);
				if (!C) {
					errs() << expr->LHS->Loc << ": no constructor for " << fv->ft << " found\n";
					return nullptr;
				}
				// interactive JIT mode - immediately call constructor from setter function
				Builder->CreateCall(C, { GV });
				if (comp_mode == comp_jit && (sym_kind & A_global) && !do_test) {
					std::string shadow_var_name = std::string("__") + varname + "_shadow_";
					auto V = new llvm::GlobalVariable(*TheModule, initializer->getType(),
					                                  false, link_type,
					                                  nullptr, shadow_var_name, nullptr,
					                                  llvm::GlobalVariable::NotThreadLocal, 0, true);
					V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(initializer->getType()));
					auto Vval = Builder->CreateLoad(initializer->getType(), GV);
					Builder->CreateStore(Vval, V);
				}
			}
			if (last_shadow_saver && comp_mode == comp_jit && !do_test) {
				auto last_saver_proto = (*lex.findProtos(last_shadow_saver))[0].get();
				auto last_saver = getFunction(last_saver_proto);
				Builder->CreateCall(last_saver_proto->FT, last_saver, std::vector<llvm::Value*>(), "callsaver");
			}
			if (initializer || !needs_store)
				Builder->CreateRet(llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context)));
			else
				Builder->CreateRet(Builder->CreateBitCast(ptrRet, llvm::Type::getInt8PtrTy(Context)));
			finishFunctionOrModule(tmpf, 3, true, false);
			auto RT = TheJIT->getMainJITDylib().createResourceTracker();
			auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), *TS_Context.get());
			ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
			InitializeModuleAndPassManager();
			// We want to remove the 'setter' module below but the global variable
			// must stay, so put the latter in a new module that is not freed
			// by the resource tracker
			if (initializer && needs_call) {
				GV = new llvm::GlobalVariable(*TheModule, initializer->getType(),
				                              false, link_type,
				                              initializer, varname, nullptr,
				                              (sym_kind & A_global) ?
				                              llvm::GlobalVariable::GeneralDynamicTLSModel :
				                              llvm::GlobalVariable::NotThreadLocal, 0, false);
				GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(initializer->getType()));
			}
			if (comp_mode == comp_jit && (sym_kind & A_global) && needs_constructor && !do_test) {
				std::string shadow_var_name = std::string("__") + varname + "_shadow_";
				auto V = new llvm::GlobalVariable(*TheModule, initializer->getType(),
				                                  false, link_type,
				                                  llvm::Constant::getNullValue(initializer->getType()), shadow_var_name, nullptr,
				                                  llvm::GlobalVariable::NotThreadLocal, 0, false);
				V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(initializer->getType()));
			}
			finishFunctionOrModule();
			// Search the JIT for the <setter_name> symbol.
			auto ExprSymbol = ExitOnErr(TheJIT->lookup(setter_name));
			// C syntax at its best...
			char* (*PTR)(size_t*) = (char* (*)(size_t*))(intptr_t)ExprSymbol.getAddress();
			size_t* Dims = ndim ? (size_t*)alloca(ndim * sizeof(size_t)) : nullptr;
			char* varptr = PTR(Dims);
			if (varptr) {
				jit_main_variables.emplace_back(varptr);
				std::vector<llvm::Type*> struct_type_el(ndim + 1, llvm::Type::getInt64Ty(Context));
				struct_type_el[ndim] = array_ptr_ty;
				llvm::Type* struct_type = llvm::StructType::get(Context, struct_type_el);
				llvm::Value* the_struct = llvm::UndefValue::get(struct_type);
				for (unsigned u = 0; u<ndim; u++)
					the_struct = Builder->CreateInsertValue(the_struct, Builder->getInt64(Dims[u]), u);
				the_struct = Builder->CreateInsertValue(the_struct, Builder->CreateBitCast(Builder->getInt64((uintptr_t)varptr), array_ptr_ty), ndim);
				fv->val = the_struct;
				fv->ft.type_attr &= ~A_mainvar;
			}
			ExitOnErr(RT->remove());
		}
		if (needs_constructor && (sym_kind & A_global)) {
			// Since global variables are TLS the constructor has to be called for each newly
			// started thread. For that we create a function that first calls 'last_thread_constructor_caller'
			// and then the constructor of this GV. 'last_thread_constructor_caller' is then replaced by
			// our new function
			auto void_fn_t = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {}, false);
			auto constructor_caller = std::string("__") + varname + "_constructor_caller";
			auto newConstructorCaller = llvm::Function::Create(void_fn_t, llvm::Function::ExternalLinkage,
			                                                   constructor_caller, TheModule.get());
			newConstructorCaller->addFnAttr(llvm::Attribute::AlwaysInline);
			auto BB = llvm::BasicBlock::Create(Context, "entry", newConstructorCaller);
			Builder->SetInsertPoint(BB);
			if (last_thread_constructor_caller) {
				auto last_thrconstr_proto = (*lex.findProtos(last_thread_constructor_caller))[0].get();
				auto last_caller = getFunction(last_thrconstr_proto);
				Builder->CreateCall(last_thrconstr_proto->FT, last_caller,
				                    std::vector<llvm::Value*>(), "callold");
			}
			auto C = getConstructorOrDestructor(&fv->ft);
			GV = TheModule->getGlobalVariable(varname, true);
			if (!GV) {
				GV = new llvm::GlobalVariable(*TheModule, fv->ft.type,
				                              false, link_type,
				                              nullptr, varname, nullptr,
				                              llvm::GlobalVariable::GeneralDynamicTLSModel,
				                              0, true);
				GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(fv->ft.type));
			}
			Builder->CreateCall(C, { GV });
			Builder->CreateRetVoid();
			finishFunctionOrModule(newConstructorCaller, 1, jit_repl);
			auto constructor_caller_Proto = std::make_unique<PrototypeAST>(CurLoc, constructor_caller, std::vector<std::string>());
			last_thread_constructor_caller = constructor_caller_Proto->Name.c_str();
			// constructor callers must be always accessible so force them into builtin namespace
			Module* module = (lex.source_stack.size()) ? lex.source_stack.front().module : lex.module;
			module->FunctionProtos[constructor_caller].push_back(std::move(constructor_caller_Proto));
		}
		if (comp_mode == comp_jit && (sym_kind & A_global) && !do_test) {
			llvm::Type* V_type = initializer->getType();
			size_t storage_sz = TheJIT->getDataLayout().getTypeStoreSize(V_type);
			std::string shadow_var_name = std::string("__") + varname + "_shadow_";
			llvm::GlobalVariable* V;
			if (!needs_constructor) {
				auto V = new llvm::GlobalVariable(*TheModule, V_type,
				                                  false, link_type,
				                                  initializer, shadow_var_name, nullptr,
				                                  llvm::GlobalVariable::NotThreadLocal);
				V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(V_type));
				finishFunctionOrModule();
			}
			GV = TheModule->getGlobalVariable(varname, true);
			if (!GV) {
				GV = new llvm::GlobalVariable(*TheModule, V_type,
				                              false, link_type,
				                              nullptr, varname, nullptr,
				                              llvm::GlobalVariable::GeneralDynamicTLSModel,
				                              0, true);
				GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(V_type));
			}
			V = TheModule->getGlobalVariable(shadow_var_name, true);
			if (!V) {
				V = new llvm::GlobalVariable(*TheModule, V_type,
				                             false, link_type,
				                             nullptr, shadow_var_name, nullptr,
				                             llvm::GlobalVariable::NotThreadLocal,
				                             0, true);
				V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(V_type));
			}
			auto sz_const = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), storage_sz);
			auto align = TheModule->getDataLayout().getPrefTypeAlign(V_type);
			// auto align = getAlignment(sz_const);
			auto saver = std::string("__") + varname + "_saver";
			auto restorer = std::string("__") + varname + "_restorer";
			llvm::FunctionType* void_fn_t = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {}, false);
			llvm::Function* Fsaver = llvm::Function::Create(void_fn_t, llvm::Function::ExternalLinkage, saver, TheModule.get());
			auto BB = llvm::BasicBlock::Create(Context, "entry", Fsaver);
			Builder->SetInsertPoint(BB);
			Builder->CreateMemCpy(V, align, GV, align, storage_sz);
			if (last_shadow_saver) {
				auto last_saver_proto = (*lex.findProtos(last_shadow_saver))[0].get();
				auto last_saver = getFunction(last_saver_proto);
				Builder->CreateRet(CheckTailCall(Builder->CreateCall(last_saver_proto->FT, last_saver, std::vector<llvm::Value*>(), "callold")));
			} else {
				Builder->CreateRetVoid();
			}
			finishFunctionOrModule(Fsaver, 3, false);
			auto saverProto = std::make_unique<PrototypeAST>(CurLoc, saver, std::vector<std::string>());
			last_shadow_saver = saverProto->Name.c_str();
			// savers/restorers must be always accessible so force them into builtin namespace
			Module* module = (lex.source_stack.size()) ? lex.source_stack.front().module : lex.module;
			module->FunctionProtos[saver].push_back(std::move(saverProto));
			llvm::Function* Frestorer = llvm::Function::Create(void_fn_t, llvm::Function::ExternalLinkage, restorer, TheModule.get());
			BB = llvm::BasicBlock::Create(Context, "entry", Frestorer);
			Builder->SetInsertPoint(BB);
			Builder->CreateMemCpy(GV, align, V, align, storage_sz);
			if (last_shadow_restorer) {
				auto last_restorer_proto = (*lex.findProtos(last_shadow_restorer))[0].get();
				auto last_restorer = getFunction(last_restorer_proto);
				Builder->CreateRet(CheckTailCall(Builder->CreateCall(last_restorer_proto->FT, last_restorer, std::vector<llvm::Value*>(), "callold")));
			} else {
				Builder->CreateRetVoid();
			}
			finishFunctionOrModule(Frestorer, 3, false);
			auto restorerProto = std::make_unique<PrototypeAST>(CurLoc, restorer, std::vector<std::string>());
			last_shadow_restorer = restorerProto->Name.c_str();
			module->FunctionProtos[restorer].push_back(std::move(restorerProto));
		}
	}
	return nullptr;
}

// helper function to find out if an expression is fractional
inline bool is_fractional(ExprAST* expr) {
	if (auto binexpr = dynamic_cast<BinaryExprAST*>(expr)) {
		if (!binexpr->Op[1]) {
			if (binexpr->Op[0] == '/')
				return true;
			if (binexpr->Op[0] == '+' || binexpr->Op[0] == '-' || binexpr->Op[0] == '*')
				return is_fractional(binexpr->LHS.get()) || is_fractional(binexpr->RHS.get());
		}
	}
	return false;
}

llvm::Value *BinaryExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Special assign-like ops because we don't want to emit the LHS as an expression.
	// assign op '=' is a comparison (not an assignment) when a boolean result is expected
	if (opclass == OpDeclAssign || opclass == OpAssign || opclass == OpModAssign) {
		std::pair<llvm::Type*,llvm::Value*> Variable = { nullptr, nullptr };
		const char* varname = nullptr;
		// Assignment requires the LHS to be an identifier.
		// This assume we're building without RTTI because LLVM builds that way by
		// default.  If you build LLVM with RTTI this can be changed to a
		// dynamic_cast for automatic error checking.
		LvalueExprAST *LHSE = dynamic_cast<LvalueExprAST*>(LHS.get());
		if (!LHSE) {
			errs() << "left side of '" << Op << "' must be an lvalue\n";
			return nullptr;
		}
		ReferenceExprAST* LREF = dynamic_cast<ReferenceExprAST*>(LHS.get());
		if (opclass != OpDeclAssign)
			Variable = LHSE->codegen_ref();
		if (opclass == OpModAssign) { // +=, <<=, ...
			auto new_LHS = std::make_unique<RefExprAST>(LHS->Loc, LHS->ft, LHSE->getBase(), Variable.second, LHSE->Name);
			char newOp[4];
			int m=0;
			for ( ; Op[m] != '='; m++)
				newOp[m] = Op[m];
			newOp[m] = '\0';
			RHS = std::make_unique<BinaryExprAST>(Loc, newOp, std::move(new_LHS), std::move(RHS),
			                                      std::tuple<llvm::Type*, bool, bool, OpClass, const char*>{
				                                      ft->type, ft->type_attr & A_signed, is_unknown_type, getOpClass(newOp), err_msg });
		}
		RHS->desired_type = LHSE->ft->type;
		// Codegen the RHS.
		uint64_t allocsz = LREF ?
			sizeof(void*) :
			(RHS->desired_type && RHS->desired_type->isSized()) ?
			TheModule->getDataLayout().getTypeAllocSize(RHS->desired_type) : 0; // if size is compile time const
		llvm::Value* Val = nullptr;
		llvm::Value* ValPtr = nullptr;
		llvm::Value* AllocSize = nullptr;
		llvm::Type* elem_type = nullptr;
		llvm::StructType* struct_type = nullptr;
		uint64_t el_allocsz = 0;
		llvm::Value* Struct = nullptr;
		FullVar* is_referencing = nullptr;
		std::string* rname = nullptr;
		bool is_constructor_call = false;
		if (auto callexpr = dynamic_cast<CallExprAST*>(RHS.get())) {
			if (auto type_expr = dynamic_cast<TypeExprAST*>(callexpr->Callee.get()))
				// check that this is not just an explicis basic type conversion like 'f64(i)'
				if (llvm::isa<llvm::StructType>(type_expr->ft->type))
					is_constructor_call = true;
		} else if (auto RHS_Lval = dynamic_cast<LvalueExprAST*>(RHS.get())) {
			auto ValR = RHS_Lval->codegen_ref(true);
			if (!ValR.second) {
				if (LREF) {
					errs() << RHS->Loc << ": reference requires lvalue for initialization\n";
				}
				if (ValR.first)
					goto use_val;
				errs() << RHS->Loc << ": unable to generate code for RHS of assignment\n";
				return nullptr;
			}
			// update allocsz in case codegen_ref() has revealed a fixed compile time size
			if (LREF) {
				is_referencing = RHS_Lval->getBase()->full_var;
				rname = &RHS_Lval->getBase()->Name;
			}
			else
				allocsz = RHS_Lval->ft->type->isSized() ? TheModule->getDataLayout().getTypeAllocSize(RHS_Lval->ft->type) : 0;
			if (!allocsz) {
				if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(RHS_Lval->ft->type)) {
					std::vector<llvm::Value*> Dims;
					std::vector<llvm::Value*> returnDims;
					Struct = ValR.second;
					elem_type = getArrayDims(ValR.second, array_type, Dims, returnDims);
					el_allocsz = elem_type->isSized() ? TheModule->getDataLayout().getTypeAllocSize(elem_type) : 0;
					if (!el_allocsz) {
						errs() << "array element type must be sized\n";
						return nullptr;
					}
					AllocSize = Builder->getInt64(1);
					for (auto dim: Dims)
						AllocSize = Builder->CreateMul(AllocSize, dim);
					if ((struct_type = llvm::dyn_cast<llvm::StructType>(ValR.second->getType())))
						ValPtr = Builder->CreateExtractValue(ValR.second, struct_type->getNumElements() - 1);
					else
						ValPtr = ValR.second;
				} else {
					errs() << "variable sized objects of type " << *RHS_Lval->ft->type << " not implemented\n";
					return nullptr;
				}
			} else {
				if (!LREF && allocsz <= 16 && !is_constructor_call) {
					Val = RHS_Lval->ref2val(ValR);
					if (!Val)
						goto use_val;;
				}
				else
					ValPtr = ValR.second;
			}
			goto have_val_or_valptr;
		} else {
			if (LREF) {
				errs() << RHS->Loc << ": reference requires lvalue for initialization\n";
				return nullptr;
			}
		}
	use_val:
		if (allocsz <= 16 && !is_constructor_call) {
			Val = RHS->codegen();
			if (!Val)
				return nullptr;
		}
	have_val_or_valptr:
		// Look up the name.
		ExprAST* MaybeVar = LREF ? LREF->Operand.get() : LHS.get();
		if (auto RegularVar = dynamic_cast<VariableExprAST*>(MaybeVar)) {
			varname = RegularVar->getName().c_str();
			FullVar* full_var = RegularVar->full_var;
			if (!full_var)
				goto not_found;
		}
		if (opclass == OpDeclAssign) {
			errs() << LHS->Loc << ": cannot initialize existing variable\n";
			return nullptr;
		} else {
			if (allocsz > 16 || is_constructor_call) {
				auto align = getAlignment(allocsz);
				if (target)
					Builder->CreateMemCpy(target, align, Variable.second, align, allocsz);
				else if (ValPtr)
					Builder->CreateMemCpy(Variable.second, align, ValPtr, align, allocsz);
				else {
					auto voidval = RHS->codegen_raw(Variable.second);
					if (!voidval || !voidval->getType()->isVoidTy()) {
						errs() << Loc << ": internal error: sret ++call does not return void\n";
						return nullptr;
					}
				}
				return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
			} else {
				auto OldVal = Builder->CreateLoad(Variable.first, Variable.second);
				Builder->CreateStore(Val, Variable.second);
				return handle(target, OldVal);
			}
		}
	not_found:
		if (opclass != OpDeclAssign) {
			errs() << LHS->Loc << ": unknown variable name '" << varname << "'\n";
			return nullptr;
		}
		// variable declaration - we know it's no global variable since this has already been handled
		// in parser.cc
		llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
		auto type_descr = MakeType(RHS->ft->type, RHS->ft->type_attr & A_signed, RHS->is_unknown_type);
		llvm::Type* type = std::get<0>(type_descr);
		auto conversion = std::get<1>(type_descr);
		bool is_signed = std::get<2>(type_descr);
		FullVar* entry;
		if (locals_table.empty()) {
			entry = lex.module->globals_table[varname];
			if (!entry || (entry->ft.type_attr & A_global)) {
				errs() << LHS->Loc << ": internal error - '" << varname << "' has an inconsistent state\n";
				return nullptr;
			}
		} else {
			entry = locals_table.back()[varname];
		}
		// Entry has already been created by parser but we might have to adjust the type of the new
		// variable after RHS->codegen() has been run (e.g. array dimensions might only be known by now)
		entry->ft.type = type;
		if (is_signed)
			entry->ft.type_attr |= A_signed;
		else
			entry->ft.type_attr &= ~A_signed;
		if (Val) {
			auto convertedVal = conversion(Val);
			auto Alloca = StoreValue(convertedVal, &entry->ft, nullptr, varname);
			entry->val = Alloca;
			if (comp_mode == comp_dbg) {
				// Create a debug descriptor for the variable.
				llvm::DILocalVariable *D = DBuilder->createAutoVariable(
					SP, varname, Unit, LHS->Loc.Line, lex.get_diType(type, is_signed),
					true);
				
				DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
				                        llvm::DILocation::get(SP->getContext(), LHS->Loc.Line, 0, SP),
				                        Builder->GetInsertBlock());
			}
		} else if (ValPtr) {
			if (allocsz) {
				llvm::AllocaInst* Alloca;
				auto align = getAlignment(allocsz);
				if (LREF) {
					entry->ft.type_attr |= A_ptrref;
					Alloca = Builder->CreateAlloca(ValPtr->getType(), nullptr, varname);
					Builder->CreateAlignedStore(ValPtr, Alloca, align);
					entry->mark_as_referencing(is_referencing);
				} else {
					Alloca = Builder->CreateAlloca(RHS->ft->type, nullptr, varname);
					Builder->CreateMemCpy(Alloca, align, ValPtr, align, allocsz);
				}
				entry->val = Alloca;
			} else {
				auto Alloca = Builder->CreateAlloca(elem_type, AllocSize, varname);
				auto align = TheModule->getDataLayout().getPrefTypeAlign(elem_type);
				llvm::Value* cp_size = Builder->CreateMul(Builder->getInt64(el_allocsz), AllocSize);
				Builder->CreateMemCpy(Alloca, align, ValPtr, align, cp_size);
				if (Struct) {
					auto strt = llvm::cast<llvm::StructType>(Struct->getType());
					llvm::Value* Entry = llvm::UndefValue::get(strt);
					unsigned ndim = strt->getNumElements() - 1;
					for (unsigned i = 0; i < ndim; i++)
						Entry = Builder->CreateInsertValue(Entry, Builder->CreateExtractValue(Struct, i), i);
					Entry = Builder->CreateInsertValue(Entry, Alloca, ndim);
					entry->val = Entry;
				} else {
					entry->val = Alloca;
				}
			}
		} else if (allocsz > 16 || is_constructor_call) {
			auto align = getAlignment(allocsz);
			auto Alloca = Builder->CreateAlloca(RHS->ft->type, nullptr, varname);
			auto voidval = RHS->codegen_raw(Alloca);
			if (!voidval)
				return nullptr;
			if (!voidval->getType()->isVoidTy()) {
				errs() << Loc << ": internal error: sret call-- does not return void\n";
				return nullptr;
			}
			entry->val = Alloca;
		} else {
			errs() << "unhandled case\n";
			return nullptr;
		}
		if ((entry->ft.type_attr & A_constructor) && !is_constructor_call) {
			// no explicit constructor call but there is a default constructor
			auto F = getConstructorOrDestructor(&entry->ft);
			if (!F) {
				errs() << ": internal error - default constructor not found for " << *entry->ft.type << "\n";
				return nullptr;
			} else
				Builder->CreateCall(F, { entry->val });
		}
		ft->type = llvm::Type::getVoidTy(Context);
		return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	}
	llvm::Value* result;
	bool ResSigned = ft->type_attr & A_signed;
	bool OperandSigned = LHS->ft->type_attr & A_signed || RHS->ft->type_attr & A_signed;
	const char* new_err_msg;
	std::tie(LHS->desired_type, RHS->desired_type, new_err_msg) = getDesiredTypes(
		ft->type, desired_type, LHS->ft->type, RHS->ft->type, opclass, ft->type_attr & A_signed,
		LHS->ft->type_attr & A_signed, RHS->ft->type_attr & A_signed, LHS->is_unknown_type, RHS->is_unknown_type);
	if (false) {
		if (LHS->desired_type) errs() << "LHS desired_type: " << *LHS->desired_type << ' ';
		if (RHS->desired_type) errs() << "RHS desired_type: " << *RHS->desired_type << ' ';
		errs() << "expr: ";
		if (desired_type)
			errs() << *desired_type << '\n';
		else
			errs() << "none\n";
	}
	llvm::Value *L, *R;
	L = LHS->codegen();
	if (!L)
		return nullptr;
	if (opclass == OpLogical) { // &&, ||
		R = nullptr;
		// codegen is postponed - we do lazy evaluation
	} else {
		if (opclass == OpExponentiation && !RHS->desired_type && is_fractional(RHS.get())) {
			if (desired_type)
				RHS->desired_type = desired_type;
			else
				RHS->desired_type = LHS->ft->type;
		}
		R = RHS->codegen();
		if (!R)
			return nullptr;
	}
	// for comparisons ExprAST.type is bool, but we have to look at the operands that are in desired
	TypeClass typeclass = is_unknown;
	switch(L->getType()->getTypeID()) {
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
		}
		break;
	case '/':
		switch(typeclass) {
		case is_int:
			if (ResSigned)
				result = Builder->CreateSDiv(L, R, "divtmp");
			else
				result = Builder->CreateUDiv(L, R, "divtmp");
			break;
		case is_float:
			result = Builder->CreateFDiv(L, R, "divtmp");
			break;
		default:
			errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
		}
		break;
	case '%':
		switch(typeclass) {
		case is_int:
			if (ResSigned)
				result = Builder->CreateSRem(L, R, "remtmp");
			else
				result = Builder->CreateURem(L, R, "remtmp");
			break;
		case is_float:
			result = Builder->CreateFRem(L, R, "remtmp");
			break;
		default:
			errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
		}
		break;
	case '&':
	case '|':
		switch(typeclass) {
		case is_int:
			if (!Op[1])
				// bitwise &, |
				if (Op[0] == '&')
					result = Builder->CreateAnd(L, R, "andtmp");
				else
					result = Builder->CreateOr(L, R, "ortmp");
			else {
				// lazy logical &&, ||
				auto enterBB = Builder->GetInsertBlock();
				auto TheFunction = enterBB->getParent();
				auto RHSBB = llvm::BasicBlock::Create(Context, "lazy_rhs");
				auto ContBB = llvm::BasicBlock::Create(Context, "logic_op");
				if (Op[0] == '&')
					Builder->CreateCondBr(L, RHSBB, ContBB);
				else
					Builder->CreateCondBr(L, ContBB, RHSBB);
				TheFunction->getBasicBlockList().push_back(RHSBB);
				Builder->SetInsertPoint(RHSBB);
				R = RHS->codegen();
				// if (R && convRHS)
				//	R = convRHS(R);
				if (!R)
					return nullptr;
				Builder->CreateBr(ContBB);
				TheFunction->getBasicBlockList().push_back(ContBB);
				Builder->SetInsertPoint(ContBB);
				auto PN = Builder->CreatePHI(llvm::Type::getInt1Ty(Context), 2, "merged_lazy");
				PN->addIncoming(Op[0] == '&' ? Builder->getFalse() : Builder->getTrue(), enterBB);
				PN->addIncoming(R, RHSBB);
				result = PN;
			}
			break;
		default:
			errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			return nullptr;
		}
		break;
	case '^':
		if (auto int_exp_type = llvm::dyn_cast<llvm::IntegerType>(R->getType())) {
			if (auto int_base_type = llvm::dyn_cast<llvm::IntegerType>(L->getType())) {
				if (int_base_type->getBitWidth() <= 32) {
					if (int_base_type->getBitWidth() < 32)
						L = Builder->CreateIntCast(L, llvm::Type::getInt32Ty(Context), LHS->ft->type_attr & A_signed);
					if (int_exp_type->getBitWidth() != 32)
						R = Builder->CreateIntCast(R, llvm::Type::getInt32Ty(Context), RHS->ft->type_attr & A_signed);
					auto powfn_proto = llvm::isa<llvm::Constant>(R) ? (*lex.findProtos("__i32_pow_constexp"))[0].get()
						: (*lex.findProtos("__i32_pow"))[0].get();
					auto powfn = getFunction(powfn_proto);
					result = Builder->CreateCall(powfn_proto->FT, powfn, std::vector<llvm::Value*>{ L, R });
				} else {
					if (int_base_type->getBitWidth() != 64)
						L = Builder->CreateIntCast(L, llvm::Type::getInt64Ty(Context), LHS->ft->type_attr & A_signed);
					if (int_exp_type->getBitWidth() != 32)
						R = Builder->CreateIntCast(R, llvm::Type::getInt32Ty(Context), RHS->ft->type_attr & A_signed);
					auto powfn_proto = llvm::isa<llvm::Constant>(R) ? (*lex.findProtos("__i64_pow_constexp"))[0].get()
						: (*lex.findProtos("__i64_pow"))[0].get();
					auto powfn = getFunction(powfn_proto);
					result = Builder->CreateCall(powfn_proto->FT, powfn, std::vector<llvm::Value*>{ L, R });
				}
			} else {
				result = Builder->CreateIntrinsic(llvm::Intrinsic::powi, { L->getType(), R->getType() }, { L, R });
			}
		} else {
			result = Builder->CreateIntrinsic(llvm::Intrinsic::pow, { L->getType(), R->getType() }, { L, R });
			needs_libm = true;
		}
		break;
	case '!':
		if (Op[1] == '=') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateICmpNE(L, R, "neitmp");
				break;
			case is_float:
				result = Builder->CreateFCmpONE(L, R, "neftmp");
				break;
			default:
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			}
		} else {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateNot(Builder->CreateXor(L, R, "xortmp"), "nxortmp");
				break;
			default:
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			}
		}
		break;
	case '=':
		switch(typeclass) {
		case is_int:
			result = Builder->CreateICmpEQ(L, R, "eqitmp");
			break;
		case is_float:
			result = Builder->CreateFCmpOEQ(L, R, "eqftmp");
			break;
		default:
			errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			}
		} else if (Op[1] == '<') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateShl(L, R, "remtmp");
				break;
			default:
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			}
			break;
		} else if (Op[1] == '<') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateXor(L, R, "remtmp");
				break;
			default:
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			}
		}
		break;
	}
	return handle(target, result);
}

std::pair<llvm::Value*, llvm::Instruction*> IfExprAST::createCondBranch(llvm::BasicBlock* MergeBB, bool isElse) {
	int EndKind = isElse ? ElseEndKind : ThenEndKind;
	std::vector<std::unique_ptr<ExprAST>>& Branch = isElse ? Else : Then;
	llvm::Value* BranchV = nullptr;
	llvm::Instruction* firstBreak = nullptr; // needed as insertion point to prepare merged vars
	if (EndKind == tok_return)
		Branch.back()->desired_type = theFunction_ret_ft->type;
	for (auto& expr : Branch)
		BranchV = expr->codegen();
	if (EndKind != tok_return && !Branch.empty() && Branch.back()->desired_type)
		Branch.back()->ft->type = Branch.back()->desired_type;
	if (!BranchV && !isElse)
		return { nullptr, nullptr };
	if (EndKind == tok_return) {
		if (theFunction_ret_ft->type->isVoidTy()) {
			InsertDestructors(nullptr);
			Builder->CreateRetVoid();
		} else {
			if (ret_ptr) {
				Builder->CreateStore(BranchV, ret_ptr);
				InsertDestructors(ret_ptr);
				Builder->CreateRetVoid();
			} else {
				InsertDestructors(nullptr);
				Builder->CreateRet(CheckTailCall(BranchV));
			}
		}
	} else {
		if (ft->type->isVoidTy() && (!BranchV || !BranchV->getType()->isVoidTy()))
			BranchV = llvm::UndefValue::get(ft->type);
		firstBreak = Builder->CreateBr(MergeBB);
	}
	return { BranchV, firstBreak };
}

std::pair<llvm::Type*, llvm::Value*> merge_values(
	llvm::Type* typA, llvm::Value* valA, llvm::BasicBlock* caseA, llvm::Instruction* lastA, 
	llvm::Type* typB, llvm::Value* valB, llvm::BasicBlock* caseB, llvm::Instruction* lastB) {
	auto MergeBB = Builder->GetInsertBlock();
	if (typA == typB) {
		if (valA->getType() != valB->getType()) {
			errs() << "internal error: types of if-branches do not match\n";
			return { nullptr, nullptr };
		}
		llvm::PHINode* PN = Builder->CreatePHI(valA->getType(), 2, "iftmp");
		PN->addIncoming(valA, caseA);
		PN->addIncoming(valB, caseB);
		return { typA, PN };
	} else if (auto array_tA = llvm::dyn_cast<llvm::ArrayType>(typA)) {
		std::vector<uint64_t> fixedDims;
		std::vector<llvm::Value*> varDimsA;
		std::vector<llvm::Value*> varDimsB;
		llvm::Value* Aptr;
		unsigned n_vardims_A;
		Builder->SetInsertPoint(lastA);
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(valA->getType())) {
			n_vardims_A = struct_type->getNumElements() - 1;
			Aptr = Builder->CreateExtractValue(valA, n_vardims_A);
		} else {
			n_vardims_A = 0;
			Aptr = valA;
		}
		if (!Aptr->getType()->isPointerTy()) {
			Aptr = getInterfaceArrayOrStoreValue(valA, array_tA, array_tA, true);
			if (auto struct_type = llvm::dyn_cast<llvm::StructType>(Aptr->getType()))
				Aptr = Builder->CreateExtractValue(Aptr, struct_type->getNumElements() - 1);
		}
		llvm::Value* Bptr;
		unsigned n_vardims_B;
		auto array_tB = llvm::dyn_cast<llvm::ArrayType>(typB);
		if (!array_tB) {
			errs() << "error: array / scalar mismatch\n";
			return { nullptr, nullptr };
		}
		Builder->SetInsertPoint(lastB);
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(valB->getType())) {
			n_vardims_B = struct_type->getNumElements() - 1;
			Bptr = Builder->CreateExtractValue(valB, n_vardims_B);
		} else {
			n_vardims_B = 0;
			Bptr = valB;
		}
		if (!Bptr->getType()->isPointerTy()) {
			Bptr = getInterfaceArrayOrStoreValue(valB, array_tB, array_tB, true);
			if (auto struct_type = llvm::dyn_cast<llvm::StructType>(Bptr->getType()))
				Bptr = Builder->CreateExtractValue(Bptr, struct_type->getNumElements() - 1);
		}
		unsigned iA = 0;
		unsigned iB = 0;
		do {
			if (array_tB) {
				llvm::Type* elem_tA = array_tA->getElementType();
				uint64_t nA = array_tA->getNumElements();
				llvm::Type* elem_tB = array_tB->getElementType();
				uint64_t nB = array_tB->getNumElements();
				if (nA && nA == nB) {
					fixedDims.push_back(nA);
				} else {
					Builder->SetInsertPoint(lastA);
					fixedDims.push_back(0);
					llvm::Value* dimA;
					if (nA)
						dimA = Builder->getInt64(nA);
					else
						if (iA < n_vardims_A) {
							dimA = Builder->CreateExtractValue(valA, iA++);
						} else {
							errs() << "error: mismatch in array value structure\n";
							return { nullptr, nullptr };
						}
					varDimsA.push_back(dimA);
					llvm::Value* dimB;
					Builder->SetInsertPoint(lastB);
					if (nB)
						dimB = Builder->getInt64(nB);
					else
						if (iB < n_vardims_B) {
							dimB = Builder->CreateExtractValue(valB, iB++);
						} else {
							errs() << "error: mismatch in array value structure\n";
							return { nullptr, nullptr };
						}
					varDimsB.push_back(dimB);
				}
				typA = elem_tA;
				typB = elem_tB;
			} else {
				errs() << "incompatible types\n";
				return { nullptr, nullptr };
			}
			array_tB = llvm::dyn_cast<llvm::ArrayType>(typB);
		} while ((array_tA = llvm::dyn_cast<llvm::ArrayType>(typA)));
		if (typA != typB) {
			errs() << "mismatch in array element types " << *typA << " vs. " << *typB << '\n';
			return { nullptr, nullptr };
		}
		llvm::Type* ptr_t = varDimsA.size() ? Aptr->getType() : typA->getPointerTo();
		Builder->SetInsertPoint(lastA);
		Aptr = Builder->CreateBitCast(Aptr, ptr_t);
		Builder->SetInsertPoint(lastB);
		Bptr = Builder->CreateBitCast(Bptr, ptr_t);
		llvm::Type* resultT = typA;
		std::vector<llvm::Type*> struct_type_el(varDimsA.size() + 1, llvm::Type::getInt64Ty(Context));
		struct_type_el[varDimsA.size()] = ptr_t;
		llvm::Type* struct_type = llvm::StructType::get(Context, struct_type_el);
		Builder->SetInsertPoint(lastA);
		llvm::Value* the_structA = llvm::UndefValue::get(struct_type);
		the_structA = Builder->CreateInsertValue(the_structA, Aptr, varDimsA.size());
		unsigned structIdx = 0;
		for (int d = fixedDims.size() - 1; d >=0; d--) {
			uint64_t dim = fixedDims[d];
			resultT = llvm::ArrayType::get(resultT, dim);
			if (!dim) {
				the_structA = Builder->CreateInsertValue(the_structA, varDimsA[structIdx], structIdx);
				structIdx++;
			}
		}
		if (structIdx != varDimsA.size() || structIdx != varDimsB.size()) {
			errs() << "internal error: could not create merge value\n";
			return { nullptr, nullptr };
		}
		Builder->SetInsertPoint(lastB);
		llvm::Value* the_structB = llvm::UndefValue::get(struct_type);
		the_structB = Builder->CreateInsertValue(the_structB, Bptr, varDimsB.size());
		for (structIdx = 0; structIdx < varDimsB.size(); structIdx++) {
			the_structB = Builder->CreateInsertValue(the_structB, varDimsB[structIdx], structIdx);
		}
		Builder->SetInsertPoint(MergeBB);
		llvm::PHINode* PN = Builder->CreatePHI(struct_type, 2, "ifdimtmp");
		PN->addIncoming(the_structA, caseA);
		PN->addIncoming(the_structB, caseB);
		return { resultT, PN };
	} else {
		errs() << "merge not possible " << *typA << " # " << *typB << '\n';
		return { nullptr, nullptr };
	}
}

llvm::Value* IfExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
	llvm::PHINode* condPN;
	llvm::BasicBlock* CondBB;
	// find a suitable name for the loop block
	const char* loopBBName;
	const char* contName;
	// find good label names to make blocks recognizable in IR
	switch (if_kind) {
	case tok_if:
		loopBBName = "then";
		contName = "ifcont";
		break;
	case tok_repeat:
		loopBBName = "repeat";
		contName = "repeatcont";
		break;
	default:
		loopBBName = "loop";
		contName = "whilecont";
	}
	llvm::BasicBlock* ThenBB = llvm::BasicBlock::Create(Context, loopBBName);
	if (if_kind == tok_while) {
		llvm::BasicBlock* enterBB = Builder->GetInsertBlock();
		CondBB = llvm::BasicBlock::Create(Context, "while");
		Builder->CreateBr(CondBB);
		TheFunction->getBasicBlockList().push_back(CondBB);
		Builder->SetInsertPoint(CondBB);
		condPN = Builder->CreatePHI(llvm::Type::getInt8Ty(Context), 2, "mustsavestack");
		condPN->addIncoming(Builder->getInt8(2), enterBB);
	} else if (if_kind == tok_repeat) {
		CondBB = llvm::BasicBlock::Create(Context, "until"); // will be filled at end
		condPN = nullptr;
	} else {
		CondBB = nullptr;
		condPN = nullptr;
	}
	Cond->desired_type = llvm::Type::getInt1Ty(Context);
	// Create blocks for the then and else cases.  Insert the 'then' block at the
	// end of the function.
	llvm::BasicBlock* ElseBB = (if_kind == tok_repeat) ? nullptr : llvm::BasicBlock::Create(Context, "else");
	llvm::BasicBlock* MergeBB = llvm::BasicBlock::Create(Context, contName);
	llvm::BasicBlock* StackSaveBB = (if_kind == tok_if) ? nullptr : llvm::BasicBlock::Create(Context, "stacksave");
	llvm::BasicBlock* StackRestoreBB = (if_kind == tok_if) ? nullptr : llvm::BasicBlock::Create(Context, "stackrestore");
	if (if_kind == tok_repeat) {
		// 1st iteration: save stack
		Builder->CreateBr(StackSaveBB);
	} else {
		llvm::Value *CondV = Cond->codegen();
		if (!CondV)
			return nullptr;
		if (if_kind == tok_while)
			CondBB = Builder->GetInsertBlock();
		const char* new_err_msg;
		if (!Else.empty() && !Then.empty())
			std::tie(Then.back()->desired_type, Else.back()->desired_type, new_err_msg) = getDesiredTypes(
				ft->type, desired_type, Then.back()->ft->type, Else.back()->ft->type, OpNormal, ft->type_attr & A_signed,
				Then.back()->ft->type_attr & A_signed, Else.back()->ft->type_attr & A_signed,
				Then.back()->is_unknown_type, Else.back()->is_unknown_type);
		if (CondV->getType() != llvm::Type::getInt1Ty(Context)) {
			errs() << Cond->Loc << ": bool type expected as 'if'/'while' condition\n";
			return nullptr;
		}
		if (if_kind == tok_if) {
			Builder->CreateCondBr(CondV, ThenBB, ElseBB);
		} else {
			// save stack at 1st run and restore at followind runs
			llvm::Value* CondVV = Builder->CreateIntCast(CondV, llvm::Type::getInt8Ty(Context), false, "expandcond");
			llvm::Value* switchVal = Builder->CreateOr(condPN, CondVV, "switchval");
			llvm::MDNode* branch_weights = MDBuilder->createBranchWeights({10,75,5,10});
			auto switchInst = Builder->CreateSwitch(switchVal, MergeBB, 4, branch_weights);
			switchInst->addCase(Builder->getInt8(1), StackRestoreBB);
			switchInst->addCase(Builder->getInt8(2), ElseBB);
			switchInst->addCase(Builder->getInt8(3), StackSaveBB);
		}
	}
	llvm::Instruction* StackRestoreInst = nullptr; // to insert destructors before that
	if (if_kind != tok_if) {
		TheFunction->getBasicBlockList().push_back(StackSaveBB);
		Builder->SetInsertPoint(StackSaveBB);
		llvm::Value* savedStack = Builder->CreateIntrinsic(llvm::Intrinsic::stacksave, {}, {}, nullptr, "savedstack");
		Builder->CreateBr(ThenBB);
		TheFunction->getBasicBlockList().push_back(StackRestoreBB);
		Builder->SetInsertPoint(StackRestoreBB);
		StackRestoreInst = Builder->CreateIntrinsic(llvm::Intrinsic::stackrestore, {}, savedStack);
		Builder->CreateBr(ThenBB);
	}
	TheFunction->getBasicBlockList().push_back(ThenBB);
	Builder->SetInsertPoint(ThenBB);
	// Emit then value.
	locals_table.push_back(std::move(then_locals_table));
	condnesting++;
	auto ThenVL = createCondBranch(CondBB ? CondBB : MergeBB, false);
	llvm::Value* ThenV = ThenVL.first;
	auto thenLast = ThenVL.second;
	condnesting--;
	then_locals_table = std::move(locals_table.back());
	locals_table.pop_back();
	if (!ThenV)
		return nullptr;
	// Codegen of 'Then' can change the current block, update ThenBB for the PHI.
	ThenBB = Builder->GetInsertBlock();
	if (if_kind == tok_while)
		condPN->addIncoming(Builder->getInt8(0), ThenBB);
	llvm::Value* ElseV = nullptr;
	llvm::Instruction* elseLast;
	if (if_kind == tok_repeat) {
		TheFunction->getBasicBlockList().push_back(CondBB);
		Builder->SetInsertPoint(CondBB);
		llvm::Value *CondV = Cond->codegen();
		if (!CondV)
			return nullptr;
		CondBB = Builder->GetInsertBlock();
		if (CondV->getType() != llvm::Type::getInt1Ty(Context)) {
			errs() << Cond->Loc << ": bool type expected as 'until' condition\n";
			return nullptr;
		}
		Builder->CreateCondBr(CondV, MergeBB, StackRestoreBB);
	} else {		
		// Emit else block.
		TheFunction->getBasicBlockList().push_back(ElseBB);
		Builder->SetInsertPoint(ElseBB);
		locals_table.push_back(std::move(else_locals_table));
		condnesting++;
		VarTable* old_IfWhileVarTable = IfWhileVarTable;
		IfWhileVarTable = &then_locals_table;
		auto ElseVL = createCondBranch(MergeBB, true);
		ElseV = ElseVL.first;
		elseLast = ElseVL.second;
		IfWhileVarTable = old_IfWhileVarTable;
		condnesting--;
		else_locals_table = std::move(locals_table.back());
		locals_table.pop_back();
		if (!ElseV)
			return nullptr;
		// Codegen of 'Else' can change the current block, update ElseBB for the PHI.
		ElseBB = Builder->GetInsertBlock();
	}
	// Emit merge block.
	TheFunction->getBasicBlockList().push_back(MergeBB);
	Builder->SetInsertPoint(MergeBB);
	if (if_kind == tok_repeat && then_locals_table.table) {
		for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
			MapValue* node = then_node.getValue();
			auto then_var = (FullVar*)((char*)node + node->offset);
			FullVar* entry = locals_table.back()[then_node.getKey()];
			if (!entry) {
				errs() << "internal error, could not find merge variable '" << then_node.getKey() << "' in outer scope\n";
				abort();
			}
			entry->ft.type = then_var->ft.type;
			entry->ft.type_attr = then_var->ft.type_attr;
			entry->val = then_var->val;
		}
	} else if (then_locals_table.table && else_locals_table.table && thenLast && elseLast) {
		for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
			FullVar* else_var = else_locals_table[then_node.getKey()];
			MapValue* node = then_node.getValue();
			auto then_var = (FullVar*)((char*)node + node->offset);
			if (else_var) {
				auto merge = merge_values(then_var->ft.type, then_var->val, (if_kind == tok_while) ? CondBB : ThenBB, thenLast,
				                          else_var->ft.type, else_var->val, ElseBB, elseLast);
				if (!merge.second)
					return nullptr;
				auto mergeVal = merge.second;
				FullVar* entry = locals_table.back()[then_node.getKey()];
				if (!entry) {
					errs() << "internal error, could not find merge variable '" << then_node.getKey() << "' in outer scope\n";
					abort();
				}
				entry->ft.type = merge.first;
				entry->ft.type_attr = then_var->ft.type_attr | else_var->ft.type_attr;
				entry->val = mergeVal;
				else_var->ft.type_attr |= A_merged; // mark as merged to avoid destructor call below
				then_var->ft.type_attr |= A_merged;
			}
		}
	}
	MergeBB = Builder->GetInsertBlock();
	// iterate over then/else-declared objects and insert destructors for those that are not merged
	if (if_kind != tok_repeat && then_locals_table.table && thenLast) {
		Builder->SetInsertPoint(thenLast);
		for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
			MapValue* node = then_node.getValue();
			auto then_var = (FullVar*)((char*)node + node->offset);
			if ((then_var->ft.type_attr & A_destructor) && !(then_var->ft.type_attr & A_merged))
				InsertDestructor(then_var, thenLast);
		}
	}
	// objects declared in while/repat branches have to be always destructed when the stack is restored
	if (if_kind != tok_if && then_locals_table.table && StackRestoreInst) {
		Builder->SetInsertPoint(StackRestoreInst);
		for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
			MapValue* node = then_node.getValue();
			auto then_var = (FullVar*)((char*)node + node->offset);
			if (then_var->ft.type_attr & A_destructor)
				InsertDestructor(then_var, StackRestoreInst);
		}
	}
	if (if_kind != tok_repeat && else_locals_table.table && elseLast) {
		Builder->SetInsertPoint(elseLast);
		for (auto else_node = else_locals_table.first(); else_node; ++else_node) {
			MapValue* node = else_node.getValue();
			auto else_var = (FullVar*)((char*)node + node->offset);
			if ((else_var->ft.type_attr & A_destructor) && !(else_var->ft.type_attr & A_merged))
				InsertDestructor(else_var, elseLast);
		}
	}
	Builder->SetInsertPoint(MergeBB);
	if (ft->type->isVoidTy())
		return llvm::UndefValue::get(ft->type);
	else {
		auto merge = merge_values(Then.back()->ft->type, ThenV, (if_kind == tok_while) ? CondBB : ThenBB, thenLast,
		                          Else.back()->ft->type, ElseV, ElseBB, elseLast);
		if (ft->type != merge.first) {
			ft = new_FullType(*ft);
			ft-> type = merge.first;
		}
		return handle(target, merge.second);
	}
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
llvm::Value *ForExprAST::codegen_raw(llvm::Value* target) {
	llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Create an alloca for the variable in the entry block.
	llvm::Type* AllocaT = llvm::Type::getInt32Ty(Context);
	unsigned AllocaF = A_signed;
	llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(AllocaT, VarName, TheFunction);

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
	llvm::BasicBlock *LoopBB = llvm::BasicBlock::Create(Context, "loop", TheFunction);

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
		StepVal = llvm::ConstantFP::get(Context, llvm::APFloat(1.0));
	}

	// Compute the end condition.
	llvm::Value *EndCond = End->codegen();
	if (!EndCond)
		return nullptr;

	// Reload, increment, and restore the alloca.  This handles the case where
	// the body of the loop mutates the variable.
	llvm::Value *CurVar = Builder->CreateLoad(llvm::Type::getDoubleTy(Context), Alloca,
	                                          VarName.c_str());
	llvm::Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
	Builder->CreateStore(NextVar, Alloca);

	// Convert condition to a bool by comparing non-equal to 0.0.
	EndCond = Builder->CreateFCmpONE(
		EndCond, llvm::ConstantFP::get(Context, llvm::APFloat(0.0)), "loopcond");

	// Create the "after loop" block and insert it.
	llvm::BasicBlock *AfterBB =
		llvm::BasicBlock::Create(Context, "afterloop", TheFunction);

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
	return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(Context));
}
