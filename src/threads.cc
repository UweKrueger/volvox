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

llvm::Value* TaskExprAST::codegen_raw(llvm::Value* target) {
	llvm::Value* AllocSz = Call->alloc_size();
	// offset, allocsz, var_indices, is_reference
	std::vector<std::tuple<llvm::Value*,llvm::Value*,std::vector<unsigned>,bool>> Alloc;
	Alloc.reserve(Call->Args.size());
	unsigned i = 0;
	for (auto& arg: Call->Args) {
		unsigned attr = Call->Proto->ArgTypes[i]->type_attr;
		llvm::Type* ty = Call->Proto->ArgTypes[i]->type;
		std::vector<unsigned> var_dims = get_vardims(ty);
		unsigned n_var_dims = var_dims.size();
		// align AllocSz to target_bytes
		AllocSz = Builder->CreateAdd(AllocSz, getSize(target_bytes - 1));
		AllocSz = Builder->CreateAnd(AllocSz, getSize(-target_bytes));
		if (n_var_dims)
			AllocSz = Builder->CreateAdd(AllocSz, getSize(n_var_dims * target_bytes));
		if (attr & A_ref) {
			if (attr & (A_unique | A_shared | A_const)) {
				llvm::Value* sz = getSize(target_bytes);
				Alloc.push_back({ AllocSz, sz, var_dims, true });
				AllocSz = Builder->CreateAdd(AllocSz, sz);
			} else {
				errs() << arg->Loc << ": reference argument for 'task' only allowed when 'unique', 'shared' or 'const'";
				return nullptr;
			}
		} else {
			llvm::Value* sz = arg->alloc_size();
			Alloc.push_back({ AllocSz, sz, var_dims, false });
			AllocSz = Builder->CreateAdd(AllocSz, sz);
		}
		i++;
	}
#if LLVM_VERSION_MAJOR >= 18
	llvm::Value* Malloc = Builder->CreateMalloc(
		llvm_size_type, llvm::Type::getInt8Ty(Context),
		AllocSz, nullptr, nullptr, "task");
#else
	llvm::Value* Malloc = llvm::CallInst::CreateMalloc(
		Builder->GetInsertBlock(),
		llvm_size_type, llvm::Type::getInt8Ty(Context),
		AllocSz, nullptr, nullptr, "task");
#endif
	Malloc =  Builder->Insert(Malloc);
	for (unsigned j=0; j<i; j++) {
		auto [ offs, sz, var_dims, is_ref ] = Alloc[j];
		llvm::Value* val = nullptr;
		llvm::Value* ref = nullptr;
		llvm::Value* Adr;
		if (!var_dims.empty()) {
			llvm::Value* IdxOffs = Builder->CreateSub(offs, getSize(var_dims.size() * target_bytes));
			llvm::Value* IdxsAdr = Builder->CreateGEP(llvm::Type::getInt8Ty(Context), Malloc, IdxOffs);
			auto dim_vals = Call->Args[j]->codegen_dims();
			for (unsigned n=0; ; n++) {
				Adr = Builder->CreateConstGEP1_32(
					llvm_size_type, Builder->CreatePointerCast(Adr, llvm_size_type->getPointerTo()), n);
				if (n == var_dims.size())
					break;
				Builder->CreateStore(Adr, (*dim_vals.second)[var_dims[n]]);
			}
		} else {
			Adr = Builder->CreateGEP(llvm::Type::getInt8Ty(Context), Malloc, offs);
		}
	}
	// return Malloc;
	return llvm::Constant::getNullValue(ft->type);
}
