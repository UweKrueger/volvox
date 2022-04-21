#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

// some names for environment variables

#define PROMPT_COL "VOLVOX_COLORS"

CompModes comp_mode = comp_undefined;
LinkModes link_mode = link_undefined;
std::vector<std::string> include_files = {};
std::vector<std::string> source_files = {};
std::vector<std::unique_ptr<ExprAST>> GlobalExprList;
int include_index = 0;
int source_index = 0;
int prompt_indent = 0;

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
cleanup:
	locals_table[0].clear();
	locals_table = {};
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
		getNextToken();
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
	getNextToken();
	auto ft = ParseType();
	type_table.add(type_name.c_str(), ft);
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

static std::unique_ptr<ExprAST> GetTopLevelExpression() {
	if (auto E = ParseExpression()) {
		if (!E->ft || !E->ft->type) {
			if (auto B = dynamic_cast<BinaryExprAST*>(E.get())) {
				if (B->conv.compat.err_msg)
					return AutoErr(B->Loc, B->LHS->ft->type, B->RHS->ft->type, B->LHS->ft->type_attr, B->RHS->ft->type_attr, B->conv.compat.err_msg);
				if (!strcmp(B->Op, ":="))
					return HandleGlobalVariable(B);
				if (!strcmp(B->Op, "="))
					if (auto leftVar = dynamic_cast<VariableExprAST*>(B->LHS.get()))
						if (!leftVar->full_var.first) {
							errs() << "unknown variable name '" << leftVar->getName() << "' - did you mean ':='?\n";
							return nullptr;
						}
				errs() << E->Loc << ": Cannot evalute expression\n";
				return nullptr;
			} else {
				errs() << E->Loc << ": Cannot deduce type of expression\n";
				if (E->ft)
					E->ft->dump();
				return nullptr;
			}
		}
		return E;
	} else {
		return nullptr;
	}
}

std::unique_ptr<FunctionAST> CreateMain(const char* main_name) {
	volvoxc::FullType* TheType = type_table.get_full("i32");
	auto Proto = std::make_unique<PrototypeAST>(CurLoc, main_name,
	                                            std::vector<std::string>(),
	                                            CurLoc, false, TheType);
	GlobalExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
	auto ProtoRef = Proto.get();
	FunctionProtos[Proto->getName()] = std::move(Proto);
	auto main_function = std::make_unique<FunctionAST>(ProtoRef, std::move(GlobalExprList), tok_return);
	return main_function;
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
	while (true) {
		switch (CurTok.kind) {
		case tok_eof:
			return;
		case ';': // ignore top-level semicolons.
			getNextToken();
			break;
		case tok_fn:
			HandleDefinition();
			break;
		case tok_extern:
			HandleExtern();
			break;
		case tok_type:
			HandleTypeDef();
			break;
		default:
			if (comp_mode == comp_jit)
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

const char* input_file_name = "/dev/stdin";
const char* builtin_file_name = "builtin.vx";
int input_fd = 0;
int cur_input_fd;

// Windows has no CLOEXEC
#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

static void usage(const char* prog) {
	errs() << "Usage: " << prog << " [-v] [-d] [-c] [-g] [-i file] [-o file] [[-t] file [...]]\n";
	errs() << " -v ........ verbose output (may be repeated for even more verbosity)\n";
	errs() << " -d ........ dump generated LLVM IR-code (repeat to dump more code)\n";
	errs() << " -c ........ compile to optimized object file\n";
	errs() << " -g ........ compile with debug information\n";
	errs() << " -j ........ start interactive JIT session despite provided file(s)\n";
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
std::vector<std::string> TestFunctions = {};
bool jit_repl = false;
promptcolor_t p_col = { 30, 100, 236 };
char* output_file = nullptr;

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

#if defined (_MSC_VER)
// We have to switch to code page 65001 to enable UTF-8. This
// value here is used to restore the old state on exit
unsigned old_cp;
#endif

int main(int argc, char* argv[]) {
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
	while ((opt = getopt(argc, argv, "vdcgji:o:t:P:")) != -1) {
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
		case 'j':
			if (comp_mode && comp_mode != comp_jit)
				compile_mode_conflict(argv[0]);
			comp_mode = comp_jit;
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
			source_files.push_back(optarg);
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
	if (output_file) {
		if (comp_mode == comp_jit) {
			errs() << "output file ('-o ...') not supported for JIT compilation\n";
			usage(argv[0]);
		}
	} else {
		if (comp_mode != comp_jit) {
			if (source_files.size() != 1) {
				errs() << "output file name (-o ...) required if "
				       << (source_files.size() ? "more than one" : "no")
				       << " input file provided\n";
				usage(argv[0]);
			}
			int len = strlen(input_file_name);
			output_file = (char*)malloc(len + 3);
			strcpy(output_file, input_file_name);
			if(output_file[len-3]=='.' && output_file[len-2]=='v' && output_file[len-1]=='x') {
				output_file[len-2] = 'o';
				output_file[len-1] = '\0';
			} else {
				output_file[len] = '.';
				output_file[len+1] = 'o';
				output_file[len+2] = '\0';
			}
			GlobalExprList = std::vector<std::unique_ptr<ExprAST>>{};
		}
	}
	if (source_index < source_files.size()) {
		input_file_name = source_files[source_index++].c_str();
		input_fd = open(input_file_name, O_CLOEXEC);
		if (input_fd < 0) {
			errs() << llvm::format("Cannot open input file \"%s\": %s\n", input_file_name, strerror(errno));
			exit(1);
		}
	}

	// always read builtin definitions first
	cur_input_fd = open(builtin_file_name, O_CLOEXEC);
	if (cur_input_fd < 0) {
		errs() << llvm::format("Cannot open definition file for builtins\"%s\": %s\n", builtin_file_name, strerror(errno));
		exit(1);
	}
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

	if (comp_mode != comp_jit) {
		if (auto FnAST = CreateMain("main")) {
			if (auto *FnIR = FnAST->codegen()) {
				if (dump_IR) {
					errs() << "Read function definition:\n";
					FnIR->print(errs());
					errs() << "\n";
				}
			} else {
				exit(1);
			}
		} else {
			exit(1);
		}
	}
	
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
		auto RM = llvm::Optional<llvm::Reloc::Model>();
		auto TheTargetMachine =
			Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);
	  
		TheModule->setDataLayout(TheTargetMachine->createDataLayout());
	  
		auto Filename = output_file;
		std::error_code EC;
		llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

		if (EC) {
			errs() << "Could not open file: " << EC.message();
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
	  
		outs() << "Wrote " << Filename << "\n";
	} else if (comp_mode == comp_dbg) {
		// Finalize the debug info.
		DBuilder->finalize();
		// Print out all of the generated code.
		TheModule->print(errs(), nullptr);
	}
#if defined (_MSC_VER)
	SetConsoleOutputCP(old_cp);
#endif
	return 0;
}
