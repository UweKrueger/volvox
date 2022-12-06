/*
 * Copyright © Uwe Krüger 2021, 2022
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"
#include "../lib/str.h"

llvm::Value* FixedArrayExprAST::getArrayLitVal(llvm::ArrayType* initializer_type, ListExprAST* List) {
	uint64_t dim = initializer_type->getNumElements();
	if (!Elements.size())
		return llvm::Constant::getNullValue(initializer_type);
	llvm::Type* sub_type = initializer_type->getElementType();
	llvm::Value* zero = llvm::Constant::getNullValue(sub_type);
	llvm::Value* ini = llvm::UndefValue::get(initializer_type);
	for (unsigned idx = 0; idx < dim; idx++)
		if (List->Elements.size() <= idx || !List->Elements[idx])
			ini = Builder->CreateInsertValue(ini, zero, idx, "arrlitzero");
		else
			if (auto sub_list = dynamic_cast<ListExprAST*>(List->Elements[idx].get()))
				ini = Builder->CreateInsertValue(ini, getArrayLitVal(llvm::cast<llvm::ArrayType>(sub_type), sub_list), idx, "arrlitsub");
			else {
				List->Elements[idx]->desired_type = sub_type;
				ini = Builder->CreateInsertValue(ini, List->Elements[idx]->codegen(), idx, "arrlitval");
	//	iter_idx++;
			}
	return ini;
}

llvm::Value* FixedArrayExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	llvm::Value* LenVal = Builder->getInt64(1);
	std::vector<llvm::Value*> LenVals;
	LenVals.reserve(Dims.size());
	auto array_type = llvm::dyn_cast<llvm::ArrayType>(ft->type);
	if (!array_type) {
		errs() << Loc << ": internal error: array literal not of array type\n";
		return nullptr;
	}
	for (int j = 0; j < Dims.size(); j++) {
		llvm::Value* curDim;
		if (Dims[j])
			curDim = Builder->CreateIntCast(Dims[j]->codegen(), llvm::Type::getInt64Ty(Context), false);
		else
			curDim = Builder->getInt64(LitDims[j]);
		LenVal = Builder->CreateMul(LenVal, curDim);
		LenVals.push_back(curDim);
	}
	llvm::Type* new_type = ft->elem_type->type;
	std::vector<llvm::Value*> Sizes; // reverse order
	for (int j = LenVals.size() - 1; j >= 0; j--)
		if (auto constlen = llvm::dyn_cast<llvm::ConstantInt>(LenVals[j])) {
			new_type = llvm::ArrayType::get(new_type, constlen->getZExtValue());
		} else {
			new_type = llvm::ArrayType::get(new_type, 0);
			Sizes.push_back(LenVals[j]);
		}
	if (new_type != ft->type) {
		ft = new_FullType(*ft);
		ft->type = new_type;
	}
	llvm::Type* initializer_type = ft->elem_type->type;
	for (int j = LitDims.size() - 1; j >= 0; j--)
		initializer_type = llvm::ArrayType::get(initializer_type, LitDims[j]);
	llvm::Value* ini = getArrayLitVal(llvm::cast<llvm::ArrayType>(initializer_type), this);
	if (!Sizes.size()) {
		return ini;
	} else {
		std::vector<llvm::Type*> struct_type_el(Sizes.size() + 1, llvm::Type::getInt64Ty(Context));
		struct_type_el[Sizes.size()] = ini->getType();
		llvm::Type* struct_type = llvm::StructType::get(Context, struct_type_el);
		llvm::Value* varini = llvm::UndefValue::get(struct_type);
		for (int j = 0; j < Sizes.size(); j++)
			varini = Builder->CreateInsertValue(varini, Sizes[Sizes.size() - j - 1], j);
		varini = Builder->CreateInsertValue(varini, ini, Sizes.size(), "arrbeg");
		return varini;
	}
}

static void StoreArray(llvm::Value* ArrayAlloc, llvm::Value* ArrData, std::vector<llvm::Value*>& Sizes, unsigned depth) {
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ArrData->getType())) {
		llvm::Value* Adr = ArrayAlloc;
		uint64_t nelem = array_type->getNumElements();
		llvm::Type* elem_type = array_type->getElementType();
		llvm::Value* Sz = Sizes[depth];
		llvm::Value* Sz2 = Builder->CreateMul(Builder->getInt64(nelem), Sizes[depth+1]);
		if (auto subarray_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)) {
			depth++;
			for (uint64_t j = 0; j < nelem; j++) {
				StoreArray(Adr, Builder->CreateExtractValue(ArrData, j), Sizes, depth);
				Adr = Builder->CreateIntToPtr(
					Builder->CreateAdd(
						Builder->CreatePtrToInt(Adr, llvm::Type::getInt64Ty(Context)), Sizes[depth]),
					Adr->getType());
			}
		} else {
			bool is_empty_initializer;
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ArrData->getType()))
				is_empty_initializer = (array_type->getNumElements() == 0);
			else
				is_empty_initializer = false;
			if (!is_empty_initializer)
				Builder->CreateStore(ArrData, Builder->CreateBitCast(ArrayAlloc, ArrData->getType()->getPointerTo()));
			Adr = Builder->CreateIntToPtr(
				Builder->CreateAdd(
					Builder->CreatePtrToInt(Adr, llvm::Type::getInt64Ty(Context)),
					Sz2),
				Adr->getType());
		}
		Builder->CreateMemSet(
			Adr, Builder->getInt8(0),
			Builder->CreateSub(Sz, Sz2),
			TheModule->getDataLayout().getPrefTypeAlign(elem_type));
	} else if (auto pointer_type = llvm::dyn_cast<llvm::PointerType>(ArrData->getType())) {
		auto align = getAlignment(Sizes[Sizes.size()-1]);
		Builder->CreateMemCpy(ArrayAlloc, align, ArrData, align, Sizes[0]);
	} else {
		errs() << "depth: " << depth << " data: " << *ArrData << " Internal error!\n";
		abort();
	}
}

static std::pair<llvm::Value*,llvm::Value*> StoreArrayValue(llvm::Value* val, llvm::Type* elem_type,
                                                            std::vector<llvm::Value*>& Dims, const llvm::Twine& Name = "") {
	auto ElemSize = Builder->getInt64(TheModule->getDataLayout().getTypeAllocSize(elem_type));
	std::vector<llvm::Value*> Sizes(Dims.size() + 1, nullptr);
	Sizes[Dims.size()] = ElemSize;
	for (int j = Dims.size() - 1; j >= 0; j--)
		Sizes[j] = Builder->CreateMul(Dims[j], Sizes[j + 1]);
	llvm::Value* ArrData;
	if (auto struct_type = llvm::dyn_cast<llvm::StructType>(val->getType()))
		ArrData = Builder->CreateExtractValue(val, struct_type->getNumElements() - 1);
	else
		ArrData = val;
	llvm::Value* ArrayAlloc;
	llvm::Value* ArrayPtr;
	llvm::Value* Len = Builder->CreateUDiv(Sizes[0], Sizes[Sizes.size() - 1]);
	if (auto len = llvm::dyn_cast<llvm::ConstantInt>(Len)) {
		llvm::Type* alloc_arr_type = llvm::ArrayType::get(elem_type, len->getZExtValue());
		if (condnesting) {
			// We are inside an if/while/repeat/else branch. An array should *always* be
			// allocated dynamically since it might be of variable size in the other branch
			ArrayAlloc = Builder->CreateAlloca(alloc_arr_type, nullptr, Name);
		} else {
			ArrayAlloc = CreateEntryBlockAlloca(alloc_arr_type, Name);
		}
	} else {
		if (inside_function || comp_mode != comp_jit || do_test)
			ArrayAlloc = Builder->CreateAlloca(elem_type, Len, Name);
		else {
			ArrayAlloc = llvm::CallInst::CreateMalloc(Builder->GetInsertBlock(),
			                                          llvm::Type::getInt64Ty(Context), llvm::Type::getInt8PtrTy(Context),
			                                          ElemSize, Len,
			                                          nullptr, Name);
			ArrayAlloc = Builder->Insert(ArrayAlloc);
		}
	}
	ArrayPtr = Builder->CreateBitCast(ArrayAlloc, elem_type->getPointerTo());
	// TODO: Insert run time check that initialization values fit into allocation size
	StoreArray(ArrayPtr, ArrData, Sizes, 0);
	// REMARK: returning the same pointer value as two different types will be obsolete with opaque pointers
	return { ArrayAlloc, ArrayPtr };
}

llvm::Type* getArrayDims(llvm::Value* val, llvm::ArrayType* array_type, std::vector<llvm::Value*>& Dims,
                         std::vector<llvm::Value*>& returnDims, llvm::ArrayType* expected_array_type) {
	if (!expected_array_type)
		expected_array_type = MakeInterfaceArrayType(array_type);
	llvm::Type* elem_type;
	unsigned idx = 0;
	unsigned level = 0;
	while (true) {
		if (!expected_array_type) {
			errs() << "getArrayDims(): internal error\n";
			abort();
		}
		uint64_t nominal_dim = array_type->getNumElements();
		uint64_t expected_dim = expected_array_type->getNumElements();
		if (nominal_dim) {
			if (expected_dim) {
				if (expected_dim != nominal_dim) {
					errs() << CurLoc << ": mismatch in array dimension (level " << level << ") - required "
					       << expected_dim << ", got " << nominal_dim << '\n';
					return nullptr;
				}
			} else {
				// expect RT-dimension, got CT-dimension
				returnDims.push_back(Builder->getInt64(nominal_dim));
			}
			Dims.push_back(Builder->getInt64(nominal_dim));
		} else {
			// val has RT-dim for this level
			llvm::Value* Dim = Builder->CreateExtractValue(val, idx++);
			if (expected_dim) {
				; // TODO: add RT detection if RT-dim matches CT-expectation
			} else {
				returnDims.push_back(Dim);
			}
			Dims.push_back(Dim);
		}
		elem_type = array_type->getElementType();
		array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type);
		if (array_type) {
			llvm::Type* expected_elem_type = expected_array_type->getElementType();
			expected_array_type = llvm::dyn_cast<llvm::ArrayType>(expected_elem_type);
		} else {
			break;
		}
	}
	return elem_type;
}

llvm::Value* getInterfaceArrayOrStoreValue(llvm::Value* val, llvm::ArrayType* array_type,
                                           llvm::ArrayType* expected_array_type, bool do_store, const llvm::Twine &Name) {
	std::vector<llvm::Value*> Dims = {};
	std::vector<llvm::Value*> returnDims = {};
	llvm::Type* elem_type = getArrayDims(val, array_type, Dims, returnDims, expected_array_type);
	llvm::Value* ArrayAlloc;
	if (do_store) {
		auto p  = StoreArrayValue(val, elem_type, Dims, Name);
		ArrayAlloc = p.first;
		val = p.second;
	} else {
		ArrayAlloc = val;
	}
	if (!returnDims.size()) {
		return ArrayAlloc;
	} else {
		if (auto str_ty = llvm::dyn_cast<llvm::StructType>(val->getType()))
			val = Builder->CreateExtractValue(val, str_ty->getNumElements() - 1);
		std::vector<llvm::Type*> struct_types(returnDims.size() + 1, llvm::Type::getInt64Ty(Context));
		struct_types[returnDims.size()] = val->getType();
		llvm::Type* ret_struct_type = llvm::StructType::get(Context, struct_types);
		llvm::Value* ret = llvm::UndefValue::get(ret_struct_type);
		for (unsigned j = 0; j < returnDims.size(); j++)
			ret = Builder->CreateInsertValue(ret, returnDims[j], j, "arrlen");
		ret = Builder->CreateInsertValue(ret, val, returnDims.size(), "arraystore");
		return ret;
	}
}

static std::pair<llvm::Value*, SourceLocation> GenIndex(ExprAST* Index) {
	auto aggr = dynamic_cast<AggregateExprAST*>(Index);
	if (aggr) {
		if (aggr->Elements.size() != 1) {
			errs() << (aggr->Elements.size() > 1 ? aggr->Elements[1]->Loc : Index->Loc)
			       << ": exactly one index expected (for now)\n";
			return { nullptr, {0} };
		}
		return { aggr->Elements[0]->codegen(), aggr->Elements[0]->Loc };
	} else {
		errs() << "internal compiler error\n";
		return { nullptr, {0} };
	}
}

llvm::Value* IndexExprAST::codegen_raw(llvm::Value* target) {
	// first try to get a reference to the element ...
	auto V = codegen_ref(true);
	if (auto val = ref2val(V))
		return val;
	if (V.first) {
		// we know the type, but field is an rvalue
		auto fld = Field->codegen();
		auto fld_save = fld;
		auto idx_loc = GenIndex(Index.get());
		auto idx = idx_loc.first;
		if (!idx)
			return nullptr;
		auto IdxLoc = idx_loc.second;
		if (!fld)
			return nullptr;
		int64_t len = -1;
		llvm::Value* Len = nullptr;
		llvm::ArrayType* array_type = llvm::dyn_cast<llvm::ArrayType>(fld->getType());
		if (array_type)
			len = array_type->getNumElements();
		else
			if (auto st = llvm::dyn_cast<llvm::StructType>(fld->getType())) {
				Len = Builder->CreateExtractValue(fld, 0);
				fld = Builder->CreateExtractValue(fld, 1);
				array_type = llvm::dyn_cast<llvm::ArrayType>(fld->getType());
				if (auto len2 = llvm::dyn_cast<llvm::ConstantInt>(Len)) {
					errs() << "unexpected constant length: " << *len2 << '\n';
					len = len2->getZExtValue();
				}
			}
		if (!array_type) {
			// if it's no array it must be a pointer - but then this would be an lvalue
			// and codegen_ref() above would have succeeded - so we should not get here
			errs() << "internal compiler error\n";
			abort();
		}
		auto FixedField = dynamic_cast<FixedArrayExprAST*>(Field.get());
		SourceLocation LenLoc = FixedField ? FixedField->LenLocs[0] : SourceLocation{0};
		// if both the array size and the index are CT consts we can get the element
		// without having to allocate space to store the array
		if (auto Idx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
			uint64_t i = Idx->getZExtValue();
			if (Len) {
				if (auto clen = llvm::dyn_cast<llvm::ConstantInt>(Len))
					len = clen->getSExtValue();
				else
					goto run_time_len;
			}
			if (len < 0) {
				errs() << LenLoc << ": array length must be non-negative\n";
				return nullptr;
			} else if ((uint64_t)len < array_type->getNumElements()) {
				errs() << LenLoc << ": array length must be greater than any element index\n";
				return nullptr;
			} else if (i >= (uint64_t)len) {
				errs() << IdxLoc << ": index out of range (should >= 0 and < "
				       << len << ")\n";
				return nullptr;
			}
			return Builder->CreateExtractValue(fld, i);
		}
	run_time_len:
		// TODO: Insert run time check of index
		llvm::Value* ptr = StoreValue(fld_save, Field->ft);
		if (llvm::isa<llvm::StructType>(ptr->getType())) {
			Len = Builder->CreateExtractValue(ptr, 0);
			ptr = Builder->CreateExtractValue(ptr, 1);
		}
		// TODO: insert code for index range check
		return Builder->CreateLoad(
			array_type->getElementType(),
			Builder->CreateGEP(array_type->getElementType(),
			                   Builder->CreateBitCast(ptr, array_type->getElementType()->getPointerTo()),
			                   idx));
	} else {
		errs() << "cound not create code for index expression\n";
		return nullptr;
	}
}

// const_elem_size, var_elem_size, offset
std::tuple<uint64_t,llvm::Value*,llvm::Value*> IndexExprAST::getMLIdxOffset(llvm::Type* elem_typex,
	      std::vector<llvm::Value*>& Idxs, llvm::Value* Dims, int idx_idx, int dim_idx) {
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(elem_typex)) {
		auto subtype = array_type->getElementType();
		uint64_t n_elem = array_type->getNumElements();
		auto sub_descr = getMLIdxOffset(subtype, Idxs, Dims, idx_idx + 1, n_elem ? dim_idx : dim_idx + 1);
		auto const_elem_size = std::get<0>(sub_descr);
		auto var_elem_size = std::get<1>(sub_descr);
		auto offset = std::get<2>(sub_descr);
		llvm::Value* cur_Offset = nullptr;
		if (idx_idx < Idxs.size()) {
			cur_Offset = Idxs[idx_idx];
			if (const_elem_size != 1)
				cur_Offset = Builder->CreateMul(Builder->getInt64(const_elem_size), cur_Offset);
			if (var_elem_size)
				cur_Offset = Builder->CreateMul(cur_Offset, var_elem_size);
		} else if (idx_idx == Idxs.size()) {
			ml_elem_type = elem_typex; // overwrite previous "deepest" element type - result is sub array
		}
		if (n_elem)
			const_elem_size *= n_elem;
		else {
			auto dim = Builder->CreateExtractValue(Dims, dim_idx++);
			if (idx_idx < Idxs.size())
				if (dim_idx > num_dims_to_strip_from_val) {
					num_dims_to_strip_from_val = dim_idx;
				}
			if (var_elem_size)
				var_elem_size = Builder->CreateMul(dim, var_elem_size);
			else
				var_elem_size = dim;
		}
		if (cur_Offset && offset)
			cur_Offset = Builder->CreateAdd(cur_Offset, offset);
		else if (offset)
			cur_Offset = offset;
		return { const_elem_size, var_elem_size, cur_Offset };
	} else {
		uint64_t elem_size = TheModule->getDataLayout().getTypeAllocSize(elem_typex);
		ml_elem_type = elem_typex; // may be overwritten if # indices < array order
		return { elem_size, nullptr, nullptr };
	}
}

// Idxs, val - returns full field of nested IndexExprs, i.e. something like '{ i64, i64, i64, double* }'
llvm::Value* IndexExprAST::codegen_ref0(std::vector<llvm::Value*>& Idxs, llvm::Type*& ml_field_type) {
	llvm::Value* res = nullptr;
	if (auto fieldidxexpr = dynamic_cast<IndexExprAST*>(Field.get())) {
		auto fieldval = fieldidxexpr->codegen_ref0(Idxs, ml_field_type);
		if (!fieldval)
			return nullptr;
		res = fieldval;
	} else if (auto lval = dynamic_cast<LvalueExprAST*>(Field.get())) {
		auto elem = lval->codegen_ref();
		ml_field_type = Field->ft->type;
		res = elem.second;
	}
	if (res) {
		ft = new_FullType(*ft);
		ft->type = llvm::cast<llvm::ArrayType>(Field->ft->type)->getElementType();
		if (auto aggr = dynamic_cast<AggregateExprAST*>(Index.get())) {
			if (aggr->Elements.size() != 1) {
				errs() << "exactly one index expected (for now)\n";
				return nullptr;
			}
			auto idx = aggr->Elements[0]->codegen();
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
				uint64_t num_elem = array_type->getNumElements();
				if (num_elem) {
					if (auto c_idx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
						uint64_t u_idx = c_idx->getZExtValue();
						if (u_idx >= num_elem) {
							errs() << aggr->Elements[0]->Loc << ": array index (" << u_idx << ") must be less than array length (" << num_elem << ")\n";
							return nullptr;
						}
					}
					// TODO: run time check for index range
				}
			}
			if (auto int_type = llvm::dyn_cast<llvm::IntegerType>(idx->getType())) {
				if (int_type->getBitWidth() != 64)
					idx = Builder->CreateIntCast(idx, llvm::Type::getInt64Ty(Context), false);
			} else {
				errs() << aggr->Elements[0]->Loc << ": array indices must be integers - not " << *idx->getType() << '\n';
				return nullptr;
			}
			Idxs.push_back(idx);
		} else {
			errs() << "internal compiler error\n";
			abort();
		}
	}
	return res;
}

std::pair<llvm::Type*,llvm::Value*> IndexExprAST::codegen_ref(bool silent_fail) {
	llvm::Value* NumElem = nullptr;
	if (!Field->ft || !Field->ft->type) {
		errs() << Field->Loc << ": unknown type\n";
		return { nullptr, nullptr };
	}
	if (auto a_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
		std::vector<llvm::Value*> Idxs;
		llvm::Type* ml_field_type = nullptr;
		auto LV = codegen_ref0(Idxs, ml_field_type);
		if (!LV) {
			if (!silent_fail)
				errs() << "LHS of index expression must be an lvalue\n";
			return { Field->ft->type, nullptr };
		}
		auto OffsetDescr = getMLIdxOffset(ml_field_type, Idxs, LV, 0, 0);
		auto offset = std::get<2>(OffsetDescr);
		llvm::Value* Ptr;
		int n_var_dims;
		if (LV->getType()->isPointerTy()) {
			Ptr = LV;
			n_var_dims = 0;
		} else if (auto struct_type = llvm::dyn_cast<llvm::StructType>(LV->getType())) {
			Ptr = Builder->CreateExtractValue(LV, struct_type->getNumElements() - 1);
			n_var_dims = struct_type->getNumElements() - 1 - num_dims_to_strip_from_val;
		} else {
			errs() << "internal error\n";
			abort();
		}
		if (offset)
			Ptr = Builder->CreateIntToPtr(Builder->CreateAdd(Builder->CreatePtrToInt(Ptr, llvm::Type::getInt64Ty(Context)), offset), Field->ft->elem_type->type->getPointerTo());
		else
			Ptr = Builder->CreateBitCast(Ptr, Field->ft->elem_type->type->getPointerTo());
		if (!n_var_dims)
			return { ml_elem_type, Ptr };
		std::vector<llvm::Type*> new_struct_el(n_var_dims + 1, llvm::Type::getInt64Ty(Context));
		new_struct_el[n_var_dims] = Ptr->getType();
		llvm::Type* new_struct_type = llvm::StructType::get(Context, new_struct_el);
		llvm::Value* res = llvm::UndefValue::get(new_struct_type);
		for (int j = 0; j < n_var_dims; j++)
			res = Builder->CreateInsertValue(res, Builder->CreateExtractValue(LV, j + num_dims_to_strip_from_val), j);
		res = Builder->CreateInsertValue(res, Ptr, n_var_dims);
		return { ml_elem_type, res };
	} else if (auto a_type = llvm::dyn_cast<llvm::PointerType>(Field->ft->type)) {
		// if (Field->ft->type_attr & A_map) {
			llvm::Value* Map = Field->codegen();
			llvm::Value* Key = Index->codegen();
			if (!Map || !Key)
				return { nullptr, nullptr };
			// Key = Builder->CreateGlobalStringPtr("aer", "", 0, TheModule.get());
			const char* getter;
			if (Field->ft->elem_type[0].type == llvm::Type::getInt8PtrTy(Context)) // string key type
				getter = "_ZN6volvox3map16volvoxstring_getEPNS0_4NodeEPKc";
			else {
				errs() << Loc << ": maps with key type " << ft->elem_type[0] << " not supported\n";
				return { nullptr, nullptr };
			}
			PrototypeAST* getter_proto = (*lex.findProtos(std::string(getter)))[0].get();
			if (!getter_proto) {
				errs() << Loc << ": prototype " << getter << "() not found\n";
				return { nullptr, nullptr };
			}
			auto getter_fn = getFunction(getter_proto);
			auto value_wrapped = Builder->CreateCall(getter_proto->FT, getter_fn, std::vector<llvm::Value*>{ Map, Key });
			auto value = Builder->CreateExtractValue(value_wrapped, 0);
			auto pointee_type = Field->ft->elem_type[1].type;
			auto ref = Builder->CreatePointerCast(value, pointee_type->getPointerTo());
			return { pointee_type, ref };
		// }
	}
	errs() << "LHS of index expression must be an array (or map) " << *ft->type << "\n";
	return { nullptr, nullptr };
}

llvm::Value* expandArrayInitializer(llvm::Value* initializer, llvm::ArrayType* ini_array_type, llvm::ArrayType* array_type) {
	uint64_t n_type = array_type->getNumElements();
	uint64_t n_ini = ini_array_type->getNumElements();
	llvm::Type* elem_type = array_type->getElementType();
	// array initialized with short literal - as we can't use "memset" for
	// llvm::Values we must create an expanded array constant - filled up with zero values
	llvm::Value* expanded_ini = llvm::UndefValue::get(array_type);
	for (int i = 0; i < n_ini; i++) {
		llvm::Value* elem = Builder->CreateExtractValue(initializer, i);
		auto ini_elem_array_type = llvm::dyn_cast<llvm::ArrayType>(elem->getType());
		auto elem_array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type);
		if (elem_array_type)
			elem = expandArrayInitializer(elem, ini_elem_array_type, elem_array_type);
		expanded_ini = Builder->CreateInsertValue(expanded_ini, elem, i);
	}
	for (int i = n_ini; i < n_type; i++)
		expanded_ini = Builder->CreateInsertValue(expanded_ini, llvm::Constant::getNullValue(elem_type), i);
	return expanded_ini;
}

MapExprAST::MapExprAST(SourceLocation Loc, volvoxc::FullType* map_ft, std::vector<std::unique_ptr<ExprAST>> _Elements) :
	ListExprAST(Loc, std::move(_Elements), map_ft)
{
	keys.reserve(Elements.size());
	values.reserve(Elements.size());
	for (auto& elem: Elements) {
		if (auto bin_expr = dynamic_cast<BinaryExprAST*>(elem.get())) {
			if (bin_expr->Op[0] == ':' && !bin_expr->Op[1]) {
				keys.push_back(bin_expr->LHS.get());
				values.push_back(bin_expr->RHS.get());
				continue;
			}
			errs() << bin_expr->Loc << ": ':' expected\n";
			ft = nullptr;
			return;
		}
		errs() << elem->Loc << ": binary expression 'key: value' expected\n";
		ft = nullptr;
		return;
	}
	if (!ft->elem_type) {
		auto key_ft = getCommonType(keys);
		auto val_ft = getCommonType(values);
		if (!key_ft || !val_ft) {
			ft = nullptr;
			return;
		}			
		auto ftpair = new_FullType(*key_ft, 1); // reserve space for 1 additional FullType
		ftpair[1] = *val_ft;
		ft->elem_type = ftpair;
	}
}

llvm::Value* createStringConst(const char* str, const llvm::Twine &Name) {
	char* stra;
	char* tmpres;
	unsigned l_alloc;
	cstr2volvoxstr(tmpres, l_alloc, stra, str);
	auto llvmstr = llvm::ConstantDataArray::getString(Context, llvm::StringRef(stra, l_alloc), false);
	errs() << "String const " << *llvmstr << '\n';
	auto GV = new llvm::GlobalVariable(*TheModule, llvmstr->getType(), true, llvm::GlobalValue::PrivateLinkage,
	                                   llvmstr, Name, nullptr, llvm::GlobalVariable::NotThreadLocal, 0);
	GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
	GV->setAlignment(llvm::Align(4));
	llvm::Constant* Indices[] = {Builder->getInt32(0), Builder->getInt32(l_alloc - 4)};
	return llvm::ConstantExpr::getInBoundsGetElementPtr(GV->getValueType(), GV, Indices);
}

llvm::Value* createStringVal(const char* str, const llvm::Twine &Name) {
	char* stra;
	char* tmpres;
	unsigned l_alloc;
	cstr2volvoxstr(tmpres, l_alloc, stra, str);
	auto llvmstr = llvm::ConstantDataArray::getString(Context, llvm::StringRef(stra, l_alloc), false);
	errs() << "String const " << *llvmstr << '\n';
	llvm::Value* GV = llvm::CallInst::CreateMalloc(Builder->GetInsertBlock(),
	                                               llvm::Type::getInt64Ty(Context), llvm::Type::getInt8PtrTy(Context),
	                                               Builder->getInt64(l_alloc), nullptr, nullptr);
	GV = Builder->Insert(GV);
	Builder->CreateStore(llvmstr, GV);
	return Builder->CreateConstGEP2_32(llvmstr->getType(), Builder->CreatePointerCast(GV, llvmstr->getType()->getPointerTo()),
	                                   0, l_alloc - 4);
}
