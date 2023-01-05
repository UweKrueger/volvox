/*
 * Copyright © Uwe Krüger 2021, 2022
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"
#include "../lib/str.h"

// variable size main vars are "malloc()ed" in jit mode. On exit these blocks would be
// orphaned - so let's keep track of then to avoid memory leaks:
MainVars jit_main_variables;
llvm::DISubprogram *SP;
llvm::DIFile *Unit;
volvoxc::FullType* theFunction_ret_ft = nullptr;
std::vector<FullVar> expr_temps; // to call destructors immediatelly after expr

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
		errs() << fnargs[i];
	}
}

static void printCandidate(PrototypeAST* proto, const char* name) {
	errs() << proto->retLoc << ": " << name << '(';
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

inline static void printAllProtos(std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name) {
	for (auto& proto: *protos)
		printCandidate(proto.get(), name);
}

int selectProto(std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name,
                std::vector<FnArg>& fnargs, SourceLocation Loc = {0}) {
	bool exact_match = false;
	int noundefcandidate = -1;
	int candidate = -1;
	// there are 3 classes of match for a function signature:
	// 1. exact match - all arguments match without conversion (with the default type for untyped parameters)
	// 2. all arguments match with automatic conversions when the default type for untyped is used
	// 3. all arguments match with automatic conversions that include those that are only allowed due to
	//    relaxed constraints for untyped arguments
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
			bool arg_matches_exactly;
			if (i >= proto->ArgTypes.size()) {
				if (candidate < 0)
					fnargs[i].Conv = nullptr; // for var args - but see comment above
			} else {
				auto conv = getConv(fnargs[i].argtype, proto->ArgTypes[i]->type, SourceLocation{0},
				                    fnargs[i].arg_signed, (bool)(proto->ArgTypes[i]->type_attr & A_signed),
				                    false, false, &arg_matches_exactly);
				if (arg_matches_exactly) {
					if (!cands1)
						fnargs[i].Conv = nullptr;
					if (!cands2)
						convs2[i] = nullptr;
					if (!cands3)
						convs3[i] = nullptr;
				} else if (conv) {
					exact = false;
					if (!cands2)
						convs2[i] = conv;
					if (!cands3)
						convs3[i] = conv;
				} else {
					exact = with_conv = false;
					if (fnargs[i].arg_unknown_type) {
						conv = getConv(fnargs[i].argtype, proto->ArgTypes[i]->type, SourceLocation{0},
						               fnargs[i].arg_signed, (bool)(proto->ArgTypes[i]->type_attr & A_signed),
						               false, true, nullptr);
						if (conv) {
							if (!cands3)
								convs3[i] = conv;
							continue; // with i, i.e. next argument
						}
					}
					with_undefconv = false;
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
			errs() << Loc << ": call of '" << name << '(';
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
			errs() << Loc << ": call of '" << name << '(';
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
	errs() << Loc << ": signature of call to '" << name << '(';
	printArgTypes(fnargs, (!(*protos)[0]->Args.empty() && (*protos)[0]->Args[0] == "this") ? 1 : 0);
	errs() << ")' does not match any known candidate - candidates are:\n";
	printAllProtos(protos, name);
	return -1;
check_selected_proto:
	auto selected_proto = (*protos)[selected_idx].get();
	for (int i=0; i<selected_proto->ArgTypes.size(); i++)
		if ((selected_proto->ArgTypes[i]->type_attr & A_ref) && fnargs[i].Conv && getFnAddress(fnargs[i].Conv) != (uintptr_t)NoConversion) {
			errs() << Loc << ": cannot call '" << name << "()' candidate with matching signature would require conversion of "
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
		fn_args.push_back(FnArg{nullptr, type_expr->ft->type, static_cast<bool>(type_expr->ft->type_attr & A_signed), false});
	if (!type_expr || type_expr->ft->type->isStructTy()) {
		for (auto& arg: Args)
			fn_args.push_back(FnArg{nullptr, arg->ft->type, static_cast<bool>(arg->ft->type_attr & A_signed), arg->is_unknown_type});
		std::vector<std::unique_ptr<PrototypeAST>>* protos;
		if (type_expr) {
			protos = findProtos(std::string(type_expr->ft->mangled_name), type_expr->Name);
			if (!protos)
				errs() << type_expr->Loc << ": no constructor " << type_expr->Name << "() found\n";
		} else {
			protos = Callee->ft->Protos;
		}
		int selected_proto = selectProto(protos, name, fn_args, Callee->Loc);
		if (selected_proto >= 0)
			Proto = (*protos)[selected_proto].get();
		else
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

static void EraseInstruction(llvm::Instruction* inst) {
	llvm::BasicBlock::iterator BI(inst);
	llvm::BasicBlock::InstListType& BIL = inst->getParent()->getInstList();
	BIL.erase(BI);
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
	TheFunction->getBasicBlockList().push_back(DestructorBB);
	Builder->SetInsertPoint(DestructorBB);
	Ptr = Builder->CreateLoad(llvm_size_type, PtrStore);
	llvm::Value* ElPtr = Builder->CreateIntToPtr(Ptr, elem_ptr_ty);
	Builder->CreateCall(elDestructorFT, destructor, ElPtr);
	llvm::Value* NewPtr = Builder->CreateAdd(Ptr, ElemAllocSize);
	Builder->CreateStore(NewPtr, PtrStore);
	Builder->CreateBr(CondBB);
	TheFunction->getBasicBlockList().push_back(CondBB);
	Builder->SetInsertPoint(CondBB);
	Ptr = Builder->CreateLoad(llvm_size_type, PtrStore);
	auto is_less = Builder->CreateICmpULT(Ptr, UpperLimit);
	Builder->CreateCondBr(is_less, DestructorBB, ContBB);
	if (before)
		Builder->SetInsertPoint(before);
	else {
		TheFunction->getBasicBlockList().push_back(ContBB);
		Builder->SetInsertPoint(ContBB);
	}
}

// insert destructors for given var table - retp is a pointer to the function return value
// in case of struct-return - so this one will not be destructed but moved to the caller, instead
void InsertDestructors(VarTable& t, llvm::Value* retp) {
	for (auto var_node = t.first(); var_node; ++var_node) {
		MapValue* node = var_node.getValue();
		auto fv = (FullVar*)((char*)node + node->offset);
		if ((fv->ft.type_attr & (A_destructor | A_string | A_map)) && fv->val && fv->val != retp)
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
	auto vp = Builder->CreatePointerCast(v, llvm_size_type->getPointerTo());
	llvm::Value* subtrahend = Builder->CreateLoad(llvm_size_type, vp);
	return subtrahend;
}

llvm::Value* Volvox2CStr2(llvm::Value* v, llvm::Value* subtrahend) {
	subtrahend = Builder->CreateAdd(subtrahend, getSize(target_bytes - 1));
	subtrahend = Builder->CreateAnd(subtrahend, ~((1ULL << (target_bits - 1)) | (target_bytes - 1)));
	auto cstr = Builder->CreateIntToPtr(
		Builder->CreateSub(
			Builder->CreatePtrToInt(v, llvm_size_type),
			Builder->CreateIntCast(subtrahend, llvm_size_type, false)),
		llvm::Type::getInt8PtrTy(Context));
	return cstr;
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
	llvm::Value* destructflag = Builder->CreateAnd(subtrahend, 1ULL << (target_bits - 1));
	destructflag = Builder->CreateIsNotNull(destructflag);
	llvm::BasicBlock* DestructorBB = llvm::BasicBlock::Create(Context, "stringdestr");
	llvm::BasicBlock* ContBB = llvm::BasicBlock::Create(Context, "contdestr");
	Builder->CreateCondBr(destructflag, DestructorBB, ContBB);
	TheFunction->getBasicBlockList().push_back(DestructorBB);
	Builder->SetInsertPoint(DestructorBB);
	auto cstr = Volvox2CStr2(v, subtrahend);
	Builder->Insert(llvm::CallInst::CreateFree(cstr, DestructorBB));
	Builder->CreateBr(ContBB);
	TheFunction->getBasicBlockList().push_back(ContBB);
	Builder->SetInsertPoint(ContBB);
}

void InsertMapDestructor(llvm::Value* v, llvm::Instruction* before) {
	std::string destr = "_ZN6volvox3map7destroyEPNS0_4NodeEPFvPNS0_5ValueEE";
	PrototypeAST* destr_proto = (*lex.findProtos(destr))[0].get();
	auto destr_fn = getFunction(destr_proto);
	auto elem_destructor = llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(Context));
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

void finishFunctionOrModule(llvm::Function* F, unsigned dumpLevel, bool finishModule, bool newModule) {
	if (F) {
		verifyFunction(*F);
		if (dump_IR >= dumpLevel && dump_raw) {
			errs() << "Read \"" << F->getName() << "()\" definition (raw):\n";
			F->print(errs());
			errs() << "\n";
		}
#ifdef LEGACY_PASS_MANAGER
		TheFPM->run(*F);
#endif
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
			auto MPM = (optimization_level == llvm::OptimizationLevel::O0) ?
				PB.buildO0DefaultPipeline(optimization_level) :
				PB.buildPerModuleDefaultPipeline(optimization_level);
			MPM.run(*TheModule, MAM);
		}
#endif
		if (dump_IR >= dumpLevel && dump_opt)
			TheModule->print(errs(), nullptr);
		if (newModule) {
			ExitOnErr(TheJIT->addModule(
				          llvm::orc::ThreadSafeModule(std::move(TheModule), *TS_Context.get())));
			InitializeModuleAndPassManager();
		}
	}
	Builder->ClearInsertionPoint();
}

llvm::Function *PrototypeAST::codegen() {
	llvm::Function *F =
		llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, TheModule.get());
	// Set names for all arguments.
	unsigned Idx = 0;
	if (IsStructRet) {
		if (ArgAttrs[Idx].hasAttributes()) {
#if LLVM_VERSION_MAJOR >= 14
			llvm::AttrBuilder attr_builder(Context, ArgAttrs[Idx]);
			F->getArg(Idx)->addAttrs(attr_builder);
#else
			for (auto attr: ArgAttrs[Idx])
				F->getArg(Idx)->addAttr(attr);
#endif
		}
		Idx++;
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

llvm::Value *CallExprAST::codegen_raw(llvm::Value* target) {
	if (comp_mode == comp_dbg) {
		KSDbgInfo.emitLocation(this);
	}
	bool is_constructor_call;
	//std::vector<std::unique_ptr<PrototypeAST>>* protos = nullptr;
	if (auto type_expr = dynamic_cast<TypeExprAST*>(Callee.get())) {
		uint64_t allocsz = TheModule->getDataLayout().getTypeAllocSize(type_expr->ft->type);
		llvm::Value* ret_val = nullptr;
		if ((!target || (intptr_t)target == -1) && (allocsz > 16 || (type_expr->ft->type_attr & A_constructor)))
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
		} else {
			is_constructor_call = true;
		}
	} else
		is_constructor_call = false;
	if (!Proto) {
		return nullptr;
	}
	llvm::Value* theFunction = Callee->codegen();
	if (!theFunction)
		theFunction = getFunction(Proto);
	if (!theFunction)
		abort();
	auto FT = Proto->FT;
	// If argument mismatch error.
	unsigned proto_arg_offs = (Proto->visibility & A_method) ? 1 : 0;
	unsigned arg_offs = proto_arg_offs + (Proto->IsStructRet ? 1 : 0);
	unsigned proto_args_size = Proto->Args.size() - proto_arg_offs;
	unsigned ft_num_params = FT->getNumParams() - arg_offs;
	std::vector<llvm::Value *> ArgsV;
	llvm::Value* ret_struct = nullptr;
	if (Proto->IsStructRet || (Proto->visibility & A_constructor)) {
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
				receiver_ref = StoreValue(method->Receiver->codegen(), method->Receiver->ft);
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
	for (unsigned i = 0, e = Args.size(), v = proto_args_size; i != e; ++i) {
		if (i < v && !Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByRef)
		    && (Proto->ArgTypes[i+arg_offs]->type->isIntegerTy()
		        || Proto->ArgTypes[i+arg_offs]->type->isFloatingPointTy())) {
			auto conversion = fn_args[i+arg_offs].Conv;
			llvm::Value* arg;
			if (conversion) {
				if (auto binexpr = dynamic_cast<BinaryExprAST*>(Args[i].get())) {
					binexpr->desired_type = Proto->ArgTypes[i+arg_offs]->type;
					arg = Args[i]->codegen();
				} else {
					arg = conversion(Args[i]->codegen());
				}
			} else {
				arg = Args[i]->codegen();
			}
			ArgsV.push_back(arg);
		} else {
			llvm::Value* arg = nullptr;
			bool is_address = i < v && (Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByVal)
			                            || Proto->ArgAttrs[i+arg_offs].hasAttribute(llvm::Attribute::ByRef));
			if (auto call = dynamic_cast<CallExprAST*>(Args[i].get())) {
				auto functionexpr = dynamic_cast<FunctionExprAST*>(call->Callee.get());
				unsigned sub_sel_proto = functionexpr ? functionexpr->selected_proto : 0;
				PrototypeAST* CallProto = (*call->Callee->ft->Protos)[sub_sel_proto].get(); // 'g' in 'f(g())'
				if (CallProto->IsStructRet || (CallProto->visibility & A_constructor)) {
					if (is_address) {
						// 'g' returns by reference and 'f' exprects a reference (i.e. an address)
						// so we have to allocate memory for the indermediate result
						arg = Builder->CreateAlloca(call->ft->type);
						auto voidval = call->codegen_raw(arg);
						if (!voidval || !voidval->getType()->isVoidTy()) {
							errs() << Loc << ": cannot create function call\n";
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
						auto argref = lval->codegen_ref(true);
						if (!argref.first) {
							errs() << Args[i]->Loc << ": cannot generate code for expression\n";
							return nullptr;
						}
						arg = argref.second;
					}
					if (!arg) {
						arg = Builder->CreateAlloca(Proto->ArgTypes[i+arg_offs]->type);
						auto tmparg = Args[i]->codegen();
						if (!tmparg) {
							errs() << Args[i]->Loc << ": cannot generate code for expression\n";
							return nullptr;
						}
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
				} else
					arg = Args[i]->codegen();
			}
			if (!arg)
				return nullptr;
			if (arg->getType()->isFloatingPointTy() && !arg->getType()->isDoubleTy()) {
				// C convention: variadic float args must be promoted to double
				if (!arg->getType()->isFloatTy())
					arg = Builder->CreateFPCast(arg, llvm::Type::getFloatTy(Context), "convfptmp");
				arg = Builder->CreateBitCast(arg, llvm::Type::getInt32Ty(Context));
			} else if (auto intT = llvm::dyn_cast<llvm::IntegerType>(arg->getType())) {
				// same with short integers 
				if (intT->getBitWidth() < 32)
					arg = Builder->CreateIntCast(arg, llvm::Type::getInt32Ty(Context), !(!(Args[i]->ft->type_attr & A_signed)));
			}
			if (auto interf_t = dynamic_cast<InterfaceExprAST*>(Args[i].get()))
				if (auto struct_type = llvm::dyn_cast<llvm::StructType>(arg->getType()))
					for (unsigned i = 0; i < struct_type->getNumElements(); i++) {
						llvm::Value* argi = Builder->CreateExtractValue(arg, i);
						if (argi->getType()->isFloatingPointTy() && !argi->getType()->isDoubleTy()) {
							// C convention: variadic float args must be promoted to double
							if (!argi->getType()->isFloatTy())
								argi = Builder->CreateFPCast(argi, llvm::Type::getFloatTy(Context), "convfptmp");
							argi = Builder->CreateBitCast(argi, llvm::Type::getInt32Ty(Context));
						} else if (auto intT = llvm::dyn_cast<llvm::IntegerType>(argi->getType())) {
							// same with short integers 
							if (intT->getBitWidth() < 32)
								argi = Builder->CreateIntCast(argi, llvm::Type::getInt32Ty(Context), Args[i]->ft->type_attr & A_signed);
						}
						ArgsV.push_back(argi);
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
		if (Proto->const_result)
			return Proto->const_result;
		return Builder->CreateCall(F, ArgsV, "calltmp");
	} else {
		// theFunction is a function pointer, i.e. a function call address (e.g. loaded from a variable)
		return Builder->CreateCall(FT, theFunction, ArgsV, "callptrtmp");
	}
}

bool FunctionAST::prepare_codegen() {
	// Transfer ownership of the prototype to the lex.module->FunctionProtos map, but keep a
	// reference to it for use below.
	already_returned = false;
	if (Proto->visibility & (A_method | A_constructor))
		if (Proto->IsStructRet)
			receiver_ft = Proto->ArgTypes[1];
		else
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
	ret_ft = (Proto->visibility & A_constructor) ? void_type : Proto->RetType;
	theFunction_ret_ft = ret_ft; // global variable used by IfExprAST to return from branches
	if (Proto->IsStructRet && !(Proto->visibility & A_constructor))
		ret_ptr = this_ret_ptr = TheFunction->getArg(ArgIdx++);
	else
		ret_ptr = this_ret_ptr = nullptr;
	for (; ArgIdx < TheFunction->arg_size(); ArgIdx++) {
		auto Arg = TheFunction->getArg(ArgIdx);
		FullVar* mapitem = locals_table.back()[Arg->getName().str().c_str()];
		if (!mapitem) {
			errs() << "internal compiler error: arg #" << ArgIdx << " - '" << Arg->getName() << "' not found in table\n";
			exit(1);
		}
		if (Arg->hasByValAttr() || Arg->hasByRefAttr()) {
			mapitem->val = Arg;
		} else {
			// Create an alloca for this variable.
			llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(Proto->LLVMArgTypes[ArgIdx], Arg->getName(), TheFunction);
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
	for (auto& Expr : thisBody) {
		if ((RetVal = Expr->codegen())) {
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

llvm::Function* FunctionAST::finish_codegen(bool finishModule, bool getNewModule) {
	ret_ptr = this_ret_ptr;
	theFunction_ret_ft = ret_ft;
	Builder->SetInsertPoint(BB);
	if (InterRetVal)
		RetVal = InterRetVal;
	// Finish off the function.
	if (!Body.empty())
		if (auto ifexpr = dynamic_cast<IfExprAST*>(Body.back().get()))
			already_returned = ifexpr->always_return;
	if (!already_returned) {
		if (Proto->RetType->type->isVoidTy() || (Proto->visibility & A_constructor)) {
			if (Proto->visibility & A_destructor) {
				insert_field_destructors(receiver_ft, TheFunction->getArg(0));
			}
			InsertDestructors(nullptr);
			Builder->CreateRetVoid();
		} else {
			// auto ret_type = RetVal->getType();
			//type = ret_type; // TODO: hande conversion if != proto->type;
			if (Proto->IsStructRet) {
				Builder->CreateStore(RetVal, ret_ptr);
				InsertDestructors(ret_ptr);
				Builder->CreateRetVoid();
			} else {
				if (RetVal->getType()->isPointerTy())
					InsertDestructors(RetVal);
				else
					InsertDestructors(nullptr);
				Builder->CreateRet(CheckTailCall(RetVal));
				if (!ArgIdx && Body.size() == 1 && !InterRetVal && TheFunction->hasFnAttribute(llvm::Attribute::AlwaysInline))
					if (auto const_ret = llvm::dyn_cast<llvm::Constant>(RetVal))
						// hack to allow trivial static functions to be used as constexpr
						Proto->const_result = const_ret;
			}
		}
	}
	if (comp_mode == comp_dbg) {
		// Pop off the lexical block for the function.
		KSDbgInfo.LexicalBlocks.pop_back();
	}
	// Validate the generated code, checking for consistency.
	finishFunctionOrModule(TheFunction, 1, finishModule, getNewModule);
	ret_ptr = nullptr;
	theFunction_ret_ft = nullptr;
	expr_temps.clear();
	return TheFunction;
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
	theFunction_ret_ft = nullptr;
	expr_temps.clear();
	return nullptr;
}
