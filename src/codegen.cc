#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"
#include "../lib/str.h"

//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::DIBuilder> DBuilder;
bool inside_function = false;
extern llvm::ExitOnError ExitOnErr;
const char* last_shadow_saver;
const char* last_shadow_restorer;

inline static llvm::Value* CheckTailCall(llvm::Value* V) {
	if (auto C = llvm::dyn_cast<llvm::CallInst>(V))
		C->setTailCall();
	return V;
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

static llvm::DISubroutineType *CreateFunctionType(volvoxc::FullType* RetType, std::vector<volvoxc::FullType*>& ArgTypes, llvm::DIFile *Unit) {
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

std::pair<llvm::Function*, PrototypeAST*> getFunction(std::string Name) {
	auto FI = FunctionProtos.find(Name);
	if (FI == FunctionProtos.end())
		return { nullptr, nullptr };
	// else
	// 	for (auto FF = FI; FF != FunctionProtos.end(); ++FF)
	// 		errs() << "Function " << FF->second->Name << '\n';
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

llvm::Value *LiteralExprAST::codegen_raw() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	switch (ft->type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		return llvm::ConstantInt::get(Context, llvm::APInt(ft->type->getIntegerBitWidth(), Val.Uint, ft->type_attr & A_signed));
	case llvm::Type::HalfTyID:
	case llvm::Type::BFloatTyID:
		errs() << "Sorry, 16 bit floats are not supported, yet\n";
		return nullptr;
		// passthrough to 32 bit float for now - but expect problems...
	case llvm::Type::FloatTyID:
		return llvm::ConstantFP::get(Context, llvm::APFloat((float)Val.Float));
	case llvm::Type::DoubleTyID:
		return llvm::ConstantFP::get(Context, llvm::APFloat(Val.Float));
	case llvm::Type::PointerTyID:
		if (ft->type_attr & A_signed)
			return Builder->CreateIntToPtr(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), Val.Uint, false), llvm::Type::getInt8PtrTy(Context));
		else
			return Builder->CreateGlobalStringPtr(Val.Str, "", 0, TheModule.get());
	default:
		errs() << "internal compiler error: unhandled literal type " << *ft->type << "\n";
		return nullptr;
	}
}

llvm::Value *ListExprAST::codegen_raw() {
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
		return V;
	} else {
		return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	}
}

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
			else
				ini = Builder->CreateInsertValue(ini, Elem_convs[iter_idx++](List->Elements[idx]->codegen_raw()), idx, "arrlitval");
	return ini;
}

llvm::Value* FixedArrayExprAST::codegen_raw() {
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
	iter_idx = 0;
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

llvm::Value* LvalueExprAST::codegen_raw() {
	auto V = codegen_ref();
	// Load the value.
	return Builder->CreateLoad(V.first, V.second, Name.c_str());
}

std::pair<llvm::Type*,llvm::Value*> VariableExprAST::codegen_ref(bool silent_fail) {
	if (!full_var.first) {
		errs() << "Unknown variable name " << Name << "\n";
		return { nullptr, nullptr };
	}
	llvm::Value* V;
	llvm::Type* storage_type;
	if (full_var.second) { // global variable
		V = TheModule->getGlobalVariable(Name, true);
		if (!V) {
			V = new llvm::GlobalVariable(*TheModule, full_var.first->storage_type,
			                             false, llvm::GlobalValue::ExternalLinkage,
			                             nullptr, Name, nullptr,
			                             llvm::GlobalVariable::GeneralDynamicTLSModel,
			                             0, true);
		}
		storage_type = full_var.first->storage_type;
	} else {
		V = full_var.first->val;
		storage_type = ft->type; // full_var.first->val->getType() - deprecated;
		if (storage_type->isFunctionTy())
			storage_type = storage_type->getPointerTo();
	}
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	return { storage_type, V };
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
	} else {
		errs() << "depth: " << depth << " Internal error!\n";
		abort();
	}
}

static std::pair<llvm::Value*,llvm::Value*> StoreArrayValue(llvm::Value* val, llvm::Type* elem_type, std::vector<llvm::Value*>& Dims, const llvm::Twine &Name = "") {
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
		llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
		llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
		                       TheFunction->getEntryBlock().begin());
		ArrayAlloc = TmpB.CreateAlloca(alloc_arr_type, nullptr, Name);
		ArrayPtr = Builder->CreateBitCast(ArrayAlloc, elem_type->getPointerTo());
	} else {
		ArrayAlloc = Builder->CreateAlloca(elem_type, Len, Name);
		ArrayPtr = Builder->CreateBitCast(ArrayAlloc, elem_type->getPointerTo());
	}
	// TODO: Insert run time check that initialization values fit into allocation size
	StoreArray(ArrayPtr, ArrData, Sizes, 0);
	return { ArrayAlloc, ArrayPtr };
}

static llvm::Value* getInterfaceArrayOrStoreValue(llvm::Value* val, llvm::ArrayType* array_type, llvm::ArrayType* expected_array_type = nullptr, bool do_store = false, const llvm::Twine &Name = "") {
	if (!expected_array_type)
		expected_array_type = MakeInterfaceArrayType(array_type);
	std::vector<llvm::Value*> Dims = {};
	std::vector<llvm::Value*> returnDims = {};
	llvm::Type* elem_type;
	unsigned idx = 0;
	unsigned level = 0;
	while (true) {
		if (!expected_array_type) {
			errs() << "getInterfaceArrayOrStoreValue(): internal error\n";
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

static llvm::Value* StoreValue(llvm::Value* val, volvoxc::FullType* ft,
                               llvm::Type* expected_type = nullptr, const llvm::Twine &Name = "") {
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
		llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
		llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
		                       TheFunction->getEntryBlock().begin());
		llvm::AllocaInst* Alloca = TmpB.CreateAlloca(val->getType(), nullptr, Name);
		Builder->CreateStore(val, Alloca);
		return Alloca;
	}
}

inline llvm::Value* getInterfaceArrayValue(llvm::Value* val, llvm::ArrayType* array_type, llvm::ArrayType* expected_array_type = nullptr) {
	return getInterfaceArrayOrStoreValue(val, array_type, expected_array_type, false);
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

llvm::Value* IndexExprAST::codegen_raw() {
	// first try to get a reference to the element ...
	auto V = codegen_ref(true);
	// ... and load the value.
	if (V.second) // we have a reference and need a value - just load it...
		return Builder->CreateLoad(V.first, V.second, Name.c_str());
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
	errs() << "elem_typex0: " <<  *elem_typex << '\n';
	if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(elem_typex)) {
		auto subtype = array_type->getElementType();
		errs() << "array_type: " <<  *array_type << " Subtype: " << *subtype
		       << " idx_idx: " << idx_idx << " IdxSz: " << Idxs.size() << '\n';
		uint64_t n_elem = array_type->getNumElements();
		auto sub_descr = getMLIdxOffset(subtype, Idxs, Dims, idx_idx + 1, n_elem ? dim_idx : dim_idx + 1);
		auto const_elem_size = std::get<0>(sub_descr);
		auto var_elem_size = std::get<1>(sub_descr);
		auto offset = std::get<2>(sub_descr);
		llvm::Value* cur_Offset = nullptr;
		if (idx_idx < Idxs.size()) {
			cur_Offset = Idxs[idx_idx];
			errs() << "Cur offset: " << *cur_Offset << '\n';
			if (const_elem_size != 1)
				cur_Offset = Builder->CreateMul(Builder->getInt64(const_elem_size), cur_Offset);
			if (var_elem_size)
				cur_Offset = Builder->CreateMul(cur_Offset, var_elem_size);
			if (n_elem)
				const_elem_size *= n_elem;
			else {
				auto dim = Builder->CreateExtractValue(Dims, dim_idx++);
				if (dim_idx > num_dims_to_strip_from_val)
					num_dims_to_strip_from_val = dim_idx;
				if (var_elem_size)
					var_elem_size = Builder->CreateMul(dim, var_elem_size);
				else
					var_elem_size = dim;
			}
		}
		if (cur_Offset && offset)
			cur_Offset = Builder->CreateAdd(cur_Offset, offset);
		else if (offset)
			cur_Offset = offset;
		errs() << "Offset: " << *cur_Offset << '\n';
		return { const_elem_size, var_elem_size, cur_Offset };
	} else {
		uint64_t elem_size = TheModule->getDataLayout().getTypeAllocSize(elem_typex);
		ml_elem_type = elem_typex;
		errs() << "elem_size: " << elem_size << " ElemType: " << *ml_elem_type << '\n';
		return { elem_size, nullptr, nullptr };
	}
}

// Idxs, val
llvm::Value* IndexExprAST::codegen_ref0(std::vector<llvm::Value*>& Idxs, llvm::Type*& ml_field_type) {
	llvm::Value* res = nullptr;
	if (auto fieldidxexpr = dynamic_cast<IndexExprAST*>(Field.get())) {
		errs() << "type before0: " << *Field->ft->type << '\n';
		auto fieldval = fieldidxexpr->codegen_ref0(Idxs, ml_field_type);
		errs() << "type after0: " << *Field->ft->type << '\n';
		if (!fieldval)
			return nullptr;
		res = fieldval;
	} else if (auto lval = dynamic_cast<LvalueExprAST*>(Field.get())) {
		errs() << "type before: " << *Field->ft->type << '\n';
		auto elem = lval->codegen_ref();
		ml_field_type = Field->ft->type;
		errs() << "type after: " << *ml_field_type << '\n';
		res = elem.second;
	}
	if (res)
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
			Idxs.push_back(idx);
		} else {
			errs() << "internal compiler error\n";
			abort();
		}
	return res;
}		

std::pair<llvm::Type*,llvm::Value*> IndexExprAST::codegen_ref(bool silent_fail) {
	llvm::Value* field_ptr;
	llvm::Value* idx;
	uint64_t num_elem;
	llvm::Value* NumElem = nullptr;
	if (!Field->ft || !Field->ft->type) {
		errs() << Field->Loc << ": unknown type\n";
		return { nullptr, nullptr };
	}
	if (auto a_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
		std::vector<llvm::Value*> Idxs;
		llvm::Type* ml_field_type = nullptr;
		auto LV = codegen_ref0(Idxs, ml_field_type);
		if (LV && ml_field_type)
			errs() << "LV0: " << *LV << " Type: " << *ml_field_type << '\n';
		if (!LV) {
			if (!silent_fail)
				errs() << "LHS of index expression must be an lvalue\n";
			return { a_type->getElementType(), nullptr };
		}
		errs() << "LV: " << *LV << " Type: " << *ml_field_type << '\n';
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
			Ptr = Builder->CreateIntToPtr(Builder->CreateAdd(Builder->CreatePtrToInt(Ptr, llvm::Type::getInt64Ty(Context)), offset), Ptr->getType());
		if (!n_var_dims)
			return { ml_elem_type, Ptr };
		std::vector<llvm::Type*> new_struct_el(n_var_dims + 1, llvm::Type::getInt64Ty(Context));
		new_struct_el[n_var_dims] = Ptr->getType();
		llvm::Type* new_struct_type = llvm::StructType::get(Context, new_struct_el);
		llvm::Value* res = llvm::UndefValue::get(new_struct_type);
		for (int j = 0; j < n_var_dims; j++)
			res = Builder->CreateInsertValue(res, Builder->CreateExtractValue(LV, j + num_dims_to_strip_from_val), j);
		res = Builder->CreateInsertValue(res, Ptr, num_dims_to_strip_from_val);
		return { ml_elem_type, res };
	} else {
		errs() << "LHS of index expression must be an array (or map) " << *ft->type << "\n";
		return { nullptr, nullptr };
	}
	errs() << Loc << ": error generating index expr\n";
	// return { nullptr, nullptr };
	// return { elem_type, Builder->CreateGEP(elem_type, field_ptr, idx) };
}

llvm::Value* FunctionExprAST::codegen_raw() {
	if (auto F = TheModule->getFunction(Name)) {
		return F;
	}
	return ft->proto->codegen();
}

llvm::Value* InterfaceExprAST::codegen_raw() {
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
			// if it's an rvalue we have to store it on stack to get a reference
			llvm::Value* array = expr->codegen();
			val = StoreValue(array, expr->ft, MakeInterfaceArrayType(array_type));
		}
	} else {
		// pass by value
		val = expr->codegen();
	}
	llvm::Constant* rttype_ptr = getRtType(expr->ft);
	llvm::Type* real_type = expr->ft->type;
	std::vector<llvm::Type*> types = { rttype_ptr->getType(), val->getType() };
	llvm::Type* struct_type = llvm::StructType::get(Context, types);
	llvm::Value* the_struct = llvm::UndefValue::get(struct_type);
	the_struct = Builder->CreateInsertValue(the_struct, rttype_ptr, 0);
	the_struct = Builder->CreateInsertValue(the_struct, val, 1);
	return the_struct;
}

llvm::Value *UnaryExprAST::codegen_raw() {
	if (Opcode[0] == '&') {
		if (auto V = dynamic_cast<LvalueExprAST*>(Operand.get())) {
			return V->codegen_ref().second;
		} else {
			auto operand = Operand->codegen();
			// the above and the below lines have to be separated because 'codegen()' may change 'ft'
			// and the order of function arg evaluation is "unspecified" in C++ ISO 14882
			return StoreValue(operand, Operand->ft);
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
			return OperandV;
		case '-':
			return Builder->CreateNeg(OperandV, "negtmp");
		case '!':
			return Builder->CreateNot(OperandV, "nottmp");
		default:
			errs() << "unary operator '" << Opcode[0] << "' undefined for integers";
			return nullptr;
		}
	default:
		auto F = getFunction(std::string("unary") + Opcode);
		if (!F.first) {
			errs() << "Unknown unary operator";
			return nullptr;
		}
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
	comparison_op
};

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
		llvm::GlobalValue::LinkageTypes link_type = (is_pub || comp_mode == comp_jit) ?
			llvm::GlobalValue::ExternalLinkage :
			llvm::GlobalValue::InternalLinkage;
		if (auto initializer = llvm::dyn_cast<llvm::Constant>(convertedVal)) {
			llvm::GlobalVariable* GV;
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(expr->RHS->ft->type)) {
				if (auto ini_array_type = llvm::dyn_cast<llvm::ArrayType>(initializer->getType()))
					initializer = llvm::dyn_cast<llvm::Constant>(expandArrayInitializer(initializer, ini_array_type, array_type));
				if (!initializer) {
					errs() << expr->RHS->Loc << ": non-const initializer for global variable\n";
					return nullptr;
				}
			}
			if (comp_mode == comp_dbg) {
				// Create a debug descriptor for the variable.
				DBuilder->createGlobalVariableExpression(
					SP, varname, varname, Unit, expr->Loc.Line, type_table.get_diType(type, is_signed), false);
			}
			GV = new llvm::GlobalVariable(*TheModule, initializer->getType(),
			                              false, link_type,
			                              initializer, varname, nullptr,
			                              llvm::GlobalVariable::GeneralDynamicTLSModel);
			volvoxc::FullType ft = *expr->RHS->ft;
			ft.type = type;
			ft.type_attr = is_signed ? 1U : 0U;
			FullVar fv = {
				.storage_type = initializer->getType(),
				.ft = ft,
			};
			globals_table.insert(varname, fv);
			if (comp_mode == comp_jit) {
				llvm::Type* V_type = initializer->getType();
				size_t storage_sz = TheJIT->getDataLayout().getTypeStoreSize(V_type);
#if LLVM_VERSION_MAJOR >= 12
				ExitOnErr(TheJIT->addModule(
					          llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context)));
#else
				TheJIT->addModule(std::move(TheModule));
#endif
				InitializeModuleAndPassManager();

				auto V = TheModule->getGlobalVariable(varname, true);
				if (!V) {
					V = new llvm::GlobalVariable(*TheModule, V_type,
					                             false, link_type,
					                             nullptr, varname, nullptr,
					                             llvm::GlobalVariable::GeneralDynamicTLSModel,
					                             0, true);
				}
				auto sz_const = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), storage_sz);
				std::string shadow_fn_name = "new_global_var_shadow";
				auto shadow_fn = getFunction(shadow_fn_name);
				std::vector<llvm::Value*> ArgsS = { V, sz_const };
				llvm::FunctionType* uintptr_fn_t = llvm::FunctionType::get(llvm::Type::getInt64Ty(Context), {}, false);
				std::string anon_name = "__anon_shadow";
				llvm::Function* Fshadow = llvm::Function::Create(uintptr_fn_t, llvm::Function::ExternalLinkage, anon_name, TheModule.get());
				llvm::BasicBlock* BB = llvm::BasicBlock::Create(Context, "entry", Fshadow);
				Builder->SetInsertPoint(BB);
				Builder->CreateRet(CheckTailCall(Builder->CreateCall(shadow_fn.second->FT, shadow_fn.first, { V, sz_const }, "callshadow")));
				verifyFunction(*Fshadow);
				TheFPM->run(*Fshadow);
				if (dump_IR >= 3) {
					errs() << "Read function definition:\n";
					Fshadow->print(errs());
					errs() << "\n";
				}

#if LLVM_VERSION_MAJOR >= 12
				// Create a ResourceTracker to track JIT'd memory allocated to our
				// anonymous expression -- that way we can free it after executing.
				auto RT = TheJIT->getMainJITDylib().createResourceTracker();
				auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context);
				ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
#else
				// JIT the module containing the anonymous expression, keeping a handle so
				// we can free it later.
				auto H = TheJIT->addModule(std::move(TheModule));
#endif
				InitializeModuleAndPassManager();
#if LLVM_VERSION_MAJOR >= 12
				auto ExprSymbol = ExitOnErr(TheJIT->lookup(anon_name));
#define UNWRAP(x) (x)
#else
				auto ExprSymbol = TheJIT->findSymbol(anon_name);
				assert(ExprSymbol && "Function not found");
#define UNWRAP(x) cantFail(x)
#endif
				uintptr_t (*PTR)() = (uintptr_t (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
				auto adrShadow = PTR();
#if LLVM_VERSION_MAJOR >= 12
				// Delete the anonymous expression module from the JIT.
				ExitOnErr(RT->remove());
#else
				// Delete the anonymous expression module from the JIT.
				TheJIT->removeModule(H);
#endif
				auto saver = std::string("__") + varname + "_saver";
				auto restorer = std::string("__") + varname + "_restorer";
				auto llvmGVadr = llvm::dyn_cast<llvm::Constant>(Builder->CreateIntToPtr(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), adrShadow, false), llvm::Type::getInt8PtrTy(Context)));
				auto memcpy_proto = getFunction("memcpy");
				V = TheModule->getGlobalVariable(varname, true);
				if (!V) {
					V = new llvm::GlobalVariable(*TheModule, V_type,
					                             false, llvm::GlobalValue::ExternalLinkage,
					                             nullptr, varname, nullptr,
					                             llvm::GlobalVariable::GeneralDynamicTLSModel,
					                             0, true);
				}
				llvm::FunctionType* void_fn_t = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {}, false);

				llvm::Function* Fsaver = llvm::Function::Create(void_fn_t, llvm::Function::ExternalLinkage, saver, TheModule.get());
				BB = llvm::BasicBlock::Create(Context, "entry", Fsaver);
				Builder->SetInsertPoint(BB);
				std::vector<llvm::Value*> ArgsV = { llvmGVadr, V, sz_const };
				Builder->CreateCall(memcpy_proto.second->FT, memcpy_proto.first, ArgsV, "callmcpy");
				if (last_shadow_saver) {
					auto last_saver = getFunction(last_shadow_saver);
					Builder->CreateRet(CheckTailCall(Builder->CreateCall(last_saver.second->FT, last_saver.first, std::vector<llvm::Value*>(), "callold")));
				} else {
					Builder->CreateRetVoid();
				}
				verifyFunction(*Fsaver);
				TheFPM->run(*Fsaver);
				if (dump_IR >= 3) {
					errs() << "Read function definition:\n";
					Fsaver->print(errs());
					errs() << "\n";
				}
				auto saverProto = std::make_unique<PrototypeAST>(CurLoc, saver, std::vector<std::string>());
				last_shadow_saver = saverProto->Name.c_str();
				FunctionProtos[saver] = std::move(saverProto);

				llvm::Function* Frestorer = llvm::Function::Create(void_fn_t, llvm::Function::ExternalLinkage, restorer, TheModule.get());
				BB = llvm::BasicBlock::Create(Context, "entry", Frestorer);
				Builder->SetInsertPoint(BB);
				ArgsV = { V, llvmGVadr, sz_const };
				Builder->CreateCall(memcpy_proto.second->FT, memcpy_proto.first, ArgsV, "callmcpy");
				if (last_shadow_restorer) {
					auto last_restorer = getFunction(last_shadow_restorer);
					Builder->CreateRet(CheckTailCall(Builder->CreateCall(last_restorer.second->FT, last_restorer.first, std::vector<llvm::Value*>(), "callold")));
				} else {
					Builder->CreateRetVoid();
				}
				verifyFunction(*Frestorer);
				TheFPM->run(*Frestorer);
				if (dump_IR >= 3) {
					errs() << "Read function definition:\n";
					Frestorer->print(errs());
					errs() << "\n";
				}
				auto restorerProto = std::make_unique<PrototypeAST>(CurLoc, restorer, std::vector<std::string>());
				last_shadow_restorer = restorerProto->Name.c_str();
				FunctionProtos[restorer] = std::move(restorerProto);
			}
		} else {
			goto nonconst;
		}
	} else {
		errs() << "Could not generate assigned expression\n";
	}
	return nullptr;
nonconst:
	errs() << "global variable must be initialized with compile time const\n";
	return nullptr;
}

llvm::Value *BinaryExprAST::codegen_raw() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	dbgs() << "binary desired type: ";
	if (desired_type)
		dbgs() << *desired_type;
	dbgs() << '\n';
	bool is_bool = false;
	OpKind kind;
	switch (Op[1]) {
	case '\0':
		switch (Op[0]) {
		case '=':
			kind = assign_op;
			is_bool = desired_type == llvm::Type::getInt1Ty(Context);
			break;
		case '>':
		case '<':
			kind = comparison_op;
			is_bool = true;
			break;
		default:
			kind = other_op;
		}
		break;
	case '=':
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
		case '>':
		case '<':
		case '!':
		case '=':
			kind = comparison_op;
			is_bool = true;
			break;
		default:
			kind = other_op;
		}
		break;
	default:
		kind = other_op;
	}
	// Special assign-like ops because we don't want to emit the LHS as an expression.
	// assign op '=' is a comparison (not an assignment) when a boolean result is expected
	if (kind == decl_assign_op || kind == assign_op && !is_bool) {
		const char* varname = nullptr;
		// Assignment requires the LHS to be an identifier.
		// This assume we're building without RTTI because LLVM builds that way by
		// default.  If you build LLVM with RTTI this can be changed to a
		// dynamic_cast for automatic error checking.
		LvalueExprAST *LHSE = dynamic_cast<LvalueExprAST*>(LHS.get());
		if (!LHSE) {
			errs() << "destination of '=' must be an lvalue";
			return nullptr;
		}
		RHS->desired_type = LHSE->ft->type;
		RHS->desired_type_attr = LHSE->ft->type_attr;
		// Codegen the RHS.
		llvm::Value* Val = RHS->codegen();
		if (!Val)
			return nullptr;
		if (conv.compat.RHS)
			Val = conv.compat.RHS(Val);
		// Look up the name.
		if (auto RegularVar = dynamic_cast<VariableExprAST*>(LHS.get())) {
			varname = RegularVar->getName().c_str();
			FullVar* full_var = RegularVar->full_var.first;
			if (!full_var)
				goto not_found;
		}
		if (kind == decl_assign_op) {
			errs() << "cannot initialize existing variable";
			return nullptr;
		} else {
			auto Variable = LHSE->codegen_ref();
			auto OldVal = Builder->CreateLoad(Variable.first, Variable.second);
			Builder->CreateStore(Val, Variable.second);
			return OldVal;
		}
	not_found:
		if (kind != decl_assign_op) {
			errs() << "unknown variable name '" << varname << "'\n";
			return nullptr;
		}
		// variable declaration
		if (inside_function) {
			auto type_descr = MakeType(RHS->ft->type, RHS->ft->type_attr & A_signed, RHS->is_unknown_type);
			llvm::Type* type = std::get<0>(type_descr);
			auto conversion = std::get<1>(type_descr);
			bool is_signed = std::get<2>(type_descr);
			auto convertedVal = conversion(Val);
			FullVar* entry = locals_table.back()[varname];
			// Entry has already been created by parser but we might have to adjust the type of the new
			// variable after RHS->codegen() has been run (e.g. array dimensions might only be known by now)
			entry->ft.type = type;
			if (is_signed)
				entry->ft.type_attr |= A_signed;
			else
				entry->ft.type_attr &= ~A_signed;
			auto Alloca = StoreValue(convertedVal, &entry->ft, nullptr, varname);
			entry->val = Alloca;
			if (comp_mode == comp_dbg) {
				// Create a debug descriptor for the variable.
				llvm::DILocalVariable *D = DBuilder->createAutoVariable(
					SP, varname, Unit, LHS->Loc.Line, type_table.get_diType(type, is_signed),
					true);

				DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
										llvm::DILocation::get(SP->getContext(), LHS->Loc.Line, 0, SP),
										Builder->GetInsertBlock());
			}
			ft->type = llvm::Type::getVoidTy(Context);
			return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
		} else {
			return Val;
		}
		return Val;
	}
	llvm::Value* result;
	std::function<llvm::Value*(llvm::Value*)> convLHS = nullptr;
	std::function<llvm::Value*(llvm::Value*)> convRHS = nullptr;
	llvm::Type* OperandType;
	bool OperandSigned;
	if (!conv.compat.LHS && !conv.compat.RHS && !conv.ideal.LHS && !conv.ideal.RHS) {
		OperandType = LHS->ft->type;
		OperandSigned = LHS->ft->type_attr & A_signed;
		goto no_conversion;
	}
	OperandType = conv.compat.res_type ? conv.compat.res_type : conv.ideal.res_type;
	OperandSigned = conv.compat.res_type ? !(!(conv.compat.res_attr & A_signed)) : !(!(conv.ideal.res_attr & A_signed));
	if (is_bool) {
		if (desired_type) {
			if (desired_type != llvm_bool_type) {
				errs() << Loc << "boolean expression cannot be used where " << *desired_type << " is expected\n";
				return nullptr;
			}
		}
		if (conv.compat.err_msg)
			return AutoErr(Loc, LHS->ft->type, RHS->ft->type, LHS->ft->type_attr, RHS->ft->type_attr, conv.compat.err_msg);
		LHS->desired_type = RHS->desired_type = conv.compat.res_type;
		LHS->desired_type_attr = RHS->desired_type_attr = conv.compat.res_attr;
		convLHS = conv.compat.LHS;
		convRHS = conv.compat.RHS;
	} else {
		if (desired_type) {
			auto ana_default = conv.compat.err_msg ? std::pair<bool, bool>{ false, false } : analyze_types({ conv.compat.res_type, conv.compat.res_attr }, { desired_type, desired_type_attr });
			auto ana_ideal = analyze_types({ conv.ideal.res_type, conv.ideal.res_attr }, { desired_type, desired_type_attr });
			if (ana_ideal.second) {
				LHS->desired_type = RHS->desired_type = conv.ideal.res_type;
				LHS->desired_type_attr = RHS->desired_type_attr = conv.ideal.res_attr;
				convLHS = conv.ideal.LHS;
				convRHS = conv.ideal.RHS;
			} else {
				LHS->desired_type = RHS->desired_type = desired_type;
				LHS->desired_type_attr = RHS->desired_type_attr = desired_type_attr;
				if (conv.compat.res_type) {
					convLHS = conv.compat.LHS;
					convRHS = conv.compat.RHS;
				}
			}
		} else {
			if (conv.compat.err_msg)
				return AutoErr(Loc, LHS->ft->type, RHS->ft->type, LHS->ft->type_attr, RHS->ft->type_attr, conv.compat.err_msg);
			LHS->desired_type = RHS->desired_type = conv.compat.res_type;
			LHS->desired_type_attr = RHS->desired_type_attr = conv.compat.res_attr;
			convLHS = conv.compat.LHS;
			convRHS = conv.compat.RHS;
		}
	}
	if (verbosity >= 4) {
		if (LHS->desired_type) errs() << "LHS desired_type: " << *LHS->desired_type << ' ';
		if (RHS->desired_type) errs() << "RHS desired_type: " << *RHS->desired_type << ' ';
		errs() << "expr: ";
		if (desired_type)
			errs() << *desired_type << '\n';
		else
			errs() << "none\n";
	}
no_conversion:
	llvm::Value *L, *R;
	if (convLHS) {
		L = LHS->codegen_raw();
		if (!L)
			return nullptr;
		L = convLHS(L);
	}
	else
		L = LHS->codegen();
	if (convRHS) {
		R = RHS->codegen_raw();
		if (!R)
			return nullptr;
		R = convRHS(R);
	}
	else
		R = RHS->codegen();
	if (!L || !R)
		return nullptr;
	// for comparisons ExprAST.type is bool, but we have to look at the operands that are in desired
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
		}
		break;
	case '&':
		switch(typeclass) {
		case is_int:
			result = Builder->CreateAnd(L, R, "andtmp");
			break;
		default:
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
		}
		break;
	case '|':
		switch(typeclass) {
		case is_int:
			result = Builder->CreateOr(L, R, "ortmp");
			break;
		default:
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
		}
		break;
	case '^':
		// TODO: use '^' for pow()
		switch(typeclass) {
		case is_int:
			result = Builder->CreateXor(L, R, "xortmp");
			break;
		default:
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
			}
		} else {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateNot(Builder->CreateXor(L, R, "xortmp"), "nxortmp");
				break;
			default:
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
			errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
			}
		} else if (Op[1] == '<') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateShl(L, R, "remtmp");
				break;
			default:
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
			}
			break;
		} else if (Op[1] == '<') {
			switch(typeclass) {
			case is_int:
				result = Builder->CreateXor(L, R, "remtmp");
				break;
			default:
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
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
				errs() << "Operator '" << Op << "' cannot be used for type " << *OperandType << "\n";
			}
		}
		break;
	}
	if (result) {
		if (!is_bool && desired_type) {
			auto conv = getConv(ft->type, desired_type, ft->type_attr, desired_type_attr, Loc, true, is_unknown_type);
			if (conv) {
				if (verbosity >= 4)
					errs() << "converted result of binop from " << *result->getType() << ' '
					       << *ft->type << " signed: " << !(!(ft->type_attr & A_signed)) << " to "
					       << *desired_type << " signed: " << !(!(desired_type_attr & A_signed))
					       << (is_unknown_type ? " literal" : " explicit type") << "\n";
				result = conv(result);
			}
		}
		return result;
	} else {
		return nullptr;
	}
	// If it wasn't a builtin binary operator, it must be a user defined one. Emit
	// a call to it.
	auto F = getFunction(std::string("binary") + Op);
	assert(F.first && "binary operator not found!");

	llvm::Value *Ops[] = {L, R};
	return Builder->CreateCall(F.first, Ops, "binop");
}

llvm::Value *CallExprAST::codegen_raw() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Look up the name in the global module table.
	PrototypeAST* Proto = Callee->ft->proto;
	llvm::Value* theFunction = Callee->codegen();
	auto FT = llvm::cast<llvm::FunctionType>(Callee->ft->type);
	// If argument mismatch error.
	if (FT->getNumParams() > Args.size() || FT->getNumParams() < Args.size() && !Proto->IsVarArgs || FT->getNumParams() != Proto->Args.size()) {
		errs() << "Incorrect number of arguments passed: expected " << FT->getNumParams() << (Proto->IsVarArgs ? "+" : "")
		       << ", got " << Args.size() << "\n";
		return nullptr;
	}

	std::vector<llvm::Value *> ArgsV;
	for (unsigned i = 0, e = Args.size(), v = Proto->Args.size(); i != e; ++i) {
		if (i < v && (Proto->ArgTypes[i]->type->isIntegerTy() || Proto->ArgTypes[i]->type->isFloatingPointTy())) {
			auto conversion = getConv(
				Args[i]->ft->type, Proto->ArgTypes[i]->type,
				Args[i]->ft->type_attr, Proto->ArgTypes[i]->type_attr,
				Args[i]->Loc, false, Args[i]->is_unknown_type);
			if (!conversion)
				return nullptr;
			llvm::Value* arg = conversion(Args[i]->codegen());
			ArgsV.push_back(arg);
		} else {
			if (i < v && Args[i]->ft->type->getTypeID() != Proto->ArgTypes[i]->type->getTypeID()
			    && !Proto->ArgTypes[i]->type->isPointerTy()) {
				// TODO: better check compatibility and make error message human readable
				errs() << "Wrong type passed for function arg #" << i + 1 << ": expected " << *Proto->ArgTypes[i]->type << ", got " << *Args[i]->ft->type << "\n";
				return nullptr;
			}
			llvm::Value* arg = Args[i]->codegen();
			if (!arg)
				return nullptr;
			if (arg->getType()->isFloatingPointTy() && !arg->getType()->isDoubleTy()) {
				// C convention: variadic float args must be promoted to double
				arg = Builder->CreateFPCast(arg, llvm::Type::getDoubleTy(Context), "convfptmp");
			} else if (auto intT = llvm::dyn_cast<llvm::IntegerType>(arg->getType())) {
				// same with short integers 
				if (intT->getBitWidth() < 32)
					arg = Builder->CreateIntCast(arg, llvm::Type::getInt32Ty(Context), !(!(Args[i]->ft->type_attr & A_signed)));
			}
			if (auto interf_t = dynamic_cast<InterfaceExprAST*>(Args[i].get()))
				if (auto struct_type = llvm::dyn_cast<llvm::StructType>(arg->getType()))
					for (unsigned i = 0; i < struct_type->getNumElements(); i++) {
						ArgsV.push_back(Builder->CreateExtractValue(arg, i));
					}
				else
					ArgsV.push_back(arg);
			else
				ArgsV.push_back(arg);
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

llvm::Value* IfExprAST::createCondBranch(llvm::BasicBlock *MergeBB, bool isElse) {
	int EndKind = isElse ? ElseEndKind : ThenEndKind;
	std::vector<std::unique_ptr<ExprAST>>& Branch = isElse ? Else : Then;
	llvm::Value* BranchV = nullptr;
	for (auto& expr : Branch)
		BranchV = expr->codegen_raw();
	if (!BranchV && !isElse)
		return nullptr;
	if (ft->type->isVoidTy() && !(BranchV && BranchV->getType()->isVoidTy())) {
		if (EndKind == tok_return)
			Builder->CreateRetVoid();
		else {
			BranchV = llvm::UndefValue::get(ft->type);
			Builder->CreateBr(MergeBB);
		}
	} else {
		if (!ft->type->isVoidTy()) {
			auto PreConv = getBestPreConv(Branch.back()->Loc, desired_type, conv.compat.res_type,
			                              conv.ideal.res_type, isElse ? conv.compat.RHS : conv.compat.LHS,
			                              isElse ? conv.ideal.RHS : conv.ideal.LHS,
			                              conv.ideal.res_attr & A_signed);
			if (!PreConv)
				return nullptr;
			BranchV = PreConv(BranchV);
		} else {
			BranchV = llvm::UndefValue::get(ft->type);
		}
		if (EndKind == tok_return) {
			Builder->CreateRet(CheckTailCall(BranchV));
		} else {
			Builder->CreateBr(MergeBB);
		}
	}
	return BranchV;
}

llvm::Value *IfExprAST::codegen_raw() {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	Cond->desired_type = llvm::Type::getInt1Ty(Context);
	llvm::Value *CondV = Cond->codegen();
	if (!CondV)
		return nullptr;
	if (desired_type) {
		Then.back()->desired_type = desired_type;
		Else.back()->desired_type = desired_type;
	}
	if (CondV->getType() != llvm::Type::getInt1Ty(Context)) {
		errs() << Cond->Loc << ": bool type expected as \"if\" condition\n";
		return nullptr;
	}
	llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Create blocks for the then and else cases.  Insert the 'then' block at the
	// end of the function.
	llvm::BasicBlock *ThenBB = llvm::BasicBlock::Create(Context, "then", TheFunction);
	llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(Context, "else");
	llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(Context, "ifcond");

	Builder->CreateCondBr(CondV, ThenBB, ElseBB);

	// Emit then value.
	Builder->SetInsertPoint(ThenBB);

	llvm::Value* ThenV = createCondBranch(MergeBB, false);
	if (!ThenV)
		return nullptr;
	// Codegen of 'Then' can change the current block, update ThenBB for the PHI.
	ThenBB = Builder->GetInsertBlock();

	// Emit else block.
	TheFunction->getBasicBlockList().push_back(ElseBB);
	Builder->SetInsertPoint(ElseBB);

	llvm::Value* ElseV = createCondBranch(MergeBB, true);
	if (!ElseV)
		return nullptr;
	// Codegen of 'Else' can change the current block, update ElseBB for the PHI.
	ElseBB = Builder->GetInsertBlock();
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
llvm::Value *ForExprAST::codegen_raw() {
	llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Create an alloca for the variable in the entry block.
	llvm::Type* AllocaT = llvm::Type::getInt32Ty(Context);
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
	llvm::BasicBlock *BB = llvm::BasicBlock::Create(Context, "entry", TheFunction);
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
			errs() << "internal compiler error: arg not found in table";
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
