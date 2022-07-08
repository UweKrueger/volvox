#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

// some names for environment variables

#define PROMPT_COL "VOLVOX_COLORS"

CompModes comp_mode = comp_undefined;
LinkModes link_mode = link_undefined;
std::vector<std::string> include_files = {};
std::vector<std::string> source_files = {};
std::vector<std::string> import_path = {};
std::vector<std::unique_ptr<ExprAST>> GlobalExprList = {};
const std::string single_test_result_name = "__test_result";
const std::string collector_name = "__test_results_collect";
int include_index = 0;
int source_index = 0;
int prompt_indent = 0;
bool is_pub = false;

DebugInfo KSDbgInfo;

TypeTable type_table;

//===----------------------------------------------------------------------===//
// Code Generation Globals
//===----------------------------------------------------------------------===//

#if LLVM_VERSION_MAJOR >= 12
llvm::orc::ThreadSafeContext TS_Context;
#else
llvm::LLVMContext Context;
#endif
std::unique_ptr<llvm::Module> TheModule;
std::unique_ptr<llvm::IRBuilder<>> Builder;
llvm::ExitOnError ExitOnErr;

global_var_shadow* global_list = NULL;
global_var_shadow** global_list_end = &global_list;
thread_local global_var_shadow* tl_global_list = nullptr;	

// useful definitions - "Context-time" constants
llvm::Type* llvm_int_type;
llvm::Type* llvm_size_type;
llvm::Type* llvm_bool_type;
volvoxc::FullType* void_type;
volvoxc::FullType* uintptr_type;

// static std::map<std::string, llvm::AllocaInst *> NamedValues;
std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM;
std::unique_ptr<llvm::orc::VolvoxJIT> TheJIT;
std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

llvm::raw_ostream &indent(llvm::raw_ostream &O, int size) {
	return O << std::string(size, ' ');
}
VarTable globals_table;
NameTable name_table;
std::vector<VarTable> locals_table; // including function arguments

//===----------------------------------------------------------------------===//
// Built-in Types
//===----------------------------------------------------------------------===//

unsigned stringkey;

void init() {
	// only for internal use:
	type_table.add("i*", llvm::Type::getInt64Ty(Context), nullptr, A_signed);
	type_table.add("f*", llvm::Type::getDoubleTy(Context), nullptr);

#if UINTPTR_MAX == UINT16_MAX // e.g. AVR platform
	type_table.add("int", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("int", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("uint", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("uint", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_int_type = llvm::Type::getInt16Ty(Context);
	type_table.add("size", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("size", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("usize", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("usize", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_size_type = llvm::Type::getInt16Ty(Context);
	type_table.add("real", llvm::Type::getFloatTy(Context), DBuilder ? DBuilder->createBasicType("real", 32, llvm::dwarf::DW_ATE_float) : nullptr);
#else
	type_table.add("int", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("uint", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("uint", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_int_type = llvm::Type::getInt32Ty(Context);
#if UINTPTR_MAX == UINT32_MAX
	type_table.add("size", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("size", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("usize", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("usize", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_size_type = llvm::Type::getInt32Ty(Context);
#else
	type_table.add("size", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("size", 64, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("usize", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("usize", 64, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_size_type = llvm::Type::getInt64Ty(Context);
#endif
	uintptr_type = type_table.get_full("usize");
	type_table.add("real", llvm::Type::getDoubleTy(Context), DBuilder ? DBuilder->createBasicType("real", 64, llvm::dwarf::DW_ATE_float) : nullptr);
#endif
	type_table.add("void", llvm::Type::getVoidTy(Context), nullptr);
	// TODO: make .add() return FullType*
	void_type = type_table.get_full("void");
	llvm_bool_type = llvm::Type::getInt1Ty(Context);
	type_table.add("bool", llvm::Type::getInt1Ty(Context), DBuilder ? DBuilder->createBasicType("bool", 1, llvm::dwarf::DW_ATE_boolean) : nullptr);
	type_table.add("i8", llvm::Type::getInt8Ty(Context), DBuilder ? DBuilder->createBasicType("i8", 8, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("i16", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("i16", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("i32", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("i32", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("i64", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("i64", 64, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("u8", llvm::Type::getInt8Ty(Context), DBuilder ? DBuilder->createBasicType("u8", 8, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("u16", llvm::Type::getInt16Ty(Context), DBuilder ? DBuilder->createBasicType("u16", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("u32", llvm::Type::getInt32Ty(Context), DBuilder ? DBuilder->createBasicType("u32", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("u64", llvm::Type::getInt64Ty(Context), DBuilder ? DBuilder->createBasicType("u64", 64, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("f16", llvm::Type::getBFloatTy(Context), DBuilder ? DBuilder->createBasicType("f16", 16, llvm::dwarf::DW_ATE_float) : nullptr);
	type_table.add("f32", llvm::Type::getFloatTy(Context), DBuilder ? DBuilder->createBasicType("f32", 32, llvm::dwarf::DW_ATE_float) : nullptr);
	type_table.add("f64", llvm::Type::getDoubleTy(Context), DBuilder ? DBuilder->createBasicType("f64", 64, llvm::dwarf::DW_ATE_float) : nullptr);
	type_table.add("string", llvm::Type::getInt8PtrTy(Context),
	               DBuilder ? DBuilder->createPointerType(DBuilder->createBasicType("i8", 8, llvm::dwarf::DW_ATE_signed_char), 64, 0, llvm::None, "string") : nullptr);
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

void InitializeModuleAndPassManager() {
	// Open a new module.
	TheModule = std::make_unique<llvm::Module>(input_file_name, Context);
	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		TheModule->setDataLayout(
#if LLVM_VERSION_MAJOR >= 12
			TheJIT->getDataLayout()
#else
			TheJIT->getTargetMachine().createDataLayout()
#endif
			);
	}
	
	// Create a new builder for the module.
	Builder = std::make_unique<llvm::IRBuilder<>>(Context);

	if (comp_mode == comp_jit) {
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
	}
}

static void HandleDefinition() {
	inside_function = true;
	elselevel = 0;
	FirstPassFlags.clear();
	locals_table.push_back(VarTable());
	bool success = false;
	if (auto FnAST = ParseDefinition()) {
		if (auto *FnIR = FnAST->codegen()) {
			if (dump_IR) {
				errs() << "Read function definition:\n";
				FnIR->print(errs());
				errs() << "\n";
			}
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

static void HandleExtern() {
	if (auto ProtoAST = ParseExtern()) {
		if (auto *FnIR = ProtoAST->codegen()) {
			if (dump_IR) {
				errs() << "Read extern: ";
				FnIR->print(errs());
				errs() << "\n";
			}
			FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
		} else {
			errs() << "Error reading extern\n";
		}
	} else {
		// Skip token for error recovery.
		purgeLine();
	}
}

static void HandleTypeDef() {
	getNextToken(); // eat type
	if (CurTok.kind != tok_identifier) {
		errs() << "unexpected '" << CurTok.str() << "' in type declaration - type name expected\n";
		purgeLine();
		return;
	}
	auto type_name = IdentifierStr;
	getNextToken(eType);
	auto ft = ParseType(false, eComma, type_name.c_str());
	type_table.add(type_name.c_str(), ft);
}

static void HandleImport() {
	bool from = CurTok.kind == tok_from;
	do {
		getNextToken(ePath);
		if (CurTok.kind != tok_identifier) {
			errs() << "unexpected token in import " << CurTok.kind << '\n';
			purgeLine();
			return;
		}
		import_path.push_back(IdentifierStr);
		getNextToken(ePath);
	} while (CurTok.kind == tok_selector);
	if (CurTok.kind == ';') {
		for (int j=0; j<import_path.size(); j++)
			errs() << (j ? "/" : "Import path: ") << import_path[j];
		errs() << '\n';
		import_path = {};
		return;
	}
	errs() << "unexpected identifier\n";
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
	pthread_t thread;
	int res = pthread_create(&thread, NULL, anon_expr_wrapper, (void*)expr);
	void* retval;
	res = pthread_join(thread, &retval);
	return !(!retval);
}
#endif

static void HandleTopLevelExpression() {
	// Evaluate a top-level expression into an anonymous function.
	if (auto FnAST = ParseTopLevelExpr()) {
		if (auto anon_expr = FnAST->codegen()) {
			if (dump_IR >= 2) {
				errs() << "Created temporary top level anon_expr definition:\n";
				anon_expr->print(errs());
				errs() << "\n";
			}
			auto ret_type = anon_expr->getReturnType();
			if (!anon_expr->getReturnType()->isIntegerTy() || !(anon_expr->getReturnType()->getIntegerBitWidth() == 1)) {
				errs() << "internal error: anonymous function does not return `bool`\n";
				return;
			}
			if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
				// Create a ResourceTracker to track JIT'd memory allocated to our
				// anonymous expression -- that way we can free it after executing.
				auto RT = TheJIT->getMainJITDylib().createResourceTracker();
				auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context);
				ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
#else
				// JIT the module containing the anonymous expression, keeping a handle so
				// we can free it later.
				auto H = TheJIT->addModule(std::move(TheModule));
#endif
				InitializeModuleAndPassManager();
				
				// Search the JIT for the __anon_expr symbol.
#if LLVM_VERSION_MAJOR >= 12
				auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
#define UNWRAP(x) (x)
#else
				auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
				assert(ExprSymbol && "Function not found");
#define UNWRAP(x) cantFail(x)
#endif
				// Get the symbol's address and cast it to the right type (takes no
				// arguments, returns a bool) so we can call it as a native function.
				bool (*BOOL)() = (bool (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
				bool b = spawn_bool_expr(BOOL);
				if (!b)
					errs() << "... aborted\n";
#if LLVM_VERSION_MAJOR >= 12
				// Delete the anonymous expression module from the JIT.
				ExitOnErr(RT->remove());
#else
				// Delete the anonymous expression module from the JIT.
				TheJIT->removeModule(H);
#endif
			}
		} else {
			errs() << "Error generating code for top level expr\n";
		}
	} else {
		// Skip rest for error recovery.
		purgeLine();
	}
}

std::unique_ptr<FunctionAST> CreateMain(const char* main_name, bool have_return = false, const char* ret_type = "i32") {
	volvoxc::FullType* TheType = type_table.get_full(ret_type);
	auto Proto = std::make_unique<PrototypeAST>(CurLoc, main_name,
	                                            std::vector<std::string>(),
	                                            CurLoc, false, TheType);
	if (!have_return)
		GlobalExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
	auto ProtoRef = Proto.get();
	FunctionProtos[Proto->getName()] = std::move(Proto);
	auto main_function = std::make_unique<FunctionAST>(ProtoRef, std::move(GlobalExprList), tok_return);
	return main_function;
}

void PrepareTestFramework() {
	// create (global) variables to collect Results
	auto single_res_def = std::make_unique<BinaryExprAST>(
		CurLoc, ":=",
		std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)),
		std::move(std::make_unique<LiteralExprAST>(Token(false))));
	HandleGlobalVariable(single_res_def.get());
	auto collector_def = std::make_unique<BinaryExprAST>(
		CurLoc, ":=",
		std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)),
		std::move(std::make_unique<LiteralExprAST>(Token(true))));
	HandleGlobalVariable(collector_def.get());
}

void CallTestFunction() {
	std::string showres = "showtestres";
	auto show_res_fn = FunctionProtos.find(showres);
	if (show_res_fn == FunctionProtos.end()) {
		errs() << "Cannot find function to display test results: '" << showres << "()'\n";
		return;
	}
	auto F = FunctionProtos.find(TestFunction);
	if (F != FunctionProtos.end()) {
		GlobalExprList.push_back(
			std::make_unique<BinaryExprAST>(
				CurLoc, "=",
				std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)),
				std::move(std::make_unique<CallExprAST>(
					          CurLoc, std::make_unique<FunctionExprAST>(CurLoc, TestFunction, F->second.get())))));
		std::vector<std::unique_ptr<ExprAST>> Args;
		Args.push_back(std::move(std::make_unique<LiteralExprAST>(Token(1LL))));
		Args.push_back(std::move(std::make_unique<LiteralExprAST>(Token(79LL))));
		Args.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string(TestFunction)))));
		Args.push_back(std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)));
		GlobalExprList.push_back(
			std::move(std::make_unique<CallExprAST>(
				          CurLoc, std::make_unique<FunctionExprAST>(CurLoc, showres, show_res_fn->second.get()),
				          std::move(Args))));
		GlobalExprList.push_back(
			std::make_unique<BinaryExprAST>(
				CurLoc, "=", std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)),
				std::make_unique<BinaryExprAST>(
					CurLoc, "&",
					std::move(std::make_unique<VariableExprAST>(CurLoc, collector_name)),
					std::move(std::make_unique<VariableExprAST>(CurLoc, single_test_result_name)))));
	}
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
			convBinOp(llvm_int_type, llvm_int_type, A_signed, A_signed, false, false, "-"));
		if_e->desired_type = llvm_int_type;
		if_e->desired_type_attr = A_signed;
		GlobalExprList.push_back(
			std::move(if_e));
		return CreateMain("main", true, "i32");
	}
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
	while (true) {
		if (CurTok.kind == tok_pub) {
			is_pub = true;
			getNextToken();
		}
		else
			is_pub = false;
		switch (CurTok.kind) {
		case tok_eof:
			return;
		case ';': // ignore top-level semicolons.
			getNextToken();
			break;
		case tok_fn:
			HandleDefinition();
			if (TestFunction) {
				if (do_test)
					CallTestFunction();
				TestFunction = nullptr;
			}
			break;
		case tok_extern:
			HandleExtern();
			break;
		case tok_import:
		case tok_from:
			HandleImport();
			break;
		case tok_type:
			HandleTypeDef();
			break;
		default:
			if (comp_mode == comp_jit && !do_test)
				HandleTopLevelExpression();
			else
				if (auto expr = GetTopLevelExpression())
					GlobalExprList.push_back(std::move(expr));
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

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
	errs() << X << "\n";
	return 0;
}

extern "C" DLLEXPORT uintptr_t new_global_var_shadow(void* adr, size_t size) {
	size_t alloc_size = sizeof(global_var_shadow);
	if (size > 8)
		alloc_size = alloc_size - 8 + size;
	global_var_shadow* V = (global_var_shadow*)malloc(alloc_size);
	if (!V)
		return 0;
	V->next = NULL;
	V->adr = adr;
	V->size = size;
	memcpy(V->data, V->adr, size);
	*global_list_end = V;
	global_list_end = &V->next;
	return (uintptr_t)V->data;
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

const char* input_file_name = nullptr;
const char* builtin_file_name = "builtin.vx";
int builtin_input_fd = -1;
int input_fd = -1;

// Windows has no CLOEXEC
#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

static void usage(const char* prog) {
	errs() << "Usage: " << prog << " [-v] [-d] [-c] [-fPIC] [-g] [-i file] [-o file] [[-t] file [...]]\n";
	errs() << " -v ........ verbose output (may be repeated for even more verbosity)\n";
	errs() << " -d ........ dump generated LLVM IR-code (repeat to dump more code)\n";
	errs() << " -c ........ compile to optimized object file\n";
	errs() << " -fPIC ..... generate position independent code\n";
	errs() << " -g ........ compile with debug information\n";
	errs() << " -r ........ run compiled program\n";
	errs() << " -j ........ use JIT to run file(s)\n";
	errs() << " -J ........ use JIT to run file(s) and start interactive session\n";
	errs() << " -i file ... include \"file\" in advance\n";
	errs() << " -o file ... output compiled result to \"file\"\n";
	errs() << " -t ........ compile/run all \"fn test_*() bool\" functions from given file(s)\n";
	errs() << " -C n,g,b .. set prompt colors (ANSI-256, default: 30,100,236)\n";
	errs() << " file ...... file(s) to compile (default: interactive session is started)\n";
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
bool jit_repl = false;
bool gen_pic = false;
bool run_program = false;
promptcolor_t p_col = { 30, 100, 236 };
const char* TestFunction = nullptr;
char* output_file = nullptr;
char* exe_file = nullptr;

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

bool next_input_file() {
	if (input_fd > 0)
		close(input_fd);
	if (source_index < source_files.size()) {
		input_file_name = source_files[source_index++].c_str();
		input_fd = open(input_file_name, O_CLOEXEC);
		if (input_fd < 0) {
			errs() << llvm::format("Cannot open input file \"%s\": %s\n", input_file_name, strerror(errno));
			exit(1);
		}
	} else if ((jit_repl || !source_index) && input_fd != 0) {
		input_fd = 0;
		input_file_name = "<stdin>";
	} else {
		return false;
	}
	return true;
}

#if defined (_MSC_VER)
// We have to switch to code page 65001 to enable UTF-8. This
// value here is used to restore the old state on exit
unsigned old_cp;
// glob patterns to search for linker and libraries
#define LINKER "C:\\Program Files*\\Microsoft Visual Studio\\*\\*\\VC\\Tools\\MSVC\\*\\bin\\Hostx64\\x64\\link.exe"
// patterns for library directories - file name is added to skip stale empty directories
#define LIBDIRS { \
	"C:\\Program Files*\\Microsoft Visual Studio\\*\\*\\VC\\Tools\\MSVC\\*\\lib\\x64\\libcmt.lib", \
	"C:\\Program Files*\\Windows Kits\\*\\Lib\\*\\ucrt\\x64\\libucrt.lib", \
	"C:\\Program Files*\\Windows Kits\\*\\Lib\\*\\um\\x64\\ntdll.lib" }
#endif

int main(int argc, char* argv[]) {
	// std::vector<const char*> qqq = { "xyv", "rtz" };
	// std::vector<std::pair<volvoxc::FullType*,bool>> sss = {};
	// auto pppp = Mangle(qqq, sss);
	// auto ppp = pppp.str();
	// fprintf(stderr, "Mangled name (%" PRIu64 " bytes): >%s<\n", ppp.size(), ppp.data());
#if defined (_MSC_VER)
	old_cp = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);
#endif
	setlocale(LC_ALL, "en_US.UTF-8");
	outs().SetUnbuffered();
	errs().SetUnbuffered();

	if (char* cols = getenv(PROMPT_COL))
		if (!parse_pcol(cols))
			errs() << llvm::format("Problem processing environment variables: %s\n", strerror(errno))
			       << '"' << cols << "\" is not a valid value for " << PROMPT_COL << '\n';
	int opt;
	while ((opt = getopt(argc, argv, "vdcgrjJf:i:o:tP:")) != -1) {
		switch (opt) {
		case 'v':
			verbosity++;;
			break;
		case 'd':
			if (comp_mode == comp_dbg)
				debug_mode_conflict(argv[0]);
			dump_IR++;
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
			break;
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
			else {
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
			errs() << "unknown option '-" << opt << "'\n";
			usage(argv[0]);
		}
	}
	for (;optind < argc; optind++)
		source_files.push_back(argv[optind]);
	if (!comp_mode) {
		if (source_files.size())
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
	if (run_program && link_mode == dont_link) {
		errs() << "Options '-c' and '-r' are mutually exclusive\n";
		usage(argv[0]);
	}
	if (run_program && comp_mode == comp_jit) {
		errs() << "Options '-r' makes no sense in JIT mode\n";
		usage(argv[0]);
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
			if (source_files.size() != 1) {
				errs() << "output file name (-o ...) required if "
				       << (source_files.size() ? "more than one" : "no")
				       << " input file provided\n";
				usage(argv[0]);
			}
			int len = source_files[0].size();
			output_file = (char*)malloc(len + 5);
			strcpy(output_file, source_files[0].c_str());
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
	input_fd = builtin_input_fd;
	CurLoc = LexLoc = { builtin_file_name, 0, 0 };
	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		llvm::InitializeNativeTargetAsmParser();
	}

	if (comp_mode == comp_jit || comp_mode == comp_dbg) {
		TheJIT =
#if LLVM_VERSION_MAJOR >= 12
			ExitOnErr(llvm::orc::VolvoxJIT::Create());
#else
		std::make_unique<llvm::orc::VolvoxJIT>();
#endif
	}
#if LLVM_VERSION_MAJOR >= 12
	TS_Context = llvm::orc::ThreadSafeContext(std::move(std::make_unique<llvm::LLVMContext>()));
#endif

	InitializeModuleAndPassManager();

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
			llvm::dwarf::DW_LANG_C, DBuilder->createFile(input_file_name, "."),
			"Volvox Compiler", 0, "", 0);
	}
	init();
	// Prime the first token.
	getNextToken();
	// Run the main "interpreter loop" now.
	MainLoop();

	if (do_test || comp_mode != comp_jit) {
		if (auto FnAST = do_test ? CreateTestRuns() : CreateMain("main")) {
			if (auto *FnIR = FnAST->codegen()) {
				if (dump_IR) {
					errs() << "Read function definition:\n";
					FnIR->print(errs());
					errs() << "\n";
				}
				if (comp_mode == comp_jit) {
					// call test_main()
#if LLVM_VERSION_MAJOR >= 12
					// Create a ResourceTracker to track JIT'd memory allocated to our
					// anonymous expression -- that way we can free it after executing.
					auto RT = TheJIT->getMainJITDylib().createResourceTracker();
					auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context);
					ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
#else
					// JIT the module containing the anonymous expression, keeping a handle so
					// we can free it later.
					auto H = TheJIT->addModule(std::move(TheModule));
#endif
					InitializeModuleAndPassManager();
#if LLVM_VERSION_MAJOR >= 12
					auto ExprSymbol = ExitOnErr(TheJIT->lookup("test_main"));
#define UNWRAP(x) (x)
#else
					auto ExprSymbol = TheJIT->findSymbol("test_main");
					assert(ExprSymbol && "Function not found");
#define UNWRAP(x) cantFail(x)
#endif
					// Get the symbol's address and cast it to the right type (takes no
					// arguments, returns a bool) so we can call it as a native function.
					bool (*BOOL)() = (bool (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
					bool ret = spawn_bool_expr(BOOL);
					if (ret)
						errs() << "All test cases passed\n";
					else
						errs() << "Some test cases failed\n";
#if LLVM_VERSION_MAJOR >= 12
					// Delete the anonymous expression module from the JIT.
					ExitOnErr(RT->remove());
#else
					// Delete the anonymous expression module from the JIT.
					TheJIT->removeModule(H);
#endif
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
		// Initialize the target registry etc.
		llvm::InitializeAllTargetInfos();
		llvm::InitializeAllTargets();
		llvm::InitializeAllTargetMCs();
		llvm::InitializeAllAsmParsers();
		llvm::InitializeAllAsmPrinters();
		auto TargetTriple = llvm::sys::getDefaultTargetTriple();
		TheModule->setTargetTriple(TargetTriple);

		std::string Error;
		auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);
		// Print an error and exit if we couldn't find the requested target.
		// This generally occurs if we've forgotten to initialise the
		// TargetRegistry or we have a bogus target triple.
		if (!Target) {
			errs() << Error;
			return 1;
		}

		auto CPU = "generic";
		auto Features = "";
		llvm::TargetOptions opt;
		auto RM = llvm::Optional<llvm::Reloc::Model>(gen_pic ? llvm::Reloc::Model::PIC_ : llvm::Reloc::Model::DynamicNoPIC);
		auto TheTargetMachine =
			Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);
	  
		TheModule->setDataLayout(TheTargetMachine->createDataLayout());
	  
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
			char* volvox_root = getenv("VOLVOX_ROOT");
			if (!volvox_root)
				volvox_root = const_cast<char*>(".");
			int lr = strlen(volvox_root);
			char* libpath = (char*)alloca(lr+32);
			strcpy(libpath, volvox_root);
#if defined(_MSC_VER)
			strcat(libpath, "\\lib\\libvolvox.lib");
			volvox_glob_t linkers = volvox_glob(LINKER);
			if (!linkers.size) {
				errs() << "Unable to find 'link.exe' (searched as \"" << LINKER << "\"\n";
				exit(1);
			}
			char* linker_exe = linkers.dirs[0];
			const char* libpatterns[] = LIBDIRS;
			char* libdirs[ARRAY_SIZE(libpatterns)];
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
				strcpy(libdirs[i], "-libpath:");
				strncpy(libdirs[i] + 9, lib.dirs[0], strl); // don't copy trailing '\\'
				libdirs[i][9 + strl] = '\0';
				volvox_free_glob(&lib);
			}
			char* exe_out = (char*)alloca(5 + strlen(exe_file) + 1);
			strcpy(exe_out, "-out:");
			strcat(exe_out, exe_file);
#else
			strcat(libpath, "/lib");
			char* rpath = (char*)alloca(lr+43);
			strcpy(rpath, "-Wl,-rpath,");
			strcat(rpath, libpath);
			char* linker_exe = const_cast<char*>(LINKER);
#endif
			char* clang_argv[] = {
				linker_exe,
				output_file,
#if defined(_MSC_VER)
				exe_out, const_cast<char*>("-defaultlib:libcmt"), const_cast<char*>("-defaultlib:oldnames"),
				libpath, libdirs[0], libdirs[1], libdirs[2],
				(verbosity >= 3) ? const_cast<char*>("-verbose") : const_cast<char*>("-nologo"),
#else
				const_cast<char*>("-o"), exe_file, const_cast<char*>("-O2"), 
				const_cast<char*>("-L"), libpath, const_cast<char*>("-lvolvox"), rpath,
				verbosity ? const_cast<char*>("-v") : nullptr,
#endif
				nullptr
			};
			if (verbosity) {
				for (int i=0; clang_argv[i]; i++) {
					if (i)
						errs() << ' ';
					errs()
#ifdef _WIN32
						<< '"'
#endif
						<< clang_argv[i]
#ifdef _WIN32
						<< '"'
#endif
						;
				}
				errs() << '\n';
			}
			int linker_pid;
			if (!volvox_spawn(&linker_pid, nullptr, nullptr, nullptr, clang_argv)) {
				errs() << llvm::format("Failed to link: %s\n", strerror(errno));
				result = 1;
			} else {
				result = volvox_wait(linker_pid);
				if (result) {
					errs() << "Linking failed\n";
				} else if (run_program) {
#ifndef _WIN32
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
	}
#if defined (_MSC_VER)
	SetConsoleOutputCP(old_cp);
#endif
	return result;
}
