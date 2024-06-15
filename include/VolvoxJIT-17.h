#pragma once
/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include <ctype.h>
#include <memory>

namespace llvm {
	namespace orc {

		class VolvoxJIT {
		private:
			std::unique_ptr<ExecutionSession> ES;

			DataLayout DL;
			MangleAndInterner Mangle;
			JITTargetMachineBuilder JTMB;
			RTDyldObjectLinkingLayer ObjectLayer;
			IRCompileLayer CompileLayer;

			JITDylib &MainJD;

		public:
			VolvoxJIT(std::unique_ptr<ExecutionSession> ES,
			          JITTargetMachineBuilder _JTMB, DataLayout DL)
				: ES(std::move(ES)), DL(std::move(DL)), Mangle(*this->ES, this->DL),
				  JTMB(_JTMB),
				  ObjectLayer(*this->ES,
				              []() { return std::make_unique<SectionMemoryManager>(); }),
				  CompileLayer(*this->ES, ObjectLayer,
				               std::make_unique<ConcurrentIRCompiler>(std::move(_JTMB))),
				  MainJD(this->ES->createBareJITDylib("<main>")) {
				MainJD.addGenerator(
					cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
						         DL.getGlobalPrefix())));
				if (JTMB.getTargetTriple().isOSBinFormatCOFF()) {
					ObjectLayer.setOverrideObjectFlagsWithResponsibilityFlags(true);
					ObjectLayer.setAutoClaimResponsibilityForObjectSymbols(true);
				}
			}

			~VolvoxJIT() {
				if (auto Err = ES->endSession())
					ES->reportError(std::move(Err));
			}

			static Expected<std::unique_ptr<VolvoxJIT>> Create(CodeGenOpt::Level codegenopt) {
				auto EPC = SelfExecutorProcessControl::Create();
				if (!EPC)
					return EPC.takeError();

				auto ES = std::make_unique<ExecutionSession>(std::move(*EPC));

				JITTargetMachineBuilder JTMB(
					ES->getExecutorProcessControl().getTargetTriple());
				JTMB.setCodeGenOptLevel(codegenopt);
				auto DL = JTMB.getDefaultDataLayoutForTarget();
				if (!DL)
					return DL.takeError();
				return std::make_unique<VolvoxJIT>(std::move(ES), std::move(JTMB),
				                                   std::move(*DL));
			}

			const DataLayout &getDataLayout() const { return DL; }

			JITDylib &getMainJITDylib() { return MainJD; }

			Error addModule(ThreadSafeModule TSM, ResourceTrackerSP RT = nullptr) {
				if (!RT)
					RT = MainJD.getDefaultResourceTracker();
				return CompileLayer.add(RT, std::move(TSM));
			}

			Expected<ExecutorSymbolDef> lookup(StringRef Name) {
				return ES->lookup({&MainJD}, Mangle(Name.str()));
			}

			Expected<std::unique_ptr<llvm::TargetMachine>> createTargetMachine() {
				return JTMB.createTargetMachine();
			}
		};

	} // end namespace orc
} // end namespace llvm
