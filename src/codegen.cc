/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//

bool inside_function = false;
const char* last_shadow_saver = nullptr;
const char* last_shadow_restorer = nullptr;
const char* last_thread_constructor_caller = nullptr;
const char* last_thread_destructor_caller = nullptr;
// list of boolean values that indicate that this loop branch is run for the first time
// this is used to avoid multiple  allocations of variables that are declared inside a then/while/repeat loop
VarTable* IfWhileVarTable = nullptr;
llvm::Value* ret_ptr = nullptr; // for sret
// both in loop bodies and in 'else' blocks array allocation should *not* be done in the entry block
// since the array size might be run time determined in one or the other block. To ensure this we track
// the nesting level of 'if/while/repeat/else' blocks - so we can use "if (condnesting) { ..."
unsigned condnesting = 0;
idiv_modes idiv_mode = idiv_mode_undef;
std::vector<std::tuple<llvm::Constant*,std::string,unsigned>> pending_globals;
std::vector<std::tuple<void*,llvm::Value**,llvm::Type*>> pending_arrays;

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

inline llvm::AllocaInst* CreateAlloca(llvm::Value* AllocSize, llvm::Align align,
                                      const llvm::Twine &Name = "") {
   unsigned AddrSpace = TheModule->getDataLayout().getAllocaAddrSpace();
   return Builder->Insert(new llvm::AllocaInst(llvm::Type::getInt8Ty(Context),
                                               AddrSpace, AllocSize, align), Name);
}

static bool ConstexprIntOverflow(SourceLocation& Loc, llvm::ConstantInt* Vconst, uint64_t rawVal, unsigned attr, llvm::ConstantInt* rawConst = nullptr) {
	if (attr & A_signed) {
		int64_t VInt = Vconst->getSExtValue();
		int64_t rawInt = rawConst ? rawConst->getSExtValue() : (int64_t)rawVal;
		if (VInt != rawInt) {
			errs() << Loc << ": untyped constexpr value (" << rawInt
			       << ") cannot be represented in the supposed type "
			       << *Vconst->getType() << "\n";
			return true;
		}
	} else {
		uint64_t VUint = Vconst->getZExtValue();
		uint64_t rawUint = rawConst ? rawConst->getZExtValue() : rawVal;
		if (VUint != rawUint) {
			errs() << Loc << ": untyped constexpr value (" << rawUint
			       << ") cannot be represented in the supposed type "
			       << "unsigned " << *Vconst->getType() << "\n";
			return true;
		}
	}
	return false;
}

static bool ConstexprFPOverflow(SourceLocation& Loc, llvm::ConstantFP* Vconst, double rawDouble, llvm::ConstantFP* rawConst = nullptr) {
	double rawVal = rawConst ? rawConst->getValue().convertToDouble() : rawDouble;
	if (Vconst->isInfinity() && !isinf(rawVal) ||
	    Vconst->isZero() && !(rawVal == 0.0) ||
	    Vconst->isNaN() && !isnan(rawVal)) {
		errs() << Loc << ": untyped constexpr value (" << rawVal
		       << ") cannot be represented as a" << " " << *Vconst->getType() << "\n";
		return true;
	}
	return false;
}

llvm::Value* LiteralExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	auto type_id = ft->type->getTypeID();
	switch (type_id) {
	case llvm::Type::IntegerTyID:
	{
		unsigned bw = 0;
		if (desired_type) {
			if (auto it = llvm::dyn_cast<llvm::IntegerType>(desired_type))
				bw = it->getBitWidth();
			else if (desired_type->isDoubleTy() || desired_type->isFloatTy())
				return handle(target, llvm::ConstantFP::get(desired_type, ft->type_attr & A_signed ? (double)Val.Int : (double)Val.Uint));
		}
		if (!bw)
			bw = ft->type->getIntegerBitWidth();
		if (bw == 1) // bool - treat anything != 0 as 'true'
			if (Val.Uint)
				return handle(target, Builder->getTrue());
			else
				return handle(target, Builder->getFalse());
		else {
			auto the_val = llvm::ConstantInt::get(Context, llvm::APInt(bw, Val.Uint, ft->type_attr & A_signed));
			if (ConstexprIntOverflow(Loc, the_val, Val.Uint, ft->type_attr))
				return nullptr;
			return handle(target, the_val);
		}
	}
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
		errs() << "Sorry, 16 bit floats are not supported, yet\n";
		return nullptr;
	case llvm::Type::FloatTyID:
	case llvm::Type::DoubleTyID: {
		auto the_val = llvm::ConstantFP::get(ft->type, Val.Float);
		if (type_id != llvm::Type::DoubleTyID && ConstexprFPOverflow(Loc, llvm::cast<llvm::ConstantFP>(the_val), Val.Float))
			return nullptr;
		return handle(target, the_val);
	}
	case llvm::Type::PointerTyID:
		if (ft->type_attr & A_signed)
			return handle(target, Builder->CreateIntToPtr(llvm::ConstantInt::get(llvm_size_type, Val.Uint, false), llvm_ptr_type));
		else if (ft->type_attr & A_string)
		{
			llvm::Value* theString = createStringConst(Val.CStr, Val.Len);
			return handle(target, theString);
		}
		// else fallthrough
	default:
		errs() << "internal compiler error: unhandled literal type " << *ft->type << "\n";
		return nullptr;
	}
}

llvm::Value* ListExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	if (desired_type) {
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(desired_type)) {
			// cannot move "this" so create a new unique expr...
			auto str_ft = struct_mangled_ft[std::string(struct_type->getName())];
			if (str_ft) {
				auto list = std::make_unique<ListExprAST>(Loc, std::move(Elements), str_ft);
				auto struct_expr = std::make_unique<StructExprAST>(Loc, str_ft, std::move(list));
				return struct_expr->codegen_raw(target);
			}
		}
		if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(desired_type)) {
			std::vector<std::unique_ptr<ExprAST>> Dims;
			std::vector<SourceLocation> LenLocs;
			llvm::Type* elem_type;
			do {
				uint64_t dim = array_type->getNumElements();
				if (!dim) {
					errs() << Loc << ": { ... } short syntax initialization only supported for const size arrays\n";
					return nullptr;
				}
				Dims.push_back(std::move(std::make_unique<ConstExprAST>(getSize(dim))));
				LenLocs.push_back(SourceLocation{});
				elem_type = array_type->getElementType();
			} while ((array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)));
			auto iter = ExprListIterator(std::move(Dims));
			std::vector<std::unique_ptr<ExprAST>> Elems = iter.prepare_list(std::move(Elements), 0);
			if (iter.struct_error())
				return nullptr;
			auto ft = new_FullType(desired_type, 0, nullptr, new_FullType(elem_type, 0));
			auto array_expr = std::make_unique<FixedArrayExprAST>(Loc, ft, std::move(Elems), std::move(iter.valid_exprs), std::move(iter.LitDims), std::move(iter.Dims), LenLocs);
			return array_expr->codegen_raw(target);
		}
	}
	if (Elements.size()) {
		std::vector<llvm::Type*> types;
		types.reserve(Elements.size());
		for (auto& elem: Elements)
			types.push_back(elem->ft->type);
		llvm::Type* list_type = llvm::StructType::get(Context, types);
		llvm::Value *V = llvm::UndefValue::get(list_type);
		for (unsigned i = 0; i < Elements.size(); ++i) {
			llvm::Value* vv = Elements[i]->codegen();
			V = Builder->CreateInsertValue(V, vv, i, "listpush");
		}
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
	if (ft->elem_type[0].type == llvm_ptr_type) // string key type
		inserter = "_ZN6volvox3map19volvoxstring_insertEPPNS0_4NodeEPKcNS0_5ValueEiS3_";
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
	llvm::Value* ptr = ((intptr_t)target == -1) ? nullptr : target;
	if (!ptr) {
		ptr = CreateEntryBlockAlloca(llvm_ptr_type);
		FullVar tmp = {
			.val = ptr,
			.ft = {
				.type = llvm_ptr_type,
				.type_attr = A_map
			}
		};
		expr_temps.push_back(tmp);
	}
	Builder->CreateStore(llvm::ConstantPointerNull::get(llvm_ptr_type), ptr);
	llvm::Value* do_replace = CreateEntryBlockAlloca(llvm_ptr_type);
	for (unsigned i=0; i<keys.size(); i++) {
		keys[i]->desired_type = ft->elem_type[0].type;
		llvm::Value* Key = keys[i]->codegen();
		values[i]->desired_type = ft->elem_type[1].type;
		llvm::Value* Value = values[i]->codegen();
		Value = Builder->CreateZExtOrBitCast(Value, llvm::Type::getInt64Ty(Context));
		Builder->CreateStore(llvm::ConstantPointerNull::get(llvm_ptr_type), do_replace);
		Builder->CreateCall(inserter_proto->FT, inserter_fn, std::vector<llvm::Value*>{
				ptr, Key, Value, Builder->getInt32(0), do_replace });
	}
	if (target && (intptr_t)target != -1)
		return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	return Builder->CreateLoad(llvm_ptr_type, ptr);
}

llvm::Value* StructExprAST::codegen_raw(llvm::Value* target) {
	if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
		llvm::Value* V = llvm::UndefValue::get(ft->type);
		unsigned num_fields = struct_type->getNumElements();
		std::vector<std::unique_ptr<ExprAST>> initializers(num_fields);
		if ((ft->type_attr & A_union) && Fields.size() > 1) {
			errs() << Loc << ": union literals can have at most one element\n";
			return nullptr;
		}
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
				if (!ini)
					return nullptr;
				if (auto ini_array_type = llvm::dyn_cast<llvm::ArrayType>(ini->getType())) {
					llvm::ArrayType* array_type = nullptr;
					if (initializers[i]->ft->type)
						array_type = llvm::dyn_cast<llvm::ArrayType>(initializers[i]->ft->type);
					else if (initializers[i]->desired_type)
						array_type = llvm::dyn_cast<llvm::ArrayType>(initializers[i]->desired_type);
					if (array_type)
						ini = expandArrayInitializer(ini, ini_array_type, array_type);
				}
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
						std::vector<llvm::Type*> types{ ini->getType() };
						auto struct_type = llvm::StructType::get(Context, types);
						V = llvm::UndefValue::get(struct_type);
						V = Builder->CreateInsertValue(V, ini, 0, "unioninit");
					}
					if (!target || (intptr_t)target == -1) {
						if (llvm::dyn_cast<llvm::StructType>(V->getType())->getElementType(0) == llvm::dyn_cast<llvm::StructType>(ft->type)->getElementType(0)) {
							llvm::Value* V2 = llvm::UndefValue::get(ft->type);
							V2 = Builder->CreateInsertValue(V2, Builder->CreateExtractValue(V, 0), 0);
							return V2;
						} else if (!Builder->GetInsertBlock()) {
							errs() << Loc << ": rvalue union literals only supported for 1st max-sized field element\n";
							return nullptr;
						}
					}
					llvm::Value* store = (target && (intptr_t)target != -1) ? target : CreateEntryBlockAlloca(ft->type);
					Builder->CreateStore(V, Builder->CreatePointerCast(store, V->getType()->getPointerTo()));
					if (!target || (intptr_t)target == -1)
						return Builder->CreateLoad(ft->type, store);
					else
						return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
				} else {
					V = Builder->CreateInsertValue(V, ini, i, "structinit");
				}
			} else
				V = Builder->CreateInsertValue(V, llvm::Constant::getNullValue(struct_type->getElementType(i)), i , "structzeroinit");
		}
		return handle(target, V);
	} else {
		errs() << Loc << ": '" << *ft << "' is not an aggregate type so it cannot be initialized using '{}'\n";
		return nullptr;
	}
}

llvm::Value* LvalueExprAST::codegen_raw(llvm::Value* target) {
	auto V = codegen_ref(false, true);
	if (V.first && V.second)
		// Load the value.
		return handle(target, Builder->CreateLoad(V.first, V.second, Name.c_str()));
	return nullptr;
}

llvm::Value* VariableExprAST::codegen_raw(llvm::Value* target) {
	if (!full_var) {
		errs() << Loc << ": there is no known variable/constant/function/module named '" << Name << "'\n";
		return nullptr;
	}
	if (full_var->ft.type_attr & A_rvalue) {
		if (is_unknown_type && !desired_type && full_var->val->getType()->isIntegerTy())
			return Builder->CreateIntCast(full_var->val, llvm_int_type, (bool)(full_var->ft.type_attr & A_signed));
		return full_var->val;
	}
	auto V = codegen_ref(false, true);
	if (V.first && V.second && V.second->getType()->isPointerTy()) {
		// Load the value.
		if (full_var->ft.type_attr & A_atomic)
			return handle(target, CreateAtomicLoad(V.first, V.second, Name.c_str()));
		return handle(target, Builder->CreateLoad(V.first, V.second, Name.c_str()));
	}
	return nullptr;
}

llvm::Value* VariableExprAST::codegen(bool suppress_destructor) {
	auto rawV = codegen_raw((llvm::Value*)((intptr_t)(-(int)suppress_destructor)));
	if (suppress_destructor && is_unknown_type && !desired_type) {
		if (ft->type->isIntegerTy()) {
			llvm::Value* V = Builder->CreateIntCast(rawV, ft->type, (bool)(ft->type_attr & A_signed));
			if (auto constV = llvm::dyn_cast<llvm::ConstantInt>(V)) {
				if (auto const_rawV = llvm::dyn_cast<llvm::ConstantInt>(rawV)) {
					if (ft->type_attr & A_signed) {
						auto rawv = (ft->type_attr & A_signed) ? const_rawV->getSExtValue() : (int64_t)const_rawV->getZExtValue();
						auto v = (ft->type_attr & A_signed) ? constV->getSExtValue() : (int64_t)constV->getZExtValue();
						if (v != rawv) {
							errs() << Loc << ": untyped value ";
							if (ft->type_attr & A_signed)
								errs() << rawv;
							else
								errs() << (uint64_t)rawv;
							errs() << " would be truncated to " << v
							       << " when default converted to 'int' - use explicit type/conversion\n";
							return nullptr;
						}
					}
				}
			}
			return V;
		}
	}
	return convert_raw(rawV);
}

std::pair<llvm::Type*,llvm::Value*> VariableExprAST::codegen_ref_(bool silent_fail, bool constref) {
	if (!full_var) {
		errs() << Loc << ": unknown variable name '" << Name << "'\n";
		return { nullptr, nullptr };
	}
	if (full_var->ft.type_attr & A_rvalue) {
		if (silent_fail)
			return { full_var->ft.type, nullptr };
		else {
			errs() << Loc << ": const \"variable\" can only be used as rvalue\n";
			return { nullptr, nullptr };
		}
	}
	if (!constref)
		full_var->ft.type_attr |= A_modified;
	llvm::Value* V;
	llvm::Type* storage_type;
	if ((full_var->ft.type_attr & A_globally_visible) || (full_var->ft.type_attr & A_mainvar) && (comp_mode == comp_jit && !do_test)) {
		// global variable or main var in interactive JIT
		if (!full_var->mangled_name) {
			errs() << Loc << ": no mangled name for " << Name << '\n';
			return { nullptr, nullptr };
		}
		storage_type = full_var->storage_type;
		llvm::GlobalVariable* GV = TheModule->getGlobalVariable(full_var->mangled_name, true);
		if (!GV)
			GV = new llvm::GlobalVariable(*TheModule, full_var->storage_type,
			                             false, link_type(full_var->ft.type_attr),
			                             nullptr, full_var->mangled_name, nullptr,
			                             tls_model(full_var->ft.type_attr),
			                             0, true);
		GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(full_var->storage_type));
		V = GV;
	} else {
		V = full_var->val;
		storage_type = ft->type; // full_var.first->val->getType() - deprecated;
		if (storage_type->isFunctionTy() || (ft->type_attr & A_ptrref))
			storage_type = storage_type->getPointerTo();
	}
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	if (full_var->ft.type_attr & A_ptrref) {
		if (V->getType()->isPointerTy()) {
			auto the_ref = Builder->CreateLoad(storage_type, V);
			return { full_var->ft.type, the_ref };
		} else {
			if (auto struct_type = llvm::dyn_cast<llvm::StructType>(V->getType())) {
				unsigned max_el = struct_type->getNumElements() - 1;
				llvm::Value* val = llvm::UndefValue::get(struct_type);
				for (unsigned i=0; i<max_el; i++)
					val = Builder->CreateInsertValue(val, Builder->CreateExtractValue(V, i), i);
				llvm::Value* ptr = Builder->CreateLoad(struct_type->getElementType(max_el), Builder->CreatePointerCast(Builder->CreateExtractValue(V, max_el), struct_type->getElementType(max_el)));
				V = Builder->CreateInsertValue(val, ptr, max_el);
			} else {
				errs() << Loc << ": internal error - multi level array reference inconsistent\n";
				return { nullptr, nullptr };
			}
		}
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
	} while (align < target_bytes);
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

std::pair<llvm::Type*,llvm::Value*> SelectExprAST::codegen_ref_(
	bool silent_fail, bool constref) {
	if (!ft || !ft->type)
		return { nullptr, nullptr }; // error message was already generated in AST
	if (Struct->ft->type->isArrayTy() || Struct->ft->type->isPointerTy())
		goto failure;
	if (auto LV = dynamic_cast<LvalueExprAST*>(Struct.get())) {
		auto struct_ref = LV->codegen_ref(silent_fail, constref);
		if (struct_ref.second) {
			if (Struct->ft->type_attr & A_union)
				return { ft->type, Builder->CreatePointerCast(struct_ref.second, ft->type->getPointerTo()) };
			else if ((Struct->ft->type_attr & A_complex) && Struct->ft->type == llvm_c32_type)
				return { llvm::Type::getFloatTy(Context),
					Builder->CreateConstGEP2_32(
						llvm::ArrayType::get(llvm::Type::getFloatTy(Context), 2),
						struct_ref.second, 0, FieldIndex) };
			else
				return { ft->type, Builder->CreateStructGEP(struct_ref.first, struct_ref.second, FieldIndex) };
		}
	}
failure:
	if (!silent_fail)
		errs() << Struct->Loc << ": LHS of '.' expression must be an lvalue\n";
	return { ft->type, nullptr };
}

llvm::Value* SelectExprAST::codegen_complex(llvm::Value* target) {
	auto C = Struct->codegen_raw();
	if (!C)
		return nullptr;
	if (auto int_ty = llvm::dyn_cast<llvm::IntegerType>(C->getType())) {
		if (auto const_val = llvm::dyn_cast<llvm::Constant>(C)) {
			// code to work around LLVM bug
			auto tmp_store = CreateEntryBlockAlloca(C->getType());
			Builder->CreateStore(C, tmp_store);
			C = Builder->CreateLoad(C->getType(), tmp_store);
		}
		if (FieldIndex)
			return Builder->CreateBitCast(
				Builder->CreateIntCast(
					Builder->CreateLShr(C, 32), llvm::Type::getInt32Ty(Context), false), llvm::Type::getFloatTy(Context));
		else
			return Builder->CreateBitCast(Builder->CreateIntCast(C, llvm::Type::getInt32Ty(Context), false), llvm::Type::getFloatTy(Context));
	} else {
		return Builder->CreateExtractElement(C, FieldIndex);
	}
}

llvm::Value* SelectExprAST::codegen_raw(llvm::Value* target) {
	if ((Struct->ft->type_attr & A_complex) && Struct->ft->type == llvm_c32_type)
		// We try to avoid going through codegen_ref() because this
		// migght be inefficient for a SIMD type
		return codegen_complex(target);
	auto V = codegen_ref(true);
	if (auto val = ref2val(V))
		return handle(target, val);
	if (V.first) {
		if (auto arr_type = llvm::dyn_cast<llvm::ArrayType>(Struct->ft->type)) {
			llvm::Type* arr_ty = (llvm::Type*)(intptr_t)-1;
			llvm::Value* arr = nullptr;
			if (auto array_ast = dynamic_cast<LvalueExprAST*>(Struct.get()))
				std::tie(arr_ty, arr) = array_ast->codegen_ref(true);
			if (!arr && arr_ty)
				arr = Struct->codegen_raw();
			if (!arr) {
				errs() << Loc << ": invalid array\n";
				return nullptr;
			}
			// update type after codegen
			arr_type = llvm::dyn_cast<llvm::ArrayType>(Struct->ft->type);
			// FieldIdx: 0->size, 1->order
			uint64_t size = 1;
			llvm::Value* Size = getSize(1);
			int order = 0;
			int idx = 0;
			while (arr_type) {
				order++;
				if (!FieldIndex) {
					uint64_t dim = arr_type->getNumElements();
					if (!dim)
						Size = Builder->CreateMul(Size, Builder->CreateExtractValue(arr, idx++));
					else
						size *= dim;
				}
				arr_type = llvm::dyn_cast<llvm::ArrayType>(arr_type->getElementType());
			}
			if (!FieldIndex)
				return Builder->CreateMul(Size, getSize(size));
			return Builder->getInt32(order);
		}
		if (Struct->ft->type->isPointerTy() && (Struct->ft->type_attr & A_string)) {
			llvm::Value* s = Struct->codegen_raw();
			auto Sz = Builder->CreateAnd(Builder->CreateLoad(llvm_size_type, s),
			                             getSize(target_mask >> 1));
			if (!FieldIndex)
				return Sz;
			return Builder->CreateSub(Sz, getSize(1));
		}
		llvm::Value* Store = nullptr;
		if (Struct->needs_target() || Struct->ft->type_attr & A_union) {
			Store = CreateEntryBlockAlloca(Struct->ft->type);
		}
		llvm::Value* struct_val = Struct->codegen_raw(Store);
		if (struct_val) {
			llvm::Value* val;
			if (Store) {
				if (Struct->ft->type_attr & A_union)
					val = Builder->CreateLoad(ft->type, Builder->CreatePointerCast(Store, ft->type->getPointerTo()));
				else {
					auto valptr = Builder->CreateStructGEP(Struct->ft->type, Store, FieldIndex);
					val = Builder->CreateLoad(ft->type, valptr);
				}
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
	if (target)
		errs() << Loc << ": target set\n";
	llvm::Value* val = nullptr;
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(expr->ft->type)) {
		// pass by reference
		if (auto LV = dynamic_cast<LvalueExprAST*>(expr.get())) {
			auto V = LV->codegen_ref();
			//type = V.first;
			val = V.second;
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(LV->ft->type))
				val = getInterfaceArrayValue(val, array_type);
		} else {
			llvm::Value* array = nullptr;
			if (expr->needs_target() && !target) {
				auto [allocsz, valproto, ndim] = expr->alloc_dims();
				val = CreateAlloca(allocsz,
				                   TheModule->getDataLayout().getPrefTypeAlign(llvm_size_type));
				array = expr->codegen_raw(val);
				val = Builder->CreateInsertValue(valproto, Builder->CreatePointerCast(val, llvm::cast<llvm::StructType>(valproto->getType())->getElementType(ndim)), ndim);
			} else {
				array = expr->codegen_raw(target);
			}
			if (!array) {
				errs() << Loc << ": cannot generate code for interface -expression\n";
				return nullptr;
			}
			if (array->getType()->isVoidTy()) {
				if (target) // Does this happen at all?
					return array;
				// else {
				// 	errs() << Loc << ": cannot generate code for interface expression\n";
				// 	abort();
				// 	return nullptr;
				// }
			} else {
				// if it's an rvalue we have to store it on stack to get a reference
				if (!target || (intptr_t)target == -1) {
					condnesting++; // force 'alloca()' instead of 'malloc()' - TODO: implement and use 'codegen_dims()' instead
					val = StoreValue(array, expr->ft, MakeInterfaceArrayType(array_type));
					condnesting--;
				}
			}
		}
	} else if (auto struct_type = llvm::dyn_cast<llvm::StructType>(expr->ft->type)) {
		if (auto LV = dynamic_cast<LvalueExprAST*>(expr.get())) {
			auto V = LV->codegen_ref();
			val = V.second;
		} else {
			uint64_t allocsz = 0;
			llvm::Constant* alloc_sz;
			if (expr->needs_target()) {
				allocsz = TheModule->getDataLayout().getTypeAllocSize(expr->ft->type);
				alloc_sz = Builder->getInt64(allocsz);
				val = Builder->CreateAlloca(expr->ft->type, nullptr);
			}
			llvm::Value* stuct_val = expr->codegen_raw(val);
			if (val) {
				// The following code should not be needed - but results are wrong without it... LLVM-Bug?
				auto val2 = Builder->CreateAlloca(expr->ft->type, nullptr);
				auto align = getAlignment(allocsz);
				Builder->CreateMemCpy(val2, align, val, align, allocsz);
				val = val2;
			}
			if (!val && stuct_val->getType()->isVoidTy())
				return stuct_val;
			if (!val)
				val = StoreValue(stuct_val, expr->ft);
		}
	} else {
		// pass by value
		if (expr->is_unknown_type && expr->ft->type->isIntegerTy()) {
			// usually untyped int defaults to i32
			// however, for result printing in interactive REPL we want the full value
			// so we default to i64 here
			expr->desired_type = llvm::Type::getInt64Ty(Context);
			expr->ft = lex.get_full_type((expr->ft->type_attr & A_signed) ? "i64" : "u64");
		}
		val = expr->codegen();
		if (!val)
			return nullptr;
		if (val->getType()->isFloatTy())
			val = Builder->CreateBitCast(val, llvm::Type::getInt32Ty(Context));
	}
	if (!val)
		return nullptr;
	llvm::Constant* rttype_ptr = getRtType(expr->ft);
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
		if (Operand->ft->type_attr & A_atomic) {
			if (Opcode[0] == '+')
				return handle(target, CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Add, OperandV.second, One));
			else
				return handle(target, CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Sub, OperandV.second, One));
		}
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
		if (Operand->ft->type_attr & A_atomic) {
			if (Opcode[0] == '+')
				return handle(target, CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::FAdd, OperandV.second, One));
			else
				return handle(target, CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::FSub, OperandV.second, One));
		}
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
	is_complex,
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

static std::string create_mangled_global(const std::string& unmangled_name) {
	std::string varname;
	if (lex.module->import_path.empty()) {
		varname = unmangled_name;
	} else {
		llvm::SmallString<128> buf = llvm::StringRef("_Z");
		varname = std::string(MangleBase(buf, lex.module->import_path, unmangled_name));
	}
	return varname;
}

llvm::Function* init_setter_fn(std::string& setter_name, std::string& varname, llvm::Value*& Arg) {
	// This might be a non-const initialized main var that needs a temporary
	// 'setter' function. So finish the current module to be able to remove
	// the setter after usage
	finishFunctionOrModule();
	setter_name = "__global_" + varname + "_setter";
	llvm::FunctionType* ptr_fn_t = llvm::FunctionType::get(llvm_ptr_type,
	                                                       { llvm_size_type->getPointerTo() }, false);
	llvm::Function* tmpf = llvm::Function::Create(ptr_fn_t, llvm::Function::ExternalLinkage, setter_name, TheModule.get());
	auto BB = llvm::BasicBlock::Create(Context, "entry", tmpf);
	Builder->SetInsertPoint(BB);
	Arg = tmpf->getArg(0);
	if (last_shadow_restorer && comp_mode == comp_jit && !do_test) {
		auto last_restorer_proto = (*lex.findProtos(last_shadow_restorer))[0].get();
		auto last_restorer = getFunction(last_restorer_proto);
		Builder->CreateCall(last_restorer_proto->FT, last_restorer, std::vector<llvm::Value*>());
	}
	return tmpf;
}

// helper function to to clean up global states used by HandleGlobalVariable()
static std::nullptr_t cleanupGlobal(llvm::Function* tmpf, const char* unmangled_name, std::string* varname) {
	if (tmpf)
		tmpf->eraseFromParent();
	if (unmangled_name)
		lex.module->globals_table.erase(unmangled_name);
	if (varname)
		all_global_symbols.erase(*varname);
	return nullptr;
}

static llvm::GlobalVariable* GetShadowHandle(llvm::Constant* initializer, std::string& varname) {
	std::string shadow_var_name = std::string("__") + varname + "_shadow_";
	auto V = new llvm::GlobalVariable(*TheModule, initializer->getType(),
	                                  false, llvm::GlobalValue::ExternalLinkage,
	                                  nullptr, shadow_var_name, nullptr,
	                                  llvm::GlobalVariable::NotThreadLocal, 0, true);
	V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(initializer->getType()));
	return V;
}

static bool CreateShadow(llvm::Constant* initializer, std::string& varname) {
	std::string shadow_var_name = std::string("__") + varname + "_shadow_";
	auto V = new llvm::GlobalVariable(*TheModule, initializer->getType(),
	                                  false, llvm::GlobalValue::ExternalLinkage,
	                                  initializer, shadow_var_name, nullptr,
	                                  llvm::GlobalVariable::NotThreadLocal);
	V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(initializer->getType()));
	finishFunctionOrModule();
	return true;
}

static llvm::GlobalVariable* GetGlobalHandle(llvm::Type* type, std::string& varname, unsigned sym_kind) {
	llvm::GlobalVariable* GV = TheModule->getGlobalVariable(varname, true);
	if (!GV) {
		GV = new llvm::GlobalVariable(*TheModule, type,
		                              false, link_type(sym_kind),
		                              nullptr, varname, nullptr,
		                              tls_model(sym_kind), 0, true);
		GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(type));
	}
	return GV;
}

llvm::GlobalVariable* CreateGlobal(llvm::Constant* initializer,  std::string& varname, unsigned sym_kind) {
	llvm::GlobalVariable* GV = new llvm::GlobalVariable(*TheModule, initializer->getType(),
	                                                    false, link_type(sym_kind), initializer, varname, nullptr,
	                                                    tls_model(sym_kind), 0, false);
	GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(initializer->getType()));
	return GV;
}

static std::pair<llvm::Type*,llvm::Value*> GetReference(ExprAST* RHS, FullVar*& is_referencing) {
	llvm::Value* Val = nullptr;
	llvm::Type* type = nullptr;
	if (auto refexpr = dynamic_cast<LvalueExprAST*>(RHS)) {
		auto BaseVar = refexpr->getBase();
		if (BaseVar->ft->type_attr & (A_global | A_const)) {
			errs() << BaseVar->Loc << ": cannot create reference to " << ((BaseVar->ft->type_attr & A_global) ? "global variable\n" : "constant\n");
			return { nullptr, nullptr };
		}
		auto t_v = refexpr->codegen_ref();
		Val = t_v.second;
		if (Val) {
			type = t_v.first;
			is_referencing = BaseVar->full_var;
		}
	}
	if (!Val)
		errs() << RHS->Loc << ": cannot get reference from expression\n";
	return { type, Val };
}

static void RegisterShadowHandlers(llvm::Constant* initializer, std::string& varname, bool needs_constructor);
static void RegisterThreadConstructor(std::string& varname, volvoxc::FullType* ft, unsigned sym_kind);
static void RegisterThreadDestructor(std::string& varname, volvoxc::FullType* ft, unsigned sym_kind);

std::map<std::string,bool> all_global_symbols;

static inline const char* global_kind_str(unsigned flags) {
	if (flags & A_const)
		return "const";
	if (flags & A_shared)
		return "shared";
	if (flags & A_atomic)
		return "atomic";
	if (flags & A_extern)
		return "extern";
	return "global";
}

std::nullptr_t HandleGlobalVariable(std::unique_ptr<BinaryExprAST> expr, unsigned sym_kind) {
	bool rhs_is_constexpr = !strcmp(expr->Op, ":=");
	if (rhs_is_constexpr && !(sym_kind & (A_global | A_const))) {
		errs() << expr->Loc << ": using ':=' is only valid when declaring 'const' or 'global' variables\n";
		return nullptr;
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
	bool initialization_from_main = (comp_mode != comp_jit || do_test) && (!rhs_is_constexpr || (expr->RHS->ft->type_attr & A_constructor));
	bool prepare_setter_fn = comp_mode == comp_jit && !do_test && (!rhs_is_constexpr || (expr->RHS->ft->type_attr & A_constructor));
	const std::string& unmangled_name = LHSE->getName();
	if (!rhs_is_constexpr && expr->RHS->ft->type->isArrayTy() && (sym_kind & A_globally_visible)) {
		errs() << expr->Loc << ": " << global_kind_str(sym_kind)
		       << " arrays " << *expr->RHS->ft->type << " can only be initialized with a constexpr using ':='\n";
		return cleanupGlobal(nullptr, unmangled_name.c_str(), nullptr);
	}
	auto varname = create_mangled_global(unmangled_name);
	if (verbosity >= 3)
		errs() << CurLoc << ": HandleGlobalValiable called for " << varname << "\n";
	if (sym_kind & A_globally_visible) {
		auto ins_success = all_global_symbols.insert({varname,false});
		if (!ins_success.second) {
			errs() << expr->LHS->Loc << ": '" << varname << "' already in use as global/external " << (ins_success.first->second ? "function\n" : "variable\n");
			return nullptr;
		}
	}
	// We do not know in advance if the RHS of the 'main' var  initialization is a compile
	// time const. In order to be able to run 'RHS->codegen()' in any case, a function
	// context is needed. If the initializer turns out to be a compile time const this
	// function is not needed and can be 'erased'
	std::string setter_name;
	llvm::Function* tmpf = nullptr;
	llvm::Value* Arg = nullptr;
	if (prepare_setter_fn)
		tmpf = init_setter_fn(setter_name, varname, Arg);
	llvm::Value* Val = nullptr;
	llvm::Type* type;
	FullVar* is_referencing = nullptr;
	unsigned attribs = 0;
	unsigned is_union = expr->RHS->ft->type_attr & A_union;
	bool is_call_expr = false;
	bool is_constructor_call = false;
	bool use_target = false;
	bool have_array = false;
	llvm::Value* target = nullptr;
	bool needs_constructor = false;
	size_t allocsz = expr->RHS->ft->type->isSized() ? TheModule->getDataLayout().getTypeAllocSize(expr->RHS->ft->type) : 0;
	if (LREF) {
		std::tie(type, Val) = GetReference(expr->RHS.get(), is_referencing);
		if (!Val) {
			errs() << expr->Loc << ": cannot get reference (RHS no lvalue?)\n";
			return cleanupGlobal(tmpf, unmangled_name.c_str(), &varname);
		}
	} else {
		if (llvm::isa<llvm::StructType>(expr->RHS->ft->type)) {
			if (auto callexpr = dynamic_cast<CallExprAST*>(expr->RHS.get())) {
				is_call_expr = true;
				if (auto type_expr = dynamic_cast<TypeExprAST*>(callexpr->Callee.get()))
					is_constructor_call = true;
			} else if (dynamic_cast<BinaryExprAST*>(expr->RHS.get()) ||
			           dynamic_cast<PostfixExprAST*>(expr->RHS.get()) ||
			           dynamic_cast<UnaryExprAST*>(expr->RHS.get()) ||
			           dynamic_cast<BranchExprAST*>(expr->RHS.get()))
				is_call_expr = true;
			if (is_constructor_call || ((allocsz > sret_limit) && !rhs_is_constexpr))
				use_target = true;
		} else if (expr->RHS->ft->type_attr & A_map)
			use_target = true;
		needs_constructor = !is_call_expr && (expr->RHS->ft->type_attr & A_constructor);
		if (!use_target && (!initialization_from_main || rhs_is_constexpr)) {
			if (rhs_is_constexpr && (sym_kind & A_const) && expr->RHS->is_unknown_type)
				if (expr->RHS->ft->type->isIntegerTy())
					expr->RHS->desired_type = llvm::Type::getInt64Ty(Context);
			Val = expr->RHS->codegen(true);
			allocsz = expr->RHS->ft->type->isSized() ? TheModule->getDataLayout().getTypeAllocSize(expr->RHS->ft->type) : 0;
		}
	}
	attribs = expr->RHS->ft->type_attr & (LREF ? (A_signed | A_string | A_cstring | A_map | A_complex) : (A_signed | A_string | A_cstring | A_map | A_complex | A_destructor));
	type = expr->RHS->ft->type;
	llvm::Constant* initializer = nullptr;
	// if (rhs_is_constexpr && !Val) {
	// 	errs() << expr->RHS->Loc << ": generating value for const expr\n";
	// 	Val = expr->RHS->codegen(true);
	// }
	// if (rhs_is_constexpr && !Val)
	// 	errs() << expr->RHS->Loc << ": no value for const expr\n";
	if (Val) {
		if (rhs_is_constexpr) {
			initializer = llvm::dyn_cast<llvm::Constant>(Val);
			if (!initializer) {
				errs() << expr->RHS->Loc << ": initialization with ':=' requires a compile time const on the RHS\n";
				return cleanupGlobal(tmpf, unmangled_name.c_str(), &varname);
			}
		}
	} else if (!use_target && !initialization_from_main && !dynamic_cast<LvalueExprAST*>(expr->RHS.get())) {
		errs() << expr->Loc << ": cannot generate code for RHS\n";
		return cleanupGlobal(tmpf, unmangled_name.c_str(), &varname);
	}
	bool needs_store;
	if (initializer) {
		needs_store = false;
	} else {
		if (LREF)
			initializer = llvm::Constant::getNullValue(expr->RHS->ft->type->getPointerTo());
		else if (allocsz > 0) {
			initializer = llvm::Constant::getNullValue(expr->RHS->ft->type);
		}
		needs_store = !use_target;
	}
	bool needs_call = (needs_store || use_target || needs_constructor) && !initialization_from_main;
	if (needs_store && (sym_kind & A_global)) {
		if (LREF) {
			errs() << expr->LHS->Loc << ": references are not allowed to be global or const\n";
			return cleanupGlobal(tmpf, unmangled_name.c_str(), &varname);
		}
	}
	llvm::GlobalVariable* GV;
	if (initializer && !LREF)
		if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(expr->RHS->ft->type))
			if (auto ini_array_type = llvm::dyn_cast<llvm::ArrayType>(initializer->getType()))
				if (auto const_initializer = llvm::dyn_cast<llvm::Constant>(expandArrayInitializer(initializer, ini_array_type, array_type)))
					initializer = const_initializer;
	if (comp_mode == comp_dbg) {
		// Create a debug descriptor for the variable.
		DBuilder->createGlobalVariableExpression(
			SP, varname, varname, Unit, expr->Loc.Line, lex.get_diType(type, attribs & A_signed), false);
	}
	FullVar* fv = lex.module->globals_table[unmangled_name.c_str()];
	if (!fv) {
		errs() << expr->RHS->Loc << ": internal error - variable '" << unmangled_name << "' not found in database\n";
		return nullptr;
	}
	if (initializer) { // i.e. constant size type
		// If 'needs_call' is true, this here is part of a module which is going to be
		// removed later. So in this case it's only a declaration and the 'real'
		// variable is defined below in a separate module that will stay.
		if ((sym_kind & A_rvalue) && (sym_kind & A_const) && !needs_call) {
			fv->val = initializer;
			sym_kind |= A_rvalue;
			GV = nullptr;
		} else {
			if (needs_call)
				GV = GetGlobalHandle(initializer->getType(), varname, sym_kind);
			else
				GV = CreateGlobal(initializer, varname, sym_kind);
			fv->storage_type = initializer->getType();
		}
	} else {
		if (sym_kind & A_globally_visible) {
			errs() << expr->Loc << ": internal error - no initializer\n";
			abort();
		}
		fv->storage_type = nullptr;
	}
	fv->mangled_name = strdup(varname.c_str());
	fv->ft = *expr->RHS->ft;
	fv->ft.type = use_target ? expr->RHS->ft->type : type;
	fv->ft.type_attr = sym_kind | attribs | is_union | (LREF ? A_ptrref : 0U) | A_mainvar | ((sym_kind & (A_const | A_atomic)) ? A_global : 0);
	if (sym_kind & A_rvalue) {
		if (sym_kind & A_const) {
			if (expr->RHS->is_unknown_type && (fv->ft.type->isFloatTy() || fv->ft.type->isDoubleTy()
			                                   || fv->ft.type->isIntegerTy() && fv->ft.type->getIntegerBitWidth() > 1)) {
				fv->ft.type_attr |= A_untyped;
			}
			return nullptr;
		}
		else
			fv->ft.type_attr &= ~A_rvalue;
	}
	if (is_referencing)
		fv->mark_as_referencing(is_referencing);
	bool shadow_already_created = false; // track creation to avoid duplicate symbol errors
	if (!needs_call) {
		if (initialization_from_main) {
			auto theLoc = expr->LHS->Loc;
			if (!rhs_is_constexpr) {
				// transform this declaration to an assignment and insert it into "main()"'s expr list
				// the operator remains ':=' to indicate that no destructor for the old LHS value
				// must be inserted
				expr->opclass = OpGlobalDeclAssign;
				LHSE->full_var = fv;
				GlobalExprList.push_back(std::move(expr));
			}
			if (needs_constructor) {
				auto varExpr = std::make_unique<VariableExprAST>(theLoc, unmangled_name, fv);
				auto constructor_call = std::make_unique<DefaultConstructorCall>(theLoc, std::move(varExpr));
				GlobalExprList.push_back(std::move(constructor_call));
				if (sym_kind & A_global)
					RegisterThreadConstructor(varname, &fv->ft, sym_kind);
			}
			if (fv->ft.type_attr & A_destructor)
				RegisterThreadDestructor(varname, &fv->ft, sym_kind);
			cleanupGlobal(tmpf, nullptr, nullptr);
			return nullptr;
		}
	} else {
		llvm::Type* array_ptr_ty = nullptr;
		llvm::Value* ptrRet = nullptr;
		unsigned ndim = 0;
		llvm::StructType* struct_type = nullptr;
		llvm::Value* retVal = nullptr;
		if (needs_store || use_target) {
			if (comp_mode != comp_jit) {
				errs() << expr->Loc << ": internal error - non-global main variable '" << varname
				       << "' handled by HandleGlobalVariable() in non-JIT mode\n";
				abort();
			}
			if (initializer) { // constant size initializer
				if (use_target)
					expr->RHS->codegen_raw(GV);
				else
					Builder->CreateStore(Val, GV);
			} else {
				if (auto RHS_Lval = dynamic_cast<LvalueExprAST*>(expr->RHS.get())) {
					// variable size array variable
					auto r_ref = RHS_Lval->codegen_ref(false, true);
					auto array_type = llvm::dyn_cast<llvm::ArrayType>(RHS_Lval->ft->type);
					if (!array_type || !r_ref.second) {
						errs() << expr->LHS->Loc << ": internal error - cannot generate RHS reference\n";
						cleanupGlobal(tmpf, unmangled_name.c_str(), &varname);
						return nullptr;
					}
					std::vector<llvm::Value*> Dims;
					std::vector<llvm::Value*> returnDims;
					auto elem_type = getArrayDims(r_ref.second, array_type, Dims, returnDims);
					size_t el_allocsz = elem_type->isSized() ? TheModule->getDataLayout().getTypeAllocSize(elem_type) : 0;
					if (!el_allocsz) {
						errs() << "array element type must be sized\n";
						cleanupGlobal(tmpf, unmangled_name.c_str(), &varname);
						return nullptr;
					}
					llvm::Value* Len = getSize(1);
					for (auto dim: Dims)
						Len = Builder->CreateMul(Len, dim);
					llvm::Value* ValPtr;
					if ((struct_type = llvm::dyn_cast<llvm::StructType>(r_ref.second->getType())))
						ValPtr = Builder->CreateExtractValue(r_ref.second, struct_type->getNumElements() - 1);
					else
						ValPtr = r_ref.second;
					llvm::Value* ArrayAlloc = nullptr;
					auto ElemSize = getSize(el_allocsz);
					llvm::Value* Sz = Builder->CreateMul(ElemSize, Len);
					if (inside_function || comp_mode != comp_jit || do_test)
						ArrayAlloc = Builder->CreateAlloca(elem_type, Len, varname);
					else {
						if (comp_mode != comp_jit || do_test) {
#if LLVM_VERSION_MAJOR >= 18
							ArrayAlloc = Builder->CreateMalloc(llvm_size_type, llvm::Type::getInt8Ty(Context),
							                                   ElemSize, Len, nullptr, varname);
#else
							ArrayAlloc = llvm::CallInst::CreateMalloc(Builder->GetInsertBlock(),
							                                          llvm_size_type, llvm::Type::getInt8Ty(Context),
							                                          ElemSize, Len, nullptr, varname);
							ArrayAlloc = Builder->Insert(ArrayAlloc);
#endif
						} else {
							const char* jit_malloc = "__jit_managed_malloc";
							auto jit_malloc_proto = (*lex.findProtos(jit_malloc))[0].get();
							auto jit_malloc_fn = getFunction(jit_malloc_proto);
							ArrayAlloc = Builder->CreateCall(jit_malloc_proto->FT, jit_malloc_fn, std::vector<llvm::Value*>({ Sz }));
						}
					}
					auto align = TheModule->getDataLayout().getPrefTypeAlign(elem_type);
					retVal = r_ref.second;
					Builder->CreateMemCpy(ArrayAlloc, align, ValPtr, align, Sz);
					ptrRet = ArrayAlloc;
				} else { // variable size array literal
					retVal = StoreValue(Val, expr->RHS->ft, nullptr, varname);
					struct_type = llvm::dyn_cast<llvm::StructType>(retVal->getType());
				}
				if (struct_type) {
					ndim = struct_type->getNumElements() - 1;
					for (unsigned dim = 0; ; ) {
						Builder->CreateStore(Builder->CreateExtractValue(retVal, dim), Arg);
						if (++dim >= ndim)
							break;
						Arg = Builder->CreateIntToPtr(Builder->CreateAdd(Builder->CreatePtrToInt(Arg, llvm_size_type),
						                                                 getSize(target_bytes)), Arg->getType());
					}
					if (!ptrRet)
						ptrRet = Builder->CreateExtractValue(retVal, ndim);
					array_ptr_ty = ptrRet->getType();
				} else if (auto array_type = llvm::dyn_cast<llvm::PointerType>(retVal->getType())) {
					array_ptr_ty = retVal->getType();
					ptrRet = retVal;
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
		}
		if (jit_extra_thread && (sym_kind & A_global)) {
			auto V = GetShadowHandle(initializer, varname);
			auto Vval = Builder->CreateLoad(initializer->getType(), GV);
			Builder->CreateStore(Vval, V);
		}
		InsertDestructors(expr_temps);
		if (jit_extra_thread && last_shadow_saver) {
			auto last_saver_proto = (*lex.findProtos(last_shadow_saver))[0].get();
			auto last_saver = getFunction(last_saver_proto);
			Builder->CreateCall(last_saver_proto->FT, last_saver, std::vector<llvm::Value*>());
		}
		if (initializer || !needs_store)
			Builder->CreateRet(llvm::ConstantPointerNull::get(llvm_ptr_type));
		else
			Builder->CreateRet(Builder->CreatePointerCast(ptrRet, llvm_ptr_type));
		finishFunctionOrModule(tmpf, 2, true, false);
		auto RT = TheJIT->getMainJITDylib().createResourceTracker();
		auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context);
		ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
		InitializeModuleAndPassManager();
		// We want to remove the 'setter' module below but the global variable
		// must stay, so put the latter in a new module that is not freed
		// by the resource tracker
		if (initializer && needs_call)
			GV = CreateGlobal(initializer, varname, sym_kind);
		if (jit_extra_thread && (sym_kind & A_global) && needs_call)
			shadow_already_created = CreateShadow(initializer, varname);
		finishFunctionOrModule();
		// Search the JIT for the <setter_name> symbol.
		auto ExprSymbol = ExitOnErr(TheJIT->lookup(setter_name));
		// C syntax at its best...
#if LLVM_VERSION_MAJOR >= 17
		char* (*PTR)(size_t*) = ExprSymbol.getAddress().toPtr<char* (*)(size_t*)>();
#else
		char* (*PTR)(size_t*) = (char* (*)(size_t*))(intptr_t)ExprSymbol.getAddress();
#endif
		size_t* Dims = ndim ? (size_t*)alloca(ndim * sizeof(size_t)) : nullptr;
		char* varptr = PTR(Dims);
		if (varptr) {
			if (ndim) {
				std::vector<llvm::Type*> struct_type_el(ndim + 1, llvm_size_type);
				struct_type_el[ndim] = array_ptr_ty;
				llvm::Type* struct_type = llvm::StructType::get(Context, struct_type_el);
				llvm::Value* the_struct = llvm::UndefValue::get(struct_type);
				for (unsigned u = 0; u<ndim; u++)
					the_struct = Builder->CreateInsertValue(the_struct, getSize(Dims[u]), u);
				the_struct = Builder->CreateInsertValue(the_struct, Builder->CreateIntToPtr(getSize((uintptr_t)varptr), array_ptr_ty), ndim);
				fv->val = the_struct;
			} else {
				fv->val = Builder->CreateIntToPtr(getSize((uintptr_t)varptr), expr->RHS->ft->type->getPointerTo());
			}
			fv->ft.type_attr &= ~A_mainvar;
		}
		ExitOnErr(RT->remove());
	}
	if (needs_constructor && (sym_kind & A_global))
		RegisterThreadConstructor(varname, &fv->ft, sym_kind);
	if (fv->ft.type_attr & A_destructor)
		RegisterThreadDestructor(varname, &fv->ft, sym_kind);
	if (jit_extra_thread && (sym_kind & A_global))
		RegisterShadowHandlers(initializer, varname, shadow_already_created);
	return nullptr;
}

static void RegisterShadowHandlers(llvm::Constant* initializer, std::string& varname, bool shadow_already_created) {
	llvm::Type* V_type = initializer->getType();
	size_t storage_sz = TheJIT->getDataLayout().getTypeStoreSize(V_type);
	std::string shadow_var_name = std::string("__") + varname + "_shadow_";
	auto link_type = llvm::GlobalValue::ExternalLinkage;
	if (!shadow_already_created)
		CreateShadow(initializer, varname);
	llvm::GlobalVariable* GV = TheModule->getGlobalVariable(varname, true);
	if (!GV) {
		GV = new llvm::GlobalVariable(*TheModule, V_type,
		                              false, link_type,
		                              nullptr, varname, nullptr,
		                              llvm::GlobalVariable::GeneralDynamicTLSModel,
		                              0, true);
		GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(V_type));
	}
	auto V = TheModule->getGlobalVariable(shadow_var_name, true);
	if (!V) {
		V = new llvm::GlobalVariable(*TheModule, V_type,
		                             false, link_type,
		                             nullptr, shadow_var_name, nullptr,
		                             llvm::GlobalVariable::NotThreadLocal,
		                             0, true);
		V->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(V_type));
	}
	auto sz_const = llvm::ConstantInt::get(llvm_size_type, storage_sz);
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
		Builder->CreateCall(last_saver_proto->FT, last_saver, std::vector<llvm::Value*>());
	}
	Builder->CreateRetVoid();
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
		Builder->CreateCall(last_restorer_proto->FT, last_restorer, std::vector<llvm::Value*>());
	}
	Builder->CreateRetVoid();
	finishFunctionOrModule(Frestorer, 3, false);
	auto restorerProto = std::make_unique<PrototypeAST>(CurLoc, restorer, std::vector<std::string>());
	last_shadow_restorer = restorerProto->Name.c_str();
	module->FunctionProtos[restorer].push_back(std::move(restorerProto));
}

// Since global variables are TLS the constructor has to be called for each newly
// started thread. For that we create a function that first calls 'last_thread_constructor_caller'
// and then the constructor of this GV. 'last_thread_constructor_caller' is then replaced by
// our new function
static void RegisterThreadConstructor(std::string& varname, volvoxc::FullType* ft, unsigned sym_kind) {
	if (verbosity >= 3)
		errs() << CurLoc << ": RegisterThreadConstructor called for " << varname << "\n";
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
		                    std::vector<llvm::Value*>());
	}
	auto C = getConstructorOrDestructor(ft);
	auto GV = GetGlobalHandle(ft->type, varname, sym_kind);
	Builder->CreateCall(C, { GV });
	Builder->CreateRetVoid();
	finishFunctionOrModule(newConstructorCaller, 1, jit_repl);
	auto constructor_caller_Proto = std::make_unique<PrototypeAST>(CurLoc, constructor_caller, std::vector<std::string>(), A_pub);
	last_thread_constructor_caller = constructor_caller_Proto->Name.c_str();
	// constructor callers must be always accessible so force them into builtin namespace
	Module* module = (lex.source_stack.size()) ? lex.source_stack.front().module : lex.module;
	module->FunctionProtos[constructor_caller].push_back(std::move(constructor_caller_Proto));
}

static void RegisterThreadDestructor(std::string& varname, volvoxc::FullType* ft, unsigned sym_kind) {
	if (verbosity >= 3)
		errs() << CurLoc << ": RegisterThreadDestructor called for " << varname << "\n";
	auto void_fn_t = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {}, false);
	auto destructor_caller = std::string("__") + varname + "_destructor_caller";
	auto newDestructorCaller = llvm::Function::Create(void_fn_t, llvm::Function::ExternalLinkage,
	                                                   destructor_caller, TheModule.get());
	newDestructorCaller->addFnAttr(llvm::Attribute::AlwaysInline);
	auto BB = llvm::BasicBlock::Create(Context, "entry", newDestructorCaller);
	Builder->SetInsertPoint(BB);
	auto C = getConstructorOrDestructor(ft, true);
	auto GV = GetGlobalHandle(ft->type, varname, sym_kind);
	Builder->CreateCall(C, { GV });
	if (last_thread_destructor_caller) {
		auto last_thrdestr_proto = (*lex.findProtos(last_thread_destructor_caller))[0].get();
		auto last_caller = getFunction(last_thrdestr_proto);
		Builder->CreateCall(last_thrdestr_proto->FT, last_caller,
		                    std::vector<llvm::Value*>());
	}
	Builder->CreateRetVoid();
	finishFunctionOrModule(newDestructorCaller, 1, jit_repl);
	auto destructor_caller_Proto = std::make_unique<PrototypeAST>(CurLoc, destructor_caller, std::vector<std::string>(), A_pub);
	last_thread_destructor_caller = destructor_caller_Proto->Name.c_str();
	// destructor callers must be always accessible so force them into builtin namespace
	Module* module = (lex.source_stack.size()) ? lex.source_stack.front().module : lex.module;
	module->FunctionProtos[destructor_caller].push_back(std::move(destructor_caller_Proto));
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

static llvm::Value* compare_strings(llvm::Value* L, llvm::Value* R) {
	std::string C_strcpy = "strcmp";
	auto strcmp_proto = (*lex.findProtos(C_strcpy))[0].get();
	auto strcmp_fn = getFunction(strcmp_proto);
	return Builder->CreateCall(strcmp_proto->FT, strcmp_fn, std::vector<llvm::Value*>{
			Volvox2CStr(L), Volvox2CStr(R) });
}

llvm::Value* BinaryExprAST::codegen_atomic_Xassign(llvm::Type* typ, llvm::Value* ptr) {
	RHS->desired_type = typ;
	llvm::Value* val = RHS->codegen();
	if (!val)
		return nullptr;
	switch (Op[0]) {
	case '=':
		return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Xchg, ptr, val);
	case '+':
		if (val->getType()->isFloatingPointTy())
			return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::FAdd, ptr, val);
		return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Add, ptr, val);
	case '-':
		if (val->getType()->isFloatingPointTy())
			return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::FSub, ptr, val);
		return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Sub, ptr, val);
	case '&':
		if (Op[1] == '&') {
			errs() << Loc << ": lazy '&&=' not possible for atomic LHS\n";
			return nullptr;
		}
		return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::And, ptr, val);
	case '|':
		if (Op[1] == '|') {
			errs() << Loc << ": lazy '||=' not possible for atomic LHS\n";
			return nullptr;
		}
		return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Or, ptr, val);
	case '>':
		return CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Xor, ptr, val);
	default:
		errs() << Loc << ": '" << Op << "' not supported for atomics\n";
		return nullptr;
	}
}

llvm::Value* BinaryExprAST::codegen_atomic_CmpExchange(llvm::Type* typ, llvm::Value* ptr) {
	auto rhs_expr = dynamic_cast<BinaryExprAST*>(RHS.get());
	if (!rhs_expr || rhs_expr->Op[0] != ':' || rhs_expr->Op[1] != '\0') {
		errs() << RHS->Loc << ": malformes RHS for '?=' operator\n";
		return nullptr;
	}
	ExprAST* expected = rhs_expr->LHS.get();
	ExprAST* new_val = rhs_expr->RHS.get();
	expected->desired_type = typ;
	new_val->desired_type = typ;
	llvm::Value* expected_val = expected->codegen();
	llvm::Value* new_val_val = new_val->codegen();
	if (!expected_val || !new_val_val)
		return nullptr;
	auto align = llvm::Align(TheModule->getDataLayout().getTypeStoreSize(typ));
	llvm::Value* res = Builder->CreateAtomicCmpXchg(ptr, expected_val, new_val_val, align,
	                                               llvm::AtomicOrdering::SequentiallyConsistent,
	                                               llvm::AtomicOrdering::SequentiallyConsistent);
	// res is a struct { new_val_t, bool } - so get the second part
	return Builder->CreateExtractValue(res, llvm::ArrayRef<unsigned>{ 1 });
}

std::tuple<llvm::FunctionType*,llvm::Function*,llvm::Type*> findModAssign(
	const char* Op, volvoxc::FullType* LHS_ft, volvoxc::FullType* RHS_ft)
{
	const char* func_name = nullptr;
	llvm::FunctionType* func_ty = nullptr;
	llvm::Function* func = nullptr;
	llvm::Type* des_ty = LHS_ft->type;
	if (LHS_ft->type->isPointerTy()) {
		if (!strcmp(Op, "+=")) {
			if (LHS_ft->type_attr & A_string) {
				if (RHS_ft->type_attr & A_string)
					func_name = "__string_add_assign";
				else if (RHS_ft->type_attr & A_cstring)
					func_name = "__string_add_c_assign";
			}
		}
	}
	if (func_name) {
		auto func_proto = (*lex.findProtos(func_name))[0].get();
		if (!func_proto) {
			errs() << "internal compiler error: builtin function "
			       << func_name << "() not declared\n";
			abort();
		}
		func_ty = func_proto->FT;
		func = getFunction(func_proto);
	}
	return { func_ty, func, des_ty };
}

llvm::Value *BinaryExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Special assign-like ops because we don't want to emit the LHS as an expression.
	// assign op '=' is a comparison (not an assignment) when a boolean result is expected
	if (opclass == OpDeclAssign || opclass == OpGlobalDeclAssign || opclass == OpAssign || opclass == OpModAssign || opclass == OpCmpExchange) {
		bool postpone_valgen = false;
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
		if (opclass != OpDeclAssign) {
			Variable = LHSE->codegen_ref();
			if (!Variable.second)
				return nullptr;
			if (LHSE->ft->type_attr & A_atomic) {
				if (opclass == OpCmpExchange)
					return codegen_atomic_CmpExchange(Variable.first, Variable.second);
				else
					return codegen_atomic_Xassign(Variable.first, Variable.second);
			} else {
				if (opclass == OpCmpExchange) {
					errs() << LHS->Loc << ": LHS of '?=' operator must be atomic\n";
					return nullptr;
				}
			}
		}
		if (opclass == OpModAssign) { // +=, <<=, ...
			auto new_LHS = std::make_unique<RefExprAST>(LHS->Loc, LHS->ft, LHSE->getBase(), Variable.second, LHSE->Name);
			auto [mod_func_t, mod_fn, des_rhs] = findModAssign(Op, LHS->ft, RHS->ft);
			if (mod_fn) {
				RHS->desired_type = des_rhs;
				return Builder->CreateCall(mod_func_t, mod_fn, std::vector<llvm::Value*>({ Variable.second, RHS->codegen() }));
			}
			char newOp[4];
			int m=0;
			for ( ; Op[m] != '='; m++)
				newOp[m] = Op[m];
			newOp[m] = '\0';
			RHS = std::make_unique<BinaryExprAST>(
				Loc, newOp, std::move(new_LHS), std::move(RHS),
				std::tuple<llvm::Type*, bool, bool, OpClass, const char*>{
					ft->type, ft->type_attr & (A_signed | A_string | A_map), is_unknown_type, getOpClass(newOp), err_msg });
		}
		if (opclass != OpDeclAssign && opclass != OpGlobalDeclAssign)
			RHS->desired_type = LHSE->ft->type;
		// Codegen the RHS.
		uint64_t allocsz = LREF ?
			target_bytes :
			(LHSE->ft->type && LHSE->ft->type->isSized()) ?
			TheModule->getDataLayout().getTypeAllocSize(LHSE->ft->type) :
			0; // if size is compile time const
		llvm::Value* Val = nullptr;
		llvm::Value* ValPtr = nullptr;
		llvm::Value* AllocSize = nullptr;
		llvm::Type* elem_type = nullptr;
		llvm::StructType* struct_type = nullptr;
		uint64_t el_allocsz = 0;
		llvm::Value* Struct = nullptr;
		FullVar* is_referencing = nullptr;
		std::string* rname = nullptr;
		bool is_call_expr = false;
		bool is_constructor_call = false;
		if (auto callexpr = dynamic_cast<CallExprAST*>(RHS.get())) {
			is_call_expr = true;
			if (auto type_expr = dynamic_cast<TypeExprAST*>(callexpr->Callee.get()))
				// check that this is not just an explicit basic type conversion like 'f64(i)'
				if (llvm::isa<llvm::StructType>(type_expr->ft->type))
					is_constructor_call = true;
		} else if (dynamic_cast<BinaryExprAST*>(RHS.get()) ||
		           dynamic_cast<PostfixExprAST*>(RHS.get()) ||
		           dynamic_cast<UnaryExprAST*>(RHS.get()) ||
		           dynamic_cast<BranchExprAST*>(RHS.get()))
				is_call_expr = true;
		else if (auto RHS_Lval = dynamic_cast<LvalueExprAST*>(RHS.get())) {
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
					AllocSize = getSize(1);
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
				if (!LREF && allocsz <= sret_limit && !is_constructor_call) {
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
		if (allocsz <= sret_limit && !is_constructor_call) {
			if (RHS->ft->type_attr & A_use_target) {
				postpone_valgen = true;
			} else {
				if (RHS->ft->type_attr & A_string)
					Val = RHS->codegen_raw((llvm::Value*)(intptr_t)-1);
				else
					Val = RHS->codegen(true);
				if (!Val)
					return nullptr;
			}
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
			if (allocsz > sret_limit || is_constructor_call) {
				auto align = getAlignment(allocsz);
				if (target && (intptr_t)target != -1) {
					Builder->CreateMemCpy(target, align, Variable.second, align, allocsz);
					return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
				} else {
					auto OldVal = Builder->CreateLoad(Variable.first, Variable.second);
					if (ValPtr)
						Builder->CreateMemCpy(Variable.second, align, ValPtr, align, allocsz);
					else {
						auto voidval = RHS->codegen_raw(Variable.second);
						if (!voidval || !voidval->getType()->isVoidTy()) {
							errs() << Loc << ": internal error: sret ++call does not return void\n";
							return nullptr;
						}
					}
					return OldVal;
				}
			} else {
				llvm::Value* OldVal = (target && ValPtr && !postpone_valgen) ?
					llvm::UndefValue::get(llvm::Type::getVoidTy(Context)) :
					(llvm::Value*)Builder->CreateLoad(Variable.first, Variable.second);
				if (postpone_valgen)
					RHS->codegen_raw(Variable.second);
				else
					if (ValPtr) {
						llvm::Value* dptr = Variable.second;
						if (auto struct_type = llvm::dyn_cast<llvm::StructType>(Variable.second->getType())) {
							dptr = Builder->CreateExtractValue(dptr, struct_type->getNumElements()-1);
						}
						if (!allocsz) {
							llvm::Value* Allocsz = LHSE->alloc_size();
							auto align = getAlignment(1);
							if (target)
								Builder->CreateMemCpy(target, align, dptr, align, Allocsz);
							Builder->CreateMemCpy(dptr, align, ValPtr, align, Allocsz);
						} else {
							auto align = getAlignment(allocsz);
							if (target)
								Builder->CreateMemCpy(target, align, dptr, align, allocsz);
							Builder->CreateMemCpy(dptr, align, ValPtr, align, allocsz);
						}
						if (target)
							return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
					} else
						Builder->CreateStore(Val, Variable.second);
				if (opclass == OpDeclAssign || opclass == OpGlobalDeclAssign)
					// declarations have no return type
					return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
				// call destructor for OldVal if discarded
				return handle_d(target, OldVal, LHS->ft->type_attr);
			}
		}
	not_found:
		if (opclass != OpDeclAssign) {
			errs() << LHS->Loc << ": unknown variable name '" << varname << "'\n";
			return nullptr;
		}
		// variable declaration
		llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
		FullVar* entry;
		if (locals_table.empty()) {
			entry = lex.module->globals_table[varname];
		} else {
			entry = locals_table.back()[varname];
		}
		if (!entry) {
			errs() << LHS->Loc << ": internal error - '" << varname << "' has an inconsistent state\n";
			return nullptr;
		}
		// Entry has already been created by parser but we might have to adjust the type of the new
		// variable after RHS->codegen() has been run (e.g. array dimensions might only be known by now)
		llvm::Type* type = RHS->ft->type;
		unsigned attribs = RHS->ft->type_attr & (A_signed | A_string | A_map);
		entry->ft.type = type;
		entry->ft.type_attr |= attribs;
		if (Val) {
			auto Alloca = StoreValue(Val, &entry->ft, nullptr, varname);
			entry->val = Alloca;
			if (comp_mode == comp_dbg) {
				// Create a debug descriptor for the variable.
				llvm::DILocalVariable *D = DBuilder->createAutoVariable(
					SP, varname, Unit, LHS->Loc.Line, lex.get_diType(type, attribs & A_signed),
					true);
				DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
				                        llvm::DILocation::get(SP->getContext(), LHS->Loc.Line, 0, SP),
				                        Builder->GetInsertBlock());
			}
		} else if (postpone_valgen) {
			entry->val = CreateEntryBlockAlloca(type);
			RHS->codegen_raw(entry->val);
		} else if (ValPtr) {
			if (allocsz) {
				llvm::AllocaInst* Alloca;
				if (LREF) {
					entry->ft.type_attr |= A_ptrref;
					auto align = TheModule->getDataLayout().getPrefTypeAlign(ValPtr->getType());
					Alloca = Builder->CreateAlloca(ValPtr->getType(), nullptr, varname);
					entry->mark_as_referencing(is_referencing);
					if (ValPtr->getType()->isPointerTy()) {
						Builder->CreateAlignedStore(ValPtr, Alloca, align);
						entry->val = Alloca;
					} else {
						if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ValPtr->getType())) {
							unsigned max_el = struct_type->getNumElements() - 1;
							llvm::Value* val = llvm::UndefValue::get(struct_type);
							for (unsigned i=0; i<max_el; i++)
								val = Builder->CreateInsertValue(val, Builder->CreateExtractValue(ValPtr, i), i);
							Builder->CreateAlignedStore(Builder->CreateExtractValue(ValPtr, max_el), Alloca, align);
							val = Builder->CreateInsertValue(
								val, Builder->CreatePointerCast(
									Alloca, struct_type->getElementType(max_el)), max_el);
							entry->val = val;
						} else {
							errs() << LHS->Loc << ": internal error - inconsistent reference initialization for'" << varname << "'\n";
							return nullptr;
						}
					}
				} else {
					auto align = getAlignment(allocsz);
					Alloca = Builder->CreateAlloca(RHS->ft->type, nullptr, varname);
					Builder->CreateMemCpy(Alloca, align, ValPtr, align, allocsz);
					entry->val = Alloca;
				}
			} else {
				auto Alloca = Builder->CreateAlloca(elem_type, AllocSize, varname);
				auto align = TheModule->getDataLayout().getPrefTypeAlign(elem_type);
				llvm::Value* cp_size = Builder->CreateMul(getSize(el_allocsz), AllocSize);
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
		} else if (allocsz > sret_limit || is_constructor_call) {
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
		if ((entry->ft.type_attr & A_constructor) && !is_call_expr) {
			// no explicit constructor call but there is a default constructor
			auto F = getConstructorOrDestructor(&entry->ft);
			if (!F) {
				errs() << Loc << ": internal error - default constructor not found for " << *entry->ft.type << ".\n";
				return nullptr;
			} else {
				Builder->CreateCall(F, { entry->val });
			}
		}
		ft->type = llvm::Type::getVoidTy(Context);
		return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	}
	llvm::Value* result = nullptr;
	bool ResSigned = ft->type_attr & A_signed;
	bool OperandSigned = (LHS->ft->type_attr & A_signed) && !LHS->is_unknown_type || (RHS->ft->type_attr & A_signed) && !RHS->is_unknown_type
		|| (LHS->ft->type_attr & RHS->ft->type_attr & A_signed);
	bool right_is_imag = RHS->ft && RHS->ft->type && (RHS->ft->type->isFloatTy() || RHS->ft->type->isDoubleTy()) && (RHS->ft->type_attr & A_imaginary);
	bool left_is_imag = LHS->ft && LHS->ft->type && (LHS->ft->type->isFloatTy() || LHS->ft->type->isDoubleTy()) && (LHS->ft->type_attr & A_imaginary);
	const char* new_err_msg;
	if (left_is_imag || right_is_imag)
		if (RHS->ft->type->isDoubleTy() || LHS->ft->type->isDoubleTy())
			LHS->desired_type = RHS->desired_type = llvm::Type::getDoubleTy(Context);
		else
			LHS->desired_type = RHS->desired_type = llvm::Type::getFloatTy(Context);
	else
		std::tie(LHS->desired_type, RHS->desired_type, new_err_msg) = getDesiredTypes(
			ft->type, desired_type, LHS->ft->type, RHS->ft->type, opclass, ft->type_attr & A_signed,
			LHS->ft->type_attr & A_signed, RHS->ft->type_attr & A_signed, LHS->is_unknown_type, RHS->is_unknown_type);
	if (!OperandSigned) {
		if (LHS->is_unknown_type)
			LHS->conv_kind = ConvUnsigned;
		if (RHS->is_unknown_type)
			RHS->conv_kind = ConvUnsigned;
	}
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
			// for most operators 'X' we would convert 'float X int' -> 'float X float'
			// for '^' we would keep 'float ^ int' since this can be calculated faster (without log/exp)
			// here we have the exception from that exception: 'float ^ (int/int)' or 'float ^ (int + int/int)'
			// were we do an early float conversion, so '2.0 ^ (1/2)" evaluates to '1.41421356...'
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
		if (R && R->getType()->getTypeID() == llvm::Type::PointerTyID && RHS->ft && (RHS->ft->type_attr & A_string))
			typeclass = is_string;
		else
			if (ft->type_attr & A_complex)
				typeclass = is_complex;
			else
				typeclass = is_int;
		break;
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
	case llvm::Type::FloatTyID:
	case llvm::Type::DoubleTyID:
		typeclass = is_float;
		if (ft->type_attr & A_complex)
			typeclass = is_complex;
		break;
	case llvm::Type::PointerTyID:
		if (LHS->ft->type_attr & A_string) {
			typeclass = is_string;
			break;
		}
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
		case is_string: {
			std::string stradd = "__string_add";
			auto stradd_proto = (*lex.findProtos(stradd))[0].get();
			auto stradd_fn = getFunction(stradd_proto);
			result = Builder->CreateCall(stradd_proto->FT, stradd_fn, std::vector<llvm::Value*>{ L, R });
			if (!target) {
				FullVar tmp = {
					.val = result,
					.ft = {
						.type = llvm_ptr_type,
						.type_attr = A_string | A_rvalue
					}
				};
				expr_temps.push_back(tmp);
			}
			break;
		}
		case is_complex:
			if (ft->type->isIntegerTy()) {
				// MSVC-ABI
				llvm::Value* r_int = Builder->CreateIntCast(
					Builder->CreateBitCast(R, llvm::Type::getInt32Ty(Context)), llvm::Type::getInt64Ty(Context), false);
				llvm::Value* l_int = Builder->CreateIntCast(
					Builder->CreateBitCast(L, llvm::Type::getInt32Ty(Context)), llvm::Type::getInt64Ty(Context), false);
				if (left_is_imag) {
					result = Builder->CreateOr(
						r_int, Builder->CreateShl(l_int, 32));
				} else {
					result = Builder->CreateOr(
						l_int, Builder->CreateShl(r_int, 32));
				}
			} else if (ft->type->isVectorTy()) {
				result = llvm::UndefValue::get(ft->type);
				if (left_is_imag) {
					result = Builder->CreateInsertElement(result, R, (uint64_t)0);
					result = Builder->CreateInsertElement(result, L, (uint64_t)1);
				} else {
					result = Builder->CreateInsertElement(result, L, (uint64_t)0);
					result = Builder->CreateInsertElement(result, R, (uint64_t)1);
				}
			} else {
				result = llvm::UndefValue::get(ft->type);
				if (left_is_imag) {
					result = Builder->CreateInsertValue(result, R, 0);
					result = Builder->CreateInsertValue(result, L, 1);
				} else {
					result = Builder->CreateInsertValue(result, L, 0);
					result = Builder->CreateInsertValue(result, R, 1);
				}
			}
			break;
		default:
			errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			return nullptr;
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
		case is_complex:
			R = Builder->CreateFNeg(R);
			if (ft->type->isIntegerTy()) {
				// MSVC-ABI
				llvm::Value* r_int = Builder->CreateZExtOrBitCast(R, llvm::Type::getInt64Ty(Context));
				llvm::Value* l_int = Builder->CreateZExtOrBitCast(L, llvm::Type::getInt64Ty(Context));
				if (left_is_imag) {
					result = Builder->CreateOr(
						r_int, Builder->CreateShl(l_int, 32));
				} else {
					result = Builder->CreateOr(
						l_int, Builder->CreateShl(r_int, 32));
				}
			} else if (ft->type->isVectorTy()) {
				result = llvm::UndefValue::get(ft->type);
				if (left_is_imag) {
					result = Builder->CreateInsertElement(result, R, (uint64_t)0);
					result = Builder->CreateInsertElement(result, L, (uint64_t)1);
				} else {
					result = Builder->CreateInsertElement(result, L, (uint64_t)0);
					result = Builder->CreateInsertElement(result, R, (uint64_t)1);
				}
			} else {
				result = llvm::UndefValue::get(ft->type);
				if (left_is_imag) {
					result = Builder->CreateInsertValue(result, R, 0);
					result = Builder->CreateInsertValue(result, L, 1);
				} else {
					result = Builder->CreateInsertValue(result, L, 0);
					result = Builder->CreateInsertValue(result, R, 1);
				}
			}
			break;
		default:
			errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			return nullptr;
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
			if (left_is_imag && right_is_imag)
				result = Builder->CreateFNeg(result);
			break;
		case is_string: {
			llvm::Value* theFactor = nullptr;
			llvm::Value* theString = nullptr;
			if (L->getType()->getTypeID() == llvm::Type::IntegerTyID) {
				theFactor = L;
				// being here indicates that at least one operand is a string
				theString = R;
			} else if (R->getType()->getTypeID() == llvm::Type::IntegerTyID) {
				theFactor = R;
				theString = L;
			} else {
				errs() << Loc << "If one side of '" << Op << "' is a string the other has be an integer\n";
				break;
			}
			if (theFactor->getType() != llvm_size_type)
				theFactor = Builder->CreateIntCast(theFactor, llvm_size_type, false);
			std::string strmult = "__string_mult";
			auto strmult_proto = (*lex.findProtos(strmult))[0].get();
			auto strmult_fn = getFunction(strmult_proto);
			result = Builder->CreateCall(strmult_proto->FT, strmult_fn, std::vector<llvm::Value*>{ theFactor, theString });
			if (!target) {
				FullVar tmp = {
					.val = result,
					.ft = {
						.type = llvm_ptr_type,
						.type_attr = A_string | A_rvalue
					}
				};
				expr_temps.push_back(tmp);
			}
		}
			break;
		default:
			errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			return nullptr;
		}
		break;
	case '/':
		switch(typeclass) {
		case is_int:
			if (ResSigned)
				if (idiv_mode == idiv_mode_floored) {
					unsigned bw = llvm::cast<llvm::IntegerType>(L->getType())->getBitWidth();
					llvm::Value* c = Builder->CreateSDiv(L, R, "divtmp");
					llvm::Value* x = Builder->CreateXor(R, L);
					llvm::Value* xx = Builder->CreateAShr(x, bw-1);
					result = Builder->CreateAdd(c, xx);
				} else
					result = Builder->CreateSDiv(L, R, "divtmp");
			else
				result = Builder->CreateUDiv(L, R, "divtmp");
			break;
		case is_float:
			result = Builder->CreateFDiv(L, R, "divtmp");
			if (!left_is_imag && right_is_imag)
				result = Builder->CreateFNeg(result);
			break;
		default:
			errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			return nullptr;
		}
		break;
	case '%':
		switch(typeclass) {
		case is_int:
			if (ResSigned) {
				if (idiv_mode == idiv_mode_floored) {
					// CreateSRem() conforms to C99 which means the sign of the reminder is that of the divisor
					// however in number theory (e.g. congruence classes) the reminder is supposed to have
					// the sign of the divisor. With the some bit fiddling this can be adjusted...
					unsigned bw = llvm::cast<llvm::IntegerType>(L->getType())->getBitWidth();
					llvm::Value* c = Builder->CreateSRem(L, R, "remtmp");
					llvm::Value* x = Builder->CreateXor(R, c);
					llvm::Value* xx = Builder->CreateAShr(x, bw-1);
					llvm::Value* R0 = Builder->CreateAnd(R, xx);
					result = Builder->CreateAdd(c, R0);
				} else
					result = Builder->CreateSRem(L, R, "remtmp");
			} else
				result = Builder->CreateURem(L, R, "remtmp");
			break;
		case is_float: {
			if (idiv_mode == idiv_mode_floored) {
				unsigned bw = L->getType()->isDoubleTy() ? 64 : 32;
				auto inttype = llvm::IntegerType::get(Context, bw);
				llvm::Value* c = Builder->CreateFRem(L, R, "remtmp");
				llvm::Value* x = Builder->CreateXor(Builder->CreateBitCast(R, inttype), Builder->CreateBitCast(c, inttype));
				llvm::Value* xx = Builder->CreateAShr(x, bw-1);
				llvm::Value* R0 = Builder->CreateAnd(Builder->CreateBitCast(R, inttype), xx);
				result = Builder->CreateFAdd(c, Builder->CreateBitCast(R0, L->getType()));
			} else
				result = Builder->CreateFRem(L, R, "remtmp");
		}
			break;
		default:
			errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			return nullptr;
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
				auto TheFunction = enterBB ? enterBB->getParent() : nullptr;
				auto RHSBB = enterBB ? llvm::BasicBlock::Create(Context, "lazy_rhs") : nullptr;
				auto RHSBBstart = RHSBB;
				if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
					TheFunction->insert(TheFunction->end(), RHSBB);
#else
					TheFunction->getBasicBlockList().push_back(RHSBB);
#endif
					Builder->SetInsertPoint(RHSBB);
				}
				R = RHS->codegen();
				if (!R)
					return nullptr;
				// if both sides are constexprs no lazy evaluation is needed and the result
				// can be a constexpr, too
				if (auto constL = llvm::dyn_cast<llvm::Constant>(L))
					if (auto constR = llvm::dyn_cast<llvm::Constant>(R)) {
						if (Op[0] == '&')
							result = Builder->CreateAnd(L, R, "andtmp");
						else
							result = Builder->CreateOr(L, R, "ortmp");
						if (TheFunction) {
							RHSBB = Builder->GetInsertBlock();
							Builder->SetInsertPoint(enterBB);
							Builder->CreateBr(RHSBBstart);
							Builder->SetInsertPoint(RHSBB);
						}
						break;
					}
				if (!TheFunction) {
					errs() << Loc << ": logic constexpr does not evaluate at compile time\n";
					return nullptr;
				}
				auto ContBB = llvm::BasicBlock::Create(Context, "logic_op");
				Builder->CreateBr(ContBB);
				RHSBB = Builder->GetInsertBlock();
				Builder->SetInsertPoint(enterBB);
				if (Op[0] == '&')
					Builder->CreateCondBr(L, RHSBBstart, ContBB);
				else
					Builder->CreateCondBr(L, ContBB, RHSBBstart);
#if LLVM_VERSION_MAJOR >= 16
				TheFunction->insert(TheFunction->end(), ContBB);
#else
				TheFunction->getBasicBlockList().push_back(ContBB);
#endif
				Builder->SetInsertPoint(ContBB);
				auto PN = Builder->CreatePHI(llvm::Type::getInt1Ty(Context), 2, "merged_lazy");
				PN->addIncoming(Op[0] == '&' ? Builder->getFalse() : Builder->getTrue(), enterBB);
				PN->addIncoming(R, RHSBB);
				result = PN;
			}
			break;
		default:
			errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
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
#if LLVM_VERSION_MAJOR < 13
				result = Builder->CreateIntrinsic(llvm::Intrinsic::powi, { L->getType() }, { L, Builder->CreateIntCast(R, llvm::Type::getInt32Ty(Context), RHS->ft->type_attr & A_signed) });
#else
				unsigned exponent_bw = (int_exp_type->getBitWidth() > 16) ? 32 : 16;
				if (int_exp_type->getBitWidth() == exponent_bw)
					result = Builder->CreateIntrinsic(llvm::Intrinsic::powi, { L->getType(), R->getType() }, { L, R });
				else {
					llvm::Type* exp_type = llvm::IntegerType::get(Context, exponent_bw);
					result = Builder->CreateIntrinsic(llvm::Intrinsic::powi, { L->getType(), exp_type }, { L, Builder->CreateIntCast(R, exp_type, RHS->ft->type_attr & A_signed) });
				}
#endif
			}
		} else {
			result = Builder->CreateIntrinsic(llvm::Intrinsic::pow, { L->getType() }, { L, R });
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
			case is_string:
				result = Builder->CreateICmpNE(compare_strings(L, R), Builder->getInt32(0));
				break;
			default:
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
			}
		} else {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateNot(Builder->CreateXor(L, R, "xortmp"), "nxortmp");
				break;
			default:
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
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
		case is_string:
			result = Builder->CreateICmpEQ(compare_strings(L, R), Builder->getInt32(0));
			break;
		default:
			errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
			return nullptr;
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
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
			}
		} else if (Op[1] == '<') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateShl(L, R, "remtmp");
				break;
			default:
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
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
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
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
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
			}
		} else if (Op[1] == '>') {
			switch(typeclass) {
			case is_int:
				if (LHS->ft->type_attr & A_signed)
					result = Builder->CreateAShr(L, R, "remtmp");
				else
					result = Builder->CreateLShr(L, R, "remtmp");
				break;
			default:
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
			}
			break;
		} else if (Op[1] == '<') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateXor(L, R, "remtmp");
				break;
			default:
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
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
				errs() << Loc << ": operator '" << Op << "' cannot be used for type " << *L->getType() << "\n";
				return nullptr;
			}
		}
		break;
	case '.': // range expression 2..5
		if (ft && ft->type) {
			if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
				result = llvm::UndefValue::get(struct_type);
				result = Builder->CreateInsertValue(result, L, 0); // min
				result = Builder->CreateInsertValue(result, R, 1); // max
				break;
			}
		}
		if (!result)
			errs() << Loc << ": unable to evaluate range expression\n";
			return nullptr;
		break;
	default:
		errs() << Loc << ": unexpected operator '" << Op << "' in this context\n";
		return nullptr;
	}
	return handle(target, result);
}

std::pair<llvm::Value*, llvm::Instruction*> BranchExprAST::createCondBranch(llvm::BasicBlock* MergeBB,
	      std::vector<std::unique_ptr<ExprAST>>& Branch, int EndKind, bool isElse) {
	llvm::Value* BranchV = nullptr;
	llvm::Instruction* firstBreak = nullptr; // needed as insertion point to prepare merged vars
	auto for_expr = dynamic_cast<ForExprAST*>(this);
	if (EndKind == tok_return) {
		if (for_expr) {
			errs() << Loc << ": 'return' at end of 'for' loop not allowed - use 'if' instead\n";
			return { nullptr, nullptr };
		}
		if (!theFunction_ret_ft->type->isVoidTy()) {
			if (Branch.empty()) {
				errs() << Loc << ": return value value of type " << *theFunction_ret_ft << " required\n";
				return { nullptr, nullptr };
			}
			Branch.back()->desired_type = theFunction_ret_ft->type;
		}
	}
	if (Branch.empty()) {
		BranchV = llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	} else {
		llvm::Value* ret_target = nullptr;
		unsigned n_exprs = Branch.size();
		for (auto& expr : Branch) {
			if (theFunction_struct_ret && !(--n_exprs))
				// TODO: handle automatic converstions in this case (?)
				BranchV = expr->codegen_raw(ret_ptr);
			else
				BranchV = expr->codegen();
			InsertDestructors(expr_temps);
		}
	}
	if (EndKind != tok_return && !Branch.empty() && Branch.back()->desired_type)
		Branch.back()->ft->type = Branch.back()->desired_type;
	if (for_expr && !isElse && EndKind != tok_return) {
		llvm::Value* cond = for_expr->CreateCondition(true);
		llvm::BasicBlock* IterateBB = llvm::BasicBlock::Create(Context, "Iterate");
		firstBreak = Builder->CreateCondBr(cond, IterateBB, MergeBB);
		if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
			TheFunction->insert(TheFunction->end(), IterateBB);
#else
			TheFunction->getBasicBlockList().push_back(IterateBB);
#endif
			Builder->SetInsertPoint(IterateBB);
		}
		for_expr->Iterate();
		Builder->CreateBr(StackRestoreBB0);
	}
	if (!BranchV && !isElse && !for_expr) {
		return { nullptr, nullptr };
	}
	if (EndKind == tok_return) {
		HandleReturn(Branch, BranchV);
		BranchV = llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	} else {
		if (ft->type->isVoidTy())
			BranchV = llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
		else if (!BranchV)
			BranchV = llvm::Constant::getNullValue(ft->type);
		if (MergeBB && !(for_expr && !isElse))
			firstBreak = Builder->CreateBr(MergeBB);
	}
	return { BranchV, firstBreak };
}

std::pair<llvm::Type*, llvm::Value*> merge_values(
	llvm::Type* typA, llvm::Value* valA, llvm::BasicBlock* caseA, llvm::Instruction* lastA, 
	llvm::Type* typB, llvm::Value* valB, llvm::BasicBlock* caseB, llvm::Instruction* lastB,
	llvm::Instruction* firstWhile, llvm::BasicBlock* enterBB, SourceLocation locA,
	SourceLocation locB, const char* unmangled_name = nullptr) {
	auto MergeBB = Builder->GetInsertBlock();
	if (!valA || !typA) {
		if (!valB || !typB) {
			goto uncompatible_types;
		}
		return { typB, valB };
	} else if (!valB || !typB) {
		return { typA, valA };
	}
	if (typA == typB) {
		if (valA->getType() != valB->getType()) {
			goto uncompatible_types;
		}
		if (valA == valB)
			return { typA, valA };
		llvm::PHINode* PN = Builder->CreatePHI(valA->getType(), 2, "iftmp");
		llvm::PHINode* PNW;
		if (firstWhile) {
			Builder->SetInsertPoint(firstWhile);
			PNW = Builder->CreatePHI(valA->getType(), 2, "ifw");
			PNW->addIncoming(llvm::Constant::getNullValue(valA->getType()), enterBB);
			PNW->addIncoming(valA, lastA->getParent());
			Builder->SetInsertPoint(MergeBB);
		}
		PN->addIncoming(firstWhile ? PNW : valA, caseA);
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
			goto uncompatible_types;
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
						dimA = getSize(nA);
					else
						if (iA < n_vardims_A)
							dimA = Builder->CreateExtractValue(valA, iA++);
						else
							goto uncompatible_types;
					varDimsA.push_back(dimA);
					llvm::Value* dimB;
					Builder->SetInsertPoint(lastB);
					if (nB)
						dimB = getSize(nB);
					else
						if (iB < n_vardims_B)
							dimB = Builder->CreateExtractValue(valB, iB++);
						else
							goto uncompatible_types;
					varDimsB.push_back(dimB);
				}
				typA = elem_tA;
				typB = elem_tB;
			} else {
				goto uncompatible_types;
			}
			array_tB = llvm::dyn_cast<llvm::ArrayType>(typB);
		} while ((array_tA = llvm::dyn_cast<llvm::ArrayType>(typA)));
		if (typA != typB)
			goto uncompatible_types;
		llvm::Type* ptr_t = varDimsA.size() ? Aptr->getType() : typA->getPointerTo();
		Builder->SetInsertPoint(lastA);
		Aptr = Builder->CreatePointerCast(Aptr, ptr_t);
		Builder->SetInsertPoint(lastB);
		Bptr = Builder->CreatePointerCast(Bptr, ptr_t);
		llvm::Type* resultT = typA;
		std::vector<llvm::Type*> struct_type_el(varDimsA.size() + 1, llvm_size_type);
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
		if (structIdx != varDimsA.size() || structIdx != varDimsB.size())
			goto uncompatible_types;
		Builder->SetInsertPoint(lastB);
		llvm::Value* the_structB = llvm::UndefValue::get(struct_type);
		the_structB = Builder->CreateInsertValue(the_structB, Bptr, varDimsB.size());
		for (structIdx = 0; structIdx < varDimsB.size(); structIdx++) {
			the_structB = Builder->CreateInsertValue(the_structB, varDimsB[structIdx], structIdx);
		}
		Builder->SetInsertPoint(MergeBB);
		llvm::PHINode* PN = Builder->CreatePHI(struct_type, 2, "ifdimtmp");
		llvm::PHINode* PNW;
		if (firstWhile) {
			Builder->SetInsertPoint(firstWhile);
			PNW = Builder->CreatePHI(struct_type, 2, "ifw");
			PNW->addIncoming(llvm::Constant::getNullValue(struct_type), enterBB);
			PNW->addIncoming(the_structA, lastA->getParent());
			Builder->SetInsertPoint(MergeBB);
		}
		PN->addIncoming(firstWhile ? PNW : the_structA, caseA);
		PN->addIncoming(the_structB, caseB);
		return { resultT, PN };
	}
uncompatible_types:
	if (unmangled_name)
		errs() << "declaration types for variable '" << unmangled_name << "'";
	else
		errs() << "types of final value";
	errs() << " in conditional branches do not match:\n"
	       << locA << ": " << *typA << " here vs.\n"
	       << locB << ": " << *typB << " there\n";
	return { nullptr, nullptr };
}

bool ForExprAST::PrepareIterator() {
	// integer iterator - initialize with 0
	if (!ValueFV) {
		errs() << Loc << ": internal error - variable '" << ValueName << "' not found\n";
		return false;
	}
	if (Value->ft && Value->ft->type)
		Iterator->desired_type = Value->ft->type;
	if (auto lval = dynamic_cast<LvalueExprAST*>(Iterator.get())) {
		std::tie(iterator_type, iterator_ref) = lval->codegen_ref(true);
		if (!iterator_type)
			return false;
	}
	if (!iterator_ref)
		iterator = Iterator->codegen();
	if (!iterator_ref && !iterator)
		return false;
	if (!iterator_type)
		iterator_type = iterator->getType();
	llvm::Value* initializer = nullptr;
	llvm::Value* Ptr = nullptr;
	if (iterator_type->isPointerTy()) {
		if (Iterator->ft->type_attr & A_map) {
			if (!iterator)
				iterator = Builder->CreateLoad(llvm_ptr_type, iterator_ref);
			ptr_storage = CreateEntryBlockAlloca(llvm_ptr_type);
			std::string map_min_name = "_ZN6volvox3map3MinEPNS0_4NodeE";
			std::string map_max_name = "_ZN6volvox3map3MaxEPNS0_4NodeE";
			auto min_proto = (*lex.findProtos(map_min_name))[0].get();
			auto max_proto = (*lex.findProtos(map_max_name))[0].get();
			auto min_fn = getFunction(min_proto);
			auto max_fn = getFunction(max_proto);
			initializer = Builder->CreateCall(min_proto->FT, min_fn, { iterator });
			limit = Builder->CreateCall(max_proto->FT, max_fn, { iterator });
			if (descending) {
				llvm::Value* tmp = initializer;
				initializer = limit;
				limit = tmp;
			}
		} else {
			errs() << Iterator->Loc << ": unsupported iterator type " << *Iterator->ft << "\n";
			return false;
		}
	} else if (iterator_type->isSingleValueType()) {
		if (iterator_ref)
			iterator = Builder->CreateLoad(iterator_type, iterator_ref);
		limit = iterator;
		// The following is somewhat special: Volvox 'for' compares the integer value *before*
		// incrementing it. So the limit must be the greatest *valid* value. If only one
		// integer 'n' is given we need 'limit = n-1'
		if (iterator_type->isIntegerTy()) {
			Step = llvm::ConstantInt::get(limit->getType(), 1, true);
			limit = Builder->CreateSub(limit, Step);
			initializer = llvm::Constant::getNullValue(limit->getType());
		} else if (iterator_type->isFloatingPointTy()) {
			Step = llvm::ConstantFP::get(limit->getType(), 1.0);
			limit = Builder->CreateFSub(limit, Step);
			initializer = llvm::Constant::getNullValue(limit->getType());
		} else {
			errs() << Iterator->Loc << ": unsupported iterator type " << *Iterator->ft << "\n";
			return false;
		}
		if (descending) {
			llvm::Value* tmp = initializer;
			initializer = limit;
			limit = tmp;
		}
	} else if (iterator_type->isStructTy()) {
		// to get polymorphism here we only require that the object has field
		// elements or methods called 'min' and 'max' that return the same single value type
		// to achive this we construct SelectExprASTs
		std::unique_ptr<ExprAST> receiver1;
		std::unique_ptr<ExprAST> receiver2;
		if (iterator_ref) {
			receiver1 = std::make_unique<ConstLvalueAST>(Iterator->Loc, Iterator->ft, iterator_type, iterator_ref);
			receiver2 = std::make_unique<ConstLvalueAST>(Iterator->Loc, Iterator->ft, iterator_type, iterator_ref);
		} else {
			receiver1 = std::make_unique<ConstExprAST>(Iterator->Loc, Iterator->ft, iterator);
			receiver2 = std::make_unique<ConstExprAST>(Iterator->Loc, Iterator->ft, iterator);
		}
		auto selector = std::make_unique<IdentExprAST>(Iterator->Loc, "min");
		auto min_expr = getSelect(Iterator->Loc, std::move(receiver1), std::move(selector));
		if (auto method = dynamic_cast<MethodExprAST*>(min_expr.get()))
			min_expr = std::make_unique<CallExprAST>(Iterator->Loc, std::move(min_expr));
		// we have to recreate 'receiver' because it has been moved
		selector = std::make_unique<IdentExprAST>(Iterator->Loc, "max");
		auto max_expr = getSelect(Iterator->Loc, std::move(receiver2), std::move(selector));
		if (auto method = dynamic_cast<MethodExprAST*>(max_expr.get()))
			max_expr = std::make_unique<CallExprAST>(Iterator->Loc, std::move(max_expr));
		if (!min_expr || !min_expr->ft || !max_expr || !max_expr->ft) {
			errs() << Iterator->Loc << ": could not find min/max fields of iterator\n";
			return false;
		}
		initializer = min_expr->codegen();
		limit = max_expr->codegen();
		if (!initializer || !limit) {
			errs() << Iterator->Loc << ": could not create code for 'for' range limit\n";
			return false;
		}
		bool signedness_mismatch = (bool)((min_expr->ft->type_attr ^ max_expr->ft->type_attr) & A_signed);
		if (initializer->getType() != limit->getType() || signedness_mismatch) {
			errs() << Iterator->Loc << ": types of 'min' (" << *initializer->getType();
			if (signedness_mismatch)
				errs() << " - " << (min_expr->ft->type_attr & A_signed ? "" : "un") << "signed";
			errs() << ") and 'max' (" << *limit->getType();
			if (signedness_mismatch)
				errs() << " - " << (max_expr->ft->type_attr & A_signed ? "" : "un") << "signed";
			errs() << ") do not match\n";
			return false;
		}
		if (descending) {
			llvm::Value* tmp = initializer;
			initializer = limit;
			limit = tmp;
		}
	} else if (iterator_type->isArrayTy()) {
		auto [ElType0, Ptr0, Dims] = getArrayDims(iterator_ref ? iterator_ref : iterator, iterator_type);
		ElType = ElType0;
		Step = llvm::ConstantInt::get(
			llvm_size_type, TheModule->getDataLayout().getTypeAllocSize(ElType));
		// for multi dimentsional array Step should be the storage size of the sub-tensor
		if (Dims.empty()) {
			errs() << Iterator->Loc << ": internal error - tensor without dimensions\n";
			abort();
		}
		unsigned subDims = Dims.size() - 1;
		for (unsigned i=1; i<=subDims; i++)
			Step = Builder->CreateMul(Step, Dims[i]);
		if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(Ptr0->getType())) {
			// const size rvalue array - iterate over index
			Ptr = CreateEntryBlockAlloca(Ptr0->getType(), "");
			Builder->CreateStore(Ptr0, Ptr);
		} else {
			Ptr = Ptr0;
			// lvalue array iterator - iterate over element addresses
			// The control variable 'ptr_storage' is an independent copy of the sub tensor
		}
		limit = Builder->CreateAdd(
			Builder->CreatePtrToInt(Ptr, llvm_size_type),
			Builder->CreateMul(Step, Builder->CreateSub(
				                   Dims[0], llvm::ConstantInt::get(llvm_size_type, 1, true))));
		if (descending) {
			llvm::Value* tmp = Ptr;
			Ptr = Builder->CreateIntToPtr(limit, llvm_ptr_type);
			limit = Builder->CreatePtrToInt(tmp, llvm_size_type);
		}
	}
	switch (new_Value) {
	case new_var_none:
		errs() << Loc << ": cannot initialize 'for' control variable '" << ValueName << "'\n";
		return false;
	case new_var_created:
		// we have to create an allocation for the new variable
		if (iterator_type->isArrayTy()) {
			if (!llvm::isa<llvm::PointerType>(Ptr->getType())) {
				errs() << Loc << ": internal error - array pointer " << *Ptr << " (no pointer)\n";
				return false;
			}
			ptr_storage = CreateEntryBlockAlloca(llvm_ptr_type);
			Builder->CreateStore(Ptr, ptr_storage);
			if (ValueFV->ft.type_attr & A_ptrref) {
				ValueFV->val = ptr_storage;
			} else {
				auto align = TheModule->getDataLayout().getPrefTypeAlign(ElType);
				ValueRef = ValueFV->val = CreateAlloca(Step, align);
				Builder->CreateMemCpy(ValueRef, align, Builder->CreateIntToPtr(Ptr, llvm_ptr_type), align, Step);
			}
		} else if (iterator_type->isPointerTy()) {
			if (Iterator->ft->type_attr & A_map) {
				ptr_storage = CreateEntryBlockAlloca(llvm_ptr_type);
				Builder->CreateStore(initializer, ptr_storage);
				ValueRef = ValueFV->val = CreateEntryBlockAlloca(ValueFV->ft.type);
			}
		} else {
			ValueRef = ValueFV->val = StoreValue(initializer, &ValueFV->ft);
			ValueType = ValueFV->ft.type;
		}
		break;
	case existing_var_returned:
	case generic_lvalue_returned:
		ValueLval = dynamic_cast<LvalueExprAST*>(Value.get());
		llvm::Type* dummy;
		std::tie(dummy, ValueRef) = ValueLval->codegen_ref();
		if (!ValueRef)
			return false;
		ValueType = Value->ft->type;
		if (iterator_type->isArrayTy()) {
			if (ValueFV->ft.type_attr & A_ptrref) {
				ptr_storage = ValueFV->val;
			} else {
				ptr_storage = CreateEntryBlockAlloca(llvm_ptr_type);
				auto align = TheModule->getDataLayout().getPrefTypeAlign(ElType);
				if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ValueRef->getType()))
					ValueRef = Builder->CreateExtractValue(ValueRef, struct_type->getNumElements() - 1);
				Builder->CreateMemCpy(ValueRef, align, Builder->CreateIntToPtr(Ptr, llvm_ptr_type), align, Step);
			}
			Builder->CreateStore(Ptr, ptr_storage);
		} else {
			if (initializer->getType() != ValueType)
				initializer = Builder->CreateIntCast(initializer, ValueType, Value->ft->type_attr & A_signed);
			Builder->CreateStore(initializer, ValueRef);
		}
		break;
	}
	if (limit->getType()->isIntegerTy()) {
		if (!Step)
			Step = llvm::ConstantInt::get(limit->getType(), 1, true);
	} else if (limit->getType()->isFloatingPointTy()) {
		if (!Step)
			Step = llvm::ConstantFP::get(limit->getType(), 1.0);
		// floats accumulate rounding errors so the precise upper limit might not be hit
		// we define a target intervall [limit-0.5*Step, limit+0.5*Step)
		llvm::Value* step_half = Builder->CreateFMul(Step, llvm::ConstantFP::get(limit->getType(), .5));
		// lower boundary:
		approx_limit = descending ?
			Builder->CreateFAdd(limit, step_half) :
			Builder->CreateFSub(limit, step_half);
	}
	switch (new_Key) {
	case new_var_none:
		break;
	case new_var_created:
		// we have to create an allocation for the new variable
		KeyType = KeyFV->ft.type;
		KeyRef = KeyFV->val = CreateEntryBlockAlloca(KeyType);
		break;
	case existing_var_returned:
	case generic_lvalue_returned:
		auto KeyLval = dynamic_cast<LvalueExprAST*>(Value.get());
		std::tie(KeyType, KeyRef) = KeyLval->codegen_ref();
		if (!KeyRef)
			return false;
		// Builder->CreateStore(key_initializer, KeyRef);
		break;
	}
	return true;
}

llvm::Value* ForExprAST::CreateCondition(bool at_end) {
	llvm::Value* ctrl_var = ptr_storage ?
		Builder->CreateLoad(llvm_size_type, ptr_storage) :
		Builder->CreateLoad(ValueType, ValueRef);
	if (llvm::isa<llvm::PointerType>(limit->getType())) // map iteration
		return Builder->CreateICmpNE(
			ctrl_var,
			Builder->CreatePtrToInt(limit, llvm_size_type));
	if (descending)
		if (ctrl_var->getType()->isIntegerTy())
			if (ValueFT->type_attr & A_signed)
				if (at_end)
					return Builder->CreateICmpSGT(ctrl_var, limit, "for_cond");
				else
					return Builder->CreateICmpSGE(ctrl_var, limit, "for_cond");
			else
				if (at_end)
					return Builder->CreateICmpUGT(ctrl_var, limit, "for_cond");
				else
					return Builder->CreateICmpUGE(ctrl_var, limit, "for_cond");
		else
			if (at_end)
				return Builder->CreateFCmpOGT(ctrl_var, approx_limit, "for_cond");
			else
				// for the start we check the precise limit but allow equality
				// so 'for x in 2.3..2.3' will have one iteration,
				// but 'for x in 2.3..2.29999' will only run the 'else' branch if present
				return Builder->CreateFCmpOGE(ctrl_var, limit, "for_cond");
	else
		if (ctrl_var->getType()->isIntegerTy())
			if (ValueFT->type_attr & A_signed)
				if (at_end)
					return Builder->CreateICmpSLT(ctrl_var, limit, "for_cond");
				else
					return Builder->CreateICmpSLE(ctrl_var, limit, "for_cond");
			else
				if (at_end)
					return Builder->CreateICmpULT(ctrl_var, limit, "for_cond");
				else
					return Builder->CreateICmpULE(ctrl_var, limit, "for_cond");
		else
			if (at_end)
				return Builder->CreateFCmpOLT(ctrl_var, approx_limit, "for_cond");
			else
				// for the start we check the precise limit but allow equality
				// so 'for x in 2.3..2.3' will have one iteration,
				// but 'for x in 2.3..2.29999' will only run the 'else' branch if present
				return Builder->CreateFCmpOLE(ctrl_var, limit, "for_cond");
}

bool ForExprAST::SetupLoop() {
	if (iterator_type->isPointerTy()) {
		if (Iterator->ft->type_attr & A_map) {
			llvm::Value* node_ptr = Builder->CreateLoad(llvm_ptr_type, ptr_storage);
			llvm::Value* key_ptr = Builder->CreateIntToPtr(
				Builder->CreateAdd(
					Builder->CreatePtrToInt(node_ptr, llvm_size_type),
					llvm::ConstantInt::get(llvm_size_type, offsetof(MapNode, key))),
				llvm_ptr_type);
			llvm::Value* value_ptr;
			if (ValueFT->type_attr & A_string) {
				llvm::Value* offset_ptr = Builder->CreateIntToPtr(
					Builder->CreateAdd(
						Builder->CreatePtrToInt(node_ptr, llvm_size_type),
						llvm::ConstantInt::get(llvm_size_type, offsetof(MapNode, value.offset))),
					llvm_ptr_type, "mapnode_offset");
				llvm::Value* Offset = Builder->CreateLoad(llvm_int_type, offset_ptr);
				value_ptr = Builder->CreateIntToPtr(
					Builder->CreateAdd(
						Builder->CreatePtrToInt(node_ptr, llvm_size_type),
						Builder->CreateIntCast(Offset, llvm_size_type, false)),
					llvm_ptr_type, "val_ptr");
			} else {
				value_ptr = Builder->CreateIntToPtr(
					Builder->CreateAdd(
						Builder->CreatePtrToInt(node_ptr, llvm_size_type),
						llvm::ConstantInt::get(llvm_size_type, offsetof(MapNode, value))),
					llvm_ptr_type, "val_ptr");
			}
			if (ValueRef) {
				llvm::Value* val = Builder->CreateLoad(ValueFT->type, value_ptr);
				Builder->CreateStore(val, ValueRef);
			} else
				errs() << "No ValueRef\n";
		}
	}
	return true;
}

bool ForExprAST::Iterate() {
	llvm::Value* ctrl_var = ptr_storage ?
		Builder->CreateLoad(llvm_size_type, ptr_storage) :
		Builder->CreateLoad(ValueType, ValueRef);
	if (llvm::isa<llvm::PointerType>(limit->getType())) { // map iteration
		std::string iterate_fn_name = descending ?
			"_ZN6volvox3map9iter_downEPNS0_4NodeE" :
			"_ZN6volvox3map7iter_upEPNS0_4NodeE";
		auto iterate_proto = (*lex.findProtos(iterate_fn_name))[0].get();
		auto iterate_fn = getFunction(iterate_proto);
		ctrl_var = Builder->CreateCall(
			iterate_proto->FT, iterate_fn,
			{ Builder->CreateIntToPtr(ctrl_var, llvm_ptr_type) });
		Builder->CreateStore(ctrl_var, ptr_storage);
		return true;
	}
	if (ctrl_var->getType()->isIntegerTy())
		if (descending)
			ctrl_var = Builder->CreateSub(ctrl_var, Step);
		else
			ctrl_var = Builder->CreateAdd(ctrl_var, Step);
	else
		if (descending)
			ctrl_var = Builder->CreateFSub(ctrl_var, Step);
		else
			ctrl_var = Builder->CreateFAdd(ctrl_var, Step);
	if (ptr_storage) {
		Builder->CreateStore(ctrl_var, ptr_storage);
		if (!(ValueFV->ft.type_attr & A_ptrref)) {
			auto align = TheModule->getDataLayout().getPrefTypeAlign(ElType);
			Builder->CreateMemCpy(ValueRef, align, Builder->CreateIntToPtr(ctrl_var, llvm_ptr_type), align, Step);
		}
		Builder->CreateStore(ctrl_var, ptr_storage);
	} else {
		Builder->CreateStore(ctrl_var, ValueRef);
	}
	return true;
}

llvm::Value* BranchExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	auto if_expr = dynamic_cast<IfExprAST*>(this);
	auto for_expr = dynamic_cast<ForExprAST*>(this);
	auto enterBB = Builder->GetInsertBlock();
	TheFunction = enterBB ? enterBB->getParent() : nullptr;
	llvm::PHINode* condPN;
	llvm::PHINode* savedStack;
	llvm::BasicBlock* CondBB = nullptr;
	llvm::BasicBlock* CondBBstart = nullptr;
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
	case tok_for:
		loopBBName = "for";
		contName = "forcont";
		break;
	default:
		loopBBName = "loop";
		contName = "whilecont";
	}
	llvm::BasicBlock* ThenBB = (TheFunction && if_kind != tok_if) ? llvm::BasicBlock::Create(Context, loopBBName) : nullptr;
	llvm::Instruction* firstWhile = nullptr;
	if (if_kind == tok_while || if_kind == tok_for) {
		if (if_kind == tok_while)
			CondBB = CondBBstart = llvm::BasicBlock::Create(Context, "whilecond");
		else {
			if (!for_expr->PrepareIterator())
				return nullptr;
			// CondBB = CondBBstart = llvm::BasicBlock::Create(Context, "forcond");
		}
		if (!for_expr) {
			Builder->CreateBr(CondBBstart);
			if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
				TheFunction->insert(TheFunction->end(), CondBB);
#else
				TheFunction->getBasicBlockList().push_back(CondBB);
#endif
			}
			Builder->SetInsertPoint(CondBB);
			condPN = Builder->CreatePHI(llvm::Type::getInt8Ty(Context), 2, "mustsavestack");
			savedStack = Builder->CreatePHI(llvm_ptr_type, 2, "savedstack");
			condPN->addIncoming(Builder->getInt8(2), enterBB);
			savedStack->addIncoming(llvm::ConstantPointerNull::get(llvm_ptr_type), enterBB);
		}
	} else if (if_kind == tok_repeat) {
		CondBB = CondBBstart = llvm::BasicBlock::Create(Context, "until"); // will be filled at end
		condPN = nullptr;
	} else {
		CondBB = nullptr;
		condPN = nullptr;
	}
	ExprAST* Cond;
	if (if_expr) {
		Cond = if_expr->Cond.get();
		Cond->desired_type = llvm::Type::getInt1Ty(Context);
	} else {
		Cond = nullptr;
	}
	// Create blocks for the then and else cases.  Insert the 'then' block at the
	// end of the function.
	llvm::BasicBlock* ElseBB = (if_kind == tok_repeat || if_kind == tok_if || !TheFunction) ? nullptr : llvm::BasicBlock::Create(Context, "else");
	llvm::BasicBlock* MergeBB = (always_return || !TheFunction) ? nullptr : llvm::BasicBlock::Create(Context, contName);
	llvm::BasicBlock* StackSaveBB = (if_kind == tok_if || !TheFunction) ? nullptr : llvm::BasicBlock::Create(Context, "stacksave");
	llvm::BasicBlock* StackRestoreBB = StackRestoreBB0 = (if_kind == tok_if || !TheFunction) ? nullptr : llvm::BasicBlock::Create(Context, "stackrestore");
	llvm::BasicBlock* EntryBBend;
	llvm::BasicBlock* ThenBBstart;
	llvm::BasicBlock* ElseBBstart;
	llvm::Value* CondV = nullptr;
	CTcond_t CTcond = CTcond_undef;
	if (if_kind == tok_repeat) {
		// 1st iteration: save stack
		Builder->CreateBr(StackSaveBB);
	} else {
		if (for_expr)
			CondV = for_expr->CreateCondition();
		else
			CondV = Cond->codegen();
		if (!CondV)
			return nullptr;
		if (CondV->getType() != llvm::Type::getInt1Ty(Context)) {
			errs() << Cond->Loc << ": bool type expected as 'if'/'while' condition, not " << *CondV->getType() << "\n";
			return nullptr;
		}
		if (if_kind == tok_while) {
			firstWhile = CondBB->getFirstNonPHI();
			CondBB = Builder->GetInsertBlock();
		} else if (if_kind == tok_if) {
			if (auto static_cond = llvm::dyn_cast<llvm::ConstantInt>(CondV))
				CTcond = (CTcond_t)(static_cond->getZExtValue());
			if (CTcond != CTcond_false && TheFunction)
				ThenBB = llvm::BasicBlock::Create(Context, "then");
			if (CTcond != CTcond_true && TheFunction)
				ElseBB = llvm::BasicBlock::Create(Context, "else");
		}
		if (!Else.empty() && !Then.empty() && ft->type && !ft->type->isVoidTy()) {
			const char* new_err_msg = nullptr;
			std::tie(Then.back()->desired_type, Else.back()->desired_type, new_err_msg) = getDesiredTypes(
				ft->type, desired_type, Then.back()->ft->type, Else.back()->ft->type, OpNormal, ft->type_attr & A_signed,
				Then.back()->ft->type_attr & A_signed, Else.back()->ft->type_attr & A_signed,
				Then.back()->is_unknown_type, Else.back()->is_unknown_type);
			if (new_err_msg) {
				errs() << Loc << new_err_msg << '\n';
				return nullptr;
			}
		}
		if (if_kind == tok_if || if_kind == tok_for) {
			EntryBBend = Builder->GetInsertBlock();
			ThenBBstart = ThenBB;
			ElseBBstart = ElseBB;
		} else {
			// save stack at 1st run and restore at following runs
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
	llvm::Value* savedStack0;
	if (if_kind != tok_if) {
		if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
			TheFunction->insert(TheFunction->end(), StackSaveBB);
#else
			TheFunction->getBasicBlockList().push_back(StackSaveBB);
#endif
			Builder->SetInsertPoint(StackSaveBB);
		}
#if LLVM_VERSION_MAJOR >= 18
		savedStack0 = Builder->CreateStackSave("savedstack");
#else
		savedStack0 = Builder->CreateIntrinsic(llvm::Intrinsic::stacksave, {}, {}, nullptr, "savedstack");
#endif
		Builder->CreateBr(ThenBB);
		StackSaveBB = Builder->GetInsertBlock();
		if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
			TheFunction->insert(TheFunction->end(), StackRestoreBB);
#else
			TheFunction->getBasicBlockList().push_back(StackRestoreBB);
#endif
			Builder->SetInsertPoint(StackRestoreBB);
		}
		if (if_kind == tok_repeat)
			savedStack = Builder->CreatePHI(llvm_ptr_type, 1, "savedstack");
		if (if_kind == tok_for) {
#if LLVM_VERSION_MAJOR >= 18
			StackRestoreInst = Builder->CreateStackRestore(savedStack0);
#else
			StackRestoreInst = Builder->CreateIntrinsic(llvm::Intrinsic::stackrestore, {}, savedStack0);
#endif
		} else {
#if LLVM_VERSION_MAJOR >= 18
			StackRestoreInst = Builder->CreateStackRestore(savedStack);
#else
			StackRestoreInst = Builder->CreateIntrinsic(llvm::Intrinsic::stackrestore, {}, savedStack);
#endif
		}
		Builder->CreateBr(ThenBB);
		StackRestoreBB = Builder->GetInsertBlock();
	}
	llvm::Constant* thenConstV = nullptr;
	llvm::Value* ThenV = nullptr;
	llvm::Instruction* thenLast = nullptr;
	llvm::PHINode* savedStack1;
	if (CTcond != CTcond_false) {
		if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
			TheFunction->insert(TheFunction->end(), ThenBB);
#else
			TheFunction->getBasicBlockList().push_back(ThenBB);
#endif
			Builder->SetInsertPoint(ThenBB);
		}
		if (if_kind == tok_while || if_kind == tok_repeat) {
			savedStack1 = Builder->CreatePHI(llvm_ptr_type, 2, "savedstack1");
			savedStack1->addIncoming(savedStack0, StackSaveBB);
			savedStack1->addIncoming(savedStack, StackRestoreBB);
		}
		// Emit then value.
		locals_table.push_back(std::move(then_locals_table));
		condnesting++;
		llvm::BasicBlock* BlockToJump;
		if (if_kind == tok_for) {
			for_expr->SetupLoop();
			BlockToJump = MergeBB;
		}
		else if(CondBBstart)
			BlockToJump = CondBBstart;
		else
			BlockToJump = MergeBB;
		std::tie(ThenV, thenLast) = createCondBranch(BlockToJump, Then, ThenEndKind, false);
		if (Then.size() == 1)
			thenConstV = llvm::dyn_cast<llvm::Constant>(ThenV);
		condnesting--;
		then_locals_table = std::move(locals_table.back());
		locals_table.pop_back();
		if (!ThenV) {
			errs() << Loc << ": conditional expression - then/for/while/repeat block did not compile\n";
			return nullptr;
		}
		// Codegen of 'Then' can change the current block, update ThenBB for the PHI.
		ThenBB = Builder->GetInsertBlock();
		if (if_kind == tok_while) {
			condPN->addIncoming(Builder->getInt8(0), ThenBB);
			savedStack->addIncoming(savedStack1, ThenBB);
		}
	}
	llvm::Value* ElseV = nullptr;
	llvm::Instruction* elseLast = nullptr;;
	llvm::Constant* elseConstV = nullptr;
	if (if_kind == tok_repeat) {
		if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
			TheFunction->insert(TheFunction->end(), CondBB);
#else
			TheFunction->getBasicBlockList().push_back(CondBB);
#endif
		}
		Builder->SetInsertPoint(CondBB);
		if (then_locals_table.table) {
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
		}
		llvm::Value *CondV = Cond->codegen();
		if (!CondV)
			return nullptr;
		if (CondV->getType() != llvm::Type::getInt1Ty(Context)) {
			errs() << Cond->Loc << ": bool type expected as 'until' condition\n";
			return nullptr;
		}
		Builder->CreateCondBr(CondV, MergeBB, StackRestoreBB);
		CondBB = Builder->GetInsertBlock();
		savedStack->addIncoming(savedStack1, CondBB);
	} else {
		if (CTcond != CTcond_true) {
			// Emit else block.
			if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
				TheFunction->insert(TheFunction->end(), ElseBB);
#else
				TheFunction->getBasicBlockList().push_back(ElseBB);
#endif
			}
			Builder->SetInsertPoint(ElseBB);
			locals_table.push_back(std::move(else_locals_table));
			condnesting++;
			VarTable* old_IfWhileVarTable = IfWhileVarTable;
			if (CTcond == CTcond_undef)
				IfWhileVarTable = &then_locals_table;
			std::tie(ElseV, elseLast) = createCondBranch(MergeBB, Else, ElseEndKind, true);
			if (Else.size() == 1)
				elseConstV = llvm::dyn_cast<llvm::Constant>(ElseV);
			if (CTcond == CTcond_undef)
				IfWhileVarTable = old_IfWhileVarTable;
			condnesting--;
			else_locals_table = std::move(locals_table.back());
			locals_table.pop_back();
			if (!ElseV) {
				errs() << Loc << ": if expression - 'else' block did not compile\n";
				return nullptr;
			}
			// Codegen of 'Else' can change the current block, update ElseBB for the PHI.
			ElseBB = Builder->GetInsertBlock();
		}
	}
	if (if_kind == tok_if || if_kind == tok_for) {
		Builder->SetInsertPoint(EntryBBend);
		if (CTcond != CTcond_undef && if_kind != tok_for) { // at least one branch can be removed
			if (thenConstV && ThenEndKind == tok_else && !ft->type->isVoidTy()) {
				if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
					TheFunction->insert(TheFunction->end(), MergeBB);
#else
					TheFunction->getBasicBlockList().push_back(MergeBB);
#endif
					if (ThenBBstart)
						Builder->CreateBr(ThenBBstart);
					Builder->SetInsertPoint(MergeBB);
				}
				return thenConstV;
			} else if (elseConstV && ElseEndKind == tok_end && !ft->type->isVoidTy()) {
				if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
					TheFunction->insert(TheFunction->end(), MergeBB);
#else
					TheFunction->getBasicBlockList().push_back(MergeBB);
#endif
					if (ElseBBstart)
						Builder->CreateBr(ElseBBstart);
					Builder->SetInsertPoint(MergeBB);
				}
				return elseConstV;
			} else if (CTcond == CTcond_true) {
				if (ThenBBstart)
					Builder->CreateBr(ThenBBstart);
			} else {
				if (ElseBBstart)
					Builder->CreateBr(ElseBBstart);
			}
		} else {
			if (!ThenBBstart || !ElseBBstart) {
				errs() << Loc << ": inconsistency (constexpr expected but expr not const?)\n";
				return nullptr;
			}
			Builder->CreateCondBr(CondV, if_kind == tok_for ? StackSaveBB : ThenBBstart, ElseBBstart);
		}
	}
	if (always_return)
		return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	if (TheFunction) {
#if LLVM_VERSION_MAJOR >= 16
		TheFunction->insert(TheFunction->end(), MergeBB);
#else
		TheFunction->getBasicBlockList().push_back(MergeBB);
#endif
		Builder->SetInsertPoint(MergeBB);
	}
	// Emit merge block.
	if (if_kind != tok_repeat && then_locals_table.table && else_locals_table.table && (thenLast || CTcond == CTcond_false) && (elseLast || CTcond == CTcond_true)) {
		for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
			FullVar* else_var = else_locals_table[then_node.getKey()];
			MapValue* node = then_node.getValue();
			auto then_var = (FullVar*)((char*)node + node->offset);
			if (else_var) {
				auto merge = merge_values(then_var->ft.type, then_var->val, (if_kind == tok_while) ?
				                          CondBB : (if_kind == tok_for) ? thenLast->getParent() : ThenBB, thenLast,
				                          else_var->ft.type, else_var->val, ElseBB, elseLast, firstWhile, enterBB,
				                          then_var->decl_loc, else_var->decl_loc, then_node.getKey());
				if (!merge.second)
					return nullptr;
				auto mergeVal = merge.second;
				FullVar* entry;
				if (locals_table.empty()) {
					entry = lex.module->globals_table[then_node.getKey()];
				} else {
					entry = locals_table.back()[then_node.getKey()];
				}
				if (!entry) {
					errs() << "internal error, could not find merge variable '" << then_node.getKey() << "' in outer scope\n";
					abort();
				}
				entry->ft.type = merge.first;
				entry->ft.type_attr = then_var->ft.type_attr | else_var->ft.type_attr;
				if (comp_mode == comp_jit && !do_test && locals_table.empty()) {
					std::string var_name = then_node.getKey();
					if (merge.first->isSized() && TheModule->getDataLayout().getTypeAllocSize(merge.first) > 0) {
						entry->storage_type = merge.first;
						entry->ft.type_attr |= A_mainvar;
						entry->mangled_name = strdup(var_name.c_str());
						auto initializer = llvm::Constant::getNullValue(merge.first);
						auto GV = GetGlobalHandle(merge.first, var_name, entry->ft.type_attr);
						pending_globals.push_back({ initializer, std::move(var_name), entry->ft.type_attr });
						auto align = TheModule->getDataLayout().getPrefTypeAlign(merge.first);
						auto sz = TheModule->getDataLayout().getTypeAllocSize(merge.first);
						Builder->CreateMemCpy(GV, align, mergeVal, align, sz);
					} else {
						auto struct_type = llvm::dyn_cast<llvm::StructType>(mergeVal->getType());
						auto [el_type, data_ptr, Dims] = getArrayDims(mergeVal, merge.first);
						if (el_type && struct_type) {
							// find out memory size of array in bytes: get element size and multiply witch each dimension
							llvm::Value* ElemSz = getSize(TheModule->getDataLayout().getTypeAllocSize(el_type));
							llvm::Value* Sz = getSize(1);
							for (auto Dim: Dims)
								Sz = Builder->CreateMul(Sz, Dim);
							auto num_dims = struct_type->getNumElements(); // +1 for pointer
							auto dim_array = malloc(sizeof(size_t)*num_dims);
							llvm::Constant* DimArray = llvm::cast<llvm::Constant>(Builder->CreateIntToPtr(
								llvm::ConstantInt::get(llvm_size_type, (uintptr_t)dim_array),
								struct_type->getPointerTo()));
							Builder->CreateStore(mergeVal, DimArray);
							pending_arrays.push_back({dim_array, &entry->val, struct_type});
						} else {
							errs() << Loc << ": declaring variable size array '" << then_node.getKey() << "', "
							       << *mergeVal << " " << *merge.first << " in global scopy from conditional branches not supported, yet (sorry)\n";
						}
					}
				} else {
					entry->val = mergeVal;
				}
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
		if (CTcond == CTcond_true)
			return handle(target, ThenV);
		else if (CTcond == CTcond_false)
			return handle(target, ElseV);
		auto merge = merge_values(Then.back()->ft->type, ThenV, (if_kind == tok_while || if_kind == tok_for) ?
		                          CondBB : ThenBB, thenLast, Else.back()->ft->type, ElseV, ElseBB, elseLast,
		                          firstWhile, enterBB, Then.back()->Loc, Else.back()->Loc);
		if (ft->type != merge.first) {
			ft = new_FullType(*ft);
			ft-> type = merge.first;
		}
		return handle(target, merge.second);
	}
}

void CallGlobalDestructorsJIT() {
	finishFunctionOrModule();
	std::string destr_name = "__global_destructor_caller";
	llvm::FunctionType* destr_fn_t = llvm::FunctionType::get(llvm::Type::getInt1Ty(Context),
	                                                         {}, false);
	llvm::Function* destr_fn = llvm::Function::Create(destr_fn_t, llvm::Function::ExternalLinkage, destr_name, TheModule.get());
	auto BB = llvm::BasicBlock::Create(Context, "entry", destr_fn);
	Builder->SetInsertPoint(BB);
	if (last_shadow_restorer && comp_mode == comp_jit && !do_test) {
		auto last_restorer_proto = (*lex.findProtos(last_shadow_restorer))[0].get();
		auto last_restorer = getFunction(last_restorer_proto);
		Builder->CreateCall(last_restorer_proto->FT, last_restorer, std::vector<llvm::Value*>());
	}
	for (auto& [modname, module] : Modules) {
		if (module.globals_table.table)
			InsertDestructors(module.globals_table, nullptr);
	}
	Builder->CreateRet(Builder->getInt1(true));
	finishFunctionOrModule(destr_fn, 2, true, false);
	auto RT = TheJIT->getMainJITDylib().createResourceTracker();
	auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context);
	ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
	InitializeModuleAndPassManager();
	auto ExprSymbol = ExitOnErr(TheJIT->lookup(destr_name));
#if LLVM_VERSION_MAJOR >= 17
	bool (*BOOL)() = ExprSymbol.getAddress().toPtr<bool (*)()>();
#else
	bool (*BOOL)() = (bool (*)())(intptr_t)ExprSymbol.getAddress();
#endif
	bool b;
	if (jit_extra_thread)
		b = spawn_bool_expr(BOOL);
	else
		b = BOOL();
	ExitOnErr(RT->remove());
}

llvm::Value* ExprAST::convert_raw(llvm::Value* rawV) {
	if (!rawV)
		return nullptr;
	if (desired_type && rawV && rawV->getType() != desired_type && !rawV->getType()->isVoidTy()) {
		unsigned desired_attr = 0;
		if (ft->type->isIntegerTy()) {
			if (desired_type->isIntegerTy())
				if (conv_kind == ConvSigned || conv_kind == ConvImplicit && (ft->type_attr & A_signed))
					desired_attr |= A_signed;
		} else if (!desired_type->isIntegerTy())
			desired_attr = ft->type_attr & A_imaginary;
		if (((conv_kind == ConvImplicit && (ft->type_attr & A_signed))
		     || conv_kind == ConvSigned) && !(ft->type->isIntegerTy() && !desired_type->isIntegerTy()))
			desired_attr = ft->type_attr & A_signed;
		auto postConv = getConv(rawV->getType(), desired_type, Loc, ft->type_attr,
		                        desired_attr, is_unknown_type);
		if (postConv) {
			llvm::Value* V = postConv(rawV);
			if (is_unknown_type) {
				// check for over-/underflow
				if (auto Vconst = llvm::dyn_cast<llvm::ConstantInt>(V)) {
					if (auto rawConst = llvm::dyn_cast<llvm::ConstantInt>(rawV)) {
						if (ConstexprIntOverflow(Loc, Vconst, 0ULL, desired_attr, rawConst))
							return nullptr;
					}
				} else if (auto Vconst = llvm::dyn_cast<llvm::ConstantFP>(V)) {
					if (auto rawConst = llvm::dyn_cast<llvm::ConstantFP>(rawV)) {
						if (ConstexprFPOverflow(Loc, Vconst, 0.0, rawConst))
							return nullptr;
					}
				}
			}
			return V;
		}
		auto raw_array_type = llvm::dyn_cast<llvm::ArrayType>(rawV->getType());
		auto desired_array_type = llvm::dyn_cast<llvm::ArrayType>(desired_type);
		if (!raw_array_type || !desired_array_type) {
			errs() << Loc << ": cannot automatically convert " << *rawV->getType() << " to " << *desired_type << '\n';
			return nullptr;
		}
	}
	return rawV;
}

llvm::Value* get_type_alloc_size(llvm::Type* t) {
	if (t->isVoidTy())
		return getSize(0);
	if (t->isFunctionTy())
		return getSize(target_bytes);
	if (t->isSized()) {
		uint64_t sz = TheModule->getDataLayout().getTypeAllocSize(t);
		if (sz)
			return getSize(sz);
	}
	return nullptr;
}

llvm::Value* ExprAST::alloc_size() {
	if (auto a_sz = get_type_alloc_size(ft->type))
		return a_sz;
	auto Dims = codegen_dims();
	if (!Dims.first || !Dims.second)
		return nullptr;
	uint64_t sz = TheModule->getDataLayout().getTypeAllocSize(Dims.first);
	llvm::Value* Sz = getSize(sz);
	for (auto dim: *Dims.second)
		Sz = Builder->CreateMul(Sz, dim);
	return Sz;
}

std::pair<llvm::Type*,std::unique_ptr<std::vector<llvm::Value*>>> ExprAST::codegen_dims() {
	if (ft && ft->type) {
		if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ft->type)) {
			llvm::Type* elem_type;
			auto Dims = std::make_unique<std::vector<llvm::Value*>>();
			do {
				elem_type = array_type->getElementType();
				Dims->push_back(getSize(array_type->getNumElements()));
			} while ((array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)));
			return { elem_type, std::move(Dims) };
		}
	}
	return { nullptr, nullptr };
}

std::pair<llvm::Type*,std::unique_ptr<std::vector<llvm::Value*>>> LvalueExprAST::codegen_dims() {
	auto ref = codegen_ref(true);
	if (ref.second) {
		if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ref.first)) {
			llvm::Type* elem_type;
			auto Dims = std::make_unique<std::vector<llvm::Value*>>();
			unsigned n = 0;
			do {
				elem_type = array_type->getElementType();
				size_t nelem = array_type->getNumElements();
				if (nelem)
					Dims->push_back(getSize(nelem));
				else
					Dims->push_back(Builder->CreateExtractValue(ref.second, n++));
			} while ((array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)));
			return { elem_type, std::move(Dims) };
		}
	}
	return ((ExprAST*)this)->codegen_dims();
}

// combination of the above that returns alloc_size() and an incomplete reference value
std::tuple<llvm::Value*,llvm::Value*,unsigned> ExprAST::alloc_dims() {
	llvm::Value* Sz = nullptr;
	if (ft->type->isFunctionTy())
		Sz = getSize(target_bytes);
	else if (ft->type->isSized()) {
		uint64_t sz = TheModule->getDataLayout().getTypeAllocSize(ft->type);
		if (sz)
			Sz = getSize(sz);
	}
	auto Dims = codegen_dims();
	if (!Dims.first || !Dims.second)
		return { Sz, nullptr, 0 };
	if (!Sz) {
		uint64_t sz = TheModule->getDataLayout().getTypeAllocSize(Dims.first);
		Sz = getSize(sz);
		for (auto dim: *Dims.second)
			Sz = Builder->CreateMul(Sz, dim);
	}
	std::vector<llvm::Type*> struct_type_el(Dims.second->size() + 1, llvm_size_type);
	struct_type_el[Dims.second->size()] = Dims.first->getPointerTo();
	llvm::Type* struct_type = llvm::StructType::get(Context, struct_type_el);
	llvm::Value* the_struct = llvm::UndefValue::get(struct_type);
	unsigned u = 0;
	for (auto dim: *Dims.second)
		the_struct = Builder->CreateInsertValue(the_struct, dim, u++);
	return { Sz, the_struct, Dims.second->size() };
}
