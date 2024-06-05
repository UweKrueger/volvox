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

static unsigned wrapper_idx = 0;

std::vector<unsigned> get_vardims(llvm::Type* ty) {
	std::vector<unsigned> var_dims;
	unsigned idx = 0;
	while (auto array_ty = llvm::dyn_cast<llvm::ArrayType>(ty)) {
		if (!array_ty->getNumElements())
			var_dims.push_back(idx);
		idx++;
	}
	return var_dims;
}

llvm::Function* ThreadExprAST::get_thread_wrapper() {
	auto savedInsertPoint = Builder->GetInsertPoint();
	std::string wrapper_name = "__thread_wrapper_" + std::to_string(wrapper_idx++);
	// wrappers are always of type `void* f(void* arg)`
	llvm::FunctionType* wrapper_fn_t = llvm::FunctionType::get(llvm_ptr_type, { llvm_ptr_type }, false);
	llvm::Function* wrapper_f = llvm::Function::Create(wrapper_fn_t, llvm::Function::ExternalLinkage, wrapper_name, TheModule.get());
	auto BB = llvm::BasicBlock::Create(Context, "entry", wrapper_f);
	Builder->SetInsertPoint(BB);
	llvm::Value* control_block = wrapper_f->getArg(0);
	std::vector<llvm::Value*> args;
	llvm::Value* SretAlloc = nullptr;
	args.reserve(n_args+do_sret);
	if (do_sret) {
		// we use alloca (and not malloc) here to avoid memory leaks in case
		// of abort(). In case of successful thread termination the value will be
		// copied in a malloced memory space below.
		SretAlloc = Builder->CreateAlloca(ret_typ, nullptr);
		args.push_back(SretAlloc);
	}
	for (unsigned i=0; i<n_args; i++) {
		llvm::Value* el_ptr = Builder->CreateStructGEP(args_type, control_block, i);
		auto ty = args_type->getElementType(i);
		llvm::Value* arg = Builder->CreateLoad(ty, el_ptr);
		args.push_back(arg);
	}
	llvm::Value* theFunction = Call->Callee->codegen();
	if (!theFunction)
		theFunction = getFunction(Call->Proto);
	if (!theFunction)
		abort();
	auto FT = Call->Proto->FT;
	llvm::Value* res;
	if (auto F = llvm::dyn_cast<llvm::Function>(theFunction))
		res = Builder->CreateCall(FT, F, args);
	else
		abort();
	PrototypeAST* win_thread_exit_proto = nullptr;
	llvm::Function* win_thread_exit_fn = nullptr;
	if (os_idx == OS_Windows && !target_mingw) {
		win_thread_exit_proto = (*lex.findProtos("__win_exit_thread"))[0].get();
		win_thread_exit_fn = getFunction(win_thread_exit_proto);
	}
	llvm::Value* returnres;
	if (ret_typ->isVoidTy()) {
		if (win_thread_exit_fn)
			Builder->CreateCall(win_thread_exit_proto->FT, win_thread_exit_fn, std::vector<llvm::Value*>{
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0) });
		Builder->CreateRetVoid();
	} else {
		// we try to return the result in the following order
		// 1. directly as DWORD on Windows native (max 32-Bit)
		// 2. converted to (void*) as thread result (using table on Windows-native)
		// 3. address of malloc()ed memory space (using table on Windows-native)
		if (ret_sz <= 4 && win_thread_exit_fn) {
			Builder->CreateCall(win_thread_exit_proto->FT, win_thread_exit_fn, std::vector<llvm::Value*>{
					Builder->CreateBitCast(res, llvm::Type::getInt32Ty(Context)) });
			Builder->CreateRetVoid();
		} else if (ret_sz <= target_bytes) {
			llvm::Value* retval = Builder->CreateIntToPtr(Builder->CreateBitCast(res, llvm_size_type), llvm_ptr_type);
			if (os_idx == OS_Windows && !target_mingw) {
				auto return_idx_proto = (*lex.findProtos("__get_thread_return_idx"))[0].get();
				auto return_idx_fn = getFunction(return_idx_proto);
				llvm::Value* dword_ret = Builder->CreateCall(return_idx_proto->FT, return_idx_fn,
				                                             std::vector<llvm::Value*>{ retval });
				Builder->CreateCall(win_thread_exit_proto->FT, win_thread_exit_fn,
				                    std::vector<llvm::Value*>{ dword_ret });
				Builder->CreateRetVoid();
			} else {
				Builder->CreateRet(retval);
			}
		} else {
#if LLVM_VERSION_MAJOR >= 18
			llvm::Value* Malloc = Builder->CreateMalloc(
				llvm_size_type, llvm::Type::getInt8Ty(Context),
				getSize(ret_sz), nullptr, nullptr, "threadresult");
#else
			llvm::Value* Malloc = llvm::CallInst::CreateMalloc(
				Builder->GetInsertBlock(),
				llvm_size_type, llvm::Type::getInt8Ty(Context),
				getSize(ret_sz), nullptr, nullptr, "threadresult");
			Malloc = Builder->Insert(Malloc);
#endif
			if (do_sret) {
				auto align = getAlignment(ret_sz);
				Builder->CreateMemCpy(Malloc, align, SretAlloc, align, ret_sz);
			} else {
				Builder->CreateStore(res, Malloc);
			}
			if (os_idx == OS_Windows && !target_mingw) {
				auto return_idx_proto = (*lex.findProtos("__get_thread_return_idx"))[0].get();
				auto return_idx_fn = getFunction(return_idx_proto);
				llvm::Value* dword_ret = Builder->CreateCall(return_idx_proto->FT, return_idx_fn,
				                                             std::vector<llvm::Value*>{ Malloc });
				Builder->CreateCall(win_thread_exit_proto->FT, win_thread_exit_fn,
				                    std::vector<llvm::Value*>{ dword_ret });
				Builder->CreateRetVoid();
			} else {
				Builder->CreateRet(Malloc);
			}
		}
	}
	bool fin = finishFunctionOrModule(wrapper_f, 1, false, false);
	Builder->SetInsertPoint(savedInsertPoint);
	if (fin)
		return wrapper_f;
	else
		return nullptr;
}

llvm::Value* ThreadExprAST::codegen_raw(llvm::Value* target) {
	// offset, allocsz, var_indices, is_reference
	ret_typ = Call->Proto->RetType->type;
	ret_sz = ret_typ->isVoidTy() ? 0 : TheModule->getDataLayout().getTypeAllocSize(ret_typ);
	do_sret = (ret_sz > sret_limit) ? 1 : 0;
	llvm::Value* Malloc;
	n_args = Call->Proto->LLVMArgTypes.size() - do_sret;
	if (!n_args) {
		args_type = nullptr;
		Malloc = llvm::ConstantPointerNull::get(llvm_ptr_type);
	} else {
		args_type = llvm::StructType::get(Context, llvm::ArrayRef(Call->Proto->LLVMArgTypes.data()+do_sret, n_args));
		llvm::Value* AllocSz = getSize(TheModule->getDataLayout().getTypeAllocSize(args_type));
#if LLVM_VERSION_MAJOR >= 18
		Malloc = Builder->CreateMalloc(
			llvm_size_type, llvm::Type::getInt8Ty(Context),
			AllocSz, nullptr, nullptr, "threadcontext");
#else
		Malloc = llvm::CallInst::CreateMalloc(
			Builder->GetInsertBlock(),
			llvm_size_type, llvm::Type::getInt8Ty(Context),
			AllocSz, nullptr, nullptr, "threadcontext");
		Malloc = Builder->Insert(Malloc);
#endif
		unsigned i = 0;
		for (auto& arg: Call->Args) {
			unsigned attr = Call->Proto->ArgTypes[i]->type_attr;
			llvm::Type* ty = Call->Proto->ArgTypes[i]->type;
			std::vector<unsigned> var_dims = get_vardims(ty);
			unsigned n_var_dims = var_dims.size();
			if (n_var_dims) {
				errs() << arg->Loc << ": variable size array as parameter for 'thread' call not allowed\n";
				return nullptr;
			}
			if (attr & A_ref) {
				if (!(attr & (A_unique | A_shared | A_const))) {
					errs() << arg->Loc << ": reference argument for 'thread' only allowed when 'unique', 'shared' or 'const'";
					return nullptr;
				}
			}
			llvm::Value* arg_val = Call->Args[i]->codegen();
			llvm::Value* Adr = Builder->CreateStructGEP(args_type, Malloc, i, Call->Proto->Args[i]);
			Builder->CreateStore(arg_val, Adr);
		}
	}
	llvm::Function* wrapper = get_thread_wrapper();
	if (!wrapper)
		return nullptr;
	auto thread_create_proto = (*lex.findProtos("__create_thread"))[0].get();
	auto thread_create_fn = getFunction(thread_create_proto);
	llvm::Value* thr_id =Builder->CreateCall(
		thread_create_proto->FT, thread_create_fn,
		std::vector<llvm::Value*>{ wrapper, Malloc, llvm::ConstantInt::get(llvm::Type::getInt1Ty(Context), 0) });
	return thr_id;
}
