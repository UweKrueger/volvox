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
	args.reserve(Call->Proto->LLVMArgTypes.size());
}

llvm::Value* ThreadExprAST::codegen_raw(llvm::Value* target) {
	// offset, allocsz, var_indices, is_reference
	llvm::Value* Malloc;
	if (Call->Proto->LLVMArgTypes.empty()) {
		args_type = nullptr;
		Malloc = llvm::ConstantPointerNull::get(llvm_ptr_type);
	} else {
		args_type = llvm::StructType::get(Context, Call->Proto->LLVMArgTypes);
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
	return Malloc;
}
