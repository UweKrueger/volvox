/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

// variable size main vars are "malloc()ed" in jit mode. On exit these blocks would be
// orphaned - so let's keep track of then to avoid memory leaks:
MainVars jit_main_variables;
llvm::DISubprogram *SP;
llvm::DIFile *Unit;
volvoxc::FullType* theFunction_ret_ft = nullptr;
bool theFunction_struct_ret = false;
FunctionAST* currentFunction = nullptr;
std::vector<FullVar> expr_temps; // to call destructors immediatelly after expr
#ifdef _WIN32
std::vector<HMODULE> extra_dlls; // loaded by '__link_extra' at runtime in JIT mode
#endif

// global function to find method protos
std::vector<std::unique_ptr<PrototypeAST>>* findProtos(const std::string& mangledType, const std::string& unmangledName) {
	// 'unmangledName might be fully qualified like 'module.name' so strip the module name
	const char* p = unmangledName.c_str();
	while (*p)
		if (*p++ == '.')
			break;
	auto FI = *p ? MethodProtos.find({ mangledType, p }) :
		MethodProtos.find({ mangledType, unmangledName });
	if (FI != MethodProtos.end())
		return &FI->second;
	else
		return nullptr;
}

llvm::Function* getFunction(PrototypeAST* FI) {
	if (auto F = TheModule->getFunction(FI->Name))
		return F;
	return FI->codegen();
}

llvm::Function* getConversion(std::string& mangled_name) {
	if (auto F = TheModule->getFunction(mangled_name))
		return F;
	auto fn_type = Conversions.find(mangled_name);
	if (fn_type == Conversions.end())
		return nullptr;
	auto F = llvm::Function::Create(fn_type->second, llvm::Function::ExternalLinkage, mangled_name, TheModule.get());
	return F;
}

llvm::Function* getConstructorOrDestructor(volvoxc::FullType* ft, bool destructor) {
	auto Names = AutoMethods.find(ft->mangled_name);
	if (Names == AutoMethods.end())
		return nullptr;
	std::string& thename = destructor ? Names->second.second : Names->second.first;
	if (thename.empty())
		return nullptr;
	if (auto F = TheModule->getFunction(thename))
		return F;
	auto FT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { ft->type->getPointerTo() }, false);
	auto F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, thename, TheModule.get());
	return F;
}

inline static uintptr_t getFnAddress(std::function<llvm::Value*(llvm::Value*)> f) {
    typedef llvm::Value* (fnType)(llvm::Value*);
    fnType** fnPointer = f.template target<fnType*>();
    return (uintptr_t)*fnPointer;
}

static void printArgTypes(std::vector<FnArg>& fnargs, unsigned offset = 0) {
	for (unsigned i = offset; i<fnargs.size(); i++) {
		if (i>offset)
			errs() << ", ";
		if (fnargs[i].is_anonymous_list)
			errs() << "{...}";
		else
			errs() << fnargs[i];
	}
}

static void printCandidate(PrototypeAST* proto, const char* name) {
	errs() << proto->retLoc << ": " << (name ? name : "fn") << '(';
	for (int i=0; i<proto->Args.size(); i++)
		if (i>0 || proto->Args[i] != "this") {
			errs() << proto->Args[i] << ' ' << *proto->ArgTypes[i];
			if (i != proto->Args.size()-1)
				errs() << ", ";
		}
	errs() << ")\n";
}

inline static void printCandidates(unsigned candidates[], unsigned num_candidates, std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name) {
	for(unsigned i=0; i<num_candidates; i++) {
		unsigned c = candidates[i];
		printCandidate((*protos)[c].get(), name);
	}
}

void printAllProtos(std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name) {
	for (auto& proto: *protos)
		printCandidate(proto.get(), name);
}

int selectProto(std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name,
                std::vector<FnArg>& fnargs, SourceLocation Loc) {
	int noundefcandidate = -1;
	int candidate = -1;
	// there are 3 classes of match for a function signature:
	// 1. exact match - all arguments match without conversion (with the default type for untyped parameters)
	// 2. match with no conversions except for untyped parameters
	// 3. all arguments match with automatic conversions
	//
	// a prototype is selected if either there is a canditate of class 1 (should never be more) or there is
	// exactly one candidate of class 2 or there is exactly one candidate of class 3
	//
	unsigned* candidates_2 = (unsigned*)alloca((*protos).size() * sizeof(unsigned));
	memset(candidates_2, 0, (*protos).size() * sizeof(unsigned));
	unsigned* candidates_3 = (unsigned*)alloca((*protos).size() * sizeof(unsigned));
	memset(candidates_3, 0, (*protos).size() * sizeof(unsigned));
	// class 1 can directly save in Arguments
	std::function<llvm::Value*(llvm::Value*)>* convs2 = (std::function<llvm::Value*(llvm::Value*)>*)alloca(fnargs.size() * sizeof(std::function<llvm::Value*(llvm::Value*)>));
	memset(convs2, 0, fnargs.size() * sizeof(std::function<llvm::Value*(llvm::Value*)>));
	std::function<llvm::Value*(llvm::Value*)>* convs3 = (std::function<llvm::Value*(llvm::Value*)>*)alloca(fnargs.size() * sizeof(std::function<llvm::Value*(llvm::Value*)>));
	memset(convs3, 0, fnargs.size() * sizeof(std::function<llvm::Value*(llvm::Value*)>));
	unsigned cands1 = 0;
	unsigned cands2 = 0;
	unsigned cands3 = 0;
	unsigned selected_idx;
	int i_proto = 0;
	for (auto& proto: *protos) {
		if (proto->IsVarArgs)
			return i_proto; // var args need more work... - for now we assume this is just one C functtion
		if (!proto->IsVarArgs && proto->ArgTypes.size() != fnargs.size()
		    || proto->IsVarArgs && proto->ArgTypes.size() > fnargs.size()) {
			i_proto++;
			continue;
		}
		bool exact = true;
		bool with_conv = true;
		bool with_undefconv = true;
		for (int i=0; i<fnargs.size(); i++) {
			conv_match_t match_kind = untyped_match;
			std::function<llvm::Value*(llvm::Value*)> conv = nullptr;
			if (i >= proto->ArgTypes.size()) {
				if (candidate < 0)
					fnargs[i].Conv = nullptr; // for variadic args - but see comment above
			} else {
				if (fnargs[i].argtype && fnargs[i].argtype->isPointerTy() && proto->ArgTypes[i]->type->isPointerTy()) {
					if ((fnargs[i].argtype_attr & A_string) && (proto->ArgTypes[i]->type_attr & A_string)
					    || (fnargs[i].argtype_attr & A_cstring) && (proto->ArgTypes[i]->type_attr & A_cstring)
					    || !(fnargs[i].argtype_attr & (A_string | A_cstring)) && !(proto->ArgTypes[i]->type_attr & (A_string | A_cstring))) {
						conv = nullptr;
						match_kind = exact_match;
					} else if ((fnargs[i].argtype_attr & A_string) && (proto->ArgTypes[i]->type_attr & A_cstring)) {
						conv = Volvox2CStr;
						match_kind = conversion_match;
					} else {
						conv = nullptr;
						match_kind = untyped_match;
					}
				} else if (fnargs[i].argtype && fnargs[i].argtype->isPointerTy() && !(fnargs[i].argtype_attr & A_string)
				           && (proto->ArgTypes[i]->type_attr & A_optional)) {
					conv = nullptr;
					match_kind = exact_match;
				} else {
					if (fnargs[i].is_anonymous_list && (proto->ArgTypes[i]->type->isStructTy() || proto->ArgTypes[i]->type->isArrayTy()))
						conv = NoConversion;
					else
						conv = getConv(fnargs[i].argtype, proto->ArgTypes[i]->type, SourceLocation{0},
						               fnargs[i].argtype_attr, proto->ArgTypes[i]->type_attr,
						               false, fnargs[i].arg_unknown_type, &match_kind);
				}
				if (match_kind == exact_match) {
					if (!cands1)
						fnargs[i].Conv = nullptr;
					if (!cands2)
						convs2[i] = nullptr;
					if (!cands3)
						convs3[i] = nullptr;
				} else if (match_kind == conversion_match) {
					exact = false;
					if (!cands2)
						convs2[i] = conv;
					if (!cands3)
						convs3[i] = conv;
				} else if (conv || (proto->ArgTypes[i]->type->isStructTy() || proto->ArgTypes[i]->type->isArrayTy()) && fnargs[i].is_anonymous_list) {
					exact = false;
					with_conv = false;
					if (!cands3)
						convs3[i] = conv;
				} else {
					exact = with_conv = with_undefconv = false;
					break; // no match - continue with next prototype
				}
			}
		}
		if (exact) {
			selected_idx = i_proto; // we are done - this is the best (the only exact) match
			goto check_selected_proto;
		} else if (with_conv) {
			candidates_2[cands2++] = i_proto;
		} else if (with_undefconv) {
			candidates_3[cands3++] = i_proto;
		}
		i_proto++;
	}
	// exact match has already returned - check class 2 and 3 for candidates
	if (cands2) {
		if (cands2 > 1) {
			errs() << Loc << ": call of '" << (name ? name : "fn") << '(';
			printArgTypes(fnargs, (!(*protos)[0]->Args.empty() && (*protos)[0]->Args[0] == "this") ? 1 : 0);
			errs() << ")' is ambiguous - candidates are:\n";
			printCandidates(candidates_2, cands2, protos, name);
			if (cands3 > 0) {
				errs() << "secondary (but still matching) candidates are:\n";
				printCandidates(candidates_3, cands3, protos, name);
			}
			return -1;
		}
		// there is exactly one candidate of class 2 - return this
		for (int i=0; i<fnargs.size(); i++)
			fnargs[i].Conv = convs2[i];
		 selected_idx = candidates_2[0];
		 goto check_selected_proto;
	}
	if (cands3) {
		if (cands3 > 1) {
			errs() << Loc << ": call of '" << (name ? name : "fn") << '(';
			printArgTypes(fnargs, (!(*protos)[0]->Args.empty() && (*protos)[0]->Args[0] == "this") ? 1 : 0);
			errs() << ")' is ambiguous - candidates (all secondary) are:\n";
			printCandidates(candidates_3, cands3, protos, name);
			return -1;
		}
		// there is exactly one candidate of class 3 - return this
		for (int i=0; i<fnargs.size(); i++)
			fnargs[i].Conv = convs3[i];
		 selected_idx = candidates_3[0];
		 goto check_selected_proto;
	}
	if (name && fnargs.size() == 1 && lex.source_stack.front().module->type_table.get_full(name))
		// 'name' is a built-in type - so this might be an explicit type conversion
		return -2;
	errs() << Loc << ": signature of call to '" << (name ? name : "fn") << '(';
	printArgTypes(fnargs, (!(*protos)[0]->Args.empty() && (*protos)[0]->Args[0] == "this") ? 1 : 0);
	errs() << ")' does not match any known candidate - candidates are:\n";
	printAllProtos(protos, name);
	return -1;
check_selected_proto:
	auto selected_proto = (*protos)[selected_idx].get();
	for (int i=0; i<selected_proto->ArgTypes.size(); i++)
		if ((selected_proto->ArgTypes[i]->type_attr & A_ref) && fnargs[i].Conv && getFnAddress(fnargs[i].Conv) != (uintptr_t)NoConversion) {
			errs() << Loc << ": cannot call '" << (name ? name : "fn") << "()' candidate with matching signature would require conversion of "
			       << i+1 << (!i ? "st" : (i==1) ? "nd" : (i==2) ? "rd" : "th") << " argument which is passed by reference\n";
			return -1;
		}
	return selected_idx;
}

CallExprAST::CallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Callee_,
            std::vector<std::unique_ptr<ExprAST>> Args_)
	: ExprAST(Loc), Callee(std::move(Callee_)),
	  Args(std::move(Args_)) {
	unsigned n_args = Args.size();
	auto functionexpr = dynamic_cast<FunctionExprAST*>(Callee.get());
	if (functionexpr)
		name = functionexpr->Name.c_str();
	else if (auto varexpr = dynamic_cast<VariableExprAST*>(Callee.get()))
		name = varexpr->Name.c_str();
	auto method = dynamic_cast<MethodExprAST*>(Callee.get());
	auto type_expr = dynamic_cast<TypeExprAST*>(Callee.get());
	auto select_expr = dynamic_cast<SelectExprAST*>(Callee.get());
do_analyze:
	if (type_expr) {
		name = type_expr->Name.c_str();
		ft = type_expr->ft;
	}
	if (method)
		n_args++;
	fn_args.reserve(n_args);
	if (method)
		fn_args.push_back(FnArg{nullptr, method->Receiver->ft->type,
		                        static_cast<bool>(method->Receiver->ft->type_attr & A_signed),
		                        method->Receiver->is_unknown_type});
	else if (type_expr && type_expr->ft->type->isStructTy())
		fn_args.push_back(FnArg{nullptr, type_expr->ft->type, type_expr->ft->type_attr, false, false});
	else if (select_expr)
		name = select_expr->FieldName;
	if (!type_expr || type_expr->ft->type->isStructTy()) {
		for (auto& arg: Args) {
			bool is_list = false;
			if (!arg->ft || !arg->ft->type) {
				is_list = (bool)dynamic_cast<ListExprAST*>(arg.get());
				if (!is_list) {
					errs() << arg->Loc << ": function argument indeterminate " << (void*)arg->ft->type << "\n";
					ft->type = nullptr;
					return;
				}
			}
			fn_args.push_back(FnArg{nullptr, arg->ft->type, arg->ft->type_attr, arg->is_unknown_type, is_list});
		}
		std::vector<std::unique_ptr<PrototypeAST>>* protos;
		if (type_expr) {
			protos = findProtos(std::string(type_expr->ft->mangled_name), type_expr->Name);
		} else {
			protos = Callee->ft->Protos;
		}
		if (!protos) {
			if (!type_expr) {
				errs() << Loc << ": no prototype for call expression found\n";
				ft = nullptr;
			}
			return;
		}
		int selected_proto = selectProto(protos, name, fn_args, Callee->Loc);
		if (selected_proto >= 0)
			Proto = (*protos)[selected_proto].get();
		else if (selected_proto == -2) {
			// explicit basic type conversion
			auto ft = lex.source_stack.front().module->type_table.get_full(name);
			if (!ft)
				return;
			auto thetype_expr = std::make_unique<TypeExprAST>(Callee->Loc, name, ft);
			type_expr = thetype_expr.get();
			Callee = std::move(thetype_expr);
			goto do_analyze;
		} else
			return;
		if (!type_expr)
			ft = Proto->RetType;
		if (functionexpr)
			functionexpr->selected_proto = selected_proto;
	}
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

llvm::DISubroutineType *CreateFunctionType(volvoxc::FullType* RetType, std::vector<volvoxc::FullType*>& ArgTypes, llvm::DIFile *Unit) {
	llvm::SmallVector<llvm::Metadata *, 8> EltTys;

	// Add the result type.
	EltTys.push_back(lex.get_diType(RetType->type, RetType->type_attr & A_signed));
	auto NumArgs = ArgTypes.size();
	for (unsigned i = 0; i < NumArgs; i++)
		EltTys.push_back(lex.get_diType(ArgTypes[i]->type, ArgTypes[i]->type_attr & A_signed));

	return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(EltTys));
}

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
llvm::AllocaInst* CreateEntryBlockAlloca(llvm::Type* type, const llvm::Twine& VarName,
                                         llvm::Function* TheFunction) {
	if (!TheFunction)
		TheFunction = Builder->GetInsertBlock()->getParent();
	llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
	                       TheFunction->getEntryBlock().begin());
	return TmpB.CreateAlloca(type, nullptr, VarName);
}

void InsertArrayConDestructor(llvm::Type* elem_type, // actually array_type
                              volvoxc::FullType* array_elem_type, llvm::Value* val, llvm::Instruction* before,
                              bool is_constructor) {
	llvm::Function* destructor = getDestructor(array_elem_type, false, is_constructor);
	if (!destructor)
		return;
	auto struct_type = llvm::dyn_cast<llvm::StructType>(val->getType());
	llvm::Value* AllocSize = getSize(1);
	unsigned i = 0;
	while (auto array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)) {
		elem_type = array_type->getElementType();
		if (auto n = array_type->getNumElements())
			AllocSize = Builder->CreateMul(AllocSize, getSize(n));
		else
			AllocSize = Builder->CreateMul(AllocSize, Builder->CreateExtractValue(val, i++));
	}
	if (i && (!struct_type || i != struct_type->getNumElements()-1)) {
		errs() << "internal error in array destructor creation - value does not match type " << i << ' ' << *struct_type << "\n";
		abort();
	}
	auto elem_sz = TheModule->getDataLayout().getTypeAllocSize(elem_type);
	auto ElemAllocSize = getSize(elem_sz);
	AllocSize = Builder->CreateMul(AllocSize, ElemAllocSize);
	llvm::Type* elem_ptr_ty = elem_type->getPointerTo();
	auto elDestructorFT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { elem_ptr_ty }, false);
	llvm::BasicBlock* enterBB = Builder->GetInsertBlock();
	llvm::Function* TheFunction = enterBB->getParent();
	llvm::Value* Ptr = i ? Builder->CreateExtractValue(val, i) : val;
	Ptr = Builder->CreatePtrToInt(Ptr, llvm_size_type);
	llvm::Value* PtrStore = CreateEntryBlockAlloca(llvm_size_type, "", TheFunction);
	Builder->CreateStore(Ptr, PtrStore);
	llvm::Value* UpperLimit = Builder->CreateAdd(Ptr, AllocSize);
	llvm::BasicBlock* ContBB;
	llvm::BasicBlock* CondBB = llvm::BasicBlock::Create(Context, "array_destructor_loop");
	if (!before)
		Builder->CreateBr(CondBB);
	if (before) {
		ContBB = enterBB->splitBasicBlock(before);
		auto term = enterBB->getTerminator();
		auto br = llvm::dyn_cast<llvm::BranchInst>(term);
		br->setSuccessor(0, CondBB);
	} else
		ContBB = llvm::BasicBlock::Create(Context, "contloop");
	llvm::BasicBlock* DestructorBB = llvm::BasicBlock::Create(Context, "loopwhile");
#if LLVM_VERSION_MAJOR >= 16
	TheFunction->insert(TheFunction->end(), DestructorBB);
#else
	TheFunction->getBasicBlockList().push_back(DestructorBB);
#endif
	Builder->SetInsertPoint(DestructorBB);
	Ptr = Builder->CreateLoad(llvm_size_type, PtrStore);
	llvm::Value* ElPtr = Builder->CreateIntToPtr(Ptr, elem_ptr_ty);
	Builder->CreateCall(elDestructorFT, destructor, ElPtr);
	llvm::Value* NewPtr = Builder->CreateAdd(Ptr, ElemAllocSize);
	Builder->CreateStore(NewPtr, PtrStore);
	Builder->CreateBr(CondBB);
#if LLVM_VERSION_MAJOR >= 16
	TheFunction->insert(TheFunction->end(), CondBB);
#else
	TheFunction->getBasicBlockList().push_back(CondBB);
#endif
	Builder->SetInsertPoint(CondBB);
	Ptr = Builder->CreateLoad(llvm_size_type, PtrStore);
	auto is_less = Builder->CreateICmpULT(Ptr, UpperLimit);
	Builder->CreateCondBr(is_less, DestructorBB, ContBB);
	if (before)
		Builder->SetInsertPoint(before);
	else {
#if LLVM_VERSION_MAJOR >= 16
		TheFunction->insert(TheFunction->end(), ContBB);
#else
		TheFunction->getBasicBlockList().push_back(ContBB);
#endif
		Builder->SetInsertPoint(ContBB);
	}
}

// insert destructors for given var table - retp is a pointer to the function return value
// in case of struct-return - so this one will not be destructed but moved to the caller, instead
void InsertDestructors(VarTable& t, llvm::Value* retp) {
	llvm::Value* var_adr = nullptr;
	if (retp)
		if (auto load_instr = llvm::dyn_cast<llvm::LoadInst>(retp)) {
			var_adr = load_instr->getPointerOperand();
		}
	for (auto var_node = t.first(); var_node; ++var_node) {
		MapValue* node = var_node.getValue();
		auto fv = (FullVar*)((char*)node + node->offset);
		if ((fv->ft.type_attr & (A_destructor | A_string | A_map)) && fv->val && fv->val != retp
		    && (!var_adr || fv->val != var_adr))
			InsertDestructor(fv);
	}
}

// call function above for all local variable tables of the current function
void InsertDestructors(llvm::Value* retp) {
	if (locals_table.empty() && !(comp_mode == comp_jit && !do_test))
		for (auto& [modname, module] : Modules) {
			if (module.globals_table.table)
				InsertDestructors(module.globals_table, retp);
		}
	else
		for (auto t = locals_table.rbegin(); t != locals_table.rend(); ++t )
			InsertDestructors(*t, retp);
}

// insert destructors for intermediate results - this is done afer each complete expression
void InsertDestructors(std::vector<FullVar>& t) {
	if (t.empty())
		return;
	for (auto& fv: t) {
		if (fv.ft.type_attr & (A_destructor | A_string | A_map))
			InsertDestructor(&fv);
	}
	t.clear();
}

static bool insert_field_destructors(volvoxc::FullType* ft, llvm::Argument* thisarg, bool is_constructor = false) {
	bool needs_destructors = false;
	for (auto field = ft->first(); field; ++field) {
		auto el_ft = field.getFt();
		if (el_ft->type_attr & (is_constructor ? A_constructor : A_destructor)) {
			needs_destructors = true;
			unsigned idx = field.getIndex();
			llvm::Value* elem_ref = Builder->CreateConstGEP2_32(ft->type, thisarg, 0, idx);
			llvm::Function* field_destructor = getDestructor(el_ft, false, is_constructor);
			auto FT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { el_ft->type->getPointerTo() }, false);
			Builder->CreateCall(FT, field_destructor, elem_ref);
		} else if (isa<llvm::ArrayType>(el_ft->type) && (el_ft->elem_type->type_attr & (is_constructor ? A_constructor : A_destructor))) {
			needs_destructors = true;
			unsigned idx = field.getIndex();
			llvm::Value* elem_ref = Builder->CreateConstGEP2_32(ft->type, thisarg, 0, idx);
			InsertArrayConDestructor(el_ft->type, el_ft->elem_type, elem_ref, nullptr, is_constructor);
		}
	}
	return needs_destructors;
}

llvm::Value* Volvox2CStr1(llvm::Value* v) {
	// LLVM implementation of macro '#define volvox2cstr(v)
#if LLVM_VERSION_MAJOR < 15
	v = Builder->CreatePointerCast(v, llvm_size_type->getPointerTo());
#endif
	llvm::Value* subtrahend = Builder->CreateLoad(llvm_size_type, v);
	return subtrahend;
}

llvm::Value* getArrayCap(llvm::Value* v) {
	// LLVM implementation of macro '#define volvox2cstr(v)
	auto vp = Builder->CreatePointerCast(v, llvm_size_type->getPointerTo());
	auto cp = Builder->CreateConstGEP1_32(llvm_size_type, vp, 1);
	llvm::Value* cap = Builder->CreateLoad(llvm_size_type, cp);
	return cap;
}

llvm::Value* Volvox2CStr2(llvm::Value* v, llvm::Value* subtrahend) {
	llvm::Value* cstr = Builder->CreateSub(Builder->CreatePtrToInt(v, llvm_size_type), subtrahend);
	uint64_t mask;
	if (target_bits == 64)
		mask = (uint64_t)(int64_t)(-1);
	else
		mask = (1ULL << target_bits) - 1;
	cstr = Builder->CreateAnd(cstr, mask & ~(uint64_t)(target_bytes - 1));
	return Builder->CreateIntToPtr(cstr, llvm_ptr_type);
}

llvm::Value* Volvox2CStr(llvm::Value* v) {
	auto subtrahend = Volvox2CStr1(v);
	return Volvox2CStr2(v, subtrahend);
}

void InsertStringDestructor(llvm::Value* v, llvm::Instruction* before) {
	// TODO: handle 'before' (is this even needed?)
	llvm::BasicBlock* enterBB = Builder->GetInsertBlock();
	llvm::Function* TheFunction = enterBB->getParent();
	auto subtrahend = Volvox2CStr1(v);
	llvm::Value* destructflag = getArrayCap(v);
	destructflag = Builder->CreateIsNotNull(destructflag);
	llvm::BasicBlock* DestructorBB = llvm::BasicBlock::Create(Context, "stringdestr");
	llvm::BasicBlock* ContBB = llvm::BasicBlock::Create(Context, "contdestr");
	Builder->CreateCondBr(destructflag, DestructorBB, ContBB);
#if LLVM_VERSION_MAJOR >= 16
	TheFunction->insert(TheFunction->end(), DestructorBB);
#else
	TheFunction->getBasicBlockList().push_back(DestructorBB);
#endif
	Builder->SetInsertPoint(DestructorBB);
	auto cstr = Volvox2CStr2(v, subtrahend);
#if LLVM_VERSION_MAJOR >= 18
	Builder->CreateFree(cstr);
#else
	Builder->Insert(llvm::CallInst::CreateFree(cstr, DestructorBB));
#endif
	Builder->CreateBr(ContBB);
#if LLVM_VERSION_MAJOR >= 16
	TheFunction->insert(TheFunction->end(), ContBB);
#else
	TheFunction->getBasicBlockList().push_back(ContBB);
#endif
	Builder->SetInsertPoint(ContBB);
}

void InsertMapDestructor(llvm::Value* v, llvm::Instruction* before) {
	std::string destr = "_ZN6volvox3map7destroyEPNS0_4NodeEPFvPNS0_5ValueEE";
	PrototypeAST* destr_proto = (*lex.findProtos(destr))[0].get();
	auto destr_fn = getFunction(destr_proto);
	auto elem_destructor = llvm::ConstantPointerNull::get(llvm_ptr_type);
	Builder->CreateCall(destr_proto->FT, destr_fn, std::vector<llvm::Value*>{ v, elem_destructor });
}

static void check_destructor(const char* type_name, volvoxc::FullType* ft, bool is_constructor) {
	if (llvm::isa<llvm::StructType>(ft->type)) {
		auto D = getDestructor(ft, true, is_constructor);
		auto thisarg = D->getArg(0);
		llvm::BasicBlock *BB = llvm::BasicBlock::Create(Context, "entry", D);
		Builder->SetInsertPoint(BB);
		bool needs_destructors = insert_field_destructors(ft, thisarg, is_constructor);
		if (!needs_destructors) {
			D->eraseFromParent();
			return;
		}
		Builder->CreateRetVoid();
		if (is_constructor)
			AutoMethods[ft->mangled_name].first = std::string(D->getName());
		else
			AutoMethods[ft->mangled_name].second = std::string(D->getName());
		finishFunctionOrModule(D, 1, false);
		ft->type_attr |= (is_constructor ? A_constructor : A_destructor);
	}
}

/* Destructor and constructor declarations are only allowed right after the corresponding
 * type declaration. The following function is called when the next code is something else
 * i.e. the constructor/destructor section is finished. The point is that the type may need
 * a destructor/default constructor an to handle struct fields even if none has been explicitly
 * defined. In this case a destructor/default constructor is created automatically
 */
void finish_constructors_and_destructor() {
	auto ft = lex.get_full_type(last_defined_type);
	if (ft) {
		if (!(ft->type_attr & A_destructor))
			check_destructor(last_defined_type, ft, false);
		if (!(ft->type_attr & A_constructor))
			check_destructor(last_defined_type, ft, true);
	} else {
		errs() << "could not find type '" << last_defined_type << "'\n";
	}
	last_defined_type = nullptr;
}

bool finishFunctionOrModule(llvm::Function* F, unsigned dumpLevel, bool finishModule, bool newModule) {
	bool success = true;
	if (F) {
		success = !verifyFunction(*F, &errs());
		if (success) {
			if (dump_IR >= dumpLevel && dump_raw) {
				errs() << "Read \"" << F->getName() << "()\" definition (raw):\n";
				F->print(errs());
				errs() << "\n";
			}
#ifdef LEGACY_PASS_MANAGER
			TheFPM->run(*F);
#endif
		} else {
			F->print(errs());
			F->eraseFromParent();
		}
	}
	if (finishModule) {
#ifndef LEGACY_PASS_MANAGER
		// running the new PassManager on an empty module causes trouble :-(
		// let's avoid this...
		if (TheModule->end() != TheModule->begin()) {
			LAM = llvm::LoopAnalysisManager();
			FAM = llvm::FunctionAnalysisManager();
			CGAM = llvm::CGSCCAnalysisManager();
			MAM = llvm::ModuleAnalysisManager();
			PB = llvm::PassBuilder(TheTargetMachine, PTO);
			PB.registerModuleAnalyses(MAM);
			PB.registerCGSCCAnalyses(CGAM);
			PB.registerFunctionAnalyses(FAM);
			PB.registerLoopAnalyses(LAM);
			PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
			llvm::ModulePassManager MPM;
			if (optimization_level == llvm::OptimizationLevel::O0)
				MPM = PB.buildModuleSimplificationPipeline(optimization_level, llvm::ThinOrFullLTOPhase::ThinLTOPostLink);
			else
				MPM = PB.buildPerModuleDefaultPipeline(optimization_level);
			MPM.run(*TheModule, MAM);
		}
#endif
		if (dump_IR >= dumpLevel && dump_opt)
			TheModule->print(errs(), nullptr);
		if (newModule) {
			ExitOnErr(TheJIT->addModule(
				          llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context)));
			InitializeModuleAndPassManager();
		}
	}
	Builder->ClearInsertionPoint();
	return success;
}

llvm::Function *PrototypeAST::codegen() {
	llvm::Function *F =
		llvm::Function::Create(FT, link_typ, 0, Name, TheModule.get());
	// Set names for all arguments.
	unsigned Idx = 0;
	unsigned ArgIdx = 0;
	if (IsStructRet) {
#if LLVM_VERSION_MAJOR >= 14
		llvm::AttrBuilder attr_builder(Context, llvm::Attribute::getWithStructRetType(Context, RetType->type));
		F->getArg(Idx)->addAttrs(attr_builder);
#else
		F->getArg(Idx)->addAttr(llvm::Attribute::getWithStructRetType(Context, RetType->type));
#endif
		Idx++;
	}
	for (auto &Arg : Args) {
		auto fnarg = F->getArg(Idx);
		if (ArgAttrs[ArgIdx].hasAttributes()) {
#if LLVM_VERSION_MAJOR >= 14
			llvm::AttrBuilder attr_builder(Context, ArgAttrs[ArgIdx]);
			fnarg->addAttrs(attr_builder);
#else
			for (auto attr: ArgAttrs[ArgIdx])
				fnarg->addAttr(attr);
#endif
		}
		fnarg->setName(Arg);
		Idx++;
		ArgIdx++;
	}
	if (visibility & A_inline)
		F->addFnAttr(llvm::Attribute::AlwaysInline);
	return F;
}

llvm::Value* StringDup(llvm::Value* str) {
	auto converter_name = "__cstring_dup";
	auto converter_proto = (*lex.findProtos(converter_name))[0].get();
	auto converter = getFunction(converter_proto);
	return Builder->CreateCall(converter_proto->FT, converter, std::vector<llvm::Value*>({ str }));
}

llvm::Value* CallExprAST::codegen_raw(llvm::Value* target) {
	bool is_error = Proto && Proto->Name == "__error";
	if (is_error || Proto && Proto->Name == "__link_extra") {
		if (Args.empty() || !(Args[0]->ft->type_attr & A_string)) {
			errs() << Loc << ": '" << Proto->Name << "()' requires at least 1 argument\n";
			return nullptr;
		}
		if (is_error)
			errs() << Loc << ':';
		for (auto& arg: Args) {
			if (auto lit = dynamic_cast<LiteralExprAST*>(arg.get())) {
				if (!(arg->ft->type_attr & A_string) || !strlen(lit->Val.CStr)) {
					if (is_error)
						errs() << '\n';
					errs() << Loc << ": " << Proto->Name << " requires constant non-empty string literals as arguments\n";
					return nullptr;
				}
				if (is_error) {
					errs() << ' ' << std::string(lit->Val.CStr, lit->Val.Len);
				} else {
					if (lit->Val.CStr[0] == '-' || lit->Val.CStr[0] == '/') {
						if (comp_mode != comp_jit)
							extra_libs.push_back(lit->Val.CStr);
					} else {
						if (comp_mode == comp_jit) {
							std::string dll;
#ifdef _WIN32
							dll = std::string(lit->Val.CStr, lit->Val.Len) + ".dll";
							auto handle = LoadLibraryA(dll.c_str());
							if (handle) {
								extra_dlls.push_back(handle);
							}
#else
							if (!strncmp(lit->Val.CStr, "lib", 3))
								dll = std::string(lit->Val.CStr, lit->Val.Len);
							else
								dll = std::string("lib") + lit->Val.CStr + ".so";
							auto handle = dlopen(dll.c_str(), RTLD_NOW | RTLD_GLOBAL);
#endif
							if (handle) {
								if (verbosity >= 2)
									errs() << ": loaded extra DLL '" << dll << "'\n";
							} else {
								char* msg = nullptr;
#ifdef _WIN32
								DWORD last_err = GetLastError();
								FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
								                           FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last_err, 0, (LPTSTR)&msg, 0, nullptr);
#else
								msg = dlerror();
#endif
								errs() << arg->Loc << ": cannot load extra library '" << dll << "': " << msg << '\n';
#ifdef _WIN32
								LocalFree(msg);
#endif
								return nullptr;
							}
						} else {
							std::string lib;
#ifdef _WIN32
							if (!target_mingw) {
							  lib = std::string(lit->Val.CStr, lit->Val.Len) + ".lib";
							} else {
#endif
							  if (!strncmp(lit->Val.CStr, "lib", 3))
								lib = std::string(lit->Val.CStr, lit->Val.Len);
							  else
								lib = std::string("-l") + std::string(lit->Val.CStr, lit->Val.Len);
#ifdef _WIN32
							}
#endif
							extra_libs.push_back(std::move(lib));
						}
					}
				}
			} else {
				if (is_error)
					errs() << '\n';
				errs() << Loc << ": " << Proto->Name << " requires constant string literals as arguments\n";
				return nullptr;
			}
		}
		if (is_error)
			return nullptr;
		else
			return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
	}
	if (Proto && Proto->Name == "sizeof") {
		if (Args.size() != 1) {
			errs() << Loc << ": '" << Proto->Name << "()' requires exactly 1 argument\n";
			return nullptr;
		}
		auto arg = Args[0].get();
		size_t allocsz = 0;
		if (!arg->ft || !arg->ft->type) {
			errs() << arg->Loc << ": invalid argument\n";
			return nullptr;
		}
		if (arg->ft->type->isSized())
			allocsz = TheModule->getDataLayout().getTypeAllocSize(arg->ft->type);
		// TODO: calculate run-time size of arrays when 'codegen_dims()' is available
		return getSize(allocsz);
	}
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	if (!Proto) {
		if (auto type_expr = dynamic_cast<TypeExprAST*>(Callee.get())) {
			if (Args.empty())
				return llvm::Constant::getNullValue(ft->type);
			if (Args.size() == 1) {
				Args[0]->desired_type = ft->type;
				llvm::Value* expr = Args[0]->codegen_raw();
				std::function<llvm::Value*(llvm::Value*)> conv = nullptr;
				if (expr->getType()->isPointerTy()) {
					// special handling for string types
					if ((Args[0]->ft->type_attr & A_string)
					    && (ft->type_attr & A_cstring)) {
						conv = Volvox2CStr;
					} else if ((Args[0]->ft->type_attr & A_cstring)
					         && (ft->type_attr & A_string)) {
						auto converter_name = "__cstr2volvox";
						auto converter_proto = (*lex.findProtos(converter_name))[0].get();
						auto converter = getFunction(converter_proto);
						return Builder->CreateCall(converter_proto->FT, converter, std::vector<llvm::Value*>({ expr }));
					} else {
						conv = NoConversion;
					}
				} else
					conv = getConv(expr->getType(), ft->type, Loc, (bool)(Args[0]->ft->type_attr & A_signed),
					               (bool)(ft->type_attr & A_signed), true, false, nullptr);
				if (conv)
					return conv(expr);
				else
					return nullptr;
			}
		}
	}
	if (auto selec = dynamic_cast<SelectExprAST*>(Callee.get())) {
		// usual mehod calls like 's.m(...)' are identified as CallExpr and are handled there
		// however this does not work if 's' is not a struct type. So the pseudo method
		// call 'a.dim(n)' where a is an array has to be handled here
		llvm::ArrayType* arr_type;
		if (selec->Struct->ft && selec->Struct->ft->type && (arr_type = llvm::dyn_cast<llvm::ArrayType>(selec->Struct->ft->type))) {
			auto arg = Args[0]->codegen();
			auto Dim = llvm::dyn_cast<llvm::ConstantInt>(arg);
			// if (!Dim) {
			// 	errs() << Args[0]->Loc << ": argument of 'dim()' must be a constexpr\n";
			// 	return nullptr;
			// }
			int theidx;
			if (Dim) {
				theidx = Dim->getSExtValue();
				if (theidx < 0) {
					errs() << Args[0]->Loc << ": argument of 'dim' (" << theidx << ") must not be negative\n";
					return nullptr;
				}
			} else {
				theidx = -1;
			}
			llvm::Value* arr = nullptr;
			llvm::Type* arr_ty = (llvm::Type*)(intptr_t)-1;
			if (auto array_ast = dynamic_cast<LvalueExprAST*>(selec->Struct.get()))
				std::tie(arr_ty, arr) = array_ast->codegen_ref(true);
			if (!arr && arr_ty)
				arr = selec->Struct->codegen_raw();
			if (!arr) {
				errs() << Loc << ": invalid array\n";
				return nullptr;
			}
			// update type after codegen
			arr_type = llvm::dyn_cast<llvm::ArrayType>(selec->Struct->ft->type);
			uint64_t size = 0;
			llvm::Value* Size = nullptr;
			int order = 0;
			int idx = 0;
			std::vector<llvm::Value*> dims_array; // only needed if Dim is no constexpr
			while (arr_type) {
				uint64_t dim = arr_type->getNumElements();
				if (!dim) {
					if (!Dim)
						dims_array.push_back(Builder->CreateExtractValue(arr, idx));
					else
						if (order == theidx)
							Size = Builder->CreateExtractValue(arr, idx);
					idx++;
				} else {
					if (!Dim)
						dims_array.push_back(getSize(dim));
					else
						if (order == theidx)
							size = dim;
				}
				arr_type = llvm::dyn_cast<llvm::ArrayType>(arr_type->getElementType());
				order++;
			}
			if (size)
				return handle(target, getSize(size));
			if (Size)
				return handle(target, Size);
			if (Dim) {
				errs() << Loc << ": argument of 'dim' (" << theidx << ") must be less than order of tensor ("
				       << order << ")\n";
				return nullptr;
			}
			// "Dim" aka "arg" was no compile time const - but we have all dimensions saved in dims_array
			llvm::Type* dim_arr_type = llvm::FixedVectorType::get(llvm_size_type, dims_array.size());
			llvm::Value* DimsArray = llvm::UndefValue::get(dim_arr_type);
			for (int i=0; i<dims_array.size(); i++)
				DimsArray = Builder->CreateInsertElement(DimsArray, dims_array[i], i);
			return handle(target, Builder->CreateExtractElement(DimsArray, arg));
		}
	}
	TypeExprAST* type_expr;
	if (Proto->visibility & A_constructor && (type_expr = dynamic_cast<TypeExprAST*>(Callee.get()))) {
		uint64_t allocsz = TheModule->getDataLayout().getTypeAllocSize(type_expr->ft->type);
		llvm::Value* ret_val = nullptr;
		if ((!target || (intptr_t)target == -1) && (allocsz > sret_limit || (type_expr->ft->type_attr & A_constructor)))
			target = ret_val = CreateEntryBlockAlloca(type_expr->ft->type, "");
		if (target && (intptr_t)target != -1)
			Builder->CreateStore(llvm::Constant::getNullValue(ft->type), target);
		if (!llvm::isa<llvm::StructType>(ft->type)) {
			switch (Args.size()) {
			case 0:
				return llvm::Constant::getNullValue(ft->type);
			case 1:
				Args[0]->desired_type = ft->type;
				Args[0]->conv_kind = ft->type_attr & A_signed ? ConvSigned : ConvUnsigned;
				return Args[0]->codegen();
			default:
				errs() << "conversions with #arg!=1 not supported\n";
				return nullptr;
			}
		}
	}
	llvm::Value* theFunction = nullptr;
	if (!dynamic_cast<TypeExprAST*>(Callee.get()))
		theFunction = Callee->codegen();
	if (!theFunction)
		theFunction = getFunction(Proto);
	if (!theFunction)
		abort();
	auto FT = Proto->FT;
	unsigned arg_offs = (Proto->visibility & A_method) ? 1 : 0;
	std::vector<llvm::Value *> ArgsV;
	llvm::Value* ret_struct = nullptr;
	if (needs_target()) {
		if (!target || (intptr_t)target == -1) {
			errs() << Loc << ": " << Proto->Name << " - internal error: no target for struct return\n";
			return nullptr;
		}
		ArgsV.push_back(target);
	}
	if (Proto->visibility & A_method && !(Proto->visibility & A_constructor)) {
		if (auto method = dynamic_cast<MethodExprAST*>(Callee.get())) {
			llvm::Value* receiver_ref = nullptr;
			if (auto receiver_lval = dynamic_cast<LvalueExprAST*>(method->Receiver.get())) {
				llvm::Type* receiver_type;
				std::tie(receiver_type, receiver_ref) = receiver_lval->codegen_ref(true);
				if (!receiver_type) {
					errs() << method->Receiver->Loc << ": could not get receiver\n";
					return nullptr;
				}
			}
			if (!receiver_ref) {
				if (method->Receiver->needs_target()) {
					receiver_ref = Builder->CreateAlloca(method->Receiver->ft->type, nullptr, "receiver");
					auto voidval = method->Receiver->codegen_raw(receiver_ref);
					if (!voidval || !voidval->getType()->isVoidTy()) {
						errs() << Loc << ": cannot create function call\n";
						return nullptr;
					}
				} else {
					receiver_ref = StoreValue(method->Receiver->codegen(), method->Receiver->ft);
				}
				if (!receiver_ref) {
					errs() << method->Receiver->Loc << ": internal error - could not store receiver\n";
					return nullptr;
				}
			}
			ArgsV.push_back(receiver_ref);
		} else {
			errs() << Callee->Loc << ": method prototype but not a method call\n";
			return nullptr;
		}
	}
	for (unsigned i = 0; i < Args.size(); ++i) {
		bool by_val = false;
		bool is_address = (i+arg_offs) < Proto->ArgAttrs.size()
			&& (Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByVal)
			    || (by_val = Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByRef)));
		if (!is_address && (Args[i]->ft->type_attr & (A_string | A_cstring))) {
			llvm::Value* arg = Args[i]->codegen();
			if ((Args[i]->ft->type_attr & A_string) && ((i+arg_offs) >= Proto->ArgAttrs.size() || Proto->ArgTypes[i+arg_offs]->type_attr & A_cstring))
				arg = Volvox2CStr(arg);
			ArgsV.push_back(arg);
		} else {
			llvm::Type* real_arg_type;
			if ((i+arg_offs) < Proto->ArgAttrs.size())
				real_arg_type = Args[i]->desired_type = Proto->ArgTypes[i+arg_offs]->type;
			else
				real_arg_type = Args[i]->ft->type;
			llvm::Value* arg = nullptr;
			bool is_aggregate_lit = dynamic_cast<StructExprAST*>(Args[i].get()) || dynamic_cast<ListExprAST*>(Args[i].get()) || dynamic_cast<TypeExprAST*>(Args[i].get());
			if (Args[i]->needs_target() || is_aggregate_lit && (Proto->ArgTypes[i+arg_offs]->type_attr & (A_constructor | A_destructor))) {
				arg = Builder->CreateAlloca(Args[i]->desired_type ? Args[i]->desired_type : Args[i]->ft->type, nullptr, "target");
				auto voidval = Args[i]->codegen_raw(arg);
				if (!voidval || !voidval->getType()->isVoidTy()) {
					errs() << Args[i]->Loc << ": cannot create function call argument\n";
					return nullptr;
				}
				if ((i+arg_offs) < Proto->ArgAttrs.size() && arg && arg->getType()->isPointerTy() && is_aggregate_lit) {
					if (Proto->ArgTypes[i+arg_offs]->type_attr & A_constructor) {
						auto F = getConstructorOrDestructor(Proto->ArgTypes[i+arg_offs]);
						if (!F) {
							errs() << Args[i]->Loc << ": internal error - default constructor not found for " << *Proto->ArgTypes[i+arg_offs] << "\n";
							return nullptr;
						} else
							Builder->CreateCall(F, { arg });
					}
					handle_d_0(Proto->ArgTypes[i+arg_offs], arg);
				}
				if (!is_address && !dynamic_cast<InterfaceExprAST*>(Args[i].get()))
					arg = Builder->CreateLoad(real_arg_type, arg);
			}
			if (!arg) {
				if (is_address) {
					if (auto lval = dynamic_cast<LvalueExprAST*>(Args[i].get())) {
						auto argref = lval->codegen_ref(true, by_val);
						if (!argref.first) {
							errs() << Args[i]->Loc << ": cannot generate reference function argument\n";
							return nullptr;
						}
						if (argref.second)
							// TODO: handle valiable sized arrays
							arg = Builder->CreatePointerCast(argref.second, argref.first->getPointerTo());
					}
					if (!arg) {
						auto lit = dynamic_cast<LiteralExprAST*>(Args[i].get());
						if (lit && lit->ft->type->isPointerTy()) {
							llvm::Type* target_type =
								(Proto->ArgTypes[i+arg_offs]->type_attr & A_ref) ?
								Proto->ArgTypes[i+arg_offs]->type->getPointerTo() :
								Proto->ArgTypes[i+arg_offs]->type;
							arg = Builder->CreatePointerCast(Args[i]->codegen_raw(), target_type);
						} else {
							arg = Builder->CreateAlloca(Proto->ArgTypes[i+arg_offs]->type, nullptr, "tmprefarg");
							//errs() << Loc << ": arg #" << i << " " << *arg << '\n';
							auto tmparg = Args[i]->codegen_raw();
							if (!tmparg) {
								errs() << Args[i]->Loc << ": cannot generate code for2 expression\n";
								return nullptr;
							}
							// The following sections are ugly hacks to circumvent bugs in
							// over aggressive optimizers. In particular a storage space might be
							// optimized away even if a pointer (or a pointer to a pointer) to this
							// space is passed to a called function - however, 'volatile' storage can help.
							// Not all LLVM versions and backends have these bugs and they depend
							// on optimization levels. This should be taken into account to avoid
							// unnecessary performance impacts
							bool store_volatile = false;
#ifndef LEGACY_PASS_MANAGER
							if (jit_repl && optimization_level != llvm::OptimizationLevel::O0
							    && optimization_level != llvm::OptimizationLevel::O1)
								// the new optimizer tends to optimize this store away in -O2 and higher
								// (probably a bug in LLVM) we can work around this by making this store volatile
								store_volatile = true;
#endif
							Builder->CreateStore(tmparg, arg, store_volatile);
						}
					}
#if LLVM_VERSION_MAJOR < 14 || defined(__aarch64__)
					if (
#if defined(__aarch64__)
						comp_mode == comp_jit
#else
						codegenopt != llvm::CodeGenOpt::None
#endif
						) {
						// Old LLVM versions seem to do illegal optimizations for call by reference
						// in object code generation mode. These can be suppressed by reloading the
						// reference after having stored it 'volatile'
						auto rec_ptr_loc = CreateEntryBlockAlloca(arg->getType() "tmp_refarg");
						Builder->CreateStore(arg, rec_ptr_loc, true);
						arg = Builder->CreateLoad(arg->getType(), rec_ptr_loc);
					}
#endif
				} else {
					arg = Args[i]->codegen();
					//errs() << Loc << ": valarg #" << i << " " << *arg << ' ' << Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByVal) << '\n';
				}
			}
			if (!arg)
				return nullptr;
			if ((i+arg_offs) >= Proto->ArgAttrs.size()) {
				if (arg->getType()->isFloatTy()) {
					// C convention: variadic float args must be promoted to double
					arg = Builder->CreateFPCast(arg, llvm::Type::getDoubleTy(Context));
				} else if (auto intT = llvm::dyn_cast<llvm::IntegerType>(arg->getType())) {
					// same with short integers
					if (intT->getBitWidth() < 32)
						arg = Builder->CreateIntCast(arg, llvm::Type::getInt32Ty(Context), !(!(Args[i]->ft->type_attr & A_signed)));
				}
			}
			ArgsV.push_back(arg);
		}
		if (!ArgsV.back())
			return nullptr;
	}
	if (auto F = llvm::dyn_cast<llvm::Function>(theFunction)) {
		// Callee was a function symbol like `sin`
		if (Proto->const_result)
			return Proto->const_result;
		return Builder->CreateCall(FT, F, ArgsV);
	} else {
		// theFunction is a function pointer, i.e. a function call address (e.g. loaded from a variable)
		// llvm::Type* Ft = theFunction->getType();
		// errs() << "Function: " << *theFunction << ' ' << *Ft << '\n';
		// llvm::Value* fstore = CreateEntryBlockAlloca(Ft);
		// Builder->CreateStore(theFunction, fstore, true);
		// theFunction = Builder->CreateLoad(Ft, fstore, true);
		return Builder->CreateCall(FT, theFunction, ArgsV);
	}
}

bool FunctionAST::prepare_codegen() {
	// Transfer ownership of the prototype to the lex.module->FunctionProtos map, but keep a
	// reference to it for use below.
	if ((Proto->visibility & (A_method | A_constructor)) && Proto->returnName.empty())
		receiver_ft = Proto->ArgTypes[0];
	else
		receiver_ft = nullptr;
	TheFunction = getFunction(Proto);
	if (!TheFunction) {
		return false;
	}
	// Create a new basic block to start insertion into.
	BB = llvm::BasicBlock::Create(Context, "entry", TheFunction);
	Builder->SetInsertPoint(BB);
	// llvm::DISubprogram *SP; - make static
	// llvm::DIFile *Unit;
	unsigned LineNo;
	if (comp_mode == comp_dbg) {
		// Create a subprogram DIE for this function.
		Unit = DBuilder->createFile(KSDbgInfo.TheCU->getFilename(),
		                            KSDbgInfo.TheCU->getDirectory());
		llvm::DIScope *FContext = Unit;
		LineNo = Proto->getLine();
		unsigned ScopeLine = LineNo;
		SP = DBuilder->createFunction(
			FContext, Proto->getName(), llvm::StringRef(), Unit, LineNo,
			CreateFunctionType(Proto->RetType, Proto->ArgTypes, Unit), ScopeLine,
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
	ArgIdx = 0;
	ret_ft = Proto->RetType ? Proto->RetType : void_type;
	old_theFunction_ret_ft = theFunction_ret_ft;
	theFunction_ret_ft = ret_ft; // global variable used by IfExprAST to return from branches
	old_theFunction_struct_ret = theFunction_struct_ret;
	theFunction_struct_ret = Proto->IsStructRet && !(Proto->visibility & A_constructor);
	old_currentFunction = currentFunction;
	currentFunction = this;
	if (!Proto->returnName.empty()) {
		RetVar = locals_table.back()[Proto->returnName.c_str()];
		if (!RetVar) {
			errs() << Proto->retLoc << ": internal compiler error - return variable '" << Proto->returnName << "' not found in table\n";
			abort();
		}
		if (theFunction_struct_ret)
			RetVar->val = TheFunction->getArg(ArgIdx);
		else {
			RetVar->val = CreateEntryBlockAlloca(ret_ft->type, Proto->returnName, TheFunction);
			Builder->CreateStore(llvm::Constant::getNullValue(ret_ft->type), RetVar->val);
		}
	}
	if (RetVar)
		ret_ptr = this_ret_ptr = RetVar->val;
	if (theFunction_struct_ret)
		ret_ptr = this_ret_ptr = TheFunction->getArg(ArgIdx++);
	else
		ret_ptr = this_ret_ptr = nullptr;
	for (; ArgIdx < TheFunction->arg_size(); ArgIdx++) {
		auto Arg = TheFunction->getArg(ArgIdx);
		FullVar* mapitem = locals_table.back()[Arg->getName().str().c_str()];
		if (!mapitem) {
			errs() << Proto->retLoc << ": internal compiler error: arg #" << ArgIdx << " - '" << Arg->getName() << "' not found in table\n";
			for (unsigned i=0; i < TheFunction->arg_size(); i++) {
				auto Arg = TheFunction->getArg(i);
				errs() << ">" << Arg->getName().str() << "< ";
			}
			errs() << "\n";
			abort();
		}
		if (Arg->hasByValAttr() || Arg->hasByRefAttr()) {
			mapitem->val = Arg;
		} else {
			// Create an alloca for this variable.
			llvm::AllocaInst* Alloca = CreateEntryBlockAlloca(Proto->LLVMArgTypes[ArgIdx], Arg->getName(), TheFunction);
			// get reference to argument in symbol table
			// Store the initial value into the alloca.
			Builder->CreateStore(Arg, Alloca);

			// Add storage to variable in symbol table.
			mapitem->val = Alloca;
		}
		// destructors for function arguments should always be called
		// by the caller
		mapitem->destructor = nullptr;
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
	BB = Builder->GetInsertBlock();
	RetVal = nullptr;
	InterRetVal = nullptr;
	ret_ptr = nullptr;
	theFunction_ret_ft = nullptr;
	expr_temps.clear();
	return true;
}

bool FunctionAST::process_body(std::vector<std::unique_ptr<ExprAST>>& thisBody) {
	ret_ptr = this_ret_ptr;
	theFunction_ret_ft = ret_ft;
	Builder->SetInsertPoint(BB);
	if (EndKind == tok_return && !Proto->RetType->type->isVoidTy()) {
		if (thisBody.empty() || !thisBody.back())
			return false;
		thisBody.back()->desired_type = Proto->RetType->type;
	}
	if (return_val_idx < 0)
		return_val_idx = thisBody.size() - 1;
	for (auto& Expr : thisBody) {
		if (Expr->needs_target()) {
			if (!return_val_idx && this_ret_ptr) {
				RetVal = Expr->codegen_raw(this_ret_ptr);
			} else {
				llvm::Value* tmp = CreateEntryBlockAlloca(Expr->ft->type);
				if (!Expr->codegen_raw(tmp))
					return false;
				RetVal = Builder->CreateLoad(Expr->ft->type, tmp);
			}
		} else {
			RetVal = Expr->codegen();
		}
		if (RetVal) {
			if (!return_val_idx--)
				InterRetVal = RetVal; // hack for interactive JIT to return value of Expr instead of println()
			if (comp_mode == comp_dbg) {
				KSDbgInfo.emitLocation(Expr.get());
			}
			InsertDestructors(expr_temps);
		} else {
			errs() << Expr->Loc << ": error compiling expr\n";
			return false;
		}
	}
	BB = Builder->GetInsertBlock();
	ret_ptr = nullptr;
	theFunction_ret_ft = nullptr;
	expr_temps.clear();
	return true;
}

void HandleReturn(std::vector<std::unique_ptr<ExprAST>>& Branch, llvm::Value* RetVal)
{
	bool already_returned = false; // set if both branches of last 'if ... else ...' end with 'return'
	if (!Branch.empty())
		if (auto ifexpr = dynamic_cast<IfExprAST*>(Branch.back().get()))
			already_returned = ifexpr->always_return;
	if (!already_returned) {
		if (currentFunction->Proto->RetType->type->isVoidTy()
		    || (currentFunction->Proto->visibility & A_constructor) && !currentFunction->RetVar) {
			if (currentFunction->Proto->visibility & A_destructor) {
				insert_field_destructors(currentFunction->receiver_ft, currentFunction->TheFunction->getArg(0));
			}
			InsertDestructors(nullptr);
			Builder->CreateRetVoid();
		} else {
			// auto ret_type = RetVal->getType();
			//type = ret_type; // TODO: hande conversion if != proto->type;
			if (theFunction_struct_ret) {
				if (!ret_ptr) {
					errs() << currentFunction->Proto->Name << ": ### internal error: theFunction_struct_ret but no adr";
					abort();
				}
				if (!RetVal->getType()->isVoidTy() && !currentFunction->RetVar)
					Builder->CreateStore(RetVal, ret_ptr);
				InsertDestructors(ret_ptr);
				Builder->CreateRetVoid();
			} else {
				if (currentFunction->RetVar) {
					RetVal = Builder->CreateLoad(currentFunction->ret_ft->type, currentFunction->RetVar->val);
					InsertDestructors(currentFunction->RetVar->val);
				} else if (RetVal->getType()->isPointerTy())
					InsertDestructors(RetVal);
				else {
					llvm::Value* re_ptr = nullptr;
					if (!Branch.empty())
						if (auto lval = dynamic_cast<LvalueExprAST*>(Branch.back().get())) {
							llvm::Type* dummy;
							std::tie(dummy, re_ptr) = lval->codegen_ref(true);
							if (dummy && re_ptr)
								if (auto struct_type = llvm::dyn_cast<llvm::StructType>(re_ptr->getType()))
									re_ptr = Builder->CreateExtractValue((re_ptr), struct_type->getNumElements() - 1);
						}
					InsertDestructors(re_ptr);
				}
				Builder->CreateRet(CheckTailCall(RetVal));
				if (!currentFunction->ArgIdx && Branch.size() == 1 && currentFunction->TheFunction->hasFnAttribute(llvm::Attribute::AlwaysInline))
					if (auto const_ret = llvm::dyn_cast<llvm::Constant>(RetVal))
						// hack to allow trivial static functions to be used as constexpr
						currentFunction->Proto->const_result = const_ret;
			}
		}
	}
}

llvm::Function* FunctionAST::finish_codegen(bool finishModule, bool getNewModule) {
	bool already_returned = false;
	ret_ptr = this_ret_ptr;
	theFunction_ret_ft = ret_ft;
	Builder->SetInsertPoint(BB);
	if (InterRetVal)
		RetVal = InterRetVal;
	HandleReturn(Body, RetVal);
	if (comp_mode == comp_dbg) {
		// Pop off the lexical block for the function.
		KSDbgInfo.LexicalBlocks.pop_back();
	}
	// Validate the generated code, checking for consistency.
	bool success = finishFunctionOrModule(TheFunction, 1, finishModule, getNewModule);
	ret_ptr = nullptr;
	theFunction_ret_ft = nullptr;
	expr_temps.clear();
	currentFunction = old_currentFunction;
	theFunction_ret_ft = old_theFunction_ret_ft;
	theFunction_struct_ret = old_theFunction_struct_ret;
	return success ? TheFunction : nullptr;
}

llvm::Function* FunctionAST::cleanup_codegen() {
	// Error reading body, remove function.
	TheFunction->eraseFromParent();
	if (comp_mode == comp_dbg) {
		// Pop off the lexical block for the function since we added it
		// unconditionally.
		KSDbgInfo.LexicalBlocks.pop_back();
	}
	ret_ptr = nullptr;
	expr_temps.clear();
	currentFunction = old_currentFunction;
	theFunction_ret_ft = old_theFunction_ret_ft;
	theFunction_struct_ret = old_theFunction_struct_ret;
	return nullptr;
}
