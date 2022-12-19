/*
 * Copyright © Uwe Krüger 2021, 2022
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

// some names for environment variables

#define PROMPT_COL "VOLVOX_COLORS"

CompModes comp_mode = comp_undefined;
LinkModes link_mode = link_undefined;
std::vector<std::string> include_files = {};
std::vector<std::vector<std::string>> source_files = {{}};
std::vector<std::unique_ptr<ExprAST>> GlobalExprList = {};
const std::string single_test_result_name = "__test_result";
const std::string collector_name = "__test_results_collect";
int include_index = 0;
std::vector<int> source_index = { 0 };
int prompt_indent = 0;
std::map<std::string, Module> Modules;
DebugInfo KSDbgInfo;
const char* last_defined_type = nullptr;
bool needs_libm = false;
bool support_fp80;
unsigned target_bytes; // size_t, pointer size in bytes
unsigned target_bits; // in bits

#if defined(_MSC_VER)
// some tokens from library have GNU/Itanium style mangling - so compensate
#define volvox_glob _ZN6volvox4globEPKc
#define volvox_glob2 _ZN6volvox4globEPKcS1_
#define volvox_free_glob _ZN6volvox9free_globEP13volvox_glob_t
#define volvox_spawn _ZN6volvox5spawnEPiS0_S0_S0_PKPc
#define volvox_wait _ZN6volvox4waitEi
#define volvox_try_wait _ZN6volvox8try_waitEi
extern "C" volvox_glob_t volvox_glob(const char* pattern);
extern "C" volvox_glob_t volvox_glob2(const char* patbase, const char* pattail);
extern "C" void volvox_free_glob(volvox_glob_t* rets);
extern "C" bool volvox_spawn(int* pid, int* child_stdin, int* child_stdout,
                             int* child_stderr, char* const argv[]);
extern "C" int volvox_wait(int pid);
extern "C" int volvox_try_wait(int pid);
#else
#define volvox_glob volvox::glob
#define volvox_glob2 volvox::glob
#define volvox_free_glob volvox::free_glob
#define volvox_spawn volvox::spawn
#define volvox_wait volvox::wait
#endif

//===----------------------------------------------------------------------===//
// Code Generation Globals
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::orc::ThreadSafeContext> TS_Context = nullptr;
std::unique_ptr<llvm::Module> TheModule = nullptr;
std::unique_ptr<llvm::IRBuilder<>> Builder = nullptr;
std::unique_ptr<llvm::MDBuilder> MDBuilder = nullptr;
std::unique_ptr<llvm::DIBuilder> DBuilder = nullptr;
llvm::ExitOnError ExitOnErr;

// useful definitions - "Context-time" constants
llvm::Type* llvm_int_type;
llvm::Type* llvm_size_type;
llvm::Type* llvm_bool_type;
volvoxc::FullType* void_type;
volvoxc::FullType* bool_type;
volvoxc::FullType* char_type;
volvoxc::FullType* size_type;

#ifdef LEGACY_PASS_MANAGER
std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM = nullptr;
#else
// Create the analysis managers.
llvm::LoopAnalysisManager LAM;
llvm::FunctionAnalysisManager FAM;
llvm::CGSCCAnalysisManager CGAM;
llvm::ModuleAnalysisManager MAM;
llvm::PipelineTuningOptions PTO;
#endif
llvm::TargetMachine* TheTargetMachine = nullptr;

std::unique_ptr<llvm::orc::VolvoxJIT> TheJIT = nullptr;

llvm::raw_ostream &indent(llvm::raw_ostream &O, int size) {
	return O << std::string(size, ' ');
}
std::vector<VarTable> locals_table; // including function arguments

//===----------------------------------------------------------------------===//
// Built-in Types
//===----------------------------------------------------------------------===//

unsigned stringkey;

void init(const llvm::Triple& triple) {
	init_token_map();
	// only for internal use:
	lex.add_type("i*", llvm::Type::getInt64Ty(Context), nullptr, A_signed);
	lex.add_type("f*", llvm::Type::getDoubleTy(Context), nullptr);

	if (target_bits == 16) {
		lex.add_type("int", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("int", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
		lex.add_type("uint", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("uint", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
		llvm_int_type = llvm::Type::getInt16Ty(Context);
		lex.add_type("ssize_t", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("ssize_t", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
		lex.add_type("size_t", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("size_t", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
		llvm_size_type = llvm::Type::getInt16Ty(Context);
		lex.add_type("real", llvm::Type::getFloatTy(Context), DBuilder ? DBuilder->createBasicType("real", 32, llvm::dwarf::DW_ATE_float) : nullptr);
	} else {
		lex.add_type("int", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
		lex.add_type("uint", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("uint", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
		llvm_int_type = llvm::Type::getInt32Ty(Context);
		lex.add_type("real", llvm::Type::getDoubleTy(Context), DBuilder ? DBuilder->createBasicType("real", 64, llvm::dwarf::DW_ATE_float) : nullptr);
	}
	if (target_bits == 32) {
		lex.add_type("ssize_t", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("ssize_t", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
		lex.add_type("size_t", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("size_t", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
		llvm_size_type = llvm::Type::getInt32Ty(Context);
	} else if (target_bits == 64) {
		lex.add_type("ssize_t", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("ssize_t", 64, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
		lex.add_type("size_t", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("size_t", 64, llvm::dwarf::DW_ATE_unsigned) : nullptr);
		llvm_size_type = llvm::Type::getInt64Ty(Context);
	}
	size_type = lex.get_full_type("size_t");
	void_type = new_FullType(llvm::Type::getVoidTy(Context), 0);
	llvm_bool_type = llvm::Type::getInt1Ty(Context);
	lex.add_type("bool", llvm::Type::getInt1Ty(Context), DBuilder ? DBuilder->createBasicType("bool", 1, llvm::dwarf::DW_ATE_boolean) : nullptr);
	bool_type = lex.get_full_type("bool");
	lex.add_type("i8", llvm::Type::getInt8Ty(Context), DBuilder ? DBuilder->createBasicType("i8", 8, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	lex.add_type("i16", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("i16", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	lex.add_type("i32", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("i32", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	lex.add_type("i64", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("i64", 64, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	lex.add_type("u8", llvm::Type::getInt8Ty(Context), DBuilder ? DBuilder->createBasicType("u8", 8, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	char_type = lex.get_full_type("u8");
	lex.add_type("u16", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("u16", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	lex.add_type("u32", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("u32", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	lex.add_type("u64", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("u64", 64, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	lex.add_type("f16", llvm::Type::getBFloatTy(Context), DBuilder ? DBuilder->createBasicType("f16", 16, llvm::dwarf::DW_ATE_float) : nullptr);
	lex.add_type("f32", llvm::Type::getFloatTy(Context), DBuilder ? DBuilder->createBasicType("f32", 32, llvm::dwarf::DW_ATE_float) : nullptr);
	lex.add_type("f64", llvm::Type::getDoubleTy(Context), DBuilder ? DBuilder->createBasicType("f64", 64, llvm::dwarf::DW_ATE_float) : nullptr);
	lex.add_type("string", llvm::Type::getInt8PtrTy(Context),
	             DBuilder ? DBuilder->createPointerType(DBuilder->createBasicType("i8", 8, llvm::dwarf::DW_ATE_signed_char), 64, 0, llvm::None, "string") : nullptr, A_string);
	MDBuilder = std::make_unique<llvm::MDBuilder>(Context);
	// create build in constexprs to describe target
	OS_Type_t os_idx;
	switch (triple.getOS()) {
	case llvm::Triple::DragonFly:
		os_idx = OS_DragonFlyBSD;
		break;
	case llvm::Triple::FreeBSD:
		os_idx = OS_FreeBSD;
		break;
	case llvm::Triple::Linux:
		os_idx = OS_Linux;
		break;
	case llvm::Triple::MacOSX:
		os_idx = OS_MacOSX;
		break;
	case llvm::Triple::NetBSD:
		os_idx = OS_NetBSD;
		break;
	case llvm::Triple::OpenBSD:
		os_idx = OS_OpenBSD;
		break;
	case llvm::Triple::Win32:
		os_idx = OS_Windows;
		break;
	default:
		os_idx = OS_UnknownOS;
	}
	CPU_Type_t cpu_idx;
	switch (triple.getArch()) {
	case llvm::Triple::arm:
		cpu_idx = CPU_arm;
		break;
	case llvm::Triple::aarch64:
		cpu_idx = CPU_aarch64;
		break;
	case llvm::Triple::avr:
		cpu_idx = CPU_avr;
		break;
	case llvm::Triple::x86:
		cpu_idx = CPU_x86;
		break;
	case llvm::Triple::x86_64:
		cpu_idx = CPU_x86_64;
		break;
	default:
		cpu_idx = CPU_Unknown;
	}
	FullVar os_fv = {
		.val = llvm::ConstantInt::get(llvm::Type::getInt8Ty(Context), os_idx),
		.ft = {
			.type = llvm::Type::getInt8Ty(Context),
			.type_attr = A_rvalue | A_const | A_global,
		}
	};
	if (!lex.module->globals_table.insert("__OS_Idx", os_fv)) {
		errs() << "cannot create const " << "__OS_Idx" << '\n';
		abort();
	}
	FullVar cpu_fv = {
		.val = llvm::ConstantInt::get(llvm::Type::getInt8Ty(Context), cpu_idx),
		.ft = {
			.type = llvm::Type::getInt8Ty(Context),
			.type_attr = A_rvalue | A_const | A_global,
		}
	};
	if (!lex.module->globals_table.insert("__CPU_Idx", cpu_fv)) {
		errs() << "cannot create const " << "__CPU_Idx" << '\n';
		abort();
	}
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

void InitializeModuleAndPassManager() {
	// Open a new module.
	TheModule = std::make_unique<llvm::Module>(lex.Loc.File, Context);
	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		TheModule->setDataLayout(TheJIT->getDataLayout());
	}
	static bool already_run = false;
	// Create a new builder for the module.
	if (!already_run) {
		Builder = std::make_unique<llvm::IRBuilder<>>(Context);
		already_run = true;
	}
#ifdef LEGACY_PASS_MANAGER
	// Create a new pass manager attached to it.
	TheFPM = std::make_unique<llvm::legacy::FunctionPassManager>(TheModule.get());

	// Promote allocas to registers.
	TheFPM->add(llvm::createPromoteMemoryToRegisterPass());
	// Do simple "peephole" optimizations and bit-twiddling optzns.
	TheFPM->add(llvm::createInstructionCombiningPass());
	// Reassociate expressions.
	TheFPM->add(llvm::createReassociatePass());
	// Eliminate Common SubExpressions.
	TheFPM->add(llvm::createGVNPass());
	// Simplify the control flow graph (deleting unreachable blocks, etc).
	TheFPM->add(llvm::createCFGSimplificationPass());

	TheFPM->doInitialization();
#endif
}

static void HandleDefinition(unsigned& visibility) {
	inside_function = true;
	condnesting = 0;
	IfWhileVarTable = nullptr;
	locals_table.push_back(VarTable());
	bool success = false;
	if (auto FnAST = ParseDefinition(visibility)) {
		if (auto *FnIR = FnAST->codegen()) {
			goto cleanup;
		} else {
			errs() << "Error compiling function definition\n";
		}
	} else {
		errs() << "Error parsing function definition\n";
	}
	// Skip remaining tokens for error recovery.
	purgeLine();
	TestFunction = nullptr;
cleanup:
	locals_table[0].clear();
	locals_table = std::move(std::vector<VarTable>{});
	inside_function = false;
}

static void HandleExtern(unsigned visibility) {
	getNextToken();
	switch (CurTok.kind) {
	case tok_fn:
		if (auto ProtoAST = ParseExtern(visibility)) {
			std::string unmangledName = ProtoAST->getName();
			if (auto *FnIR = ProtoAST->codegen()) {
				if (dump_IR) {
					errs() << "Read extern: ";
					FnIR->print(errs());
					errs() << "\n";
				}
				lex.module->FunctionProtos[unmangledName].push_back(std::move(ProtoAST));
			} else {
				errs() << "Error reading extern\n";
			}
		} else {
			// Skip token for error recovery.
			purgeLine();
		}
		break;
	default:
		errs() << "external variables not implemented, yet\n";
		// external variable
	}
}

static void HandleTypeDef(unsigned share_kind) {
	getNextToken(); // eat type
	if (CurTok.kind != tok_identifier) {
		errs() << "unexpected '" << CurTok.str() << "' in type declaration - type name expected\n";
		purgeLine();
		return;
	}
	auto type_name = IdentifierStr;
	std::string volvox_name;
	for (auto& p: lex.module->import_path) {
		volvox_name += p;
		volvox_name += '.';
	}
	volvox_name += type_name;
	volvoxc::FullType Ft = {
		.type = nullptr,
	};
	MapNode* replace = nullptr;
	MapNode* new_node = lex.add_type(type_name.c_str(), &Ft, replace);
	MapValue* val = &new_node->value;
	volvoxc::FullType* ft = (volvoxc::FullType*)((char*)val + val->offset);
	llvm::StructType* struct_type;
	if (replace) { // new_node is actually an old node
		struct_type = llvm::dyn_cast<llvm::StructType>(ft->type);
		if (!struct_type || !struct_type->isOpaque()) {
			errs() << "cannot define '" << type_name << "' - type already exists\n";
			return;
		}
	} else {
		struct_type = llvm::StructType::create(Context, volvox_name);
		ft->type = struct_type;
		llvm::SmallString<128> buf;
		auto mangled_name = MangleBase(buf, lex.module->import_path, type_name);
		ft->mangled_name = strdup(mangled_name.c_str());
	}
	getNextToken(eSemi);
	if (CurTok.kind == ';') {
		if (verbosity >= 2)
			errs() << "declared type " << *ft->type << '\n';
		return; // only declaration of incomplete type
	}
	auto newft = ParseType(false, eComma, 0, volvox_name.c_str(), nullptr, struct_type);
	if (!newft) {
		purgeLine();
		return;
	}
	const char* mangled_name = ft->mangled_name;
	*ft = *newft;
	ft->mangled_name = mangled_name;
	new_FullType(*ft); // to keep a handle to mangled_name after lex.module has gone out of scope
	last_defined_type = new_node->key.string;
	if (verbosity >= 2)
		errs() << "defined type " << *ft << " as " << *ft->type << '\n';
}

static void HandleImport() {
	bool from = CurTok.kind == tok_from;
	std::vector<std::string> new_import_path = {};
	do {
		getNextToken(ePath);
		if (CurTok.kind != tok_identifier) {
			errs() << "unexpected token in import " << CurTok.kind << '\n';
			purgeLine();
			return;
		}
		new_import_path.push_back(IdentifierStr);
		getNextToken(ePath);
	} while (CurTok.kind == tok_selector);
	std::string prefix = "";
	std::map<std::string, SourceLocation> direct_import_list = {};
	if (from) {
		if (!Expect(tok_import, ePath)) {
			purgeLine();
			return;
		}
		for(;;) {
			if (CurTok.kind == tok_star) {
				if (direct_import_list.empty()) {
					prefix = "";
					getNextToken(eBinOp);
					break;
				} else {
					errs() << CurLoc << ": '*' in import list is only allowed as sole element\n";
					purgeLine();
					return;
				}
			}
			if (CurTok.kind != tok_identifier) {
				errs() <<  CurLoc << ": unexpected token in import list '" << CurTok << " - identifier expected\n";
				purgeLine();
				return;
			}
			auto new_element = direct_import_list.try_emplace(IdentifierStr, CurLoc);
			if (!new_element.second) {
				errs() << CurLoc << ": repeated symbol '" << IdentifierStr << "' in import list\n";
				errs() << direct_import_list[IdentifierStr] << ": previous specification of '" << IdentifierStr << "'\n";
			}
			getNextToken(eBinOp);
			if (CurTok.kind != tok_comma)
				break;
			getNextToken(ePath);
		}
	} else if (CurTok.kind == tok_as) {
		getNextToken(ePath);
		if (CurTok.kind != tok_identifier) {
			errs() <<  CurLoc << ": unexpected token in import list '" << CurTok.kind << "' - alias name expected\n";
			purgeLine();
		} else {
			prefix = IdentifierStr;
			getNextToken(ePath);
		}
	} else {
		prefix = new_import_path.back();
	}
	if (CurTok.kind == ';') {
		lex.push_state(std::move(new_import_path), std::move(prefix), std::move(direct_import_list));
		getNextToken();
	} else {
		errs() << "unexpected identifier " << CurTok << "\n";
		purgeLine();
	}
}

// __anon_exp returns bool but thread return values are system dependent
// it's void* on Unix but DWORD, i.e. unsigned on Windows
#if defined (_MSC_VER)
#define THREAD_RETURN DWORD
#else
#define THREAD_RETURN void*
#endif

static THREAD_RETURN anon_expr_wrapper(void* expr_ptr) {
	bool (*expr)() = (bool (*)())expr_ptr;
	return (THREAD_RETURN)(uintptr_t)(expr() ? 1 : 0);
}

#if defined (_MSC_VER)
bool spawn_bool_expr(bool (*expr)()) {
	HANDLE thread = CreateThread(NULL, 0, anon_expr_wrapper, (void*)expr, 0, NULL);
	WaitForSingleObject(thread, INFINITE);
	DWORD retval;
	GetExitCodeThread(thread, &retval);
	return !(!retval);
}
#else
bool spawn_bool_expr(bool (*expr)()) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stacksize);
	pthread_t thread;
	int res = pthread_create(&thread, &attr, anon_expr_wrapper, (void*)expr);
	if (res) {
		errs() << "Error creating execution thread: " << strerror(res) << '\n';
		abort();
	}
	pthread_attr_destroy(&attr);
	void* retval;
	res = pthread_join(thread, &retval);
	return !(!retval);
}
#endif

static bool HandleTopLevelExpression(std::unique_ptr<ExprAST> E, bool suppress_output = false) {
	bool b = false; // result
	// Evaluate a top-level expression into an anonymous function.
	if (auto FnAST = ParseTopLevelExpr(std::move(E), suppress_output)) {
		if (auto anon_expr = FnAST->codegen()) {
			auto ret_type = anon_expr->getReturnType();
			if (!anon_expr->getReturnType()->isIntegerTy() || !(anon_expr->getReturnType()->getIntegerBitWidth() == 1)) {
				errs() << "internal error: anonymous function does not return `bool`\n";
				return false;
			}
			if (comp_mode == comp_jit) {
				// Create a ResourceTracker to track JIT'd memory allocated to our
				// anonymous expression -- that way we can free it after executing.
				auto RT = TheJIT->getMainJITDylib().createResourceTracker();
				auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), *TS_Context.get());
				ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
				InitializeModuleAndPassManager();
				
				// Search the JIT for the __anon_expr symbol.
				auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
				// Get the symbol's address and cast it to the right type (takes no
				// arguments, returns a bool) so we can call it as a native function.
				bool (*BOOL)() = (bool (*)())(intptr_t)ExprSymbol.getAddress();
				b = spawn_bool_expr(BOOL);
				// Delete the anonymous expression module from the JIT.
				ExitOnErr(RT->remove());
			}
		} else {
			errs() << "Error generating code for top level expr\n";
		}
	} else {
		// Skip rest for error recovery.
		purgeLine();
	}
	return b;
}

std::unique_ptr<FunctionAST> CreateMain(const char* main_name, bool have_return = false, const char* ret_type = "i32") {
	volvoxc::FullType* TheType = lex.get_full_type(ret_type);
	auto Proto = std::make_unique<PrototypeAST>(CurLoc, main_name,
	                                            std::vector<std::string>(),
	                                            A_c_api, CurLoc, false, TheType);
	if (!have_return)
		GlobalExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
	auto ProtoRef = Proto.get();
	std::string unmangledName = Proto->getName();
	lex.module->FunctionProtos[unmangledName].push_back(std::move(Proto));
	auto main_function = std::make_unique<FunctionAST>(ProtoRef, std::move(GlobalExprList), tok_return, std::move(unmangledName));
	return main_function;
}

void PrepareTestFramework() {
	// create (global) variables to collect Results
	FullVar fv = {
		.ft = {
			.type = llvm_bool_type,
			.type_attr = A_mainvar | A_global
		}
	};
	if (!lex.module->globals_table.insert(single_test_result_name.c_str(), fv)) {
		errs() << "fatal error" << ": variable '" << single_test_result_name << "' already exists in \"main\" scope\n";
		abort();
	}
	auto single_res_def = std::make_unique<BinaryExprAST>(
		CurLoc, ":=",
		std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)),
		std::move(std::make_unique<LiteralExprAST>(Token(false))));
	HandleGlobalVariable(std::move(single_res_def), A_pub | A_global);
	if (!lex.module->globals_table.insert(collector_name.c_str(), fv)) {
		errs() << "fatal error" << ": variable '" << single_test_result_name << "' already exists in \"main\" scope\n";
		abort();
	}
	auto collector_def = std::make_unique<BinaryExprAST>(
		CurLoc, ":=",
		std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)),
		std::move(std::make_unique<LiteralExprAST>(Token(true))));
	HandleGlobalVariable(std::move(collector_def), A_pub | A_global);
}

void CallTestFunction(bool immediately = false) {
	std::string showres = "showtestres";
	auto show_res_fn = lex.findProtos(showres);
	if (!show_res_fn) {
		errs() << "Cannot find function to display test results: '" << showres << "()'\n";
		return;
	}
	auto F = lex.findProtos(TestFunction);
	if (!F) {
		errs() << "internal error - could not find test function " << TestFunction << '\n';
		return;
	}
	std::vector<std::unique_ptr<ExprAST>> LocalExprList;
	
	std::vector<std::unique_ptr<ExprAST>>& ExprList = immediately ? LocalExprList : GlobalExprList;
	auto call_expr = std::make_unique<CallExprAST>(
		CurLoc, std::make_unique<FunctionExprAST>(CurLoc, TestFunction, F));
	if (immediately) {
		auto b = HandleTopLevelExpression(std::move(call_expr), true);
		char* buf;
		char* volvoxstrTestFunction;
		size_t lalloc;
		cstr2volvoxstr(volvoxstrTestFunction, lalloc, buf, TestFunction);
		showtestres(1, 79, volvoxstrTestFunction, b);
		return;
	}
	GlobalExprList.push_back(
		std::make_unique<BinaryExprAST>(
			CurLoc, "=",
			std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)),
			std::move(call_expr),
			std::tuple<llvm::Type*, bool, bool, OpClass, const char*>{
				llvm::Type::getInt1Ty(Context), false, false, OpAssign, nullptr }));
	std::vector<std::unique_ptr<ExprAST>> Args;
	Args.push_back(std::move(std::make_unique<LiteralExprAST>(Token(1LL))));
	Args.push_back(std::move(std::make_unique<LiteralExprAST>(Token(79LL))));
	Args.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string(TestFunction)))));
	Args.push_back(std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)));
	GlobalExprList.push_back(
		std::move(std::make_unique<CallExprAST>(
			          CurLoc, std::make_unique<FunctionExprAST>(CurLoc, showres, show_res_fn),
			          std::move(Args))));
	GlobalExprList.push_back(
		std::make_unique<BinaryExprAST>(
			CurLoc, "=", std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)),
			std::make_unique<BinaryExprAST>(
				CurLoc, "&",
				std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)),
				std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)),
				std::tuple<llvm::Type*, bool, bool, OpClass, const char*>{
					llvm::Type::getInt1Ty(Context), false, false, OpBitwise, nullptr }),
			std::tuple<llvm::Type*, bool, bool, OpClass, const char*>{
				llvm::Type::getInt1Ty(Context), false, false, OpAssign, nullptr }));
	return;
}

std::unique_ptr<FunctionAST> CreateTestRuns() {
	if (comp_mode == comp_jit) {
		GlobalExprList.push_back(std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)));
		return CreateMain("test_main", true, "bool");
	} else {
		std::vector<std::unique_ptr<ExprAST>> _then;
		_then.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
		std::vector<std::unique_ptr<ExprAST>> _else;
		_else.push_back(std::move(std::make_unique<LiteralExprAST>(Token(1LL))));
		auto if_e = std::make_unique<IfExprAST>(
			CurLoc, std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)),
			std::move(_then), std::move(_else), tok_end, tok_end, std::move(VarTable()), std::move(VarTable()),
			std::tuple<llvm::Type*, bool, bool, OpClass, const char*>{ llvm_int_type, true, false, OpNormal, nullptr });
		if_e->desired_type = llvm_int_type;
		GlobalExprList.push_back(
			std::move(if_e));
		return CreateMain("main", true, "i32");
	}
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
	for (;;) {
		if (last_defined_type)
			finish_constructors_and_destructor();
	startmainloop:
		unsigned sym_kind = 0; // 'pub', 'extern', 'fn', 'cfn', ...
		auto share_tok = TokenKind(0);
		for (;;) {
			unsigned sharebits = 0;
			switch (CurTok.kind) {
			case tok_cpub:
				sym_kind |= A_c_api;
			case tok_pub:
				if (sym_kind & A_pub) {
					errs() << CurLoc << ": at most one of qualifiers " << tok_pub << " or "
					       << tok_cpub << " may be given\n";
					purgeLine();
					goto startmainloop;
				}
				sym_kind |= A_pub;
				sym_kind |= A_mainvar;
				break;
			case tok_inline:
				sym_kind |= A_inline;
				break;
			case tok_global:
				sym_kind |= A_mainvar;
				sym_kind |= A_global;
				break;
			case tok_const:
				sym_kind |= (A_mainvar | A_const);
				break;
			case tok_atomic:
				sharebits = sharebits ? sharebits : A_atomic;
			case tok_shared:
				sharebits = sharebits ? sharebits : A_shared;
			case tok_unique:
				sharebits = sharebits ? sharebits : A_unique;
				if (sym_kind & SHARE_KIND_MASK) {
					errs() << CurLoc << ": at most one of qualifiers " << tok_global << ", "
					       << tok_atomic << ", " << tok_shared << ", " << tok_unique << " or "
					       << tok_const << " may be given\n";
					purgeLine();
					goto startmainloop;
				}
				sym_kind |= sharebits;
				sym_kind |= A_ref; // all share kinds imply reference
				share_tok = TokenKind(CurTok.kind);
				break;
			default:
				goto endqualifiers;
			}
			getNextToken();
		}
	endqualifiers:
		if ((sym_kind & A_global) && (sym_kind & A_const)) {
			errs() << CurLoc << ": 'const' and 'global' are mutually exclusive\n";
			purgeLine();
			goto startmainloop;
		}
		switch ((int)CurTok.kind) {
		case tok_eof:
			return;
		case ';': // ignore top-level semicolons.
			getNextToken();
			goto startmainloop;
		case tok_fn:
			if (share_tok) {
				errs() << CurLoc << "functions cannot be declared as " << share_tok << '\n';
				purgeLine();
				goto startmainloop;
			}
			HandleDefinition(sym_kind);
			if (TestFunction) {
				if (do_test)
					CallTestFunction();
				else if (do_repl_test)
					CallTestFunction(true);
				TestFunction = nullptr;
			}
			goto startmainloop;
		case tok_cdecl:
			sym_kind |= A_c_api;
		case tok_decl:
			if (sym_kind & A_inline) {
				errs() << CurLoc << ": " << CurTok.kind << " cannot be used in combination with " << tok_inline << '\n';
				purgeLine();
			} else
				HandleExtern(sym_kind);
			goto startmainloop;
		case tok_import:
		case tok_from:
			if (last_defined_type)
				finish_constructors_and_destructor();
			HandleImport();
			break;
		case tok_ctype:
			sym_kind |= A_c_api;
		case tok_type:
			if (sym_kind & A_pub)
				errs() << CurLoc << ": 'pub' is not needed for type declarations\n";
			if (last_defined_type)
				finish_constructors_and_destructor();
			HandleTypeDef(sym_kind);
			goto startmainloop;
		default:
			if (last_defined_type)
				finish_constructors_and_destructor();
			if (auto expr = GetTopLevelExpression(sym_kind)) {
				if (comp_mode == comp_jit && !do_test)
					HandleTopLevelExpression(std::move(expr));
				else
					GlobalExprList.push_back(std::move(expr));
			}
		}
	}
}

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
	fputc((char)X, stderr);
	return 0;
}

/// printadr
extern "C" DLLEXPORT void printadr(double* X) {
	fprintf(stderr, "Adr: %p %g\n", X, *X);
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

const char* builtin_file_name = "builtin.vx";
int builtin_input_fd = -1;
bool dump_opt = true;
bool dump_raw = false;
uint64_t stacksize = 10485760; // 10MB as safe fallback
// Windows has no CLOEXEC
#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

static void usage(const char* prog) {
	errs() << "Usage: " << prog << " {-[h|v|d|D|c|g|r|j|J|t] }{-[f|O|i|o|s|C][ ]<arg> }{file}\n";
	errs() << " -h ........... print this help screen\n";
	errs() << " -v ........... verbose output (may be repeated for even more verbosity)\n";
	errs() << " -d ........... dump generated LLVM IR-code (repeat to dump more code)\n";
	errs() << " -D ........... dump raw IR in addition to optimized IR (repeat to dump only raw)\n";
	errs() << " -c ........... compile to optimized object file\n";
	errs() << " -fPIC ........ generate position independent code\n";
	errs() << " -fdiv-floored  signed division is floored, remainder gets sign of divisor\n"; 
	errs() << " -fdiv-c99 .... signed division rounds towards 0, remainder gets sign of divident\n"; 
	errs() << " -g ........... compile with debug information\n";
	errs() << " -On[m] ....... optimize with level n (0-3, 's' or 'z'; default: -O2)\n";
	errs() << "                m: optional separate level machine specific codegen (default: n)\n";
	errs() << " -r ........... run compiled program\n";
	errs() << " -j ........... use JIT to run file(s)\n";
	errs() << " -J ........... use JIT to run file(s) and start interactive session\n";
	errs() << " -i file ...... include \"file\" in advance\n";
	errs() << " -o file ...... output compiled result to \"file\"\n";
	errs() << " -s size ...... stack size for .exe(Windows)/new threads (suffix kB, MB, GB)\n";
	errs() << "                default: `ulimit -s` if finite or 10MB otherwise\n";
	errs() << " -m<target> ... platform target option, e.g. '-mingw' or '-msvc' on Windows\n";
	errs() << " -t ........... compile/run all \"fn test_*() bool\" functions from given file(s)\n";
	errs() << " -C n,g,b ..... prompt colors (#, >, background; ANSI-256, default: 30,100,236)\n";
	errs() << " file ......... file(s) to compile (default: interactive session is started)\n";
	exit(1);
}

static void compile_mode_conflict(const char* prog) {
	errs() << "The flags '-c' or '-g' cannot be given for a JIT session (flag '-j')!\n";
	usage(prog);
}

static void debug_mode_conflict(const char* prog) {
	errs() << "The flags '-d' cannot be given for debug output (flag '-g')!\n";
	usage(prog);
}

int verbosity = 0;
unsigned dump_IR = 0;
bool do_test = false;
bool do_repl_test = false;
bool jit_repl = false;
bool gen_pic = false;
bool run_program = false;
promptcolor_t p_col = { 30, 100, 236 };
const char* TestFunction = nullptr;
char* output_file = nullptr;
char* exe_file = nullptr;
#ifndef LEGACY_PASS_MANAGER
llvm::OptimizationLevel optimization_level = llvm::OptimizationLevel::O2;
llvm::PassBuilder PB;
#endif

bool parse_pcol(char* s) {
	uint8_t new_col[3] = { p_col.number, p_col.greater, p_col.background };
	for (int i=0; i<3; i++) {
		errno = 0;
		long long col = strtoll(s, &s, 0);
		if (errno == EINVAL || errno == ERANGE)
			return false;
		if (col < 0 || col >= 0x100) {
			errno = ERANGE;
			return false;
		}
		new_col[i] = (uint8_t)col;
		if (*s)
			if (*s != ',' && *s != ' ') {
				errno = EINVAL;
				return false;
			}
			else
				s++;
		else
			break;
	}
	if (*s) {
		errno = EINVAL;
		return false;
	}
	p_col.number = new_col[0];
	p_col.greater = new_col[1];
	p_col.background = new_col[2];
	return true;
}

// useful tools to clasify filenames

inline bool is_obj(const char* file) {
	int l = strlen(file);
#if defined (_MSC_VER)
	return file[l-4] == '.' && (file[l-3] == 'o' || file[l-3] == 'O')
		&& (file[l-2] == 'b' || file[l-2] == 'B') && (file[l-1] == 'j' || file[l-1] == 'J');
#else
	return file[l-2] == '.' && file[l-1] == 'o';
#endif
}

inline bool is_exe(const char* file) {
	int l = strlen(file);
#if defined (_MSC_VER)
	return file[l-4] == '.' && (file[l-3] == 'e' || file[l-3] == 'E')
		&& (file[l-2] == 'x' || file[l-2] == 'X') && (file[l-1] == 'e' || file[l-1] == 'E');
#else
	// on Unix systems executables can have any name - but we don't have to cut off the extension
	return false;
#endif
}

#ifdef _WIN32
// We have to switch to code page 65001 to enable UTF-8. This
// value here is used to restore the old state on exit
unsigned old_cp;
unsigned old_input_cp;
#endif
#if defined (_MSC_VER)
// glob patterns to search for linker and libraries
#ifndef LINKER
#define LINKER "C:\\Program Files*\\Microsoft Visual Studio\\*\\*\\VC\\Tools\\MSVC\\*\\bin\\Hostx64\\x64\\link.exe"
#endif
// for mingw-w64 ('-mingw' flag) we use clang
#ifndef MINGW_W64_LINKER
#define MINGW_W64_LINKER "C:\\Program Files\\LLVM\\bin\\clang.exe"
#endif
// patterns for library directories - file name is added to skip stale empty directories
#define LIBDIRS { \
	"C:\\Program Files*\\Microsoft Visual Studio\\*\\*\\VC\\Tools\\MSVC\\*\\lib\\x64\\libcmt.lib", \
	"C:\\Program Files*\\Windows Kits\\*\\Lib\\*\\ucrt\\x64\\libucrt.lib", \
	"C:\\Program Files*\\Windows Kits\\*\\Lib\\*\\um\\x64\\ntdll.lib" }
#else
#ifndef MINGW_W64_LINKER
#define MINGW_W64_LINKER "x86_64-w64-mingw32-gcc"
#endif
#endif

int main(int argc, char* argv[]) {
#if defined (_WIN32)
	old_cp = GetConsoleOutputCP();
	old_input_cp = GetConsoleCP();
	SetConsoleCP(CP_UTF8);
	SetConsoleOutputCP(CP_UTF8);
#endif
	setlocale(LC_ALL, "en_US.UTF-8");
	outs().SetUnbuffered();
	errs().SetUnbuffered();
	llvm::CodeGenOpt::Level codegenopt = llvm::CodeGenOpt::Default;
#ifndef _WIN32
	struct rlimit rlimit_stacksize;
	// try to get default stack size for new threads from rlimits
	if (getrlimit(RLIMIT_STACK, &rlimit_stacksize))
		errs() << llvm::format("Cannot get rlimit stacksize: %s\n", strerror(errno));
	else
		if (rlimit_stacksize.rlim_cur != RLIM_INFINITY)
			stacksize = rlimit_stacksize.rlim_cur;
	// otherwise keep the default of 10MB - may be overriden by '-s stacksize' below
#endif
	if (char* cols = getenv(PROMPT_COL))
		if (!parse_pcol(cols))
			errs() << llvm::format("Problem processing environment variables: %s\n", strerror(errno))
			       << '"' << cols << "\" is not a valid value for " << PROMPT_COL << '\n';
	int opt;
	char* endptr;
	bool target_mingw = false;
	while ((opt = getopt(argc, argv, "vdDcghrjJm:f:O:i:o:s:tP:")) != -1) {
		switch (opt) {
		case 'v':
			verbosity++;;
			break;
		case 'd':
			if (comp_mode == comp_dbg)
				debug_mode_conflict(argv[0]);
			dump_IR++;
			break;
		case 'D':
			if (dump_raw)
				dump_opt = false;
			else
				dump_raw = true;
			break;
		case 'c':
			if (comp_mode == comp_jit)
				compile_mode_conflict(argv[0]);
			link_mode = dont_link;
			if (!comp_mode)
				comp_mode = comp_obj;
			break;
		case 'g':
			if (dump_IR)
				debug_mode_conflict(argv[0]);
			if (comp_mode == comp_jit)
				compile_mode_conflict(argv[0]);
			comp_mode = comp_dbg;
#ifndef LEGACY_PASS_MANAGER
			optimization_level = llvm::OptimizationLevel::O0;
#endif
			codegenopt = llvm::CodeGenOpt::None;
			break;
		case 'h':
		case'?':
			usage(argv[0]);
		case 'r':
			run_program = true;
			break;
		case 'J':
			jit_repl = true;
			// fallthrough
		case 'j':
			if (comp_mode && comp_mode != comp_jit)
				compile_mode_conflict(argv[0]);
			comp_mode = comp_jit;
			break;
		case 'f':
			if (!strcmp(optarg, "PIC"))
				gen_pic = true;
			else if (!strcmp(optarg, "div-floored")) {
				if (idiv_mode == idiv_mode_c99) {
					errs() << "-f" << optarg << " and -f" << "div-c99" << " are mutually exclusive\n";
					usage(argv[0]);
				}
				idiv_mode = idiv_mode_floored;
			} else if (!strcmp(optarg, "div-c99")) {
				if (idiv_mode == idiv_mode_floored) {
					errs() << "-f" << optarg << " and -f" << "floored-div" << " are mutually exclusive\n";
					usage(argv[0]);
				}
				idiv_mode = idiv_mode_c99;
			} else {
				errs() << "Unknown option '-f" << optarg << "'\n";
				usage(argv[0]);
			}
			break;
		case 'i':
			include_files.push_back(optarg);
		case 'o':
			if (output_file) {
				errs() << "at most one output filename may be specified\n";
				usage(argv[0]);
			}
			output_file = optarg;
			break;
		case 'm':
			if (!strcmp(optarg, "ingw") || !strcmp(optarg, "ingw-w64"))
				target_mingw = true;
			else if (!strcmp(optarg, "svc"))
				target_mingw = false;
			else {
				errs() << "unknown target option '-m" << optarg << "'\n";
				usage(argv[0]);
			}
			break;
		case 's':
			errno = 0;
			stacksize = strtoull(optarg, &endptr, 0);
			if (!errno) {
				// allowed range is very system dependent - but with a signed overflow it's definitely exceeded
				if ((long long)stacksize <= 0) {
					goto stackeinvalerr;
				}
				switch (*endptr) {
				case 'G':
					stacksize *= 1024;
				case 'M':
					stacksize *= 1024;
				case 'k':
					stacksize *= 1024;
					endptr++;
					break;
				case 'B':
					break;
				case '\0':
					goto stacksizesuccess;
				default:
					goto stackeinvalerr;
				}
				if (*endptr == 'B')
					endptr++;
				if (!(*endptr))
					goto stacksizesuccess;
				stackeinvalerr:
				errno = EINVAL;
			}
			errs() << llvm::format("'-s': \"%s\": %s\n", optarg, strerror(errno));
			errs() << "you may use units like \"-s 10MB\" \"-s 500kB\" or \"-s 10GB\"\n";
			usage(argv[0]);
		stacksizesuccess:
			break;
		case 'O':
			// we expect either one or two characters after '-O'
#ifndef LEGACY_PASS_MANAGER
			if (!optarg[0] || (optarg[1] && optarg[2]))
				goto optimizationerr;
			switch (optarg[0]) { // optimization level for IR
			case '0':
				optimization_level = llvm::OptimizationLevel::O0;
				break;
			case '1':
				optimization_level = llvm::OptimizationLevel::O1;
				break;
			case 's':
				optimization_level = llvm::OptimizationLevel::Os;
				break;
			case 'z':
				optimization_level = llvm::OptimizationLevel::Oz;
				break;
			case '2':
				optimization_level = llvm::OptimizationLevel::O2;
				break;
			case '3':
				optimization_level = llvm::OptimizationLevel::O3;
				break;
			default:
				goto optimizationerr;
			}
#endif
			// use same optization level for MIR, i.e. codegen unless
			// a second flag is specified in optarg
			switch (optarg[1] ? optarg[1] : optarg[0]) {
			case '0':
				codegenopt = llvm::CodeGenOpt::None;
				break;
			case '1':
				codegenopt = llvm::CodeGenOpt::Less;
				break;
			case 's':
			case 'z':
			case '2':
				codegenopt = llvm::CodeGenOpt::Default;
				break;
			case '3':
				codegenopt = llvm::CodeGenOpt::Aggressive;
				break;
			default:
				goto optimizationerr;
			}
			break;
		optimizationerr:
			errs() << "invalid optimization option: '-O" << optarg << "'\n";
			usage(argv[0]);
		case 't':
			do_test = true;
			break;
		case 'P':
			if (!parse_pcol(optarg)) {
				errs() << llvm::format("Invalid value for prompt colors - \"%s\": %s\n", optarg, strerror(errno));
				usage(argv[0]);
			}
			break;
		default:
			usage(argv[0]);
		}
	}
	if (verbosity >= 2) {
		errs() << "Path of Volvox Binary: >" << getThisExePath() << "<\n";
		errs() << "Lib: >" << volvox_lib() << "<\n";
		errs() << "Volvox Root: >" << volvox_root() << "<\n";
	}
	for (;optind < argc; optind++)
		source_files.front().push_back(argv[optind]);
	if (!comp_mode) {
		if (source_files.front().size())
			comp_mode = comp_obj;
		else
			comp_mode = comp_jit;
	}
	if (!link_mode) {
		if (comp_mode == comp_jit)
			link_mode = dont_link;
		else
			link_mode = do_link;
	}
	if (idiv_mode == idiv_mode_undef)
		idiv_mode = idiv_mode_floored; // default to Knuth's suggestion
	if (run_program && link_mode == dont_link) {
		errs() << "Options '-c' and '-r' are mutually exclusive\n";
		usage(argv[0]);
	}
	if (run_program && comp_mode == comp_jit) {
		errs() << "Options '-r' makes no sense in JIT mode\n";
		usage(argv[0]);
	}
	if (jit_repl && do_test) {
		do_test = false;
		do_repl_test = true;
	}
	if (output_file) {
		if (comp_mode == comp_jit) {
			errs() << "output file ('-o ...') not supported for JIT compilation\n";
			usage(argv[0]);
		}
		if (is_obj(output_file)) {
			link_mode = dont_link;
		} else if(is_exe(output_file)) {
			exe_file = output_file;
			output_file = strdup(exe_file);
			int l = strlen(exe_file);
			output_file[l-3] = 'o';
			output_file[l-2] = 'b';
			output_file[l-1] = 'j';
		} else {
			if (link_mode == dont_link) {
				errs() << "Output file must have the extension '.o"
#if defined(_MSC_VER)
				       << "bj"
#endif
				       << "' if '-c' is given\n";
				usage(argv[0]);
			}
			int l = strlen(output_file);
#ifdef _WIN32
			char* new_out = (char*)malloc(l+5);
			exe_file = (char*)malloc(l+5);
			strcpy(new_out, output_file);
			strcpy(exe_file, output_file);
			output_file = new_out;
			strcat(exe_file, ".exe");
#else
			exe_file = output_file;
			output_file = (char*)malloc(l+3);
			strcpy(output_file, exe_file);
#endif
#if defined(_MSC_VER)
			strcat(output_file, ".obj");
#else
			strcat(output_file, ".o");
#endif
		}
	} else {
		if (comp_mode != comp_jit) {
			if (source_files.front().size() != 1) {
				errs() << "output file name (-o ...) required if "
				       << (source_files.front().size() ? "more than one" : "no")
				       << " input file provided\n";
				usage(argv[0]);
			}
			int len = source_files.front().front().size();
			output_file = (char*)malloc(len + 5);
			strcpy(output_file, source_files.front().front().c_str());
			if(output_file[len-3]=='.' && output_file[len-2]=='v' && output_file[len-1]=='x') {
				output_file[len-3] = '\0';
				if (link_mode != dont_link) {
					exe_file = (char*)malloc(len+5);
					strcpy(exe_file, output_file);
#ifdef _WIN32
					strcat(exe_file, ".exe");
#endif
				}
#if defined(_MSC_VER)
				strcat(output_file, ".obj");
#else
				strcat(output_file, ".o");
#endif
			} else {
#ifdef _WIN32
				output_file = const_cast<char*>("a.obj");
				if (link_mode != dont_link)
					exe_file = const_cast<char*>("a.exe");
#else
				output_file = const_cast<char*>("a.o");
				if (link_mode != dont_link)
					exe_file = const_cast<char*>("a.out");
#endif
			}
		}
	}
	// always read builtin definitions first
	builtin_input_fd = open(builtin_file_name, O_CLOEXEC);
	if (builtin_input_fd < 0) {
		errs() << llvm::format("Cannot open definition file for builtins\"%s\": %s\n", builtin_file_name, strerror(errno));
		exit(1);
	}
	lex = Lexer(&builtin_input_fd, builtin_file_name);
	// Lexer::Lexer() above invalidates 'builtin_input_fd' so restore it
	builtin_input_fd = lex.input_fd;
	CurLoc = lex.Loc;
	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		llvm::InitializeNativeTargetAsmParser();
	}

	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		TheJIT = ExitOnErr(llvm::orc::VolvoxJIT::Create());
	}
	TS_Context = std::make_unique<llvm::orc::ThreadSafeContext>(std::move(std::make_unique<llvm::LLVMContext>()));

	InitializeModuleAndPassManager();
#ifndef LEGACY_PASS_MANAGER
	// Register all the basic analyses with the managers.
	// PTO.LoopInterleaving = false;
	// PTO.LoopVectorization = false;
	// PTO.SLPVectorization = false;
	// PTO.LoopUnrolling = false;
	// PTO.ForgetAllSCEVInLoopUnroll = false;
	// PTO.LicmMssaOptCap = false;
	// PTO.LicmMssaNoAccForPromotionCap = false;
	// PTO.CallGraphProfile = false;
	// PTO.MergeFunctions = false;
	// PTO.EagerlyInvalidateAnalyses = false;
#endif
	// Initialize the target registry etc.
	llvm::InitializeAllTargetInfos();
	llvm::InitializeAllTargets();
	llvm::InitializeAllTargetMCs();
	llvm::InitializeAllAsmParsers();
	llvm::InitializeAllAsmPrinters();
	std::string TargetTriple;
	if (target_mingw)
		TargetTriple = "x86_64-pc-windows-gnu";
	else
		TargetTriple = llvm::sys::getDefaultTargetTriple();

	std::string Error;
	auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);
	// Print an error and exit if we couldn't find the requested target.
	// This generally occurs if we've forgotten to initialise the
	// TargetRegistry or we have a bogus target triple.
	if (!Target) {
		errs() << Error;
		return 1;
	}
	if (verbosity >= 1) {
		errs() << "Target: " << TargetTriple << '\n';
		// errs() << Target->getName() << " # " << Target->getShortDescription() << " # " << Target->getBackendName() << '\n';
	}
	auto CPU = "generic";
	auto Features = "";
	llvm::TargetOptions target_opts;
	auto RM = llvm::Optional<llvm::Reloc::Model>(gen_pic ? llvm::Reloc::Model::PIC_ : llvm::Reloc::Model::DynamicNoPIC);
	std::unique_ptr<llvm::TargetMachine> u_tartgetm = nullptr;
	if (comp_mode == comp_jit) {
		if (auto ptr = TheJIT->createTargetMachine()) {
			u_tartgetm = std::move(*ptr);
			TheTargetMachine = u_tartgetm.get();
		} else {
			errs() << ptr.takeError() << '\n';
		}
	} else {
		TheTargetMachine =
			Target->createTargetMachine(TargetTriple, CPU, Features, target_opts, RM, llvm::None, codegenopt);
	}
	if (verbosity >= 1) {
		if (TheTargetMachine->useEmulatedTLS())
			errs() << "using emulated TLS\n";
		else
			errs() << "using native TLS\n";
	}
	support_fp80 = TheTargetMachine->getTargetTriple().isX86();
	if (TheTargetMachine->getTargetTriple().isArch64Bit())
		target_bits = 64;
	else if (TheTargetMachine->getTargetTriple().isArch32Bit())
		target_bits = 32;
	else if (TheTargetMachine->getTargetTriple().isArch16Bit())
		target_bits = 16;
	else {
		errs() << "fatal: cannot get pointer size of target '"
		       << TargetTriple << "'\n";
		exit(1);
	}
	target_bytes = target_bits >> 3;
	if (comp_mode == comp_obj) {
		TheModule->setTargetTriple(TargetTriple);
		TheModule->setDataLayout(TheTargetMachine->createDataLayout());
		// auto strrep = TheModule->getDataLayout().getStringRepresentation();
		// errs() << "Data Layout: >" << strrep << "<\n";
	}
	if (comp_mode == comp_dbg) {
		// Add the current debug info version into the module.
		TheModule->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
		                         llvm::DEBUG_METADATA_VERSION);
		// Darwin only supports dwarf2.
		if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
			TheModule->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);
	  
		// Construct the DIBuilder, we do this here because we need the module.
		DBuilder = std::make_unique<llvm::DIBuilder>(*TheModule);
		// Create the compile unit for the module.
		// Currently down as "fib.ks" as a filename since we're redirecting stdin
		// but we'd like actual source locations.
		KSDbgInfo.TheCU = DBuilder->createCompileUnit(
			llvm::dwarf::DW_LANG_C, DBuilder->createFile(lex.Loc.File, "."),
			"Volvox Compiler", 0, "", 0);
	}
	init(TheTargetMachine->getTargetTriple());
	// Prime the first token.
	getNextToken();
	// Run the main "interpreter loop" now.
	MainLoop();
	if (do_test || comp_mode != comp_jit) {
		if (auto FnAST = do_test ? CreateTestRuns() : CreateMain("main")) {
			if (auto *FnIR = FnAST->codegen(true)) {
				if (comp_mode == comp_jit) {
					// call test_main()

					// Create a ResourceTracker to track JIT'd memory allocated to our
					// anonymous expression -- that way we can free it after executing.
					auto RT = TheJIT->getMainJITDylib().createResourceTracker();
					auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), *TS_Context.get());
					ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
					InitializeModuleAndPassManager();
					auto ExprSymbol = ExitOnErr(TheJIT->lookup("test_main"));
					// Get the symbol's address and cast it to the right type (takes no
					// arguments, returns a bool) so we can call it as a native function.
					bool (*BOOL)() = (bool (*)())(intptr_t)ExprSymbol.getAddress();
					bool ret = spawn_bool_expr(BOOL);
					if (ret)
						errs() << "All test cases passed\n";
					else
						errs() << "Some test cases failed\n";
					// Delete the anonymous expression module from the JIT.
					ExitOnErr(RT->remove());
				}
			} else {
				exit(1);
			}
		} else {
			exit(1);
		}
	}
	int result = 0;
	if (comp_mode == comp_obj) {
		auto Filename = output_file;
		std::error_code EC;
		llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

		if (EC) {
			errs() << "Could not open output file \"" << Filename << "\": " << EC.message() << '\n';
			return 1;
		}
	  
		llvm::legacy::PassManager pass;
		auto FileType = llvm::CGFT_ObjectFile;
	  
		if (TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
			errs() << "TheTargetMachine can't emit a file of this type";
			return 1;
		}

		pass.run(*TheModule);
		dest.flush();
		dest.close();
		hints() << "Wrote " << Filename << "\n";
		if (link_mode != dont_link) {
			int lr = strlen(volvox_root());
			char* libpath = (char*)alloca(lr+32);
			strcpy(libpath, volvox_root());
			char* stack_size = nullptr;
			/* building the linker command is somewhat tricky because several things have to be considered:
			 * 1. on POSIX systems the "GNU" typical syntax should be used
			 * 2. Windows native requires the "MSVC" typical syntax
			 * 3. the mingw-w64 target should be supported on both - Windows and POSIX systems
			 *    - on Windows as semi-native target using clang as link command,
			 *    - on POSIX as cross compile target using x86_64-w64-mingw32-gcc as link command
			 * 4. in principle the mingw-w64 target is similar to the POSIX case but there are some
			 *    Windows specific flags like "-Wl,-stack,<size>"
			 * 5. some of these cases can be handled by "#ifdef"s, others need run time "if"s
			 */
			char* linker_exe = getenv("VOLVOX_LINKER");
			if (target_mingw) {
				if (!linker_exe)
					linker_exe = const_cast<char*>(MINGW_W64_LINKER);
#ifdef _WIN32
				strcat(libpath, "\\libvolvox.dll");
#else
				strcat(libpath, "/libvolvox.dll");
#endif
			}
#if defined(_MSC_VER)
			const char* libpatterns[] = LIBDIRS;
			char* libdirs[ARRAY_SIZE(libpatterns)];
			if (!target_mingw) {
				strcat(libpath, "\\lib\\libvolvox.lib");
				volvox_glob_t linkers = volvox_glob(LINKER);
				if (!linkers.size) {
					errs() << "Unable to find 'link.exe' (searched as \"" << LINKER << "\"\n";
					exit(1);
				}
				linker_exe = linkers.dirs[0];
				for (int i=0; i<ARRAY_SIZE(libpatterns); i++) {
					volvox_glob_t lib = volvox_glob(libpatterns[i]);
					if (!linkers.size) {
						errs() << "Unable to find Windows system library (searched as \"" << libpatterns[i] << "\"\n";
						exit(1);
					}
					int strl = strlen(lib.dirs[0]) - 1;
					if (lib.dirs[0][strl] == '\\' || lib.dirs[0][strl] == '/') {
						errs() << "Unexpected directory name when searching for file: " << lib.dirs[0] << '\n';
						exit(1);
					}
					while (lib.dirs[0][strl] != '\\' && lib.dirs[0][strl] != '/')
						strl--;
					libdirs[i] = (char*)alloca(strl + 10);
					strcpy(libdirs[i], "-libpath:"); // 9 characters
					strncpy(libdirs[i] + 9, lib.dirs[0], strl); // don't copy trailing '\\'
					libdirs[i][9 + strl] = '\0';
					volvox_free_glob(&lib);
				}
			}
			char* exe_out = (char*)alloca(5 + strlen(exe_file) + 1);
			if (!target_mingw) {
				strcpy(exe_out, "-out:");
				strcat(exe_out, exe_file);
			}
			stack_size = (char*)alloca(30);
			if (!target_mingw)
				sprintf(stack_size, "-stack:%" PRIu64, stacksize);
#else
			if (!target_mingw) {
				strcat(libpath, "/lib");
			}
#ifndef _WIN32
			char* rpath = nullptr;
			if (!target_mingw) {
				rpath = (char*)alloca(lr+43);
				strcpy(rpath, "-Wl,-rpath,");
				strcat(rpath, libpath);
			}
#endif
			if (!linker_exe)
				linker_exe = const_cast<char*>(LINKER);
#endif
			if (target_mingw) {
				stack_size = (char*)alloca(30);
				sprintf(stack_size, "-Wl,-stack,%" PRIu64, stacksize);
			}
			std::vector<char*> clang_argv = {};
			clang_argv.reserve(16);
			clang_argv.push_back(linker_exe);
			clang_argv.push_back(output_file);
			if (target_mingw) {
				size_t comp_offs = strlen(linker_exe) - 3;
				if (strcmp(linker_exe + comp_offs, "gcc") && strcmp(linker_exe + comp_offs, "g++")) {
					// neither 'gcc' nor 'g++' - so it's 'clang' and we must specify the target
					clang_argv.push_back(const_cast<char*>("-target"));
					clang_argv.push_back(const_cast<char*>("x86_64-pc-windows-gnu"));
				}
				clang_argv.push_back(stack_size); // mingw on Windows or cross compiler (e.g. on Linux)
			}
#ifdef _WIN32
			else {
				clang_argv.push_back(stack_size); // native Windows
				clang_argv.push_back(exe_out);
				clang_argv.push_back(const_cast<char*>("-defaultlib:libcmt"));
				clang_argv.push_back(const_cast<char*>("-defaultlib:oldnames"));
				clang_argv.push_back(libpath);
				clang_argv.push_back(libdirs[0]);
				clang_argv.push_back(libdirs[1]);
				clang_argv.push_back(libdirs[2]);
				if (verbosity >= 3)
					clang_argv.push_back(const_cast<char*>("-verbose"));
				else
					clang_argv.push_back(const_cast<char*>("-nologo"));
			}
			if (target_mingw) {
#endif
				clang_argv.push_back(const_cast<char*>("-o"));
				clang_argv.push_back(exe_file);
				clang_argv.push_back(const_cast<char*>("-O2"));
#ifndef _WIN32
				if (!target_mingw)
					clang_argv.push_back(const_cast<char*>("-L"));
#endif
				clang_argv.push_back(libpath);
#ifndef _WIN32
				if (!target_mingw) {
					clang_argv.push_back(const_cast<char*>("-lvolvox"));
					clang_argv.push_back(rpath);
				}
				if (needs_libm)
					clang_argv.push_back(const_cast<char*>("-lm"));
#endif
				if(verbosity)
					clang_argv.push_back(const_cast<char*>("-v"));
#ifdef _WIN32
			}
#endif
			clang_argv.push_back(nullptr);
			if (verbosity)
				for (auto a: clang_argv) {
					if (a)
						errs() << ' '
#ifdef _WIN32
						       << '"'
#endif
						       << a
#ifdef _WIN32
						       << '"'
#endif
							;
					else
						errs() << '\n';
				}
			int linker_pid;
			if (!volvox_spawn(&linker_pid, nullptr, nullptr, nullptr, clang_argv.data())) {
				errs() << llvm::format("Failed to link: %s\n", strerror(errno));
				result = 1;
			} else {
				result = volvox_wait(linker_pid);
				if (result) {
					errs() << "Linking failed with exit code " << result << '\n';
				} else if (run_program) {
#if !defined(_MSC_VER)
					char* exe_out = (char*)alloca(5 + strlen(exe_file) + 1);
#endif
					strcpy(exe_out, "./");
					strcat(exe_out, exe_file);
					char* prog_argv[] = { exe_out, nullptr };
					int prog_pid;
					if (!volvox_spawn(&prog_pid, nullptr, nullptr, nullptr, prog_argv)) {
						errs() << llvm::format("Failed to run program: %s\n", strerror(errno));
						result = 1;
					} else {
						result = volvox_wait(prog_pid);
					}
				}
			}
		}
	} else if (comp_mode == comp_dbg) {
		// Finalize the debug info.
		DBuilder->finalize();
		// Print out all of the generated code.
		TheModule->print(errs(), nullptr);
	} else if (comp_mode == comp_jit) {
		ExitOnErr(TheJIT->getMainJITDylib().clear());
	}
#ifdef _WIN32
	SetConsoleOutputCP(old_cp);
	SetConsoleCP(old_input_cp);
#endif
	return result;
}
