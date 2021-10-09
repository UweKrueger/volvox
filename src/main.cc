#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

CompModes comp_mode = comp_obj;

DebugInfo KSDbgInfo;

TypeTable type_table;

//===----------------------------------------------------------------------===//
// Code Generation Globals
//===----------------------------------------------------------------------===//
static std::unique_ptr<llvm::LLVMContext> TheContext;
llvm::orc::ThreadSafeContext Context;
std::unique_ptr<llvm::Module> TheModule;
std::unique_ptr<llvm::IRBuilder<>> Builder;
static llvm::ExitOnError ExitOnErr;
__thread char* __volvox_jit_tls_ptr = nullptr;
__thread size_t __volvox_jit_tls_size = 0;
char* __volvox_jit_tls_inits = nullptr;

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
#ifdef __AVR_ARCH__
	type_table.add("int", llvm::Type::getInt16Ty(*Context.getContext()), true);
	type_table.add("uint", llvm::Type::getInt16Ty(*Context.getContext()));
	type_table.add("size", llvm::Type::getInt16Ty(*Context.getContext()), true);
	type_table.add("usize", llvm::Type::getInt16Ty(*Context.getContext()));
	type_table.add("real", llvm::Type::getFloatTy(*Context.getContext()));
#else
	type_table.add("int", llvm::Type::getInt32Ty(*Context.getContext()), true);
	type_table.add("uint", llvm::Type::getInt32Ty(*Context.getContext()));
	type_table.add("size", llvm::Type::getInt64Ty(*Context.getContext()), true);
	type_table.add("usize", llvm::Type::getInt64Ty(*Context.getContext()));
	type_table.add("real", llvm::Type::getDoubleTy(*Context.getContext()));
#endif
	type_table.add("void", llvm::Type::getVoidTy(*Context.getContext()));
	type_table.add("bool", llvm::Type::getInt1Ty(*Context.getContext()));
	type_table.add("i8", llvm::Type::getInt8Ty(*Context.getContext()), true);
	type_table.add("i16", llvm::Type::getInt16Ty(*Context.getContext()), true);
	type_table.add("i32", llvm::Type::getInt32Ty(*Context.getContext()), true);
	type_table.add("i64", llvm::Type::getInt64Ty(*Context.getContext()), true);
	type_table.add("u8", llvm::Type::getInt8Ty(*Context.getContext()));
	type_table.add("u16", llvm::Type::getInt16Ty(*Context.getContext()));
	type_table.add("u32", llvm::Type::getInt32Ty(*Context.getContext()));
	type_table.add("u64", llvm::Type::getInt64Ty(*Context.getContext()));
	type_table.add("f16", llvm::Type::getBFloatTy(*Context.getContext()));
	type_table.add("f32", llvm::Type::getFloatTy(*Context.getContext()));
	type_table.add("f64", llvm::Type::getDoubleTy(*Context.getContext()));
	type_table.add("string", llvm::Type::getInt8PtrTy(*Context.getContext()));
	// only for internal use:
	type_table.add("i*", llvm::Type::getInt64Ty(*Context.getContext()), true);
	type_table.add("f*", llvm::Type::getDoubleTy(*Context.getContext()));
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

void InitializeModuleAndPassManager() {
	// Open a new module.
	static bool has_run = false;
	if (!has_run) {
		TheContext = std::make_unique<llvm::LLVMContext>();
		Context = llvm::orc::ThreadSafeContext(std::move(TheContext));
		init();
		has_run = true;
	}
	TheModule = std::make_unique<llvm::Module>("my cool jit", *Context.getContext());
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
				fprintf(stderr, "Read function definition:\n");
				FnIR->print(llvm::errs());
				fprintf(stderr, "\n");
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
			fprintf(stderr, "Error compiling function definition\n");
		}
	} else {
		fprintf(stderr, "Error parsing function definition\n");
		// Skip token for error recovery.
		purgeLine();
	}
	locals_table[0].clear();
	locals_table = {};
	if (success)
		fprintf(stderr, "definition successfully handled\n");
	inside_function = false;
}

static void HandleExtern() {
	if (auto ProtoAST = ParseExtern()) {
		if (auto *FnIR = ProtoAST->codegen()) {
			if (comp_mode != comp_dbg) {
				fprintf(stderr, "Read extern: ");
				FnIR->print(llvm::errs());
				fprintf(stderr, "\n");
			}
			FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
		} else {
			fprintf(stderr, "Error reading extern");
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleTopLevelExpression() {
	// Evaluate a top-level expression into an anonymous function.
	if (auto FnAST = ParseTopLevelExpr()) {
		printf("top level expr parsed\n");
		auto RetType = FnAST->Proto->RetTypes.size() == 1 ?
			FnAST->Proto->RetTypes[0].first :
			llvm::Type::getVoidTy(*Context.getContext());
		unsigned ret_type_attr = FnAST->Proto->RetTypes.size() == 1 ?
			FnAST->Proto->RetTypes[0].second : 0;
		auto anon_expr = FnAST->codegen();
		if (anon_expr) {
			auto ret_type = anon_expr->getReturnType();
			auto RetTypeID = RetType->getTypeID();
			unsigned IntBitWidth = RetTypeID == llvm::Type::IntegerTyID ?
				RetType->getIntegerBitWidth() : 0;
			fprintf(stderr, "ExprType: %u BitWidth: %u Volvox: %u, %u, %u\n",
			        ret_type->getTypeID(), ret_type->isIntegerTy() ? ret_type->getIntegerBitWidth() : 0,
			        RetType->getTypeID(), RetType->isIntegerTy() ? RetType->getIntegerBitWidth() : 0,
			        ret_type_attr);
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
				// arguments, returns a double) so we can call it as a native function.
				//std::vector<llvm::GenericValue> Args;
				//llvm::GenericValue gv = TheJIT->runFunction(anon_expr, Args);
				switch (RetTypeID) {
				case llvm::Type::HalfTyID:
				case llvm::Type::BFloatTyID:
				case llvm::Type::FloatTyID: {
					float (*FP)() = (float (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
					fprintf(stderr, "Evaluated to %.7g\n", FP());
					break;
				}
				case llvm::Type::DoubleTyID: {
					double (*FP)() = (double (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
					fprintf(stderr, "Evaluated to %.15g\n", FP());
					break;
				}
				case llvm::Type::IntegerTyID: {
					if (ret_type_attr & A_signed) {
						switch (IntBitWidth) {
						case 1: { // this should actually be unsigned - put it here, too, just in case
							bool (*BOOL)() = (bool (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							bool b = BOOL();
							fprintf(stderr, "Evaluated to %s\n", b ? "true" : "false");
							break;
						}
						case 8: {
							signed char (*INT8)() = (signed char (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							signed char c = INT8();
							fprintf(stderr, "Evaluated to %hhd ('%c')\n", c, c);
							break;
						}
						case 16: {
							short (*INT16)() = (short (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							fprintf(stderr, "Evaluated to %hd\n", INT16());
							break;
						}
						case 32: {
							int (*INT32)() = (int (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							fprintf(stderr, "Evaluated to %d\n", INT32());
							break;
						}
						case 64: {
							long (*INT64)() = (long (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							fprintf(stderr, "Evaluated to %ld\n", INT64());
							break;
						}
						default:
							fprintf(stderr, "Expression has unsupported integer bit width %u\n", IntBitWidth);
						}
					} else {
						switch (IntBitWidth) {
						case 1: {
							bool (*BOOL)() = (bool (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							bool b = BOOL();
							fprintf(stderr, "Evaluated to %s\n", b ? "true" : "false");
							break;
						}
						case 8: {
							unsigned char (*UINT8)() = (unsigned char (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							unsigned char c = UINT8();
							fprintf(stderr, "Evaluated to %hhu ('%c')\n", c, c);
							break;
						}
						case 16: {
							unsigned short (*UINT16)() = (unsigned short (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							fprintf(stderr, "Evaluated to %hu\n", UINT16());
							break;
						}
						case 32: {
							unsigned (*UINT32)() = (unsigned (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							fprintf(stderr, "Evaluated to %u\n", UINT32());
							break;
						}
						case 64: {
							unsigned long (*UINT64)() = (unsigned long (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
							fprintf(stderr, "Evaluated to %lu\n", UINT64());
							break;
						}
						default:
							fprintf(stderr, "Expression has unsupported integer bit width %u\n", IntBitWidth);
						}
					}
					break;
				}
				case llvm::Type::PointerTyID: { // should be more sophisticated
					const char* (*SP)() = (const char* (*)())(intptr_t)UNWRAP(ExprSymbol.getAddress());
					fprintf(stderr, "Evaluated to >%s<\n", SP());
					break;
				}
				default:
					fprintf(stderr, "unknown expression type %d\n", RetTypeID);
				}
			
#if LLVM_VERSION_MAJOR >= 12
				// Delete the anonymous expression module from the JIT.
				ExitOnErr(RT->remove());
#else
				// Delete the anonymous expression module from the JIT.
				TheJIT->removeModule(H);
#endif
			}
		} else {
			fprintf(stderr, "Error generating code for top level expr\n");
		}
	} else {
		// Skip rest for error recovery.
		purgeLine();
	}
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
	while (true) {
		if (comp_mode == comp_jit) {
			fprintf(stderr, CurTok.kind == tok_eof ? "\n" : "ready> ");
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

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
	fputc((char)X, stderr);
	return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
	fprintf(stderr, "%f\n", X);
	return 0;
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

const char* input_file_name = "/dev/stdin";

int main(int argc, char* argv[]) {
	FILE* input_file = stdin;
	const char* input_file_name = "a.vx";
	if (argc == 1) {
		comp_mode = comp_jit;
	} else {
		if ((argc == 3) && std::string(argv[1]) == "-g") {
			comp_mode = comp_dbg;
			input_file_name = argv[2];
		} else {
			input_file_name = argv[1];
		}
		input_file = fopen(input_file_name, "r");
		if (!input_file) {
			fprintf(stderr, "Cannot open input file \"%s\": %s\n", input_file_name, strerror(errno));
			exit(1);
		}
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

	InitializeModuleAndPassManager();

	// Prime the first token.
	if (comp_mode == comp_jit) {
		fprintf(stderr, "ready> ");
	}
	getNextToken();

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
