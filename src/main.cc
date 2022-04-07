#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

CompModes comp_mode = comp_obj;

DebugInfo KSDbgInfo;

TypeTable type_table;

//===----------------------------------------------------------------------===//
// Code Generation Globals
//===----------------------------------------------------------------------===//

llvm::orc::ThreadSafeContext Context;
std::unique_ptr<llvm::Module> TheModule;
std::unique_ptr<llvm::IRBuilder<>> Builder;
static llvm::ExitOnError ExitOnErr;
thread_local char* __volvox_jit_tls_ptr = nullptr;
thread_local size_t __volvox_jit_tls_size = 0;
char* __volvox_jit_tls_inits = nullptr;

global_var_shadow* global_list = nullptr;
global_var_shadow** global_list_end = &global_list;

thread_local global_var_shadow* tl_global_list = nullptr;	

// useful definitions
llvm::Type* llvm_int_type;
llvm::Type* llvm_size_type;
volvox::FullType* void_type;

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
	type_table.add("i*", llvm::Type::getInt64Ty(*Context.getContext()), nullptr, A_signed);
	type_table.add("f*", llvm::Type::getDoubleTy(*Context.getContext()), nullptr);

#if UINTPTR_MAX == UINT16_MAX // e.g. AVR platform
	type_table.add("int", llvm::Type::getInt16Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("int", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("uint", llvm::Type::getInt16Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("uint", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_int_type = llvm::Type::getInt16Ty(*Context.getContext());
	type_table.add("size", llvm::Type::getInt16Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("size", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("usize", llvm::Type::getInt16Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("usize", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_size_type = llvm::Type::getInt16Ty(*Context.getContext());
	type_table.add("real", llvm::Type::getFloatTy(*Context.getContext()), DBuilder ? DBuilder->createBasicType("real", 32, llvm::dwarf::DW_ATE_float) : nullptr);
#else
	type_table.add("int", llvm::Type::getInt32Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("uint", llvm::Type::getInt32Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("uint", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_int_type = llvm::Type::getInt32Ty(*Context.getContext());
#if UINTPTR_MAX == UINT32_MAX
	type_table.add("size", llvm::Type::getInt32Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("size", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("usize", llvm::Type::getInt32Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("usize", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_size_type = llvm::Type::getInt32Ty(*Context.getContext());
#else
	type_table.add("size", llvm::Type::getInt64Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("size", 64, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("usize", llvm::Type::getInt64Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("usize", 64, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	llvm_size_type = llvm::Type::getInt64Ty(*Context.getContext());
#endif
	type_table.add("real", llvm::Type::getDoubleTy(*Context.getContext()), DBuilder ? DBuilder->createBasicType("real", 64, llvm::dwarf::DW_ATE_float) : nullptr);
#endif
	type_table.add("void", llvm::Type::getVoidTy(*Context.getContext()), nullptr);
	// TODO: make .add() return FullType*
	void_type = type_table.get_full("void");
	type_table.add("bool", llvm::Type::getInt1Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("bool", 1, llvm::dwarf::DW_ATE_boolean) : nullptr);
	type_table.add("i8", llvm::Type::getInt8Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("i8", 8, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("i16", llvm::Type::getInt16Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("i16", 16, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("i32", llvm::Type::getInt32Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("i32", 32, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("i64", llvm::Type::getInt64Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("i64", 64, llvm::dwarf::DW_ATE_signed) : nullptr, A_signed);
	type_table.add("u8", llvm::Type::getInt8Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("u8", 8, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("u16", llvm::Type::getInt16Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("u16", 16, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("u32", llvm::Type::getInt32Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("u32", 32, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("u64", llvm::Type::getInt64Ty(*Context.getContext()), DBuilder ? DBuilder->createBasicType("u64", 64, llvm::dwarf::DW_ATE_unsigned) : nullptr);
	type_table.add("f16", llvm::Type::getBFloatTy(*Context.getContext()), DBuilder ? DBuilder->createBasicType("f16", 16, llvm::dwarf::DW_ATE_float) : nullptr);
	type_table.add("f32", llvm::Type::getFloatTy(*Context.getContext()), DBuilder ? DBuilder->createBasicType("f32", 32, llvm::dwarf::DW_ATE_float) : nullptr);
	type_table.add("f64", llvm::Type::getDoubleTy(*Context.getContext()), DBuilder ? DBuilder->createBasicType("f64", 64, llvm::dwarf::DW_ATE_float) : nullptr);
	type_table.add("string", llvm::Type::getInt8PtrTy(*Context.getContext()),
	               DBuilder ? DBuilder->createPointerType(DBuilder->createBasicType("i8", 8, llvm::dwarf::DW_ATE_signed_char), 64, 0, llvm::None, "string") : nullptr);
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

void InitializeModuleAndPassManager() {
	// Open a new module.
	TheModule = std::make_unique<llvm::Module>(input_file_name, *Context.getContext());
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
	Builder = std::make_unique<llvm::IRBuilder<>>(*Context.getContext());

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
			if (comp_mode != comp_dbg) {
				eprt("Read function definition:\n");
				FnIR->print(llvm::errs());
				eprt("\n");
				if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
					ExitOnErr(TheJIT->addModule(
						          llvm::orc::ThreadSafeModule(std::move(TheModule), Context)));
#else
					TheJIT->addModule(std::move(TheModule));
#endif
					InitializeModuleAndPassManager();
				}
			}
			success = true;
		} else {
			eprt("Error compiling function definition\n");
		}
	} else {
		eprt("Error parsing function definition\n");
		// Skip token for error recovery.
		purgeLine();
	}
	locals_table[0].clear();
	locals_table = {};
	if (success)
		dprt("definition successfully handled\n");
	inside_function = false;
}

static void HandleExtern() {
	if (auto ProtoAST = ParseExtern()) {
		if (auto *FnIR = ProtoAST->codegen()) {
			if (comp_mode != comp_dbg) {
				eprt("Read extern: ");
				FnIR->print(llvm::errs());
				eprt("\n");
			}
			FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
		} else {
			eprt("Error reading extern");
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleTypeDef() {
	getNextToken(); // eat type
	if (CurTok.kind != tok_identifier) {
		eprt("unexpected `%` in type declaration - type name expected\n", CurTok.str().c_str());
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

static bool spawn_bool_expr(bool (*expr)()) {
	pthread_t thread;
	int res = pthread_create(&thread, NULL, anon_expr_wrapper, (void*)expr);
	void* retval;
	res = pthread_join(thread, &retval);
	return !(!retval);
}

static void HandleTopLevelExpression() {
	// Evaluate a top-level expression into an anonymous function.
	if (auto FnAST = ParseTopLevelExpr()) {
		if (auto anon_expr = FnAST->codegen()) {
			auto ret_type = anon_expr->getReturnType();
			if (!anon_expr->getReturnType()->isIntegerTy() || !anon_expr->getReturnType()->getIntegerBitWidth() == 1) {
				eprt("internal error: anonymous function does not return `bool`\n");
				return;
			}
			if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
				// Create a ResourceTracker to track JIT'd memory allocated to our
				// anonymous expression -- that way we can free it after executing.
				auto RT = TheJIT->getMainJITDylib().createResourceTracker();
				auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), Context);
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
				eprt("Evaluated to %s\n", b ? "true" : "false");
#if LLVM_VERSION_MAJOR >= 12
				// Delete the anonymous expression module from the JIT.
				ExitOnErr(RT->remove());
#else
				// Delete the anonymous expression module from the JIT.
				TheJIT->removeModule(H);
#endif
			}
		} else {
			eprt("Error generating code for top level expr\n");
		}
	} else {
		// Skip rest for error recovery.
		purgeLine();
	}
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
	while (true) {
		if (comp_mode == comp_jit && cur_input_fd == 0) {
			eprt(CurTok.kind == tok_eof ? "\n" : "ready> ");
		}
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
			HandleTopLevelExpression();
			break;
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

extern "C" DLLEXPORT void new_global_var_shadow(void* adr, size_t size) {
	size_t alloc_size = sizeof(global_var_shadow);
	if (size > 8)
		alloc_size = alloc_size - 8 + size;
	global_var_shadow* V = (global_var_shadow*)malloc(alloc_size);
	V->next = nullptr;
	V->adr = adr;
	V->size = size;
	memcpy(V->data, V->adr, size);
}

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
	fputc((char)X, stderr);
	return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
	eprt("%f\n", X);
	return 0;
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

int main(int argc, char* argv[]) {
	if (argc == 1) {
		comp_mode = comp_jit;
	} else {
		if ((argc == 3) && std::string(argv[1]) == "-g") {
			comp_mode = comp_dbg;
			input_file_name = argv[2];
		} else {
			input_file_name = argv[1];
		}
		input_fd = open(input_file_name, O_CLOEXEC);
		if (input_fd < 0) {
			eprt("Cannot open input file \"%s\": %s\n", input_file_name, strerror(errno));
			exit(1);
		}
	}
	// always read builtin definitions first
	cur_input_fd = open(builtin_file_name, O_CLOEXEC);
	if (cur_input_fd < 0) {
		eprt("Cannot open definition file for builtins\"%s\": %s\n", builtin_file_name, strerror(errno));
		exit(1);
	}

	int len = strlen(input_file_name);
	auto output_file = (char*)malloc(len + 3);
	strcpy(output_file, input_file_name);
	if(output_file[len-3]=='.' && output_file[len-2]=='v' && output_file[len-1]=='x') {
		output_file[len-2] = 'o';
		output_file[len-1] = '\0';
	} else {
		output_file[len] = '.';
		output_file[len+1] = 'o';
		output_file[len+2] = '\0';
	}
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

	Context = llvm::orc::ThreadSafeContext(std::move(std::make_unique<llvm::LLVMContext>()));
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
			llvm::errs() << Error;
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
			llvm::errs() << "Could not open file: " << EC.message();
			return 1;
		}
	  
		llvm::legacy::PassManager pass;
		auto FileType = llvm::CGFT_ObjectFile;
	  
		if (TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
			llvm::errs() << "TheTargetMachine can't emit a file of this type";
			return 1;
		}
	  
		pass.run(*TheModule);
		dest.flush();
	  
		llvm::outs() << "Wrote " << Filename << "\n";
	} else if (comp_mode == comp_dbg) {
		// Finalize the debug info.
		DBuilder->finalize();
		// Print out all of the generated code.
		TheModule->print(llvm::errs(), nullptr);
	}
	return 0;
}
