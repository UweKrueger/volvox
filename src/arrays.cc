/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"

std::vector<const char*> jit_string_consts;
bool do_range_checks = true;

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
				ini = Builder->CreateInsertValue(ini, List->Elements[idx]->codegen(true), idx, "arrlitval");
	//	iter_idx++;
			}
	return ini;
}

llvm::Value* FixedArrayExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	llvm::Value* LenVal = getSize(1);
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
			if (auto dims_j = Dims[j]->codegen())
				curDim = Builder->CreateIntCast(dims_j, llvm_size_type, false);
			else
				return nullptr;
		else
			curDim = getSize(LitDims[j]);
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
		// array literal "value" has special format:
		// struct { dim, dim, ..., min-tensor }
		// dim: run-time sized dimensions
		// min-tensor: retangular initializer - will be extended with zeros
		std::vector<llvm::Type*> struct_type_el(Sizes.size() + 1, llvm_size_type);
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
		llvm::Value* Sz2 = Builder->CreateMul(getSize(nelem), Sizes[depth+1]);
		if (auto subarray_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)) {
			depth++;
			for (uint64_t j = 0; j < nelem; j++) {
				StoreArray(Adr, Builder->CreateExtractValue(ArrData, j), Sizes, depth);
				Adr = Builder->CreateIntToPtr(
					Builder->CreateAdd(
						Builder->CreatePtrToInt(Adr, llvm_size_type), Sizes[depth]),
					Adr->getType());
			}
		} else {
			bool is_empty_initializer;
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(ArrData->getType()))
				is_empty_initializer = (array_type->getNumElements() == 0);
			else
				is_empty_initializer = false;
			if (!is_empty_initializer)
				Builder->CreateStore(ArrData, Builder->CreatePointerCast(ArrayAlloc, llvm_ptr_type));
			Adr = Builder->CreateIntToPtr(
				Builder->CreateAdd(
					Builder->CreatePtrToInt(Adr, llvm_size_type),
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

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

// in interactive JIT mode we must keep handles to heap memory blocks
// allocated from global context, so we can free them when the interpreter
// finishes and make Valgrind happy...
//
extern "C" DLLEXPORT void* __jit_managed_malloc(size_t s) {
	void* adr = malloc(s);
	jit_main_variables.emplace_back((char*)adr);
	return adr;
}

extern "C" DLLEXPORT void* __jit_malloc(size_t s) {
	void* adr = malloc(s);
	return adr;
}

extern "C" DLLEXPORT void __jit_free(void* buf) {
	// fprintf(stderr, "### free called for %p\n", buf);
	free(buf);
}

static std::pair<llvm::Value*,llvm::Value*> StoreArrayValue(llvm::Value* val, llvm::Type* elem_type,
                                                            std::vector<llvm::Value*>& Dims, const llvm::Twine& Name = "") {
	auto ElemSize = getSize(TheModule->getDataLayout().getTypeAllocSize(elem_type));
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
		if (!merge_points.empty()) {
			// We are inside an if/while/repeat/else branch. An array should *always* be
			// allocated dynamically since it might be of variable size in the other branch
			ArrayAlloc = Builder->CreateAlloca(alloc_arr_type, nullptr, Name);
		} else {
			if (inside_function || !jit_repl)
				ArrayAlloc = CreateEntryBlockAlloca(alloc_arr_type, Name);
			else {
				if (!jit_repl) {
					ArrayAlloc = CreateMalloc(ElemSize, Len, Name);
				} else {
					const char* jit_malloc = "__jit_managed_malloc";
					auto jit_malloc_proto = (*lex.findProtos(jit_malloc))[0].get();
					auto jit_malloc_fn = getFunction(jit_malloc_proto);
					llvm::Value* Sz = Builder->CreateMul(ElemSize, Len);
					ArrayAlloc = Builder->CreateCall(jit_malloc_proto->FT, jit_malloc_fn, std::vector<llvm::Value*>({ Sz }));
				}
			}
		}
	} else {
		if (inside_function || !jit_repl)
			ArrayAlloc = Builder->CreateAlloca(elem_type, Len, Name);
		else {
			if (!jit_repl) {
				ArrayAlloc = CreateMalloc(ElemSize, Len, Name);
			} else {
				const char* jit_malloc = "__jit_managed_malloc";
				auto jit_malloc_proto = (*lex.findProtos(jit_malloc))[0].get();
				auto jit_malloc_fn = getFunction(jit_malloc_proto);
				llvm::Value* Sz = Builder->CreateMul(ElemSize, Len);
				ArrayAlloc = Builder->CreateCall(jit_malloc_proto->FT, jit_malloc_fn, std::vector<llvm::Value*>({ Sz }));
			}
		}
	}
	ArrayPtr = Builder->CreatePointerCast(ArrayAlloc, llvm_ptr_type);
	// TODO: Insert run time check that initialization values fit into allocation size
	StoreArray(ArrayPtr, ArrData, Sizes, 0);
	// REMARK: returning the same pointer value as two different types will be obsolete with opaque pointers
	return { ArrayAlloc, ArrayPtr };
}

// get element type, memory pointer, dimensions of array in memory
//
std::tuple<llvm::Type*,llvm::Value*,std::vector<llvm::Value*>> getArrayDims(
	llvm::Value* val, llvm::Type* _type) {
	std::vector<llvm::Value*> Dims;
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(_type)) {
		int idx = 0;
		llvm::Type* elem_type;
		do {
			elem_type = array_type->getElementType();
			uint64_t nominal_dim = array_type->getNumElements();
			llvm::Value* Dim;
			if (nominal_dim)
				Dim = getSize(nominal_dim);
			else
				Dim = Builder->CreateExtractValue(val, idx++);
			Dims.push_back(Dim);
			array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type);
		} while (array_type);
		if (val->getType()->isStructTy())
			return { elem_type, Builder->CreateExtractValue(val, idx), Dims };
		else
			// either pointer to constant size allocation or rvalue array
			return { elem_type, val, Dims };
	} else {
		return { nullptr, nullptr, Dims };
	}
}

// a more complex variant of the above for special purposes
//
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
				returnDims.push_back(getSize(nominal_dim));
			}
			Dims.push_back(getSize(nominal_dim));
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
		std::vector<llvm::Type*> struct_types(returnDims.size() + 1, llvm_size_type);
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
			return { nullptr, SourceLocation() };
		}
		return { aggr->Elements[0]->codegen(), aggr->Elements[0]->Loc };
	} else {
		errs() << "internal compiler error\n";
		return { nullptr, SourceLocation() };
	}
}

static void CheckArrayIndex(llvm::Value* idx, llvm::Value* Len, SourceLocation Loc,
                     bool idx_is_signed = true) {
	if (!do_range_checks)
		return;
	auto checker_proto = (*lex.findProtos("__check_array_index"))[0].get();
	auto checker = getFunction(checker_proto);
	if (idx->getType() != llvm_size_type)
		idx = Builder->CreateIntCast(idx, llvm_size_type, idx_is_signed);
	auto File = Builder->CreateGlobalString(Loc.File, "", 0, TheModule.get());
	auto Line = llvm::ConstantInt::get(llvm_int_type, Loc.Line, true);
	auto Col = llvm::ConstantInt::get(llvm_int_type, Loc.Col, true);
	Builder->CreateCall(checker_proto->FT, checker, { idx, Len, File, Line, Col });
}

llvm::Value* IndexExprAST::codegen_raw(llvm::Value* target) {
	// first try to get a reference to the element ...
	auto V = codegen_ref(true, true);
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
		if (array_type) {
			len = array_type->getNumElements();
			if (len > 0)
				Len = llvm::ConstantInt::get(llvm_size_type, len);
		} else
			if (auto st = llvm::dyn_cast<llvm::StructType>(fld->getType())) {
				Len = Builder->CreateExtractValue(fld, 0);
				fld = Builder->CreateExtractValue(fld, 1);
				array_type = llvm::dyn_cast<llvm::ArrayType>(fld->getType());
				if (auto len2 = llvm::dyn_cast<llvm::ConstantInt>(Len)) {
					errs() << "unexpected constant length: " << *len2 << '\n';
					len = len2->getZExtValue();
				}
			}
		if (!array_type || (!len && !Len)) {
			// if it's no array it must be a pointer - but then this would be an lvalue
			// and codegen_ref() above would have succeeded - so we should not get here
			errs() << Loc << ": internal compiler error - inconsistent array\n";
			abort();
		}
		auto FixedField = dynamic_cast<FixedArrayExprAST*>(Field.get());
		SourceLocation LenLoc = FixedField ? FixedField->LenLocs[0] : SourceLocation();
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
				if (!error_already_printed)
					errs() << IdxLoc << ": index (" << (ssize_t)i << ") out of range (0.."
					       << (len-1) << ")\n";
				return nullptr;
			}
			return Builder->CreateExtractValue(fld, i);
		}
	run_time_len:
		llvm::Value* ptr = StoreValue(fld_save, Field->ft);
		if (llvm::isa<llvm::StructType>(ptr->getType())) {
			Len = Builder->CreateExtractValue(ptr, 0);
			ptr = Builder->CreateExtractValue(ptr, 1);
		}
		CheckArrayIndex(idx, Len, Index->Loc, Index->ft->type_attr & A_signed);
		return Builder->CreateLoad(
			array_type->getElementType(),
			Builder->CreateGEP(array_type->getElementType(),
			                   Builder->CreatePointerCast(ptr, llvm_ptr_type),
			                   idx));
	} else {
		errs() << "cound not create code for index expression\n";
		return nullptr;
	}
}

std::pair<llvm::Type*,llvm::Value*> IndexExprAST::codegen_ref_(
	bool silent_fail, bool constref) {
	const_ref = constref;
	llvm::Value* NumElem = nullptr;
	if (!Field->ft || !Field->ft->type) {
		errs() << Field->Loc << ": unknown type\n";
		return { nullptr, nullptr };
	}
	if (auto a_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
		auto field_ref_ast = dynamic_cast<ReferencableExprAST*>(Field.get());
		if (!field_ref_ast) {
			if (silent_fail)
				return { a_type->getElementType(), nullptr };
			errs() << Field->Loc << ": cannot generate index expression with non-referencable object\n";
			return { nullptr, nullptr };
		}
		auto field_ref = field_ref_ast->codegen_ref(silent_fail, constref);
		if (!field_ref.second) {
			if (silent_fail && field_ref.first)
				return { llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)->getElementType(), nullptr };
			errs() << Index->Loc << ": connot generate field reference\n";
			return { nullptr, nullptr };
		}
		// field_ref_ast->codegen_ref() above has adjusted type by inserting const dimensions
		// reget array type
		a_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type);
		// and adjust our type accordingly
		ft->type = a_type->getElementType();
		llvm::Value* ElemSz = getAllocSize();
		llvm::Value* _Idx = Index->codegen();
		llvm::Value* Idx;
		if (auto arr_ty = llvm::dyn_cast<llvm::ArrayType>(_Idx->getType())) {
			if (arr_ty->getNumElements() == 1) {
				Idx = Builder->CreateExtractValue(_Idx, 0);
				if (Idx->getType()->isIntegerTy())
					if (Idx->getType() != llvm_size_type)
						Idx = Builder->CreateIntCast(Idx, llvm_size_type, false);
				goto idx_ok;
			}
		}
		errs() << Index->Loc << ": invalid vec index\n";
		return { nullptr, nullptr };
	idx_ok:
		// TODO: range check
		llvm::Value* Offset = Builder->CreateMul(Idx, ElemSz);
		llvm::Value* Ptr;
		unsigned n_struct_elem = 0;
		auto struct_ty = llvm::dyn_cast<llvm::StructType>(field_ref.second->getType());
		if (struct_ty) {
			n_struct_elem = struct_ty->getNumElements();
			Ptr = Builder->CreateExtractValue(field_ref.second, n_struct_elem - 1);
		} else
			Ptr = field_ref.second;
		if (Ptr->getType() != llvm_ptr_type) {
			errs() << Loc << ": internal error - cannot get array memory location\n";
			return { nullptr, nullptr };
		}
		Ptr = Builder->CreateIntToPtr(
			Builder->CreateAdd(
				Builder->CreatePtrToInt(Ptr, llvm_size_type), Offset), llvm_ptr_type);
		llvm::Type* new_struct_ty;
		unsigned struct_offs;
		if (!n_struct_elem)
			return { a_type->getElementType(), Ptr };
		if (a_type->getNumElements()) {
			// kepp dims from old ref but replace Ptr
			new_struct_ty = field_ref.second->getType();
			struct_offs = 0;
		} else {
			n_struct_elem--;
			struct_offs = 1;
			if (n_struct_elem <= 1) {
				if (!n_struct_elem) {
					errs() << Loc << ": internal error - inconsistent array reference\n";
					return { nullptr, nullptr };
				}
				return { a_type->getElementType(), Ptr };
			}
			// build new array reference descriptor
			std::vector<llvm::Type*> array_struct_types(n_struct_elem, llvm_size_type);
			array_struct_types[n_struct_elem-1] = llvm_ptr_type;
			new_struct_ty = llvm::StructType::get(Context, array_struct_types);
		}
		llvm::Value* new_struct = llvm::UndefValue::get(new_struct_ty);
		n_struct_elem--;
		for (unsigned i=0; i<n_struct_elem; i++)
			new_struct = Builder->CreateInsertValue(
				new_struct, Builder->CreateExtractValue(field_ref.second, i + struct_offs), i);
		new_struct = Builder->CreateInsertValue(new_struct, Ptr, n_struct_elem);
		return { a_type->getElementType(), new_struct };
	} else if (Field->ft->type == llvm_vec_type) {
		llvm::Value* Idx;
		llvm::Value* _Idx = Index->codegen();
		if (!_Idx)
			return { nullptr, nullptr };
		if (auto arr_ty = llvm::dyn_cast<llvm::ArrayType>(_Idx->getType())) {
			if (arr_ty->getNumElements() == 1) {
				Idx = Builder->CreateExtractValue(_Idx, 0);
				if (Idx->getType()->isIntegerTy())
					goto idx_ok_vec;
			}
		}
		errs() << Index->Loc << ": invalid vec index\n";
		return { nullptr, nullptr };
	idx_ok_vec:
		llvm::Value* StructAdr = nullptr;
		if (auto vec_struct_ref = dynamic_cast<LvalueExprAST*>(Field.get())) {
			llvm::Type* v_ty;
			std::tie(v_ty,StructAdr) = vec_struct_ref->codegen_ref(true);
		}
		llvm::Value* ptr;
		llvm::Value* size;
		if (StructAdr) {
			llvm::Value* ptr_adr = Builder->CreateStructGEP(llvm_vec_type, StructAdr, 0);
			ptr = Builder->CreateLoad(llvm_ptr_type, ptr_adr);
			llvm::Value* size_adr = Builder->CreateStructGEP(llvm_vec_type, StructAdr, 1);
			size = Builder->CreateLoad(llvm_size_type, size_adr);
		} else {
			llvm::Value* the_struct = Field->codegen();
			if (!the_struct) {
				errs() << Field->Loc << ": cannot generate field expression\n";
				return { nullptr, nullptr };
			}
			ptr = Builder->CreateExtractValue(the_struct, 0);
			size = Builder->CreateExtractValue(the_struct, 1);
		}
		llvm::Value* vec_elem_ptr = Builder->CreateGEP(ft->type, ptr, Idx);
		return { ft->type, vec_elem_ptr };
	} else if (auto a_type = llvm::dyn_cast<llvm::PointerType>(Field->ft->type)) {
		llvm::Value* Map = Field->codegen();
		if (!Map)
			return { nullptr, nullptr };
		llvm::Value* Key;
		llvm::Value* _Key = Index->codegen();
		if (!_Key)
			return { nullptr, nullptr };
		if (auto arr_ty = llvm::dyn_cast<llvm::ArrayType>(_Key->getType())) {
			if (arr_ty->getNumElements() == 1) {
				Key = Builder->CreateExtractValue(_Key, 0);
				if (Key->getType() == llvm_string_type)
					goto key_ok;
			}
		}
		errs() << Index->Loc << ": invalid map index\n";
		return { nullptr, nullptr };
	key_ok:
		const char* getter;
		if (Field->ft->elem_type[0].type == llvm_string_type) // string key type
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
		return { pointee_type, value };
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
			errs() << Loc << ": unable to infer common key/value types of list elements\n";
			ft = nullptr;
			return;
		}			
		auto ftpair = new_FullType(*key_ft, 0, 1); // reserve space for 1 additional FullType
		ftpair[1] = *val_ft;
		ft->elem_type = ftpair;
	}
}

SetExprAST::SetExprAST(SourceLocation Loc, volvoxc::FullType* set_ft, std::vector<std::unique_ptr<ExprAST>> _Elements) :
	ListExprAST(Loc, std::move(_Elements), set_ft)
{
	if (!ft->elem_type) {
		auto val_ft = getCommonType(Elements);
		if (!val_ft) {
			errs() << Loc << ": unable to infer common value type of list elements\n";
			ft = nullptr;
			return;
		}
		ft->elem_type = val_ft;
	}
}

// in interactive JIT mode addresses of globals change from call to call, so
// a "string constant" has to be stored at a fixed place on heap to emulate
// the behaviour we have in other modes
llvm::Value* createJITStringConst(const char* str, size_t Len, const llvm::Twine &Name) {
	char* v_str = __cstr2volvoxstr(str, Len, false);
	size_t new_l = *(size_t*)v_str;
	const char* new_cstr = volvox2cstr(v_str);
	jit_string_consts.push_back(new_cstr);
	llvm::Constant* iadr = getSize((uintptr_t)v_str);
	llvm::Value* strptr = Builder->CreateIntToPtr(iadr, llvm_ptr_type);
	llvm::Value* res = llvm::UndefValue::get(llvm_string_type);
	return Builder->CreateInsertValue(res, strptr, 0);
}

llvm::Value* createStringConst(const char* str, size_t Len, const llvm::Twine &Name) {
	if (!str)
		return nullptr;
	if (comp_mode == comp_jit)
		return createJITStringConst(str, Len, Name);
	char* stra;
	char* tmpres;
	size_t l_alloc;
	cstr2volvoxstr_l(tmpres, l_alloc, stra, str, alloca, Len, 0);
	auto llvmstr = llvm::ConstantDataArray::getString(Context, llvm::StringRef(stra, l_alloc), false);
	auto GV = new llvm::GlobalVariable(*TheModule, llvmstr->getType(), true, llvm::GlobalValue::PrivateLinkage,
	                                   llvmstr, Name, nullptr, llvm::GlobalVariable::NotThreadLocal, 0);
	GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
	GV->setAlignment(llvm::Align(target_bytes));
	llvm::Value* strptr = Builder->CreateIntToPtr(
		Builder->CreateAdd(
			Builder->CreatePtrToInt(GV, llvm_size_type),
			getSize(l_alloc - 2*target_bytes)), llvm_ptr_type);
	llvm::Value* res = llvm::UndefValue::get(llvm_string_type);
	return Builder->CreateInsertValue(res, strptr, 0);
}

llvm::Value* createJITCStringConst(const char* str) {
	const char* new_cstr = strdup(str);
	jit_string_consts.push_back(new_cstr);
	llvm::Constant* iadr = getSize((uintptr_t)new_cstr);
	return Builder->CreateIntToPtr(iadr, llvm_ptr_type);
}

llvm::Value* createCStringConst(const char* str) {
	if (!str)
		return nullptr;
	if (jit_repl)
		return createJITCStringConst(str);
	return Builder->CreateGlobalString(str, "", 0, TheModule.get());
}

llvm::Value* InterpStrLitExprAST::codegen_raw(llvm::Value* target) {
	unsigned n_inter = interpolations.size();
	auto interface_array_t = llvm::ArrayType::get(llvm_interface_type, n_inter);
	auto int_array_t = llvm::ArrayType::get(llvm_int_type, n_inter);
	auto str_array_t = llvm::ArrayType::get(llvm_ptr_type, n_inter+1);
	llvm::Value* InterfaceStore = CreateEntryBlockAlloca(interface_array_t);
	llvm::Value* WidthStore = CreateEntryBlockAlloca(int_array_t);
	llvm::Value* PrecisionStore = CreateEntryBlockAlloca(int_array_t);
	llvm::Value* FlagsStore = CreateEntryBlockAlloca(int_array_t);
	llvm::Value* StringStore = CreateEntryBlockAlloca(str_array_t);
	llvm::Value* volvox_var_array_ref = llvm::UndefValue::get(llvm_va_arg_ref_type);
	volvox_var_array_ref = Builder->CreateInsertValue(volvox_var_array_ref, getSize(n_inter), 0);
	volvox_var_array_ref = Builder->CreateInsertValue(volvox_var_array_ref, InterfaceStore, 1);
	std::vector<llvm::Value*> values = {
		WidthStore, PrecisionStore, FlagsStore, StringStore, volvox_var_array_ref };
	for (unsigned idx=0; ; idx++) {
		llvm::Value* interface_val_adr = Builder->CreateGEP(llvm_interface_type, InterfaceStore, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), idx));
		llvm::Value* width_val_adr = Builder->CreateGEP(llvm_int_type, WidthStore, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), idx));
		llvm::Value* precision_val_adr = Builder->CreateGEP(llvm_int_type, PrecisionStore, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), idx));
		llvm::Value* flags_val_adr = Builder->CreateGEP(llvm_int_type, FlagsStore, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), idx));
		llvm::Value* string_val_adr = Builder->CreateGEP(llvm_ptr_type, StringStore, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), idx));
		llvm::Value* str_val;
		if (str_parts[idx]) {
			str_val = createCStringConst(str_parts[idx]);
		}
		else
			str_val = llvm::ConstantPointerNull::get(llvm_ptr_type);
		Builder->CreateStore(str_val, string_val_adr);
		if (idx >= n_inter)
			break;
		auto interface_expr = std::make_unique<InterfaceExprAST>(std::move(std::get<0>(interpolations[idx])));
		auto inter_val = interface_expr->codegen_raw(interface_val_adr);
		if (!inter_val) {
			errs() << interface_expr->Loc << ": cannot generate interpolation value\n";
			return nullptr;
		}
		llvm::Value* field_width;
		if (std::get<1>(interpolations[idx])) {
			std::get<1>(interpolations[idx])->desired_type = llvm_int_type;
			field_width = std::get<1>(interpolations[idx])->codegen();
			if (!field_width) {
				errs() << std::get<1>(interpolations[idx])->Loc << ": cannot generate width value\n";
				return nullptr;
			}
		} else
			field_width = llvm::Constant::getNullValue(llvm_int_type);
		Builder->CreateStore(field_width, width_val_adr);
		llvm::Value* precision;
		if (std::get<2>(interpolations[idx])) {
			std::get<2>(interpolations[idx])->desired_type = llvm_int_type;
			precision = std::get<2>(interpolations[idx])->codegen();
			if (!precision) {
				errs() << std::get<2>(interpolations[idx])->Loc << ": cannot generate precision value\n";
				return nullptr;
			}
		} else
			precision = llvm::ConstantInt::getSigned(llvm_int_type, -1);
		Builder->CreateStore(precision, precision_val_adr);
		llvm::Value* flags = llvm::ConstantInt::get(llvm_int_type, std::get<3>(interpolations[idx]));
		Builder->CreateStore(flags, flags_val_adr);
	}
	std::string volvox_sprt = "__builtin_sprint";
	auto sprt_proto = (*lex.findProtos(volvox_sprt))[0].get();
	if (!sprt_proto) {
		errs() << Loc << ": cannot find function " << volvox_sprt << "()\n";
		return nullptr;
	}
	auto sprt_fn = getFunction(sprt_proto);
	llvm::Value* result = Builder->CreateCall(sprt_proto->FT, sprt_fn, values);
	return handle(target, result, Loc, ft);
}
