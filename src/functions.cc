/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
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
std::map<std::string,SourceLocation> defined_functions;
std::vector<std::vector<var_usage_marker_t>*> all_usage_markers;

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
	return llvm::dyn_cast<llvm::Function>(FI->codegen());
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

llvm::Function* getShadowConstructorDestructor(std::string& mangled_name, int n, bool destructor) {
	auto thename = "___" + mangled_name + "_arg" + std::to_string(n) + (destructor ? "_destr" : "_constr");
	if (auto F = TheModule->getFunction(thename))
		return F;
	auto F = llvm::Function::Create(constr_destr_fn_type, llvm::Function::ExternalLinkage, thename, TheModule.get());
	return F;
}

llvm::Function* getConstructorOrDestructor(volvoxc::FullType* ft, bool destructor, bool basic, std::string* deletion_loc) {
	if (!ft->mangled_name)
		return nullptr;
	auto Names = AutoMethods.find(ft->mangled_name);
	if (Names == AutoMethods.end())
		return nullptr;
	std::string& thename = destructor ? std::get<1>(Names->second) :
		basic ? std::get<2>(Names->second) : std::get<0>(Names->second);
	if (thename.empty())
		return nullptr;
	if (!destructor && is_deleted(thename)) {
		if (deletion_loc) { // suppress normal error message
			*deletion_loc = invalidation_loc(thename);
		} else {
			errs() << invalidation_loc(thename) << ": info: " << (basic ? "init" : "clone ")
			       << "constructor of type " << *ft << " has been deleted here\n";
		}
		return nullptr;
	}
	if (auto F = TheModule->getFunction(thename))
		return F;
	auto F = llvm::Function::Create(constr_destr_fn_type, llvm::Function::ExternalLinkage, thename, TheModule.get());
	return F;
}

void InsertDestructor(FullVar* fv, llvm::Instruction* before) {
	if (!fv) {
		errs() << "InsertDestructor(): internal error no variable\n";
		return;
	}
	llvm::Value* V;
	if ((fv->ft.type_attr & A_mainvar) && jit_repl && !(llvm::isa<llvm::ArrayType>(fv->ft.type) && (!fv->ft.type->isSized() || TheModule->getDataLayout().getTypeAllocSize(fv->ft.type) == 0)) || (fv->ft.type_attr & A_globally_visible)) { // global variable
		if (fv->ft.type_attr & A_rvalue)
			return; // constexpr -> nothing to do
		if (!fv->mangled_name) {
			errs() << "Global Destructors: no mangled name for variable declared at " << fv->decl_loc << "\n";
			return;
		}
		V = TheModule->getGlobalVariable(fv->mangled_name, true);
		if (!V) {
			auto GV = new llvm::GlobalVariable(*TheModule, fv->storage_type,
			                                   false, link_type(fv->ft.type_attr),
			                                   nullptr, fv->mangled_name, nullptr,
			                                   tls_model(fv->ft.type_attr),
			                                   0, true);
			GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(fv->storage_type));
			V = GV;
		}
	} else {
		V = fv->val;
	}
	if (llvm::isa<llvm::ArrayType>(fv->ft.type))
		InsertArrayDestructor(fv, V, before);
	else
		InsertSingleDestructor(fv, V, before);
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

void printCandidate(PrototypeAST* proto, const char* name) {
	errs() << proto->retLoc << ": ";
	if (proto->visibility & A_method) {
		if (proto->visibility & A_interface)
			errs() << "this";
		else
			errs() << *proto->ArgTypes[0];
		errs() << '.';
	}
	errs() << (name ? name : "fn")
	       << ((proto->visibility & A_setter) ? "=(" : "(");
	for (int i=0; i<proto->Args.size(); i++)
		if (i || !(proto->visibility & A_method)) {
			errs() << proto->Args[i] << ' ' << *proto->ArgTypes[i];
			if (i != proto->Args.size()-1)
				errs() << ", ";
		}
	errs() << ")";
	if (proto->RetType && proto->RetType->type && !proto->RetType->type->isVoidTy())
		errs() << " " << *proto->RetType;
	errs() << "\n";
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
	memset((void*)convs2, 0, fnargs.size() * sizeof(std::function<llvm::Value*(llvm::Value*)>));
	std::function<llvm::Value*(llvm::Value*)>* convs3 = (std::function<llvm::Value*(llvm::Value*)>*)alloca(fnargs.size() * sizeof(std::function<llvm::Value*(llvm::Value*)>));
	memset((void*)convs3, 0, fnargs.size() * sizeof(std::function<llvm::Value*(llvm::Value*)>));
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
		unsigned n_proto_args = proto->ArgTypes.size();
		if (proto->IsVarArgs && !(proto->visibility & A_c_api))
			n_proto_args--;
		unsigned arg_offset = 0;
		if (proto->visibility & A_method)
			arg_offset = 1;
		for (unsigned i=arg_offset; i<fnargs.size(); i++) {
			conv_match_t match_kind = untyped_match;
			std::function<llvm::Value*(llvm::Value*)> conv = nullptr;
			if (i >= n_proto_args) {
				if (candidate < 0)
					fnargs[i].Conv = nullptr; // for variadic args - but see comment above
			} else {
				if (fnargs[i].argtype && fnargs[i].argtype->isPointerTy()
				           && (proto->ArgTypes[i]->type_attr & A_optional)) {
					conv = nullptr;
					match_kind = exact_match;
				} else {
					if (fnargs[i].is_anonymous_list && (proto->ArgTypes[i]->type->isStructTy() || proto->ArgTypes[i]->type->isArrayTy()))
						conv = NoConversion;
					else {
						conv = getConv(fnargs[i].argtype, proto->ArgTypes[i]->type, SourceLocation(),
						               fnargs[i].argtype_attr, proto->ArgTypes[i]->type_attr,
						               false, fnargs[i].arg_unknown_type, &match_kind);
					}
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

// Store value to target, otherwise register for destruction unless target == suppress_destructor_flag

llvm::Value* handle(llvm::Value* target, llvm::Value* val, SourceLocation& Loc, volvoxc::FullType* ft) {
	if (!val)
		return nullptr;
	if (val->getType()->isVoidTy())
		return val;
	if (!target) {
		register_destructor(Loc, ft, val, true);
		return val;
	} else if (target == suppress_destructor_flag)
		return val;
	Builder->CreateStore(val, target);
	return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
}

llvm::Value* handleC(llvm::Value* target, llvm::Value* val, SourceLocation& Loc, volvoxc::FullType* ft, bool basic_constructor) {
	if (!val)
		return nullptr;
	if (val->getType()->isVoidTy())
		return val;
	if (!target || target == suppress_destructor_flag) {
		if (ft->type_attr & (A_constructor | A_destructor)) {
			llvm::Type* val_t = val->getType();
			llvm::Value* tmpstore = CreateEntryBlockAlloca(val_t);
			Builder->CreateStore(val, tmpstore);
			if (target != suppress_destructor_flag)
				register_destructor(Loc, ft, tmpstore);
			if (ft->type_attr & A_constructor) {
				auto constructor = getConstructorOrDestructor(ft, false, basic_constructor); // only basic constructor
				if (!constructor) {
					errs() << Loc << ": cannot find constructor for type " << *ft << "\n";
					abort();
				}
				Builder->CreateCall(constructor, { tmpstore });
				val = Builder->CreateLoad(val_t, tmpstore);
			}
		}
		return val;
	}
	Builder->CreateStore(val, target);
	if (ft->type_attr & A_constructor) {
		auto constructor = getConstructorOrDestructor(ft, false, basic_constructor);
		if (!constructor) {
			errs() << Loc << ": cannot find constructor for type " << *ft << "\n";
			abort();
		}
		Builder->CreateCall(constructor, { target });
	}
	return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
}

CallExprAST::CallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Callee_,
            std::vector<std::unique_ptr<ExprAST>> Args_)
	: ReferencableExprAST(Loc, "*"), Callee(std::move(Callee_)),
	  Args(std::move(Args_)) {
	unsigned n_args = Args.size();
	auto functionexpr = dynamic_cast<FunctionExprAST*>(Callee.get());
	if (functionexpr)
		Name = functionexpr->Name;
	else if (auto varexpr = dynamic_cast<VariableExprAST*>(Callee.get()))
		Name = varexpr->Name;
	auto method = dynamic_cast<MethodExprAST*>(Callee.get());
	auto type_expr = dynamic_cast<TypeExprAST*>(Callee.get());
	auto select_expr = dynamic_cast<SelectExprAST*>(Callee.get());
do_analyze:
	if (type_expr) {
		Name = type_expr->Name;
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
		fn_args.push_back(FnArg{nullptr, type_expr->ft->type, type_expr->ft->type_attr, false, false, nullptr});
	else if (select_expr)
		Name = select_expr->FieldName;
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
			fn_args.push_back(FnArg{nullptr, arg->ft->type, arg->ft->type_attr, arg->is_unknown_type, is_list, nullptr});
			if (!is_list)
				register_usage_marker(arg.get(), &fn_args.back().is_referenced_after_call);
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
		int selected_proto = selectProto(protos, Name.c_str(), fn_args, Callee->Loc);
		if (selected_proto >= 0) {
			Proto = (*protos)[selected_proto].get();
			if (method && Proto->vtable_offs >= 0)
				vtable_offs = target_bytes * Proto->vtable_offs;
		} else if (selected_proto == -2) {
			// explicit basic type conversion
			auto ft = lex.source_stack.front().module->type_table.get_full(Name.c_str());
			if (!ft)
				return;
			auto thetype_expr = std::make_unique<TypeExprAST>(Callee->Loc, Name, ft);
			type_expr = thetype_expr.get();
			Callee = std::move(thetype_expr);
			goto do_analyze;
		} else
			return;
		if (!type_expr)
			ft = Proto->RetType;
		if (functionexpr)
			functionexpr->ft->selected_proto = selected_proto;
	}
}

CallExprAST::~CallExprAST() {
	for (auto& arg: fn_args)
		free(arg.is_referenced_after_call);
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

bool InsertArrayConDestructor(llvm::Type* elem_type, // actually array_type
                              volvoxc::FullType* array_elem_type, llvm::Value* val, llvm::Instruction* before,
                              bool is_constructor, std::string* deletion_loc) {
	llvm::Function* destructor = getConstructorOrDestructor(array_elem_type, !is_constructor, false, deletion_loc);
	if (!destructor)
		return false;
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
	llvm::Type* elem_ptr_ty = llvm_ptr_type;
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
	TheFunction->insert(TheFunction->end(), DestructorBB);
	Builder->SetInsertPoint(DestructorBB);
	Ptr = Builder->CreateLoad(llvm_size_type, PtrStore);
	llvm::Value* ElPtr = Builder->CreateIntToPtr(Ptr, elem_ptr_ty);
	Builder->CreateCall(elDestructorFT, destructor, ElPtr);
	llvm::Value* NewPtr = Builder->CreateAdd(Ptr, ElemAllocSize);
	Builder->CreateStore(NewPtr, PtrStore);
	Builder->CreateBr(CondBB);
	TheFunction->insert(TheFunction->end(), CondBB);
	Builder->SetInsertPoint(CondBB);
	Ptr = Builder->CreateLoad(llvm_size_type, PtrStore);
	auto is_less = Builder->CreateICmpULT(Ptr, UpperLimit);
	Builder->CreateCondBr(is_less, DestructorBB, ContBB);
	if (before)
		Builder->SetInsertPoint(before);
	else {
		TheFunction->insert(TheFunction->end(), ContBB);
		Builder->SetInsertPoint(ContBB);
	}
	return true;
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
		// errs() << fv->decl_loc << ": ### Destructor for '" << var_node.getKey() << "'\n";
		if ((fv->ft.type_attr & (A_destructor | A_map)) && fv->val && fv->val != retp
		    && (!var_adr || fv->val != var_adr))
			InsertDestructor(fv);
	}
}

// destr_vars: variables that have been defined in correspondig levels at brk and have A_destructor
// merged_vars: variables that are still valid at corresponding merge point
//
void InsertDestructors(std::map<std::string,FullVar*>& destr_vars, std::set<std::string>* merged_vars, llvm::Value* retp) {
	// errs() << " *** destructors for ";
	for (auto it = destr_vars.begin(); it != destr_vars.end(); it++) {
		// errs() << it->second->decl_loc << ": ### Destructor for '" << it->first << "': "
		//  << (void*)(it->second->destructor) << "\n";
		if (!merged_vars || !merged_vars->contains(it->first)) {
			if (it->second->val != retp) {
				// errs() << it->first << " ";
				InsertDestructor(it->second);
			}
		}
	// 	else
	// 		errs() << "!" << it->first << " ";
	// errs() << "\n";
	}
}

// insert destructors for intermediate results - this is done afer each complete expression
void InsertDestructors(std::vector<FullVar>& t) {
	if (t.empty())
		return;
	for (auto& fv: t) {
		if (fv.ft.type_attr & (A_destructor | A_map))
			InsertDestructor(&fv);
	}
	t.clear();
}

enum field_con_de_structors_state : uint8_t {
	field_con_de_structors_needed,
	field_con_de_structors_not_needed,
	field_con_de_structors_deleted
};

static field_con_de_structors_state insert_field_destructors(volvoxc::FullType* ft, llvm::Argument* thisarg,
                                                             bool is_constructor = false,
                                                             std::string* deletion_loc = nullptr) {
	field_con_de_structors_state needs_destructors = field_con_de_structors_not_needed;
	for (auto field = ft->first(); field; ++field) {
		auto el_ft = field.getFt();
		if (el_ft->type_attr & (is_constructor ? A_constructor : A_destructor)) {
			needs_destructors = field_con_de_structors_needed;
			llvm::Function* field_destructor = getConstructorOrDestructor(el_ft, !is_constructor, false, deletion_loc);
			if (!field_destructor)
				return field_con_de_structors_deleted;
			unsigned idx = field.getIndex();
			llvm::Value* elem_ref = Builder->CreateConstGEP2_32(ft->type, thisarg, 0, idx);
			Builder->CreateCall(constr_destr_fn_type, field_destructor, elem_ref);
		} else if (isa<llvm::ArrayType>(el_ft->type) && (el_ft->elem_type->type_attr & (is_constructor ? A_constructor : A_destructor))) {
			needs_destructors = field_con_de_structors_needed;
			unsigned idx = field.getIndex();
			llvm::Value* elem_ref = Builder->CreateConstGEP2_32(ft->type, thisarg, 0, idx);
			if (!InsertArrayConDestructor(el_ft->type, el_ft->elem_type, elem_ref, nullptr, is_constructor, deletion_loc))
				return field_con_de_structors_deleted;
		}
	}
	return needs_destructors;
}

llvm::Value* Volvox2CStr1(llvm::Value* v) {
	// LLVM implementation of macro '#define volvox2cstr(v)
	llvm::Value* subtrahend = Builder->CreateLoad(llvm_size_type, v);
	return subtrahend;
}

llvm::Value* getArrayCap(llvm::Value* v) {
	// LLVM implementation of macro '#define volvox2cstr(v)
	auto cp = Builder->CreateConstGEP1_32(llvm_size_type, v, 1);
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
#ifndef NO_NULLPTR_STRING
	llvm::BasicBlock* enterBB = Builder->GetInsertBlock();
	llvm::Function* TheFunction = enterBB->getParent();
	llvm::BasicBlock* ContBB = llvm::BasicBlock::Create(Context, "cont2cstr");
	llvm::BasicBlock* ConvBB = llvm::BasicBlock::Create(Context, "conv");
	llvm::BasicBlock* ConvBB0 = llvm::BasicBlock::Create(Context, "conv0");
	llvm::Value* IsNotNull = Builder->CreateIsNotNull(Builder->CreatePtrToInt(v, llvm_size_type));
	Builder->CreateCondBr(IsNotNull, ConvBB, ConvBB0);
	TheFunction->insert(TheFunction->end(), ConvBB0);
	Builder->SetInsertPoint(ConvBB0);
	llvm::Value* nullStr = Builder->CreateGlobalString("", "", 0, TheModule.get());
	Builder->CreateBr(ContBB);
	TheFunction->insert(TheFunction->end(), ConvBB);
	Builder->SetInsertPoint(ConvBB);
#endif
	auto subtrahend = Volvox2CStr1(v);
	llvm::Value* Res = Volvox2CStr2(v, subtrahend);
#ifdef NO_NULLPTR_STRING
	return Res;
#else
	Builder->CreateBr(ContBB);
	TheFunction->insert(TheFunction->end(), ContBB);
	Builder->SetInsertPoint(ContBB);
	llvm::PHINode* PN = Builder->CreatePHI(llvm_ptr_type, 2, "strconv");
	PN->addIncoming(nullStr, ConvBB0);
	PN->addIncoming(Res, ConvBB);
	return PN;
#endif
}

void CreateFree(llvm::Value* buf) {
#if LLVM_VERSION_MAJOR >= 18
#ifdef _MSC_VER
	const char* __free = "__free";
	auto __free_proto = (*lex.findProtos(__free))[0].get();
	auto __free_fn = getFunction(__free_proto);
	Builder->CreateCall(__free_proto->FT, __free_fn, std::vector<llvm::Value*>({ buf }));
#else
	Builder->CreateFree(buf);
#endif
#else
	Builder->Insert(llvm::CallInst::CreateFree(buf, Builder->GetInsertBlock()));
#endif
}

llvm::Value* CreateMalloc(llvm::Value* elem_sz, llvm::Value* n_elem, const llvm::Twine &Name) {
#if LLVM_VERSION_MAJOR >= 18
#ifdef _MSC_VER
	const char* __malloc = "__malloc";
	auto __malloc_proto = (*lex.findProtos(__malloc))[0].get();
	auto __malloc_fn = getFunction(__malloc_proto);
	return Builder->CreateCall(__malloc_proto->FT, __malloc_fn, std::vector<llvm::Value*>({ Builder->CreateMul(elem_sz, n_elem) }));
#else
	return Builder->CreateMalloc(
		llvm_size_type, llvm::Type::getInt8Ty(Context),
		elem_sz, n_elem,
		nullptr, Name);
#endif
#else
	auto ArrayAlloc = llvm::CallInst::CreateMalloc(Builder->GetInsertBlock(),
	                                          llvm_size_type, llvm::Type::getInt8Ty(Context),
	                                          elem_sz, n_elem,
	                                          nullptr, Name);
	return Builder->Insert(ArrayAlloc);
#endif
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
		// we only get here if no constructor/destructor has been defined explicitly
		// check if any fields need constructor calls
		auto D = createConstructorOrDestructorFnProto(ft, is_constructor);
		auto thisarg = D->getArg(0);
		llvm::BasicBlock* BB = llvm::BasicBlock::Create(Context, "entry", D);
		Builder->SetInsertPoint(BB);
		auto needs_destructors = insert_field_destructors(ft, thisarg, is_constructor);
		if (needs_destructors != field_con_de_structors_needed) {
			D->eraseFromParent();
			return;
		}
		Builder->CreateRetVoid();
		if (is_constructor) {
			if (!is_deleted(std::get<0>(AutoMethods[ft->mangled_name])))
				std::get<0>(AutoMethods[ft->mangled_name]) = std::string(D->getName());
		} else
			std::get<1>(AutoMethods[ft->mangled_name]) = std::string(D->getName());
		finishFunctionOrModule(D, 1, false);
		if (is_constructor && !is_deleted(std::get<2>(AutoMethods[ft->mangled_name]))) {
			// provide an empty basic constructor
			auto basicD = createConstructorOrDestructorFnProto(ft, is_constructor, true);
			auto thatarg = basicD->getArg(0);
			BB = llvm::BasicBlock::Create(Context, "entry", basicD);
			Builder->SetInsertPoint(BB);
			Builder->CreateRetVoid();
			std::get<2>(AutoMethods[ft->mangled_name]) = std::string(basicD->getName());
			finishFunctionOrModule(basicD, 1, false);
		}
		ft->type_attr |= (is_constructor ? A_constructor : A_destructor);
	}
}

// FIXME: this function is actually in libgcc but is not found by JIT
#ifdef _WIN32
#ifndef _MSC_VER
extern "C" __declspec(dllexport) void ___chkstk_ms() __attribute__((weak)) {}
#endif
#endif

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
			return false;
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
			if (optimization_level == llvm::OptimizationLevel::O0) {
#if LLVM_VERSION_MAJOR < 18
				// work around broken -O0 optimization
				MPM = PB.buildModuleSimplificationPipeline(optimization_level, llvm::ThinOrFullLTOPhase::ThinLTOPostLink);
#else
				MPM = PB.buildO0DefaultPipeline(optimization_level);
#endif
			} else {
				if (lto_mode == lto_thin)
					MPM = PB.buildThinLTOPreLinkDefaultPipeline(optimization_level);
				else
					MPM = PB.buildPerModuleDefaultPipeline(optimization_level);
			}
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

llvm::Value* FunctionExprAST::codegen_raw(llvm::Value* target) {
	if (auto F = TheModule->getFunction((*ft->Protos)[ft->selected_proto]->Name)) {
		return handle(target, F, Loc, ft);
	}
	return handle(target, (*ft->Protos)[ft->selected_proto]->codegen(need_address), Loc, ft);
}

std::pair<llvm::Type*,llvm::Value*> FunctionExprAST::codegen_ref_(bool silent_fail, bool constref) {
	auto the_func = codegen_raw();
	auto ref_struct_ty = llvm::cast<llvm::StructType>(llvm_closure_type);
	llvm::Value* ref_val = llvm::UndefValue::get(ref_struct_ty);
	ref_val = Builder->CreateInsertValue(ref_val, the_func, 0);
	ref_val = Builder->CreateInsertValue(ref_val, llvm::ConstantPointerNull::get(llvm_ptr_type), 1);
	return { the_func->getType(), ref_val };
}

llvm::Value* PrototypeAST::codegen(bool need_address) {
	if (need_address && !inside_function && jit_repl) {
		// force JIT engine to generate code
		auto ExprSymbolOpt = TheJIT->lookup(Name);
		auto error = ExprSymbolOpt.takeError();
		if (error) {
			errs() << retLoc << ": Prototype has no implementation\n";
			return nullptr;
		}
		auto ExprSymbol = ExprSymbolOpt.get();
#if LLVM_VERSION_MAJOR >= 17
		auto adr = ExprSymbol.getAddress().getValue();
#else
		auto adr = ExprSymbol.getAddress();
#endif
		return Builder->CreateIntToPtr(getSize(adr), llvm_ptr_type);
	}
	llvm::Function *F =
		llvm::Function::Create(FT, link_typ, 0, Name, TheModule.get());
	// Set names for all arguments.
	unsigned Idx = 0;
	unsigned ArgIdx = 0;
	if (IsStructRet) {
		llvm::AttrBuilder attr_builder(Context, llvm::Attribute::getWithStructRetType(Context, RetType->type));
		F->getArg(Idx)->addAttrs(attr_builder);
		Idx++;
	}
	for (auto &Arg : Args) {
		auto fnarg = F->getArg(Idx);
		if (ArgAttrs[ArgIdx].hasAttributes()) {
			llvm::AttrBuilder attr_builder(Context, ArgAttrs[ArgIdx]);
			fnarg->addAttrs(attr_builder);
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
		if (Args.empty() || Args[0]->ft->type != llvm_string_type) {
			errs() << Loc << ": '" << Proto->Name << "()' requires at least 1 argument\n";
			return nullptr;
		}
		if (is_error)
			errs() << Loc << ':';
		for (auto& arg: Args) {
			if (auto lit = dynamic_cast<LiteralExprAST*>(arg.get())) {
				if (arg->ft->type != llvm_string_type || !strlen(lit->Val.CStr)) {
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
								                           FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last_err, 0, (LPSTR)&msg, 0, nullptr);
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
	if (Proto && Proto->Name == "__load_size") {
		if (Args.size() != 1) {
			errs() << Loc << ": '" << Proto->Name << "()' requires exactly 1 argument\n";
			return nullptr;
		}
		auto arg = Args[0].get();
		arg->desired_type = llvm_ptr_type;
		llvm::Value* adr = arg->codegen();
		if (!adr) {
			errs() << arg->Loc << ": invalid argument\n";
			return nullptr;
		}
		return Builder->CreateLoad(llvm_size_type, adr);
	}
	if (Proto && Proto->Name == "__store_size") {
		if (Args.size() != 2) {
			errs() << Loc << ": '" << Proto->Name << "()' requires exactly 2 arguments\n";
			return nullptr;
		}
		Args[0]->desired_type = llvm_ptr_type;
		Args[1]->desired_type = llvm_size_type;
		llvm::Value* adr = Args[0]->codegen();
		llvm::Value* val = Args[1]->codegen();
		if (!adr || !val) {
			errs() << (adr ? Args[1]->Loc : Args[0]->Loc) << ": invalid argument\n";
			return nullptr;
		}
		llvm::Value* old_val = Builder->CreateLoad(llvm_size_type, adr);
		Builder->CreateStore(val, adr);
		return old_val;
	}
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	TypeExprAST* type_expr = dynamic_cast<TypeExprAST*>(Callee.get());
	if (!Proto) {
		if (type_expr) {
			if (Args.empty())
				return handleC(target, llvm::Constant::getNullValue(ft->type), Loc, ft, false);
			if (Args.size() == 1) {
				Args[0]->desired_type = ft->type;
				llvm::Value* expr = Args[0]->codegen_raw();
				if (!expr)
					return nullptr;
				std::function<llvm::Value*(llvm::Value*)> conv = nullptr;
				if (expr->getType()->isPointerTy() && ((ft->type_attr & A_cstring) || ft->type == llvm_string_type)) {
					// special handling for string types
					if (Args[0]->ft->type == llvm_string_type
					    && (ft->type_attr & A_cstring)) {
						return handle(target, Volvox2CStr(expr), Loc, ft);
					} else if ((Args[0]->ft->type_attr & A_cstring)
					         && ft->type == llvm_string_type) {
						auto converter_name = "__cstr2volvox";
						auto converter_proto = (*lex.findProtos(converter_name))[0].get();
						auto converter = getFunction(converter_proto);
						return handle(target, Builder->CreateCall(converter_proto->FT, converter, std::vector<llvm::Value*>({ expr })), Loc, string_type);
					} else if (ft->type == llvm_string_type) {
						llvm::Value* the_struct = llvm::UndefValue::get(llvm_string_type);
						the_struct = Builder->CreateInsertValue(the_struct, expr, 0);
						return handle(target, the_struct, Loc, string_type);
					} else {
						return handle(target, expr, Loc, ft);
					}
				} else {
					conv = getConv(expr->getType(), ft->type, Loc, (bool)(Args[0]->ft->type_attr & A_signed),
					               (bool)(ft->type_attr & A_signed), true, false, nullptr);
				}
				if (conv)
					return handle(target, conv(expr), Loc, ft);
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
				return handle(target, getSize(size), Loc, ft);
			if (Size)
				return handle(target, Size, Loc, ft);
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
			return handle(target, Builder->CreateExtractElement(DimsArray, arg), Loc, ft);
		}
	}
	if (!Proto)
		return nullptr;
	if ((Proto->visibility & A_constructor) && type_expr) {
		uint64_t allocsz = TheModule->getDataLayout().getTypeAllocSize(type_expr->ft->type);
		llvm::Value* ret_val = nullptr;
		if ((!target || (intptr_t)target == -1) && (allocsz > sret_limit || (type_expr->ft->type_attr & A_constructor)))
			target = ret_val = CreateEntryBlockAlloca(type_expr->ft->type, "");
		if (target && (intptr_t)target != -1)
			Builder->CreateStore(llvm::Constant::getNullValue(ft->type), target);
		if (!llvm::isa<llvm::StructType>(ft->type)) {
			switch (Args.size()) {
			case 0:
				return handle(target, llvm::Constant::getNullValue(ft->type), Loc, ft);
			case 1:
				Args[0]->desired_type = ft->type;
				Args[0]->conv_kind = ft->type_attr & A_signed ? ConvSigned : ConvUnsigned;
				return handle(target, Args[0]->codegen(), Loc, ft);
			default:
				errs() << "conversions with #arg!=1 not supported\n";
				return nullptr;
			}
		}
	}
	llvm::Value* theFunction = nullptr;
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
	llvm::Value* getter = nullptr;
	llvm::Value* setter = nullptr;
	llvm::Value* method_adr = nullptr;
	if ((Proto->visibility & A_method) && !(Proto->visibility & A_constructor)) {
		if (auto method = dynamic_cast<MethodExprAST*>(Callee.get())) {
			llvm::Value* receiver_ref = nullptr;
			if (auto receiver_lval = dynamic_cast<LvalueExprAST*>(method->Receiver.get())) {
				llvm::Type* receiver_type;
				std::tie(receiver_type, receiver_ref) = receiver_lval->codegen_ref(true);
				if (!receiver_type) {
					errs() << method->Receiver->Loc << ": could not get receiver\n";
					return nullptr;
				}
				if (vtable_offs >= 0) { // method to polymorphic object
					llvm::Value* rt_type_ptr = Builder->CreateLoad(
						llvm_ptr_type, Builder->CreateStructGEP(llvm_interface_type, receiver_ref, 0));
					receiver_ref = Builder->CreateLoad(
						llvm_ptr_type, Builder->CreateStructGEP(llvm_interface_type, receiver_ref, 1));
					method_adr = Builder->CreateIntToPtr(
						Builder->CreateAdd(
							Builder->CreatePtrToInt(rt_type_ptr, llvm_size_type),
							getSize(vtable_offs)), llvm_ptr_type);
					theFunction = Builder->CreateLoad(llvm_ptr_type, method_adr);
					if (Proto->visibility & A_getter) {
						getter = Builder->CreatePtrToInt(theFunction, llvm_size_type);
					} else if (Proto->visibility & A_setter) {
						setter = Builder->CreatePtrToInt(theFunction, llvm_size_type);
						getter = Builder->CreatePtrToInt(
							Builder->CreateLoad(
								llvm_ptr_type, Builder->CreateIntToPtr(
									Builder->CreateSub(
										Builder->CreatePtrToInt(method_adr, llvm_size_type),
										getSize(target_bytes)), llvm_ptr_type)), llvm_size_type);
					}
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
					// register destructor call for intermediate receiver
					register_destructor(method->Receiver->Loc, method->Receiver->ft, receiver_ref);
				} else {
					receiver_ref = StoreValue(method->Receiver->codegen(), method->Receiver->ft);
				}
				if (!receiver_ref) {
					errs() << method->Receiver->Loc << ": internal error - could not store receiver\n";
					return nullptr;
				}
			}
			ArgsV.push_back(receiver_ref);
			for (llvm::Value* iarg: Proto->implicitArgs)
				ArgsV.push_back(iarg);
		} else {
			errs() << Callee->Loc << ": method prototype but not a method call\n";
			return nullptr;
		}
	}
	unsigned n_proto_args = Proto->ArgAttrs.size();
	bool is_volvox_variadic = Proto->IsVarArgs && !(Proto->visibility & A_c_api) && !(!Args.empty() && (Args.back()->ft->type_attr & A_va_arg));
	llvm::Value* volvox_var_array = nullptr;
	llvm::Value* volvox_var_array_ref = nullptr;
	unsigned n_volovox_va_arg = 0;
	if (is_volvox_variadic) {
		n_proto_args--; // Volvox variadic functions have an interface-array as last regular argument
		n_volovox_va_arg = Args.size() + arg_offs - n_proto_args;
		auto va_arg_array_type = llvm::ArrayType::get(llvm_interface_type, n_volovox_va_arg);
		if (n_volovox_va_arg)
			volvox_var_array = CreateEntryBlockAlloca(va_arg_array_type, "va_arg_arrayp");
		else
			volvox_var_array = llvm::ConstantPointerNull::get(llvm_ptr_type);
		volvox_var_array_ref = llvm::UndefValue::get(llvm_va_arg_ref_type);
		volvox_var_array_ref = Builder->CreateInsertValue(volvox_var_array_ref, getSize(n_volovox_va_arg), 0);
		volvox_var_array_ref = Builder->CreateInsertValue(volvox_var_array_ref, volvox_var_array, 1);
	}
	if (fn_args.size() != Args.size()+arg_offs || Proto->ArgNeedsConstructor.size() != n_proto_args+(is_volvox_variadic ? 1 : 0))
		errs() << Loc << ": ### Internal compiler error - fn_args: " << fn_args.size() << " Args: " << Args.size() << " arg_offs: " << arg_offs << " n_proto_args: " << n_proto_args << " NeedsConstructor: " << Proto->ArgNeedsConstructor.size() << "\n";
	for (unsigned i = 0; i < Args.size(); ++i) {
		if (is_volvox_variadic && (i+arg_offs) >= n_proto_args) {
			unsigned idx = (i+arg_offs) - n_proto_args;
			auto interface_expr = std::make_unique<InterfaceExprAST>(std::move(Args[i]));
			llvm::Value* interface_val_adr = Builder->CreateGEP(llvm_interface_type, volvox_var_array, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), idx));
			llvm::Value* interface_val = interface_expr->codegen_raw(interface_val_adr);
			if (!interface_val) {
				errs() << interface_expr->Loc << ": cannot generate interface expr value\n";
				return nullptr;
			}
			continue;
		}
		bool is_var_array = false;
		bool by_val = false;
		bool is_address = false;
		bool needs_constructor_call = false;
		bool is_moved = false;
		SourceLocation* laterUsage = nullptr;
		if (i+arg_offs < n_proto_args) {
			if (Proto->ArgTypes[i+arg_offs]->type_attr & A_interface) {
				auto interface_expr = std::make_unique<InterfaceExprAST>(std::move(Args[i]), Proto->ArgTypes[i+arg_offs]);
				llvm::Value* interface_val = interface_expr->codegen_raw(nullptr);
				if (!interface_val)
					return nullptr;
				ArgsV.push_back(interface_val);
				continue;
			}
			is_var_array =
				Proto->ArgTypes[i+arg_offs]->type->isArrayTy()
				&& !Proto->FT->getFunctionParamType(i+arg_offs)->isArrayTy();
			by_val = Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByVal);
			is_address =
				by_val
				|| Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByRef)
				|| is_var_array
				|| (i+arg_offs < n_proto_args && (Proto->ArgTypes[i+arg_offs]->type_attr & A_by_value));
			by_val = by_val || (Proto->ArgTypes[i+arg_offs]->type_attr & A_by_value);
			// we treat 'maybe_arg_needs_constructor' like 'arg_needs_constructor' in the following
			// lines for now. Maybe we optimize this sometime by introducing a declaration for
			// a constructor wrapper that is defined once we know if the the call is needed
			laterUsage = fn_args[i+arg_offs].is_referenced_after_call;
			std::tie(needs_constructor_call, is_moved) = needs_constructor_call_or_is_moved(
				Proto->ArgNeedsConstructor[i+arg_offs],
				(bool)laterUsage || dynamic_cast<SelectExprAST*>(Args[i].get()));
/*
			if (is_moved)
				errs() << Args[i]->Loc << ": mark arg as moved " << *Proto->ArgTypes[i+arg_offs] << " " << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], arg_is_owned) << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], maybe_arg_is_owned) << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], arg_has_constructor) << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], arg_has_destructor) << "\n";
			else
				errs() << Args[i]->Loc << ": not moved " << *Proto->ArgTypes[i+arg_offs] << " " << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], arg_is_owned) << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], maybe_arg_is_owned) << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], arg_has_constructor) << get_arg_flag(Proto->ArgNeedsConstructor[i+arg_offs], arg_has_destructor) << "\n";
*/
		}
		if (!is_address && (Args[i]->ft->type_attr & A_cstring)) {
			// errs() << Args[i]->Loc << ": ### function argument 1\n";
			llvm::Value* arg = Args[i]->codegen();
			if (!arg) {
				errs() << Args[i]->Loc << ": error generating function argument\n";
				return nullptr;
			}
			if (Args[i]->ft->type == llvm_string_type && ((i+arg_offs) >= n_proto_args || Proto->ArgTypes[i+arg_offs]->type_attr & A_cstring))
				arg = Volvox2CStr(arg);
			ArgsV.push_back(arg);
		} else {
			// errs() << Args[i]->Loc << ": ### function argument 2\n";
			llvm::Type* real_arg_type;
			if ((i+arg_offs) < n_proto_args)
				real_arg_type = Args[i]->desired_type = Proto->ArgTypes[i+arg_offs]->type;
			else
				real_arg_type = Args[i]->ft->type;
			llvm::Value* arg = nullptr;
			bool is_aggregate_lit = dynamic_cast<StructExprAST*>(Args[i].get()) || dynamic_cast<ListExprAST*>(Args[i].get()) || dynamic_cast<TypeExprAST*>(Args[i].get());
			if (Args[i]->needs_target() || is_aggregate_lit && (Proto->ArgTypes[i+arg_offs]->type_attr & (A_constructor | A_destructor)) || needs_constructor_call || is_moved)
				arg = HandleMove(Args[i].get(), Proto->ArgTypes[i+arg_offs], real_arg_type, is_address, is_moved, needs_constructor_call, nullptr, laterUsage);
			if (!arg) {
				if (is_address) {
					if (auto lval = dynamic_cast<LvalueExprAST*>(Args[i].get())) {
						auto argref = lval->codegen_ref(true, by_val);
						if (!argref.first) {
							errs() << Args[i]->Loc << ": cannot generate reference function argument\n";
							return nullptr;
						}
						if (argref.second)
							arg = argref.second;
					}
					if (!arg) {
						auto lit = dynamic_cast<LiteralExprAST*>(Args[i].get());
						if (lit && lit->ft->type->isPointerTy()) {
							llvm::Type* target_type =
								(Proto->ArgTypes[i+arg_offs]->type_attr & A_ref) ?
								llvm_ptr_type :
								Proto->ArgTypes[i+arg_offs]->type;
							arg = Builder->CreatePointerCast(Args[i]->codegen_raw(), target_type);
						} else {
							arg = Builder->CreateAlloca(Proto->ArgTypes[i+arg_offs]->type, nullptr, "tmprefarg");
							//errs() << Loc << ": arg #" << i << " " << *arg << '\n';
							auto tmparg = Args[i]->codegen_raw();
							if (!tmparg) {
								errs() << Args[i]->Loc << ": cannot generate code for expression\n";
								return nullptr;
							}
							Builder->CreateStore(tmparg, arg);
						}
					}
				} else {
					// errs() << Args[i]->Loc << ": ### function argument 23 " << needs_constructor_call << is_moved << " " << *Args[i]->ft << "\n";
					// errs() << Loc << ": valarg #" << i << " " << *arg << ' ' << Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByVal) << '\n';
					if ((i+arg_offs) < n_proto_args && (Proto->ArgTypes[i+arg_offs]->type_attr & (A_constructor | A_destructor)) && Proto->ArgNeedsConstructor[i+arg_offs] == arg_is_borrowed_or_pod)
						arg = Args[i]->codegen_borrow();
					else
						arg = Args[i]->codegen();

				}
			}
			if (!arg) {
				errs() << Args[i]->Loc << ": cannot create function call argument\n";
				return nullptr;
			}
			if ((i+arg_offs) >= n_proto_args) {
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
	if (is_volvox_variadic)
		ArgsV.push_back(volvox_var_array_ref);
	if (!theFunction && !type_expr)
		theFunction = Callee->codegen();
	if (!theFunction)
		theFunction = getFunction(Proto);
	if (!theFunction)
		abort();
	if (auto F = llvm::dyn_cast<llvm::Function>(theFunction)) {
		// Callee was a function symbol like `sin`
		if (Proto->const_result)
			return Proto->const_result;
		return handle(target, Builder->CreateCall(FT, F, std::move(ArgsV)), Loc, ft);
		// return Builder->CreateCall(FT, F, std::move(ArgsV));
	} else {
		// theFunction is a function pointer, i.e. a function call address (e.g. loaded from a variable)
		if (getter) {
			// check if getter is NULL - if so, we have no method but a data field
			auto fieldBB = llvm::BasicBlock::Create(Context, "getset_field");
			auto callBB = llvm::BasicBlock::Create(Context, "getset_call");
			auto contBB = llvm::BasicBlock::Create(Context, "getset_cont");
			llvm::Value* vtable_entry_zero = Builder->CreateICmpEQ(getter, llvm::ConstantInt::get(llvm_size_type, 0));
			auto enterBB = Builder->GetInsertBlock();
			auto TheFunction = enterBB ? enterBB->getParent() : nullptr;
			if (!TheFunction) {
				errs() << "*** internal error: no function\n";
				abort();
			}
			llvm::Value* PN_store = CreateEntryBlockAlloca(Proto->RetType->type);
			Builder->CreateCondBr(vtable_entry_zero, fieldBB, callBB);
			TheFunction->insert(TheFunction->end(), fieldBB);
			Builder->SetInsertPoint(fieldBB);
			if (!setter)
				setter = Builder->CreatePtrToInt(
					Builder->CreateLoad(
						llvm_size_type, Builder->CreateIntToPtr(
							Builder->CreateAdd(
								Builder->CreatePtrToInt(method_adr, llvm_size_type),
								getSize(target_bytes)), llvm_ptr_type)), llvm_size_type);
			llvm::Value* FieldPtr = Builder->CreateIntToPtr(
				Builder->CreateAdd(
					Builder->CreatePtrToInt(ArgsV[0], llvm_size_type),
					setter), llvm_ptr_type);
			// TODO: handle sret
			llvm::Value* retVal_field = Builder->CreateLoad(Proto->RetType->type, FieldPtr);
			Builder->CreateStore(retVal_field, PN_store);
			// TODO: handle atomic data fields
			if (Proto->visibility & A_setter)
				Builder->CreateStore(ArgsV[1], FieldPtr);
			Builder->CreateBr(contBB);
			TheFunction->insert(TheFunction->end(), callBB);
			Builder->SetInsertPoint(callBB);
			llvm::Value* retVal_call = Builder->CreateCall(FT, theFunction, ArgsV);
			Builder->CreateStore(retVal_call, PN_store);
			Builder->CreateBr(contBB);
			TheFunction->insert(TheFunction->end(), contBB);
			Builder->SetInsertPoint(contBB);
			// * Using a PHI node does not work with '-O0' for unknown reasons
			// * but PN_store does. Leaving this code commented out for reference
			// llvm::PHINode* PN = Builder->CreatePHI(Proto->RetType->type, 2, "iface_res");
			// PN->addIncoming(retVal_field, fieldBB);
			// PN->addIncoming(retVal_call, callBB);
			llvm::Value* PN = Builder->CreateLoad(Proto->RetType->type, PN_store);
			return handle(target, PN, Loc, ft);
		}
		// errs() << Loc << ": ### call expr with function pointer " << *theFunction << "\n";
		if (theFunction->getType()->isPointerTy())
			// interface method or simple function pointer
			return handle(target, Builder->CreateCall(FT, theFunction, std::move(ArgsV)), Loc, ft);
		if (auto closure_ty = llvm::dyn_cast<llvm::StructType>(theFunction->getType())) {
			// closure: pointer pair - function pointer and pointer to captured variables
			llvm::Value* closure_fn_ptr = Builder->CreateExtractValue(theFunction, 0);
			llvm::Value* captures_ptr = Builder->CreateExtractValue(theFunction, 1);
			auto ArgsV_closure = ArgsV;
			ArgsV_closure.insert(ArgsV_closure.begin(), captures_ptr);
			std::vector<llvm::Type*> closure_args;
			closure_args.reserve(FT->getNumParams() + 1);
			closure_args.push_back(llvm_ptr_type);
			for (auto it = FT->param_begin(); it != FT->param_end(); it++)
				closure_args.push_back(*it);
			llvm::Type* ret_ty = FT->getReturnType();
			auto FT_closure = llvm::FunctionType::get(ret_ty, std::move(closure_args), FT->isVarArg());
			auto callBB_simple = llvm::BasicBlock::Create(Context, "call_simple");
			auto callBB_closure = llvm::BasicBlock::Create(Context, "call_closure");
			auto contBB = llvm::BasicBlock::Create(Context, "cont");
			llvm::Value* IsSimple = Builder->CreateIsNull(Builder->CreatePtrToInt(captures_ptr, llvm_size_type));
			auto enterBB = Builder->GetInsertBlock();
			auto TheFunction = enterBB ? enterBB->getParent() : nullptr;
			if (!TheFunction) {
				errs() << "*** internal error: no function\n";
				abort();
			}
			Builder->CreateCondBr(IsSimple, callBB_simple, callBB_closure);
			TheFunction->insert(TheFunction->end(), callBB_simple);
			Builder->SetInsertPoint(callBB_simple);
			llvm::Value* simpleRes = Builder->CreateCall(FT, closure_fn_ptr, std::move(ArgsV));
			Builder->CreateBr(contBB);
			TheFunction->insert(TheFunction->end(), callBB_closure);
			Builder->SetInsertPoint(callBB_closure);
			llvm::Value* closureRes = Builder->CreateCall(FT_closure, closure_fn_ptr, std::move(ArgsV_closure));
			Builder->CreateBr(contBB);
			TheFunction->insert(TheFunction->end(), contBB);
			Builder->SetInsertPoint(contBB);
			if (ret_ty->isVoidTy())
				return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
			llvm::PHINode* PN = Builder->CreatePHI(ret_ty, 2, "closure_ret");
			PN->addIncoming(simpleRes, callBB_simple);
			PN->addIncoming(closureRes, callBB_closure);
			return handle(target, PN, Loc, ft);
		}
		errs() << Loc << ": internal error - cannot create function call\n";
		return nullptr;
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
	for (int ConstrIdx=0; ArgIdx < TheFunction->arg_size(); ArgIdx++, ConstrIdx++) {
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
		if (mapitem->needs_constructor
		    && (get_arg_flag(*mapitem->needs_constructor, arg_is_owned)
		        || get_arg_flag(*mapitem->needs_constructor, maybe_arg_is_owned))
		    && (mapitem->ft.type_attr & A_destructor))
		{
			std::string mangled_fn_name = TheFunction->getName().str();
			mapitem->destructor = getShadowConstructorDestructor(mangled_fn_name, ConstrIdx, true);
			// errs() << mapitem->decl_loc << ": ### insert destructor " << mapitem->destructor << "\n";
		} else {
			// destructors for function argument is called by the caller
			mapitem->destructor = nullptr;
		}
		if (Arg->hasByValAttr() || Arg->hasByRefAttr() || mapitem->ft.type->isArrayTy() && !Arg->getType()->isArrayTy()) {
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
		if (Proto->ArgNeedsConstructor[ConstrIdx])
			mapitem->needs_constructor = &Proto->ArgNeedsConstructor[ConstrIdx];
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

bool FunctionAST::process_body(std::vector<std::unique_ptr<ExprAST>>& thisBody, bool main_partial) {
	ret_ptr = this_ret_ptr;
	theFunction_ret_ft = ret_ft;
	Builder->SetInsertPoint(BB);
	bool branch_returns_value = false;
	if (bBody.second.end_kind == tok_return && !Proto->RetType->type->isVoidTy()) {
		if (thisBody.empty() || !thisBody.back())
			return false;
		thisBody.back()->desired_type = Proto->RetType->type;
		branch_returns_value = true;
	}
	if (return_val_idx < 0)
		return_val_idx = thisBody.size() - 1;
	for (auto& Expr : thisBody) {
		if (Expr->needs_target()) {
			if (!main_partial && !return_val_idx && this_ret_ptr) {
				RetVal = Expr->codegen_raw(this_ret_ptr);
			} else {
				llvm::Value* target = CreateEntryBlockAlloca(Expr->ft->type);
				if (!Expr->codegen_raw(target))
					return false;
				if (return_val_idx)
					register_destructor(Expr->Loc, Expr->ft, target);
				RetVal = Builder->CreateLoad(Expr->ft->type, target);
			}
		} else {
			if (main_partial || return_val_idx) {
				RetVal = Expr->codegen();
			} else {
				RetVal = Expr->codegen(true);
			}
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

llvm::Value* HandleMove(
	ExprAST* expr, volvoxc::FullType* proto_ft, llvm::Type* real_arg_type, bool is_address,
	bool is_moved, bool needs_constructor_call, llvm::Value* val, SourceLocation* laterUsage) {
	llvm::Type* arg_type = val ? val->getType() : expr->desired_type ? expr->desired_type : expr->ft->type;
	llvm::Value* arg;
	if (jit_repl && !inside_function) {
		llvm::GlobalVariable* GV = new llvm::GlobalVariable(*TheModule, arg_type, false, llvm::GlobalValue::InternalLinkage, llvm::Constant::getNullValue(arg_type));
		GV->setAlignment(TheModule->getDataLayout().getPrefTypeAlign(arg_type));
		arg = GV;
	} else
		arg = CreateEntryBlockAlloca(arg_type);
	if (val) {
		Builder->CreateStore(val, arg);
	} else {
		auto voidval = expr->codegen_raw(arg);
		if (!voidval || !voidval->getType()->isVoidTy()) {
			errs() << expr->Loc << ": cannot create function call argument\n";
			return nullptr;
		}
	}
	if (is_moved) {
		// errs() << expr->Loc << ": ### function argument moved\n";
		// we set the original value to zero to invalidate the object
		// the destructor is supposed to ignore this value
		if (auto arg_ref_expr = dynamic_cast<VariableExprAST*>(expr)) {
			auto ty_ref = arg_ref_expr->codegen_ref();
			if (!ty_ref.second)
				return nullptr;
			auto nullval = llvm::Constant::getNullValue(ty_ref.first);
			Builder->CreateStore(nullval, ty_ref.second);
		}
	} else {
		if (needs_constructor_call)
			if (auto var_expr = dynamic_cast<VariableExprAST*>(expr)) {
				auto F = getConstructorOrDestructor(proto_ft);
				if (!F) {
					errs() << expr->Loc << ": clone constructor not available for type " << *proto_ft << " and move is not possible\n";
					if (laterUsage)
						errs() << *laterUsage << ": info: " << var_expr->Name << " is used later here\n";
					return nullptr;
				} else
					Builder->CreateCall(F, { arg });
			}
		if (!dynamic_cast<VariableExprAST*>(expr) && is_address)
			register_destructor(expr->Loc, proto_ft, arg);
	}
	if (!is_address && !dynamic_cast<InterfaceExprAST*>(expr))
		arg = Builder->CreateLoad(real_arg_type, arg);
	return arg;
}

void HandleReturn(BranchDescription& bBranch, llvm::Value* RetVal)
{
	bool already_returned = false; // set if both branches of last 'if ... else ...' end with 'return'
	std::vector<std::unique_ptr<ExprAST>>& Branch = bBranch.first;
	BreakDescription& brk_descr = bBranch.second;
	if (!Branch.empty())
		if (auto ifexpr = dynamic_cast<IfExprAST*>(Branch.back().get()))
			already_returned = ifexpr->always_return;
	if (!already_returned) {
		VariableExprAST* ret_var = Branch.empty() ? nullptr
			: dynamic_cast<VariableExprAST*>(Branch.back().get()); // suppress destructor for "return ret_var"
		llvm::Value* var_ptr;
		if (ret_var) {
			var_ptr = ret_var->codegen_ref(true).second;
		} else
			var_ptr = nullptr;
		if (currentFunction->Proto->RetType->type->isVoidTy()
		    || (currentFunction->Proto->visibility & A_constructor) && !currentFunction->RetVar) {
			// if (currentFunction->Proto->visibility & A_destructor) {
			// 	insert_field_destructors(currentFunction->receiver_ft, currentFunction->TheFunction->getArg(0));
			// }
			InsertDestructors(brk_descr.vars_to_destruct);
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
				InsertDestructors(brk_descr.vars_to_destruct, nullptr, var_ptr);
				Builder->CreateRetVoid();
			} else {
				if (currentFunction->RetVar) {
					RetVal = Builder->CreateLoad(currentFunction->ret_ft->type, currentFunction->RetVar->val);
					InsertDestructors(brk_descr.vars_to_destruct, nullptr, currentFunction->RetVar->val);
				} else if (RetVal->getType()->isPointerTy())
					InsertDestructors(brk_descr.vars_to_destruct, nullptr, RetVal);
				else {
					llvm::Value* re_ptr = nullptr;
					InsertDestructors(brk_descr.vars_to_destruct, nullptr, var_ptr);
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
	HandleReturn(bBody, RetVal);
	if (comp_mode == comp_dbg) {
		// Pop off the lexical block for the function.
		KSDbgInfo.LexicalBlocks.pop_back();
	}
	// Validate the generated code, checking for consistency.
	bool success = finishFunctionOrModule(TheFunction, 1, false, false);
	if (success) {
		std::string& FnName = Proto->Name;
		if (FnName != "__anon_expr") {
			auto registred = defined_functions.insert({FnName, Proto->retLoc});
			if (!registred.second) {
				errs() << Proto->retLoc << ": internal compiler error - function '" << FnName << "' seems to have been previously defined\n";
				errs() << registred.first->second << ": this is the location of the previous definition\n";
				success = false;
			} else if (AutoMethod && !((Proto->visibility & A_constructor) && is_deleted(std::get<0>(*AutoMethod)))) {
				// provide "full" versions of constructor/destructor that handles elements
				auto D = createConstructorOrDestructorFnProto(Proto->ArgTypes[0], (bool)(Proto->visibility & A_constructor));
				auto thisarg = D->getArg(0);
				llvm::BasicBlock* BB = llvm::BasicBlock::Create(Context, "entry", D);
				Builder->SetInsertPoint(BB);
				if (Proto->visibility & A_destructor)
					insert_field_destructors(Proto->ArgTypes[0], thisarg, false);
				Builder->CreateCall(constr_destr_fn_type, TheFunction , std::vector<llvm::Value*>{ thisarg });
				if (Proto->visibility & A_constructor)
					insert_field_destructors(Proto->ArgTypes[0], thisarg, true);
				Builder->CreateRetVoid();
				success = success && finishFunctionOrModule(D, 1, false, false);
				if (success) {
					if (Proto->visibility & A_constructor) {
						std::get<0>(*AutoMethod) = D->getName();
					} else {
						std::get<1>(*AutoMethod) = D->getName();
					}
				}
			}
		}
		std::string mangled_fn_name = TheFunction->getName().str();
		int idx=0;
		for (auto& flag: Proto->ArgNeedsConstructor) {
			if (get_arg_flag(flag, maybe_arg_is_owned) && (Proto->ArgTypes[idx]->type_attr & A_destructor)) {
				auto destr_shadow_fn = getShadowConstructorDestructor(mangled_fn_name, idx, true);
				auto BB = llvm::BasicBlock::Create(Context, "entry", destr_shadow_fn);
				Builder->SetInsertPoint(BB);
				llvm::Value* Arg = destr_shadow_fn->getArg(0);
				if (get_arg_flag(flag, arg_is_owned)) {
					auto destr_fn =  getConstructorOrDestructor(Proto->ArgTypes[idx], true);
					if (!destr_fn)
						abort();
					Builder->CreateCall(constr_destr_fn_type, destr_fn, std::vector<llvm::Value*>{ Arg });
				}
				Builder->CreateRetVoid();
				success = success && finishFunctionOrModule(destr_shadow_fn, 1, false, false);
				if (!success)
					break;
			}
			unset_arg_flag(&flag, maybe_arg_is_owned);
			if (!get_arg_flag(flag, arg_is_owned)) {
				unset_arg_flag(&flag, arg_has_constructor);
				unset_arg_flag(&flag, arg_has_destructor);
			}
			idx++;
		}
		success = success && finishFunctionOrModule(nullptr, 1, finishModule, getNewModule);
	}
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
