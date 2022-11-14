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

// global function to find method protos
std::vector<std::unique_ptr<PrototypeAST>>* findProtos(const std::string& mangledType, const std::string& unmangledName) {
	auto FI = MethodProtos.find({ mangledType, unmangledName });
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

llvm::Function* getAutoMethod(std::string& mangled_name) {
	if (auto F = TheModule->getFunction(mangled_name))
		return F;
	auto fn_type = AutoMethods.find(mangled_name);
	if (fn_type == AutoMethods.end())
		return nullptr;
	auto F = llvm::Function::Create(fn_type->second, llvm::Function::ExternalLinkage, mangled_name, TheModule.get());
	return F;
}

inline static uintptr_t getFnAddress(std::function<llvm::Value*(llvm::Value*)> f) {
    typedef llvm::Value* (fnType)(llvm::Value*);
    fnType** fnPointer = f.template target<fnType*>();
    return (uintptr_t)*fnPointer;
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

inline static void printCandidates(std::vector<int>& candidates, std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name) {
	for(auto c: candidates)
		printCandidate((*protos)[c].get(), name);
}

inline static void printAllProtos(std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name) {
	for (auto& proto: *protos)
		printCandidate(proto.get(), name);
}

int selectProto(std::vector<std::unique_ptr<PrototypeAST>>* protos, const char* name,
                std::vector<FnArg>& fnargs, SourceLocation Loc = {0}) {
	bool exact_match = false;
	int candidate = -1;
	// in regular cases there is only one candidate
	// only when there are more we create a vector
	std::vector<int> candidates;
	int i_proto = 0;
	for (auto& proto: *protos) {
		if (proto->IsVarArgs)
			return i_proto;
		if (!proto->IsVarArgs && proto->ArgTypes.size() != fnargs.size()
		    || proto->IsVarArgs && proto->ArgTypes.size() > fnargs.size()) {
			i_proto++;
			continue;
		}
		bool exact = true;
		bool with_conv = true;
		for (int i=0; i<fnargs.size(); i++) {
			bool arg_matches_exactly;
			if (i >= proto->ArgTypes.size()) {
				if (candidate < 0)
					fnargs[i].Conv = nullptr;
			} else {
				auto conv = getConv(fnargs[i].argtype, proto->ArgTypes[i]->type, SourceLocation{0},
				                    fnargs[i].arg_signed, (bool)(proto->ArgTypes[i]->type_attr & A_signed),
				                    false, fnargs[i].arg_unknown_type, &arg_matches_exactly);
				if (arg_matches_exactly) {
					if (candidate < 0)
						fnargs[i].Conv = conv;
				} else if (conv) {
					exact = false;
					if (candidate < 0)
						fnargs[i].Conv = conv;
				} else {
					exact = with_conv = false;
					break;
				}
			}
		}
		if (exact) {
			for (int i=0; i<fnargs.size(); i++)
				fnargs[i].Conv = NoConversion;
			return i_proto;
		} else if (with_conv) {
			if (candidate >= 0)
				if (candidates.empty()) {
					candidates.reserve(2);
					candidates.push_back(candidate);
					candidates.push_back(i_proto);
				}
				else
					candidates.push_back(i_proto);
			else
				candidate = i_proto;
		}
		i_proto++;
	}
	if (!candidates.empty()) {
		errs() << Loc << ": call of '" << name << "()' is ambiguous - candidates are:\n";
		printCandidates(candidates, protos, name);
		return -1;
	}
	if (candidate < 0) {
		errs() << Loc << ": signature of call to '" << name << "()' does not match any known candidate - candidates are:\n";
		printAllProtos(protos, name);
		return -1;
	}
	auto selected_proto = (*protos)[candidate].get();
	for (int i=0; i<selected_proto->ArgTypes.size(); i++)
		if ((selected_proto->ArgTypes[i]->type_attr & A_ref) && fnargs[i].Conv && getFnAddress(fnargs[i].Conv) != (uintptr_t)NoConversion) {
			errs() << Loc << ": cannot call '" << name << "()' canditate with matching signature would require conversion of "
			       << i+1 << (!i ? "st" : (i==1) ? "nd" : (i==2) ? "rd" : "th") << " argument which is passed by reference\n";
			return -1;
		}
	return candidate;
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
	if (method)
		n_args++;
	fn_args.reserve(n_args);
	if (method)
		fn_args.push_back(FnArg{nullptr, method->Receiver->ft->type, static_cast<bool>(method->Receiver->ft->type_attr & A_signed), method->Receiver->is_unknown_type});
	for (auto& arg: Args)
		fn_args.push_back(FnArg{nullptr, arg->ft->type, static_cast<bool>(arg->ft->type_attr & A_signed), arg->is_unknown_type});
	int selected_proto = selectProto(Callee->ft->Protos, name, fn_args, Callee->Loc);
	if (selected_proto == 0 || selected_proto > 0 && functionexpr)
		ft = (*Callee->ft->Protos)[selected_proto]->RetType;
	if (functionexpr)
		functionexpr->selected_proto = selected_proto;
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
	llvm::Value* AllocSize = Builder->getInt64(1);
	unsigned i = 0;
	while (auto array_type = llvm::dyn_cast<llvm::ArrayType>(elem_type)) {
		elem_type = array_type->getElementType();
		if (auto n = array_type->getNumElements())
			AllocSize = Builder->CreateMul(AllocSize, Builder->getInt64(n));
		else
			AllocSize = Builder->CreateMul(AllocSize, Builder->CreateExtractValue(val, i++));
	}
	if (i && (!struct_type || i != struct_type->getNumElements()-1)) {
		errs() << "internal error in array destructor creation - value does not match type " << i << ' ' << *struct_type << "\n";
		abort();
	}
	auto elem_sz = TheModule->getDataLayout().getTypeAllocSize(elem_type);
	auto ElemAllocSize = Builder->getInt64(elem_sz);
	AllocSize = Builder->CreateMul(AllocSize, ElemAllocSize);
	llvm::Type* elem_ptr_ty = elem_type->getPointerTo();
	auto elDestructorFT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { elem_ptr_ty }, false);
	llvm::BasicBlock* enterBB = Builder->GetInsertBlock();
	llvm::Function* TheFunction = enterBB->getParent();
	llvm::Value* Ptr = i ? Builder->CreateExtractValue(val, i) : val;
	Ptr = Builder->CreatePtrToInt(Ptr, llvm::Type::getInt64Ty(Context));
	llvm::Value* PtrStore = CreateEntryBlockAlloca(llvm::Type::getInt64Ty(Context), "", TheFunction);
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
	Ptr = Builder->CreateLoad(llvm::Type::getInt64Ty(Context), PtrStore);
	llvm::Value* ElPtr = Builder->CreateIntToPtr(Ptr, elem_ptr_ty);
	Builder->CreateCall(elDestructorFT, destructor, ElPtr);
	llvm::Value* NewPtr = Builder->CreateAdd(Ptr, ElemAllocSize);
	Builder->CreateStore(NewPtr, PtrStore);
	Builder->CreateBr(CondBB);
	TheFunction->getBasicBlockList().push_back(CondBB);
	Builder->SetInsertPoint(CondBB);
	Ptr = Builder->CreateLoad(llvm::Type::getInt64Ty(Context), PtrStore);
	auto is_less = Builder->CreateICmpULT(Ptr, UpperLimit);
	Builder->CreateCondBr(is_less, DestructorBB, ContBB);
	if (before)
		Builder->SetInsertPoint(before);
	else {
		TheFunction->getBasicBlockList().push_back(ContBB);
		Builder->SetInsertPoint(ContBB);
	}
}

// insert destructors for given var table - retp is a poniter to the function return value
// in case of struct-return - so this one will not be destructed but moved to the caller, instead
void InsertDestructors(VarTable& t, llvm::Value* retp) {
	for (auto var_node = t.first(); var_node; ++var_node) {
		MapValue* node = var_node.getValue();
		auto fv = (FullVar*)((char*)node + node->offset);
		if ((fv->ft.type_attr & A_destructor) && fv->val && fv->val != retp)
			InsertDestructor(fv);
	}
}

void InsertDestructors(llvm::Value* retp) {
	for (auto t = locals_table.rbegin(); t != locals_table.rend(); ++t )
		InsertDestructors(*t, retp);
}

static bool insert_field_destructors(volvoxc::FullType* ft, llvm::Argument* thisarg, bool is_constructor = false) {
	bool needs_destructors = false;
	for (auto field = ft->first(); field; ++field) {
		auto el_ft = field.getFt();
		if (el_ft->type_attr & A_destructor) {
			needs_destructors = true;
			unsigned idx = field.getIndex();
			llvm::Value* elem_ref = Builder->CreateConstGEP2_32(ft->type, thisarg, 0, idx);
			llvm::Function* field_destructor = getDestructor(el_ft, false, is_constructor);
			auto FT = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), { el_ft->type->getPointerTo() }, false);
			Builder->CreateCall(FT, field_destructor, elem_ref);
		} else if (isa<llvm::ArrayType>(el_ft->type) && (el_ft->elem_type->type_attr & A_destructor)) {
			needs_destructors = true;
			unsigned idx = field.getIndex();
			llvm::Value* elem_ref = Builder->CreateConstGEP2_32(ft->type, thisarg, 0, idx);
			InsertArrayConDestructor(el_ft->type, el_ft->elem_type, elem_ref, nullptr, is_constructor);
		}
	}
	return needs_destructors;
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
	if (auto type_expr = dynamic_cast<TypeExprAST*>(Callee.get())) {
		uint64_t allocsz = TheModule->getDataLayout().getTypeAllocSize(type_expr->ft->type);
		llvm::Value* ret_val = nullptr;
		if (!target && (allocsz > 16 || (type_expr->ft->type_attr & A_constructor)))
			target = ret_val = CreateEntryBlockAlloca(type_expr->ft->type, "");
		if (target)
			Builder->CreateStore(llvm::Constant::getNullValue(ft->type), target);
		switch (Args.size()) {
		case 0:
			return llvm::Constant::getNullValue(ft->type);
		case 1:
			Args[0]->desired_type = ft->type;
			Args[0]->conv_kind = ft->type_attr & A_signed ? ConvSigned : ConvUnsigned;
			return Args[0]->codegen();
		default:
			errs() << "constructors with #arg!=1 not supported, yet\n";
			return nullptr;
		}
	}
	PrototypeAST* Proto;
	if (auto functionexpr = dynamic_cast<FunctionExprAST*>(Callee.get())) {
		if (functionexpr->selected_proto < 0)
			return nullptr;
		Proto = (*Callee->ft->Protos)[functionexpr->selected_proto].get();
	} else {
		Proto = (*Callee->ft->Protos)[0].get();
	}
	llvm::Value* theFunction = Callee->codegen();
	auto FT = llvm::cast<llvm::FunctionType>(Callee->ft->type);
	// If argument mismatch error.
	unsigned proto_arg_offs = (Proto->visibility & A_method) ? 1 : 0;
	unsigned arg_offs = proto_arg_offs + (Proto->IsStructRet ? 1 : 0);
	unsigned proto_args_size = Proto->Args.size() - proto_arg_offs;
	unsigned ft_num_params = FT->getNumParams() - arg_offs;
	std::vector<llvm::Value *> ArgsV;
	llvm::Value* ret_struct = nullptr;
	if (Proto->IsStructRet) {
		if (!target) {
			errs() << Loc << ": " << Proto->Name << " - internal error: no target for struct return\n";
			return nullptr;
		}
		ArgsV.push_back(target);
	}
	if (Proto->visibility & A_method) {
		if (auto method = dynamic_cast<MethodExprAST*>(Callee.get())) {
			if (auto receiver_lval = dynamic_cast<LvalueExprAST*>(method->Receiver.get())) {
				auto receiver_ref = receiver_lval->codegen_ref();
				if (!receiver_ref.second) {
					errs() << method->Receiver->Loc << ": could not get receiver reference\n";
					return nullptr;
				}
				ArgsV.push_back(receiver_ref.second);
			} else {
				errs() << method->Receiver->Loc << ": receiver is not an lvalue\n";
			}
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
				arg = conversion(Args[i]->codegen());
			}
			ArgsV.push_back(arg);
		} else {
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
						Builder->CreateStore(tmparg, arg);
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
		return Builder->CreateCall(F, ArgsV, "calltmp");
	} else {
		// theFunction is a function pointer, i.e. a function call address (e.g. loaded from a variable)
		return Builder->CreateCall(FT, theFunction, ArgsV, "callptrtmp");
	}
}

llvm::Function *FunctionAST::codegen(bool finishModule, bool getNewModule) {
	// Transfer ownership of the prototype to the lex.module->FunctionProtos map, but keep a
	// reference to it for use below.
	auto &P = *Proto;
	volvoxc::FullType* receiver_ft;
	if (Proto->visibility & A_method)
		if (Proto->IsStructRet)
			receiver_ft = Proto->ArgTypes[1];
		else
			receiver_ft = Proto->ArgTypes[0];
	else
		receiver_ft = nullptr;
	llvm::Function* TheFunction = getFunction(Proto);
	if (!TheFunction) {
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
	theFunction_ret_ft = P.RetType;
	if (P.IsStructRet)
		ret_ptr = TheFunction->getArg(ArgIdx++);
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
		if (Body.empty() || !Body.back())
			goto cleanup;
		Body.back()->desired_type = P.RetType->type;
	}
	llvm::Value* RetVal;
	for (auto& Expr : Body) {
		if ((RetVal = Expr->codegen())) {
			if (comp_mode == comp_dbg) {
				KSDbgInfo.emitLocation(Expr.get());
			}
		} else {
			goto cleanup;
		}
	}
	// Finish off the function.
	if (P.RetType->type->isVoidTy()) {
		if (P.visibility & A_destructor) {
			insert_field_destructors(receiver_ft, TheFunction->getArg(0));
		}
		InsertDestructors(nullptr);
		Builder->CreateRetVoid();
	} else {
		// auto ret_type = RetVal->getType();
		//type = ret_type; // TODO: hande conversion if != proto->type;
		if (P.IsStructRet) {
			Builder->CreateStore(RetVal, ret_ptr);
			InsertDestructors(ret_ptr);
			Builder->CreateRetVoid();
		} else {
			InsertDestructors(nullptr);
			Builder->CreateRet(CheckTailCall(RetVal));
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
	return TheFunction;
cleanup:
	// Error reading body, remove function.
	TheFunction->eraseFromParent();
	if (comp_mode == comp_dbg) {
		// Pop off the lexical block for the function since we added it
		// unconditionally.
		KSDbgInfo.LexicalBlocks.pop_back();
	}
	ret_ptr = nullptr;
	theFunction_ret_ft = nullptr;
	return nullptr;
}
