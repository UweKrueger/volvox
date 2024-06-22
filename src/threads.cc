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

static void windows_ret_ptr_as_dword(llvm::Value* retval) {
	auto return_idx_proto = (*lex.findProtos("__get_thread_return_idx"))[0].get();
	auto return_idx_fn = getFunction(return_idx_proto);
	llvm::Value* dword_ret = Builder->CreateCall(return_idx_proto->FT, return_idx_fn,
	                                             std::vector<llvm::Value*>{ retval });
	Builder->CreateRet(dword_ret);
}

llvm::Function* ThreadExprAST::get_thread_wrapper(bool have_target) {
	auto savedInsertPoint = Builder->GetInsertBlock();
	bool finish_module = (comp_mode == comp_jit);
	std::unique_ptr<llvm::Module> savedModule = nullptr;
	if (finish_module) {
		savedModule = std::move(TheModule);
		InitializeModuleAndPassManager();
	}
	std::string wrapper_name = "__thread_wrapper_" + std::to_string(wrapper_idx++);
	// wrappers are of type `unsigned f(void* arg)` on Windown
	// and `void* f(void* arg)` on POSIX systems
	llvm::FunctionType* wrapper_fn_t = llvm::FunctionType::get(
		(os_idx == OS_Windows) ? llvm_int_type : llvm_ptr_type,
		{ llvm_ptr_type }, false);
	llvm::Function* wrapper_f = llvm::Function::Create(wrapper_fn_t, llvm::Function::ExternalLinkage, wrapper_name, TheModule.get());
	auto BB = llvm::BasicBlock::Create(Context, "entry", wrapper_f);
	Builder->SetInsertPoint(BB);
	llvm::Value* control_block = wrapper_f->getArg(0);
	if (last_thread_constructor_caller) {
		auto last_thrconstr_proto = (*lex.findProtos(last_thread_constructor_caller))[0].get();
		auto last_caller = getFunction(last_thrconstr_proto);
		Builder->CreateCall(last_thrconstr_proto->FT, last_caller,
		                    std::vector<llvm::Value*>());
	}
	std::vector<llvm::Value*> args;
	args.reserve(args_type->getNumElements());
	llvm::Value* SretAlloc = nullptr;
	llvm::Value* ret_adr = nullptr;
	if (ret_sz) {
		// address for result of real function
		ret_adr = Builder->CreateStructGEP(args_type, control_block, arg_offs0);
		if (do_sret)
			args.push_back(ret_adr);
	}
	// errs() << Loc << ": ### args " << arg_offs << " "  << arg_offs0 << " "  << n_args << "\n";
	for (unsigned i=arg_offs; i < (arg_offs + n_args); i++) {
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
	// errs() << " ### " << args.size() << " call args\n";
	if (auto F = llvm::dyn_cast<llvm::Function>(theFunction))
		res = Builder->CreateCall(FT, F, args);
	else
		abort();
	if (ret_sz && !do_sret)
		Builder->CreateStore(res, ret_adr);
	if (last_thread_destructor_caller) {
		auto last_thrdestr_proto = (*lex.findProtos(last_thread_destructor_caller))[0].get();
		auto last_caller = getFunction(last_thrdestr_proto);
		Builder->CreateCall(last_thrdestr_proto->FT, last_caller,
		                    std::vector<llvm::Value*>());
	}
	if (have_target) {
		std::function<llvm::Value*(llvm::Value*)> Destructor = nullptr;
		std::function<llvm::Value*(llvm::Value*)> Keeper = nullptr;
		if (use_eventfd || use_pipe) {
			Keeper = [=,this](llvm::Value* ptr) {
				auto Signal_val = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), 1, false);
				auto GV = new llvm::GlobalVariable(*TheModule, Signal_val->getType(), true,
				                                   llvm::GlobalValue::PrivateLinkage, Signal_val);
				GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
				GV->setAlignment(llvm::Align(8));
				llvm::Value* fd_ptr = Builder->CreateStructGEP(args_type, ptr, use_pipe ? 2 : 1);
				llvm::Value* fd = Builder->CreateLoad(llvm_int_type, fd_ptr);
				auto write_proto = (*lex.findProtos("__write"))[0].get();
				auto write_fn = getFunction(write_proto);
				return Builder->CreateCall(write_proto->FT, write_fn, std::vector<llvm::Value*>{
						fd, GV, getSize(8) });
			};
			Destructor = [=,this](llvm::Value* ptr) {
				auto close_proto = (*lex.findProtos("__close"))[0].get();
				auto close_fn = getFunction(close_proto);
				llvm::Value* fd_ptr = Builder->CreateStructGEP(args_type, ptr, 1);
				llvm::Value* fd = Builder->CreateLoad(llvm_int_type, fd_ptr);
				auto res = Builder->CreateCall(close_proto->FT, close_fn, std::vector<llvm::Value*>{ fd });
				if (use_pipe) {
					llvm::Value* fd2_ptr = Builder->CreateStructGEP(args_type, ptr, 2);
					llvm::Value* fd2 = Builder->CreateLoad(llvm_int_type, fd_ptr);
					auto res2 =Builder->CreateCall(close_proto->FT, close_fn, std::vector<llvm::Value*>{ fd2 });
					if (!res2)
						res = nullptr;
				}
				return res;
			};
		}
		if (!CreateReleaseRefC(control_block, Destructor, Keeper))
			return nullptr;
	}
	if (os_idx == OS_Windows)
		Builder->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0));
	else
		Builder->CreateRet(llvm::ConstantPointerNull::get(llvm_ptr_type));
	bool fin = finishFunctionOrModule(wrapper_f, 1, finish_module, finish_module);
	if (finish_module) {
		TheModule = std::move(savedModule);
		wrapper_f = llvm::Function::Create(wrapper_fn_t, llvm::Function::ExternalLinkage, wrapper_name, TheModule.get());
	}
	Builder->SetInsertPoint(savedInsertPoint);
	if (fin)
		return wrapper_f;
	else
		return nullptr;
}

llvm::Value* ThreadExprAST::codegen_raw(llvm::Value* target) {
	ret_typ = Call->Proto->RetType->type;
	ret_sz = ret_typ->isVoidTy() ? 0 : TheModule->getDataLayout().getTypeAllocSize(ret_typ);
	do_sret = (ret_sz > sret_limit);
	n_args = Call->Proto->LLVMArgTypes.size(); // including sret_pointer as 1st arg
	// errs() << Loc << ": ### ret_sz: " << ret_sz << " " << do_sret << "!\n";
	arg_offs = 1; // reference counter
	// !target means the handle is discarded, i.e. the thread is detached
	if (target && os_idx != OS_Windows) {
		// We want to 'poll()' the event of a finished thread together with other events.
		// On Windows we can use 'WaitForMultipleObjects()'
		// On POSIX systems we add an additional file descriptor which is an
		// On Linux, FreeBSD and NetBSD we add and additional 'eventfd' file descriptor
		// On other systems we use a 'pipe' (2 file descriptors - less efficient)
		if (os_idx == OS_FreeBSD || os_idx == OS_Linux || os_idx == OS_NetBSD) {
			use_eventfd = true;
			arg_offs += 1;
		} else {
			use_pipe = true;
			arg_offs += 2;
		}
	}
	arg_offs0 = arg_offs; // up to here all elements are Int32
	bool have_ret_val = (bool)ret_sz;
	if (have_ret_val)
		arg_offs += 1;
	std::vector<llvm::Type*> types;
	std::vector<llvm::Value*> args;
	types.reserve(arg_offs + n_args); // ref-counter + eventfd + return
	args.reserve(arg_offs + n_args);
	for (int m = 0; m < arg_offs0; m++)
		types.push_back(llvm_int_type);
	if (have_ret_val)
		types.push_back(ret_typ);
	for (auto t: Call->Proto->LLVMArgTypes)
		types.push_back(t);
	// TODO: handle vararg
	args_type = llvm::StructType::get(Context, types);
	// errs() << "### stuct: " << *args_type << "\n";
	llvm::Value* AllocSz = getSize(TheModule->getDataLayout().getTypeAllocSize(args_type));
#if LLVM_VERSION_MAJOR >= 18
	llvm::Value* Malloc = Builder->CreateMalloc(
		llvm_size_type, llvm::Type::getInt8Ty(Context),
		AllocSz, nullptr, nullptr, "threadcontext");
#else
	llvm::Value* Malloc = llvm::CallInst::CreateMalloc(
		Builder->GetInsertBlock(),
		llvm_size_type, llvm::Type::getInt8Ty(Context),
		AllocSz, nullptr, nullptr, "threadcontext");
	Malloc = Builder->Insert(Malloc);
#endif
	refcount_adr = Builder->CreateStructGEP(args_type, Malloc, 0);
	 // reference counter contains number of references - 1
	CreateAtomicStore(Builder->getInt32(target ? 1 : 0), refcount_adr);
	if (use_eventfd || use_pipe) {
		eventfd_or_piperead_adr = Builder->CreateStructGEP(args_type, Malloc, 1);
		llvm::Value* fd_res;
		// we do not cross compile so get flags from host system ...
		llvm::Value* fd_flags = Builder->getInt32(O_CLOEXEC | O_NONBLOCK);
		if (use_pipe) {
			pipewrite_adr = Builder->CreateStructGEP(args_type, Malloc, 2);
			auto pipe2_proto = (*lex.findProtos("__pipe2"))[0].get();
			auto pipe2_fn = getFunction(pipe2_proto);
			fd_res = Builder->CreateCall(
				pipe2_proto->FT, pipe2_fn, std::vector<llvm::Value*>{
					eventfd_or_piperead_adr, fd_flags });
		} else {
			auto eventfd_proto = (*lex.findProtos("__eventfd"))[0].get();
			auto eventfd_fn = getFunction(eventfd_proto);
			// again, we do not cross compile ...
			constexpr int _flags = O_CLOEXEC | O_NONBLOCK;
			fd_res = Builder->CreateCall(
				eventfd_proto->FT, eventfd_fn, std::vector<llvm::Value*>{
					Builder->getInt32(0), fd_flags });
		}
		auto abort_proto = (*lex.findProtos("_abort_if_negative"))[0].get();
		auto abort_fn = getFunction(abort_proto);
		Builder->CreateCall(abort_proto->FT, abort_fn, std::vector<llvm::Value*>{ fd_res });
	}
	unsigned j = arg_offs0; // j is control block struct elements counter
	if (have_ret_val)
		j++; // skip return value
	unsigned i = 0; // i is call argument counter
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
		llvm::Value* Adr = Builder->CreateStructGEP(args_type, Malloc, j, Call->Proto->Args[i]);
		Builder->CreateStore(arg_val, Adr);
		i++;
		j++;
	}
	llvm::Function* wrapper = get_thread_wrapper((bool)target);
	if (!wrapper)
		return nullptr;
	auto thread_create_proto = (*lex.findProtos("__create_thread"))[0].get();
	auto thread_create_fn = getFunction(thread_create_proto);
	llvm::Value* thr_id = Builder->CreateCall(
		thread_create_proto->FT, thread_create_fn,
		std::vector<llvm::Value*>{ wrapper, Malloc, llvm::ConstantInt::get(llvm::Type::getInt1Ty(Context), 0) });
	llvm::Value* thread_handle = llvm::UndefValue::get(ft->type);
	thread_handle = Builder->CreateInsertValue(thread_handle, Malloc, 0);
	thread_handle = Builder->CreateInsertValue(thread_handle, thr_id, 1);
	return thread_handle;
}

/* CreateReleaseRefC() gets a pointer to an atomic reference pointer that is the 1st
 * element in a memory block. If reference counter is decremented and if it has been zero
 * the optional ValDestructor() function is called and the memory block is freed.
 * Otherwise the optional ValKeeper() function is called.
 */
llvm::Value* CreateReleaseRefC(llvm::Value* ptr, std::function<llvm::Value*(llvm::Value*)> ValDestructor,
                               std::function<llvm::Value*(llvm::Value*)> ValKeeper) {
	llvm::Value* last_val = CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Sub, ptr, llvm::ConstantInt::get(llvm_int_type, 1));
	llvm::Value* was_zero = Builder->CreateICmpEQ(last_val, llvm::ConstantInt::get(llvm_int_type, 0));
	auto enterBB = Builder->GetInsertBlock();
	auto TheFunction = enterBB ? enterBB->getParent() : nullptr;
	if (!TheFunction) {
		errs() << "*** internal error: no function\n";
		abort();
	}
	auto freeBB = llvm::BasicBlock::Create(Context, "free");
	auto keepBB = llvm::BasicBlock::Create(Context, "keep");
	auto contBB = llvm::BasicBlock::Create(Context, "cont");
	Builder->CreateCondBr(was_zero, freeBB, keepBB);
#if LLVM_VERSION_MAJOR >= 16
	TheFunction->insert(TheFunction->end(), freeBB);
#else
	TheFunction->getBasicBlockList().push_back(freeBB);
#endif
	Builder->SetInsertPoint(freeBB);
	if (ValDestructor)
		if (!ValDestructor(ptr))
			return nullptr;
#if LLVM_VERSION_MAJOR >= 18
	Builder->CreateFree(ptr);
#else
	Builder->Insert(llvm::CallInst::CreateFree(ptr, freeBB));
#endif
	Builder->CreateBr(contBB);
#if LLVM_VERSION_MAJOR >= 16
	TheFunction->insert(TheFunction->end(), keepBB);
#else
	TheFunction->getBasicBlockList().push_back(keepBB);
#endif
	Builder->SetInsertPoint(keepBB);
	if (ValKeeper)
		if (!ValKeeper(ptr))
			return nullptr;
	Builder->CreateBr(contBB);
#if LLVM_VERSION_MAJOR >= 16
	TheFunction->insert(TheFunction->end(), contBB);
#else
	TheFunction->getBasicBlockList().push_back(contBB);
#endif
	Builder->SetInsertPoint(contBB);
	return was_zero;
}

llvm::Value* SelectExprAST::codegen_thread_wait(llvm::Value* control_block, llvm::Value* thread_handle) {
	auto thread_join_proto = (*lex.findProtos("__join_thread"))[0].get();
	auto thread_join_fn = getFunction(thread_join_proto);
	Builder->CreateCall(
		thread_join_proto->FT, thread_join_fn,
		std::vector<llvm::Value*>{ thread_handle, control_block });
	unsigned offs = 1;
	if (os_idx != OS_Windows) {
		offs++;
		if (os_idx != OS_FreeBSD && os_idx != OS_Linux && os_idx != OS_NetBSD)
			offs++;
	}
	std::vector<llvm::Type*> types;
	types.reserve(offs+1);
	for (unsigned i=0; i<offs; i++)
		types.push_back(llvm_int_type);
	types.push_back(ft->type);
	llvm::Type* args_type = llvm::StructType::get(Context, types);
	llvm::Value* ret_adr = Builder->CreateStructGEP(args_type, control_block, offs);
	return Builder->CreateLoad(ft->type, ret_adr);
}

llvm::Value* SelectExprAST::codegen_thread_kill(llvm::Value* control_block, llvm::Value* thread_handle) {

	return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
}

