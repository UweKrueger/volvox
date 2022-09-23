#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"
#include "../lib/str.h"

//===----------------------------------------------------------------------===//
// Debug Info Support
//===----------------------------------------------------------------------===//

bool inside_function = false;
extern llvm::ExitOnError ExitOnErr;
const char* last_shadow_saver = nullptr;
const char* last_shadow_restorer = nullptr;
// list of boolean values that indicate that this loop branch is run for the first time
// this is used to avoid multiple  allocations of variables that are declared inside a then/while/repaet loop
VarTable* IfWhileVarTable = nullptr;
// both in loop bodies and in 'else' blocks array allocation should *not* be done in the entry block
// since the array size might be run time determined in one or the other block. To ensure this we track
// the nesting level of 'if/while/repeat/else' blocks - so we can use "if (condnesting) { ..."
unsigned condnesting = 0;

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
	EltTys.push_back(lex.get_diType(RetType->type, RetType->type_attr & A_signed));
	auto NumArgs = ArgTypes.size();
	for (unsigned i = 0; i < NumArgs; i++)
		EltTys.push_back(lex.get_diType(ArgTypes[i]->type, ArgTypes[i]->type_attr & A_signed));

	return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(EltTys));
}

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

static llvm::DISubprogram *SP;
static llvm::DIFile *Unit;

std::pair<llvm::Function*, PrototypeAST*> getFunction(std::string unmangledName, std::vector<volvoxc::FullType*>* ArgTypes) {
	auto FI = lex.findProtos(unmangledName);
	if (!FI)
		return { nullptr, nullptr };
	// See if the function has already been added to the current module.
	// TODO: find index of matching overloaded prototype (instead of "0")
	int matching_idx = 0;
	if (auto F = TheModule->getFunction((*FI)[matching_idx]->Name)) {
		return { F, (*FI)[matching_idx].get() };
	}
	
	// codegen the declaration from the existing prototype.
	auto F = (*FI)[matching_idx]->codegen();
	return { F, (*FI)[matching_idx].get() };
}

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
static llvm::AllocaInst* CreateEntryBlockAlloca(llvm::Type* type, const llvm::Twine& VarName = "",
                                                llvm::Function* TheFunction = nullptr) {
	if (!TheFunction)
		TheFunction = Builder->GetInsertBlock()->getParent();
	llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
	                       TheFunction->getEntryBlock().begin());
	return TmpB.CreateAlloca(type, nullptr, VarName);
}

llvm::Value* LiteralExprAST::codegen_raw(llvm::Value* target) {
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

llvm::Value* StructExprAST::codegen_raw(llvm::Value* target) {
	if (auto struct_type = llvm::dyn_cast<llvm::StructType>(ft->type)) {
		unsigned num_fields = struct_type->getNumElements();
		std::vector<std::unique_ptr<ExprAST>> initializers(num_fields);
		for (auto& [fname, ini]: Fields) {
			MapValue* mv = map_string_get(ft->fields, fname.c_str());
			unsigned index = *(unsigned*)((char*)mv + mv->offset);
			initializers[index] = std::move(ini);
		}
		llvm::Value* V = llvm::UndefValue::get(ft->type);
		for (unsigned i=0; i<initializers.size(); i++) {
			if (initializers[i])
				V = Builder->CreateInsertValue(V, initializers[i]->codegen(), i, "structinit");
			else
				V = Builder->CreateInsertValue(V, llvm::Constant::getNullValue(struct_type->getElementType(i)), i , "structzeroinit");
		}
		if (target) {
			Builder->CreateStore(V, target);
			return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
		} else {
			return V;
		}
	} else
		abort();
}

llvm::Value* LvalueExprAST::codegen_raw(llvm::Value* target) {
	auto V = codegen_ref();
	// Load the value.
	return Builder->CreateLoad(V.first, V.second, Name.c_str());
}

std::pair<llvm::Type*,llvm::Value*> VariableExprAST::codegen_ref(bool silent_fail) {
	if (!full_var.first) {
		errs() << Loc << ": unknown variable name '" << Name << "'\n";
		return { nullptr, nullptr };
	}
	llvm::Value* V;
	llvm::Type* storage_type;
	if (full_var.second) { // global variable
		if (!full_var.first->mangled_name) {
			errs() << Loc << ": no mangled name for " << Name << '\n';
			return { nullptr, nullptr };
		}
		V = TheModule->getGlobalVariable(full_var.first->mangled_name, true);
		if (!V) {
			V = new llvm::GlobalVariable(*TheModule, full_var.first->storage_type,
			                             false, llvm::GlobalValue::ExternalLinkage,
			                             nullptr, full_var.first->mangled_name, nullptr,
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

llvm::MaybeAlign getAlignment(size_t elem_size) {
	uint64_t align = 1;
	// MaybeAlign constructor only accepts powers of 2, so create one from elem_size
	do {
		if (align & elem_size)
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
		ArrayAlloc = Builder->CreateAlloca(elem_type, Len, Name);
	}
	ArrayPtr = Builder->CreateBitCast(ArrayAlloc, elem_type->getPointerTo());
	// TODO: Insert run time check that initialization values fit into allocation size
	StoreArray(ArrayPtr, ArrData, Sizes, 0);
	// REMARK: returning the same pointer value as two different types will be obsolete with opaque pointers
	return { ArrayAlloc, ArrayPtr };
}

static llvm::Type* getArrayDims(llvm::Value* val, llvm::ArrayType* array_type, std::vector<llvm::Value*>& Dims, std::vector<llvm::Value*>& returnDims, llvm::ArrayType* expected_array_type = nullptr) {
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

static llvm::Value* getInterfaceArrayOrStoreValue(llvm::Value* val, llvm::ArrayType* array_type,
                                                  llvm::ArrayType* expected_array_type = nullptr, bool do_store = false,
                                                  const llvm::Twine &Name = "") {
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

std::pair<llvm::Type*,llvm::Value*> SelectExprAST::codegen_ref(bool silent_fail) {
	if (!ft || !ft->type)
		return { nullptr, nullptr }; // error message was already generated in AST
	if (auto LV = dynamic_cast<LvalueExprAST*>(Struct.get())) {
		auto struct_ref = LV->codegen_ref(silent_fail);
		if (struct_ref.second)
			return { ft->type, Builder->CreateConstGEP2_32(struct_ref.first, struct_ref.second, 0, FieldIndex) };
	}
	if (!silent_fail)
		errs() << Struct->Loc << ": LHS of '.' expression must be an lvalue\n";
	return { ft->type, nullptr };
}

llvm::Value* SelectExprAST::codegen_raw(llvm::Value* target) {
	auto V = codegen_ref(true);
	if (auto val = ref2val(V))
		return val;
	if (V.first) {
		llvm::Value* struct_val = Struct->codegen_raw(target);
		if (struct_val)
			return Builder->CreateExtractValue(struct_val, FieldIndex);
	}
	errs() << Loc << ": cannot generate code for select expression\n";
	return nullptr;
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
			Ptr = Builder->CreateIntToPtr(Builder->CreateAdd(Builder->CreatePtrToInt(Ptr, llvm::Type::getInt64Ty(Context)), offset), Ptr->getType());
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
	} else {
		errs() << "LHS of index expression must be an array (or map) " << *ft->type << "\n";
		return { nullptr, nullptr };
	}
	errs() << Loc << ": error generating index expr\n";
	// return { nullptr, nullptr };
	// return { elem_type, Builder->CreateGEP(elem_type, field_ptr, idx) };
}

llvm::Value* FunctionExprAST::codegen_raw(llvm::Value* target) {
	if (auto F = TheModule->getFunction((*ft->Protos)[selected_proto]->Name)) {
		return F;
	}
	return (*ft->Protos)[selected_proto]->codegen();
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
			if (array->getType()->isVoidTy())
				return array;
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
				return stuct_val;
			if (!target)
				val = StoreValue(stuct_val, expr->ft);
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

llvm::Value *UnaryExprAST::codegen_raw(llvm::Value* target) {
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
		std::vector<volvoxc::FullType*> ArgTypes = { Operand->ft };
		auto F = getFunction(std::string("unary") + Opcode, &ArgTypes);
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

std::nullptr_t HandleGlobalVariable(BinaryExprAST* expr, unsigned sym_kind) {
	if (auto Val = expr->RHS->codegen()) {
		VariableExprAST* LHSE = static_cast<VariableExprAST *>(expr->LHS.get());
		const std::string& unmangled_name = LHSE->getName();
		std::string varname = lex.module->import_path.empty() ?
			unmangled_name :
			std::string(MangleBase(lex.module->import_path, unmangled_name));
		llvm::Type* val_type = Val->getType();
		auto type_descr = MakeType(expr->RHS->ft->type, expr->RHS->ft->type_attr & A_signed, expr->RHS->is_unknown_type);
		llvm::Type* type = std::get<0>(type_descr);
		auto conversion = std::get<1>(type_descr);
		bool is_signed = std::get<2>(type_descr);
		auto convertedVal = conversion(Val);
		llvm::GlobalValue::LinkageTypes link_type = ((sym_kind & A_pub) || comp_mode == comp_jit) ?
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
					SP, varname, varname, Unit, expr->Loc.Line, lex.get_diType(type, is_signed), false);
			}
			GV = new llvm::GlobalVariable(*TheModule, initializer->getType(),
			                              false, link_type,
			                              initializer, varname, nullptr,
			                              llvm::GlobalVariable::GeneralDynamicTLSModel);
			volvoxc::FullType ft = *expr->RHS->ft;
			ft.type = type;
			ft.type_attr = sym_kind | (is_signed ? A_signed : 0U);
			FullVar fv = {
				.storage_type = initializer->getType(),
				.mangled_name = strdup(varname.c_str()),
				.ft = ft,
			};
			lex.module->globals_table.insert(unmangled_name.c_str(), fv);
			if (comp_mode == comp_jit && !do_test) {
				llvm::Type* V_type = initializer->getType();
				size_t storage_sz = TheJIT->getDataLayout().getTypeStoreSize(V_type);
				std::string shadow_var_name = std::string("__") + varname + "_shadow_";
				auto V = new llvm::GlobalVariable(*TheModule, V_type,
					                             false, link_type,
					                             initializer, shadow_var_name, nullptr,
					                             llvm::GlobalVariable::NotThreadLocal);
#ifndef LEGACY_PASS_MANAGER
				// running the new PassManager on an empty module causes trouble :-(
				// let's avoid this...
				if (TheModule->end() != TheModule->begin()) {
					MPM.run(*TheModule, MAM);
					if (dump_IR && dump_opt) {
						auto end = TheModule->end();
						for (auto it = TheModule->begin(); it != end; ++it)
							it->print(errs());
					}
				}
#endif
				ExitOnErr(TheJIT->addModule(
					          llvm::orc::ThreadSafeModule(std::move(TheModule), *TS_Context.get())));
				InitializeModuleAndPassManager();
				GV = TheModule->getGlobalVariable(varname, true);
				if (!GV) {
					GV = new llvm::GlobalVariable(*TheModule, V_type,
					                              false, link_type,
					                              nullptr, varname, nullptr,
					                              llvm::GlobalVariable::GeneralDynamicTLSModel,
					                              0, true);
				}
				V = TheModule->getGlobalVariable(shadow_var_name, true);
				if (!V) {
					V = new llvm::GlobalVariable(*TheModule, V_type,
					                             false, link_type,
					                             nullptr, shadow_var_name, nullptr,
					                             llvm::GlobalVariable::NotThreadLocal,
					                             0, true);
				}
				auto sz_const = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), storage_sz);
				auto align = getAlignment(sz_const);
				auto saver = std::string("__") + varname + "_saver";
				auto restorer = std::string("__") + varname + "_restorer";
				llvm::FunctionType* void_fn_t = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {}, false);

				llvm::Function* Fsaver = llvm::Function::Create(void_fn_t, llvm::Function::ExternalLinkage, saver, TheModule.get());
				auto BB = llvm::BasicBlock::Create(Context, "entry", Fsaver);
				Builder->SetInsertPoint(BB);
				Builder->CreateMemCpy(V, align, GV, align, storage_sz);
				if (last_shadow_saver) {
					auto last_saver = getFunction(last_shadow_saver, nullptr);
					Builder->CreateRet(CheckTailCall(Builder->CreateCall(last_saver.second->FT, last_saver.first, std::vector<llvm::Value*>(), "callold")));
				} else {
					Builder->CreateRetVoid();
				}
				verifyFunction(*Fsaver);
				if (dump_IR >= 3 && dump_raw) {
					errs() << "Read saver definition (raw):\n";
					Fsaver->print(errs());
					errs() << "\n";
				}
#ifdef LEGACY_PASS_MANAGER
				TheFPM->run(*Fsaver);
				if (dump_IR >= 3 && dump_opt) {
					errs() << "Read saver definition (after optimization):\n";
					Fsaver->print(errs());
					errs() << "\n";
				}
#endif
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
					auto last_restorer = getFunction(last_shadow_restorer, nullptr);
					Builder->CreateRet(CheckTailCall(Builder->CreateCall(last_restorer.second->FT, last_restorer.first, std::vector<llvm::Value*>(), "callold")));
				} else {
					Builder->CreateRetVoid();
				}
				verifyFunction(*Frestorer);
				if (dump_IR >= 3 && dump_raw) {
					errs() << "Read restorer definition (raw):\n";
					Frestorer->print(errs());
					errs() << "\n";
				}
#ifdef LEGACY_PASS_MANAGER
				TheFPM->run(*Frestorer);
				if (dump_IR >= 3 && dump_opt) {
					errs() << "Read restorer definition (after optimization):\n";
					Frestorer->print(errs());
					errs() << "\n";
				}
#endif
				auto restorerProto = std::make_unique<PrototypeAST>(CurLoc, restorer, std::vector<std::string>());
				last_shadow_restorer = restorerProto->Name.c_str();
				module->FunctionProtos[restorer].push_back(std::move(restorerProto));
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

llvm::Value *BinaryExprAST::codegen_raw(llvm::Value* target) {
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
		uint64_t allocsz = (RHS->desired_type && RHS->desired_type->isSized()) ?
			TheModule->getDataLayout().getTypeAllocSize(RHS->desired_type) : 0; // if size is compile time const
		llvm::Value* Val = nullptr; // 
		llvm::Value* ValPtr = nullptr;
		llvm::Value* AllocSize = nullptr;
		llvm::Type* elem_type = nullptr;
		llvm::StructType* struct_type = nullptr;
		uint64_t el_allocsz = 0;
		llvm::Value* Struct = nullptr;
		if (auto RHS_Lval = dynamic_cast<LvalueExprAST*>(RHS.get())) {
			auto ValR = RHS_Lval->codegen_ref(true);
			if (!ValR.second) {
				if (ValR.first)
					goto use_val;
				errs() << RHS->Loc << ": unable to generate code for RHS of assignment\n";
				return nullptr;
			}
			// update allocsz in case codegen_ref() has revealed a fixed compile time size
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
				if (allocsz <= 16) {
					Val = RHS_Lval->ref2val(ValR);
					if (!Val)
						goto use_val;;
				}
				else
					ValPtr = ValR.second;
			}
			goto have_val_or_valptr;
		}
	use_val:
		if (allocsz <= 16) {
			Val = RHS->codegen();
			if (!Val)
				return nullptr;
			if (conv.compat.RHS)
				Val = conv.compat.RHS(Val);
		}
	have_val_or_valptr:
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
			if (allocsz > 16) {
				auto align = getAlignment(allocsz);
				if (target)
					Builder->CreateMemCpy(target, align, Variable.second, align, allocsz);
				if (ValPtr)
					Builder->CreateMemCpy(Variable.second, align, Val, align, allocsz);
				else {
					auto voidval = RHS->codegen_raw(Variable.second);
					if (!voidval->getType()->isVoidTy()) {
						errs() << Loc << ": internal error: sret call does not return void\n";
						return nullptr;
					}
				}
				return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
			} else {
				auto OldVal = Builder->CreateLoad(Variable.first, Variable.second);
				Builder->CreateStore(Val, Variable.second);
				return OldVal;
			}
		}
	not_found:
		if (kind != decl_assign_op) {
			errs() << LHS->Loc << ": unknown variable name '" << varname << "'\n";
			return nullptr;
		}
		// variable declaration
		if (inside_function) {
			llvm::Function* TheFunction = Builder->GetInsertBlock()->getParent();
			auto type_descr = MakeType(RHS->ft->type, RHS->ft->type_attr & A_signed, RHS->is_unknown_type);
			llvm::Type* type = std::get<0>(type_descr);
			auto conversion = std::get<1>(type_descr);
			bool is_signed = std::get<2>(type_descr);
			FullVar* entry = locals_table.back()[varname];
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
					auto align = getAlignment(allocsz);
					auto Alloca = Builder->CreateAlloca(RHS->ft->type, nullptr, varname);
					Builder->CreateMemCpy(Alloca, align, ValPtr, align, allocsz);
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
			} else if (allocsz > 16) {
				auto align = getAlignment(allocsz);
				auto Alloca = Builder->CreateAlloca(RHS->ft->type, nullptr, varname);
				auto voidval = RHS->codegen_raw(Alloca);
				if (!voidval->getType()->isVoidTy()) {
					errs() << Loc << ": internal error: sret call does not return void\n";
					return nullptr;
				}
				entry->val = Alloca;
			} else {
				errs() << "unhandled case\n";
				return nullptr;
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
	auto F = getFunction(std::string("binary") + Op, nullptr);
	assert(F.first && "binary operator not found!");

	llvm::Value *Ops[] = {L, R};
	return Builder->CreateCall(F.first, Ops, "binop");
}

llvm::Value *CallExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	// Look up the name in the global module table.
	PrototypeAST* Proto = (*Callee->ft->Protos)[0].get();
	llvm::Value* theFunction = Callee->codegen();
	auto FT = llvm::cast<llvm::FunctionType>(Callee->ft->type);
	// If argument mismatch error.
	unsigned params_offset = 0;
	if (Proto->IsStructRet)
		params_offset++;
	unsigned ft_num_params = FT->getNumParams() - params_offset;
	if (ft_num_params > Args.size() || ft_num_params < Args.size() && !Proto->IsVarArgs || ft_num_params != Proto->Args.size()) {
		errs() << "Incorrect number of arguments passed: expected " << ft_num_params << (Proto->IsVarArgs ? "+" : "")
		       << ", got " << Args.size() << "\n";
		return nullptr;
	}

	std::vector<llvm::Value *> ArgsV;
	llvm::Value* ret_struct = nullptr;
	if (Proto->IsStructRet) {
		if (!target) {
			errs() << Loc << ": " << Proto->Name << " - internal error: no target for struct return\n";
			return nullptr;
		}
		ArgsV.push_back(target);
	}
	unsigned arg_offs = (Proto->IsMethod ? 1 : 0) + (Proto->IsStructRet ? 1 : 0);
	for (unsigned i = 0, e = Args.size(), v = Proto->Args.size(); i != e; ++i) {
		if (i < v && !Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByRef)
		    && (Proto->ArgTypes[i+arg_offs]->type->isIntegerTy()
		        || Proto->ArgTypes[i+arg_offs]->type->isFloatingPointTy())) {
			auto conversion = getConv(
				Args[i]->ft->type, Proto->ArgTypes[i+arg_offs]->type,
				Args[i]->ft->type_attr, Proto->ArgTypes[i+arg_offs]->type_attr,
				Args[i]->Loc, false, Args[i]->is_unknown_type);
			if (!conversion)
				return nullptr;
			llvm::Value* arg = conversion(Args[i]->codegen());
			ArgsV.push_back(arg);
		} else {
			if (i < v && Args[i]->ft->type->getTypeID() != Proto->ArgTypes[i+arg_offs]->type->getTypeID()
			    && !Proto->ArgTypes[i+arg_offs]->type->isPointerTy()) {
				// TODO: better check compatibility and make error message human readable
				errs() << "Wrong type passed for function arg #" << i + 1 << ": expected " << *Proto->ArgTypes[i+arg_offs]->type << ", got " << *Args[i]->ft->type << "\n";
				return nullptr;
			}
			llvm::Value* arg = nullptr;
			bool is_address = i < v && (Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByVal)
			                            || Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByRef));
			if (auto call = dynamic_cast<CallExprAST*>(Args[i].get())) {
				PrototypeAST* CallProto = (*call->Callee->ft->Protos)[0].get(); // 'g' in 'f(g())'
				if (CallProto->IsStructRet) {
					if (is_address) {
						// 'g' returns by reference and 'f' exprects a reference (i.e. an address)
						// so we have to allocate memory for the indermediate result
						arg = Builder->CreateAlloca(call->ft->type);
						auto voidval = call->codegen_raw(arg);
						if (!voidval->getType()->isVoidTy()) {
							errs() << Loc << ": internal error: sret call does not return void\n";
							return nullptr;
						}
					} else {
						errs() << Loc << ": " << Proto->Name << " arg: " << i << " missing ByVal attribute\n";
						return nullptr;
					}
				}
			}
			if (!arg) {
				if (is_address) {
					if (auto lval = dynamic_cast<LvalueExprAST*>(Args[i].get())) {
						auto argref = lval->codegen_ref();
						arg = argref.second;
					} else {
						arg = Builder->CreateAlloca(Proto->LLVMArgTypes[i+arg_offs]);
						auto tmparg = Args[i]->codegen();
						Builder->CreateStore(tmparg, arg);
					}
				} else
					arg = Args[i]->codegen();
			}
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

std::pair<llvm::Value*, llvm::Instruction*> IfExprAST::createCondBranch(llvm::BasicBlock* MergeBB, bool isElse) {
	int EndKind = isElse ? ElseEndKind : ThenEndKind;
	std::vector<std::unique_ptr<ExprAST>>& Branch = isElse ? Else : Then;
	llvm::Value* BranchV = nullptr;
	llvm::Instruction* firstBreak = nullptr; // needed as insertion point to prepare merged vars
	for (auto& expr : Branch)
		BranchV = expr->codegen_raw();
	if (!BranchV && !isElse)
		return { nullptr, nullptr };
	if (ft->type->isVoidTy() && !(BranchV && BranchV->getType()->isVoidTy())) {
		if (EndKind == tok_return)
			Builder->CreateRetVoid();
		else {
			BranchV = llvm::UndefValue::get(ft->type);
			firstBreak = Builder->CreateBr(MergeBB);
		}
	} else {
		if (ft->type->isVoidTy()) {
			BranchV = llvm::UndefValue::get(ft->type);
		} else if (ft->type->isSingleValueType()) {
			auto PreConv = getBestPreConv(Branch.back()->Loc, desired_type, conv.compat.res_type,
			                              conv.ideal.res_type, isElse ? conv.compat.RHS : conv.compat.LHS,
			                              isElse ? conv.ideal.RHS : conv.ideal.LHS,
			                              conv.ideal.res_attr & A_signed);
			if (!PreConv)
				return { nullptr, nullptr };
			BranchV = PreConv(BranchV);
			if (Branch.back()->ft->type != BranchV->getType()) {
				Branch.back()->ft = new_FullType(*Branch.back()->ft);
				Branch.back()->ft->type = BranchV->getType();
			}
		}
		if (EndKind == tok_return) {
			Builder->CreateRet(CheckTailCall(BranchV));
		} else {
			firstBreak = Builder->CreateBr(MergeBB);
		}
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
		if (desired_type) {
			Then.back()->desired_type = desired_type;
			Else.back()->desired_type = desired_type;
		}
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
	llvm::Value* savedStack;
	if (if_kind != tok_if) {
		TheFunction->getBasicBlockList().push_back(StackSaveBB);
		Builder->SetInsertPoint(StackSaveBB);
		savedStack = Builder->CreateIntrinsic(llvm::Intrinsic::stacksave, {}, {}, nullptr, "savedstack");
		Builder->CreateBr(ThenBB);
		TheFunction->getBasicBlockList().push_back(StackRestoreBB);
		Builder->SetInsertPoint(StackRestoreBB);
		Builder->CreateIntrinsic(llvm::Intrinsic::stackrestore, {}, savedStack);
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
			entry->ft.type = then_var->ft.type; // TODO: merge different but compatible array types
			entry->ft.type_attr = then_var->ft.type_attr;
			entry->val = then_var->val;
		}
	} else if (then_locals_table.table && else_locals_table.table && thenLast && elseLast) {
		for (auto then_node = then_locals_table.first(); then_node; ++then_node) {
			FullVar* else_var = else_locals_table[then_node.getKey()];
			if (else_var) {
				MapValue* node = then_node.getValue();
				auto then_var = (FullVar*)((char*)node + node->offset);
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
			}
		}
	}
	if (ft->type->isVoidTy())
		return llvm::UndefValue::get(ft->type);
	else {
		auto merge = merge_values(Then.back()->ft->type, ThenV, (if_kind == tok_while) ? CondBB : ThenBB, thenLast,
		                          Else.back()->ft->type, ElseV, ElseBB, elseLast);
		if (ft->type != merge.first) {
			ft = new_FullType(*ft);
			ft-> type = merge.first;
		}
		return merge.second;
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

llvm::Function *PrototypeAST::codegen() {
	llvm::Function *F =
		llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, TheModule.get());

	// Set names for all arguments.
	unsigned Idx = 0;
	if (IsStructRet && ArgAttrs[Idx].hasAttributes()) {
#if LLVM_VERSION_MAJOR >= 14
		llvm::AttrBuilder attr_builder(Context, ArgAttrs[Idx]);
		F->getArg(Idx++)->addAttrs(attr_builder);
#else
		for (auto attr: ArgAttrs[Idx])
			F->getArg(Idx)->addAttr(attr);
		Idx++;
#endif
	}
	for (auto &Arg : Args) {
		auto fnarg = F->getArg(Idx);
		if (ArgAttrs[Idx].hasAttributes()) {
#if LLVM_VERSION_MAJOR >= 14
			llvm::AttrBuilder attr_builder(Context, ArgAttrs[Idx]);
			fnarg->addAttrs(attr_builder);
#else
			for (auto attr: ArgAttrs[Idx])
				fnarg->addAttr(attr);
#endif
		}
		fnarg->setName(Arg);
		Idx++;
	}
	if (visibility & A_inline)
		F->addFnAttr(llvm::Attribute::AlwaysInline);
	return F;
}

llvm::Function *FunctionAST::codegen() {
	// Transfer ownership of the prototype to the lex.module->FunctionProtos map, but keep a
	// reference to it for use below.
	auto &P = *Proto;
	auto CalleeF = getFunction(unmangledName, &P.ArgTypes);
	llvm::Function* TheFunction = CalleeF.first;
	if (!TheFunction) {
		errs() << "Function '" << unmangledName << "()' not found in module\n";
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
	llvm::Value* ret_ptr; // for sret
	if (P.IsStructRet)
		ret_ptr = TheFunction->getArg(ArgIdx++);
	else
		ret_ptr = nullptr;
	for (; ArgIdx < TheFunction->arg_size(); ArgIdx++) {
		auto Arg = TheFunction->getArg(ArgIdx);
		FullVar* mapitem = locals_table.back()[Arg->getName().str().c_str()];
		if (!mapitem) {
			errs() << "internal compiler error: arg not found in table";
			exit(1);
		}
		if (Arg->hasByValAttr() || Arg->hasByRefAttr()) {
			mapitem->val = Arg;
		} else {
			// Create an alloca for this variable.
			llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(P.LLVMArgTypes[ArgIdx], Arg->getName(), TheFunction);
			// get reference to argument in symbol table
			// Store the initial value into the alloca.
			Builder->CreateStore(Arg, Alloca);

			// Add storage to variable in symbol table.
			mapitem->val = Alloca;
		}
		if (comp_mode == comp_dbg) {
			// Create a debug descriptor for the variable.
			llvm::DILocalVariable *D = DBuilder->createParameterVariable(
				SP, Arg->getName(), ArgIdx + 1, Unit, LineNo, lex.get_diType(mapitem->ft.type, mapitem->ft.type_attr & A_signed),
				true);
			DBuilder->insertDeclare(mapitem->val, D, DBuilder->createExpression(),
			                        llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
			                        Builder->GetInsertBlock());
		}
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
		// auto ret_type = RetVal->getType();
		//type = ret_type; // TODO: hande conversion if != proto->type;
		if (P.IsStructRet) {
			Builder->CreateStore(RetVal, ret_ptr);
			Builder->CreateRetVoid();
		}
		else
			Builder->CreateRet(CheckTailCall(RetVal));
	}		
	if (comp_mode == comp_dbg) {
		// Pop off the lexical block for the function.
		KSDbgInfo.LexicalBlocks.pop_back();
	}
	// Validate the generated code, checking for consistency.
	verifyFunction(*TheFunction);
	if (dump_raw && (dump_IR >= 2 || dump_IR && unmangledName != "__anon_expr")) {
		errs() << "Read function definition (raw):\n";
		TheFunction->print(errs());
		errs() << "\n";
	}
#ifdef LEGACY_PASS_MANAGER
	// Run the optimizer on the function.
	TheFPM->run(*TheFunction);
	if (dump_opt && (dump_IR >= 2 || dump_IR && unmangledName != "__anon_expr")) {
		errs() << "Read function definition (after optimization):\n";
		TheFunction->print(errs());
		errs() << "\n";
	}
#endif
	return TheFunction;
}
