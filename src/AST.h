/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#pragma once

#if defined(__GLIBC__) && __GLIBC__ == 2 && __GLIBC_MINOR__ < 38
// very old GLIBC lacks secure BSD string functions - replace with similar ones
#define strlcpy(dst, src, bufsz) strncpy(dst, src, bufsz)
#define strlcat(dst, src, bufsz) strncat(dst, src, bufsz - strlen(dst) - 1)
#elif defined(_MSC_VER)
// WinAPI does provide secure string functions but with different names and APIs
#define strlcpy(dst, src, bufsz) strcpy_s(dst, bufsz, src)
#define strlcat(dst, src, bufsz) strcat_s(dst, bufsz, src)
#endif

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//

inline static llvm::Value* handle(llvm::Value* target, llvm::Value* val) {
	if (!val)
		return nullptr;
	if (!target || (intptr_t)target == -1)
		return val;
	Builder->CreateStore(val, target);
	return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
}

inline static void handle_d_0(volvoxc::FullType* ft, llvm::Value* target) {
	if (ft->type_attr & A_destructor) {
		auto destructor = getConstructorOrDestructor(ft, true);
		FullVar tmp = {
			.val = target,
			.destructor = destructor,
			.ft = *ft
		};
		expr_temps.push_back(tmp);		
	}
}

inline static llvm::Value* handle_d(llvm::Value* target, llvm::Value* val, unsigned attribs) {
	if (!target || (intptr_t)target == -1) {
		if (!target && (attribs & (A_destructor | A_map | A_string))) {
			FullVar tmp = {
				.val = val,
				.ft = {
					.type = llvm_ptr_type,
					.type_attr = (attribs & (A_destructor | A_map | A_string)) | A_rvalue
				}
			};
			expr_temps.push_back(tmp);
		}
		return val;
	}
	Builder->CreateStore(val, target);
	return llvm::UndefValue::get(llvm::Type::getVoidTy(Context));
}

/// ExprAST - Base class for all expression nodes.

class InterfaceExprAST : public ExprAST {
	std::unique_ptr<ExprAST> expr;
	volvoxc::FullType* interface_ft;
	llvm::Value* rttype = nullptr;
public:
	InterfaceExprAST(std::unique_ptr<ExprAST> _expr, volvoxc::FullType* interface_ft = nullptr) :
		ExprAST(interface_type, _expr->Loc, false), expr(std::move(_expr)), interface_ft(interface_ft) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr);
};

// internal AST node to hold an already (i.e. no more changing) evaluated
// expression value
//
class ConstExprAST : public ExprAST {
	llvm::Value* val;
public:
	ConstExprAST(llvm::Value* val) : val(val) {
		if (!val)
			errs() << "ConstExprAST: no valid value\n";
		else
			ft->type = val->getType();
	}
	ConstExprAST(SourceLocation Loc, volvoxc::FullType* full_type, llvm::Value* val, bool is_unknown_type = false)
		: ExprAST(full_type, Loc, is_unknown_type), val(val) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) { return val; }
};

// empty parameter list in calls like "f()"
class EmptyExprAST : public ExprAST {
public:
	EmptyExprAST(SourceLocation Loc = CurLoc) : ExprAST(Loc) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) { return nullptr; }
};

class IdentExprAST : public ExprAST {
public:
	std::string Name;
	IdentExprAST(SourceLocation Loc, std::string _Name) : ExprAST(Loc), Name(std::move(_Name)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) { return nullptr; }
};

class ModuleExprAST : public ExprAST {
public:
	SourceLocation importLoc;
	std::string Name;
	ModuleExprAST(SourceLocation Loc, SourceLocation importLoc, std::string _Name) : ExprAST(Loc), importLoc(importLoc), Name(std::move(_Name)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) { return nullptr; }
};

// Class for all literals - 1.2, 3u, "str"
class LiteralExprAST : public ExprAST {

public:
	union LitValue Val;
	LiteralExprAST(Token&& tok, SourceLocation Loc = CurLoc)
		: ExprAST(tok.key, (((tok.int_type.ID == llvm::Type::IntegerTyID &&
		     tok.int_type.is_signed) || tok.kind == tok_ptr_lit) ? A_signed : 0),
		          Loc, tok.is_unknown_type), Val(tok.Val) {
		if (tok.kind == tok_str_lit) {
			ft->type_attr |= A_string;
			tok.Val.Ptr = nullptr; // has been moved manually - avoid free by destructor
		}
	}
	~LiteralExprAST() {
		if (ft->type && ft->type->getTypeID() == llvm::Type::PointerTyID && !(ft->type_attr & A_signed))
			free((void*)Val.Ptr);
	}
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		switch (ft->type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			if (ft->type_attr & A_signed)
				return ExprAST::dump(out << Val.Int, ind);
			else
				return ExprAST::dump(out << Val.Uint, ind);
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return ExprAST::dump(out << Val.Float, ind);
		case llvm::Type::PointerTyID:
			if (ft->type_attr & A_signed)
				return ExprAST::dump(out << Val.Ptr, ind);
			else
				return ExprAST::dump(out << Val.CStr, ind);
		default:
			errs() << "internal compiler error: unhandled literal type '" << *ft->type << "'\n";
			return out;
		}
	}
#endif
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
};

// interpolated string literals - "Result: $x, Average: ${x+y+z/3}"
class InterpStrLitExprAST : public ExprAST {
public:
	std::vector<char*> str_parts;
	std::vector<std::tuple<std::unique_ptr<ExprAST>,std::unique_ptr<ExprAST>,std::unique_ptr<ExprAST>,unsigned>> interpolations;
	InterpStrLitExprAST(SourceLocation Loc, std::vector<char*> _str_parts,
		std::vector<std::tuple<std::unique_ptr<ExprAST>,std::unique_ptr<ExprAST>,std::unique_ptr<ExprAST>,unsigned>> _ints)
		: ExprAST(string_type, Loc), str_parts(std::move(_str_parts)),
		  interpolations(std::move(_ints)) {}
	~InterpStrLitExprAST() {
		for (auto p: str_parts)
			free(p);
	}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
};

inline bool is_cfn(std::vector<std::unique_ptr<PrototypeAST>>* Proto) {
	return Proto && (*Proto).size() == 1 && ((*Proto)[0]->getName().c_str()[0] != '_' || (*Proto)[0]->getName().c_str()[1] != 'Z');
}

inline bool is_ccfn(std::vector<std::unique_ptr<PrototypeAST>>* Proto) {
	return Proto && (*Proto).size() >= 1 && (*Proto)[0]->getName().c_str()[0] == '_' && (*Proto)[0]->getName().c_str()[1] == 'Z';
}

// Usually we need a reference of an object to use it as an Lvalue (see LvalueExprAST below)
// However there are also function references / closures that are created with a CallExpr
//
class ReferencableExprAST : public ExprAST {
protected:
	std::pair<llvm::Type*,llvm::Value*> ref_cache = { nullptr, nullptr };
public:
	std::string Name;
	ReferencableExprAST(SourceLocation Loc, std::string Name = "")
		: ExprAST(Loc), Name(std::move(Name)) {}
	ReferencableExprAST(llvm::Type* type = llvm::Type::getVoidTy(Context), unsigned type_attr = 0,
	                    SourceLocation Loc = CurLoc, bool is_unknown_type = false)
		: ExprAST(type, type_attr, Loc, is_unknown_type) {}
	// get a reference to the value
	// if this is an rvalue and silent_fail=true then the llvm::Type is returned
	// but the llvm::Value is NULL
	// constref = true means generated reference is not used to modify object
	virtual std::pair<llvm::Type*,llvm::Value*> codegen_ref_(
		bool silent_fail = false, bool constref = false) {
		return { nullptr, nullptr };
	}
	std::pair<llvm::Type*,llvm::Value*> codegen_ref(
		bool silent_fail = false, bool constref = false) {
		if (ref_cache.first) {
			if (!ref_cache.second && !silent_fail)
				errs() << Loc << ": cannot get reference\n";
			return ref_cache;
		}
		// auto res = codegen_ref_(silent_fail, constref);
		// if (res.second)
		// 	errs() << Loc << ": got reference " << *res.second << "\n";
		// return res;
		return codegen_ref_(silent_fail, constref);
	}
	virtual std::vector<llvm::Value*> _getAllocSize(llvm::Type** el_ty = nullptr);
	virtual llvm::Value* getAllocSize(llvm::Type** el_ty = nullptr) override;
	virtual VariableExprAST* getBase() { return nullptr; }
};

// Expressions that can be the LHS of an assignment: `a = 1`, `b[3] = 4.5`, `s.a = 9`
// However, there are exceptions: LvalueExprAST is base class for IndexExprAST and SelectExprAST
// but it is possible that the array/struct is in fact an rvalue. These cases need special
// treatment is the derived classes. Here we provide an option 'silent_fail' for 'codegen_ref()'
// that makes the method return a type but no pointer in these cases.
// Usually generating the reference of an expression
//
class LvalueExprAST : public ReferencableExprAST {
public:
	LvalueExprAST(SourceLocation Loc, std::string Name = "")
		: ReferencableExprAST(Loc, std::move(Name)) {}
	bool error_already_printed = false;
	std::pair<llvm::Type*,std::unique_ptr<std::vector<llvm::Value*>>> codegen_dims() override;
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	virtual llvm::Value* ref2val(std::pair<llvm::Type*,llvm::Value*> ref) {
		if (ref.second && ref.first->isSized() && TheModule->getDataLayout().getTypeAllocSize(ref.first) > 0)
			return Builder->CreateLoad(ref.first, ref.second, Name.c_str());
		else
			return nullptr;
	}
};

// internal AST node to hold an already (i.e. no more changing) evaluated
// lvalue expression value
//
class ConstLvalueAST : public LvalueExprAST {
public:
	ConstLvalueAST(SourceLocation Loc, volvoxc::FullType* _ft, llvm::Type* type, llvm::Value* ref_val, std::string Name = "")
		: LvalueExprAST(Loc, std::move(Name))
		{
			ref_cache.first = type;
			ref_cache.second = ref_val;
			ft = _ft;
		}
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public LvalueExprAST {

public:
	FullVar* full_var; // description in local or global database
	VariableExprAST(SourceLocation Loc, const std::string &Name)
		: LvalueExprAST(Loc, Name), full_var(lookup_var(Name.c_str())) {
		if (full_var) {
			ft = &full_var->ft;
			if (ft->type_attr & A_untyped)
				is_unknown_type = true;
		}
		// if the variable name has not found in the databases we don't generate
		// an error message here because this VariableExprAST could be the LHS of
		// an initialization e.g. `a = 42`
	}
	VariableExprAST(SourceLocation Loc, const std::string &Name, FullVar* fv)
		: LvalueExprAST(Loc, Name), full_var(fv) {
		if (fv) {
			ft = &fv->ft;
			if (ft->type_attr & A_untyped)
				is_unknown_type = true;
		}
		else
			ft = nullptr;
	}
	const std::string &getName() const { return Name; }
	VariableExprAST* getBase() override { return this; }
	// create reference to this variable - second result is the storage_type
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false, bool constref = false) override;
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	llvm::Value* codegen(bool suppress_destructor = false) override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		return ExprAST::dump(out << Name, ind);
	}
#endif
};

class RefExprAST : public LvalueExprAST {
	llvm::Value* ref;
	VariableExprAST* var;
public:
	RefExprAST(SourceLocation Loc, volvoxc::FullType* _ft, VariableExprAST* var, llvm::Value* ref, std::string Name = "") : LvalueExprAST(Loc, Name), var(var), ref(ref) {
		if (!ref) {
			errs() << "RefExprAST: no valid value\n";
			ft->type = nullptr;
		} else {
			*ft = *_ft;
			ft->type_attr |= A_ref;
		}
	}
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false, bool constref = false) override {
		return { ft->type, ref };
	}
	VariableExprAST* getBase() override { return var; }
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ReferencableExprAST {
public:
	std::vector<FnArg> fn_args;
	std::unique_ptr<ExprAST> Callee;
	std::vector<std::unique_ptr<ExprAST>> Args;
	PrototypeAST* Proto = nullptr;
	ssize_t vtable_offs = -1;
	CallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Callee_,
	            std::vector<std::unique_ptr<ExprAST>> Args = {});
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	bool needs_target() override { return Proto && (Proto->IsStructRet || ((Proto->visibility & A_constructor) && Proto->returnName.empty())); }
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "call", ind);
		Callee->dump(out, ind);
		for (const auto &Arg : Args)
			Arg->dump(indent(out, ind + 1), ind + 1);
		return out;
	}
#endif
};

/// FunctionAST - This class represents a function definition itself.
class FunctionAST {
public:
	volvoxc::FullType* receiver_ft;
	llvm::Function* TheFunction;
	llvm::BasicBlock* BB;
	unsigned ArgIdx;
	volvoxc::FullType* ret_ft;
	llvm::Value* this_ret_ptr = nullptr;
	llvm::Value* RetVal = nullptr;
	llvm::Value* InterRetVal = nullptr;
	FullVar* RetVar = nullptr;
	FunctionAST* old_currentFunction = nullptr;
	volvoxc::FullType* old_theFunction_ret_ft = nullptr;
	bool old_theFunction_struct_ret = false;
	bool prepare_codegen();
	bool process_body(std::vector<std::unique_ptr<ExprAST>>& thisBody);
	llvm::Function* finish_codegen(bool finishModule = false, bool getNewModule = false);
	llvm::Function* cleanup_codegen();
	PrototypeAST* Proto = nullptr;
	std::string unmangledName;
	std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription> bBody;
	std::vector<std::unique_ptr<ExprAST>>& Body;
	int return_val_idx = -1;
	FunctionAST(PrototypeAST* Proto,
	            std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription> _bBody, std::string unmName, int return_val_idx = -1)
		: Proto(Proto), bBody(std::move(_bBody)), Body(bBody.first),
		  unmangledName(std::move(unmName)),
		  return_val_idx(return_val_idx) {}
	llvm::Function* codegen(bool finishModule = false, bool getNewModule = false) {
		if (prepare_codegen() && process_body(Body))
			return finish_codegen(finishModule, getNewModule);
		else
			return cleanup_codegen();
	}
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) {
		indent(out, ind) << "FunctionAST\n";
		++ind;
		indent(out, ind) << "Body:";
		if (Body.size())
			for (const auto& expr : Body)
				expr->dump(out, ind);
		else
			out << "null\n";
		return out;
	}
#endif
	~FunctionAST() = default;
};

/// FunctionExprAST - classic named functions (not function pointers)
class FunctionExprAST : public ExprAST {

public:
	std::unique_ptr<ExprAST> Exponent = nullptr;
	std::unique_ptr<FunctionAST> Func = nullptr;
	std::string Name;
	bool need_address = false; // for JIT repl - trigger codegen if function reference is desired
	FunctionExprAST(SourceLocation Loc, const std::string &Name, std::vector<std::unique_ptr<PrototypeAST>>* Protos, int selected_proto = 0)
		: ExprAST(Loc), Name(Name) {
		ft = new_FullType((*Protos)[selected_proto]->FT, 0);
		ft->selected_proto = selected_proto;
		ft->Protos = Protos;
	}
	// function references are created by a pseudo call expression (to be able to match the signature)
	FunctionExprAST(CallExprAST* call)
		: ExprAST(*call->Callee), Name(call->Proto->Name), need_address(true) {
		ft = new_FullType(call->Proto->FT, 0);
		ft->Protos = new_AnonProto(call->Proto, call->Loc);
	}
	FunctionExprAST(SourceLocation Loc, std::unique_ptr<FunctionAST> _Func)
		: ExprAST(Loc), Func(std::move(_Func)), Name(Func->Proto->Name) {
		ft = new_FullType(Func->Proto->FT, 0);
		ft->Protos = new_AnonProto(Func->Proto, Loc);
	}
	const std::string &getName() const { return Name; }
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		return ExprAST::dump(out << Name, ind);
	}
#endif
};

// regular method calls and interface methos calls
class MethodExprAST : public FunctionExprAST {
public:
	std::unique_ptr<ExprAST> Receiver;
	std::unique_ptr<IdentExprAST> Method;
	MethodExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Receiver, std::unique_ptr<IdentExprAST> _Method, std::vector<std::unique_ptr<PrototypeAST>>* Protos, std::vector<size_t>* vtable_idx = nullptr)
		: FunctionExprAST(Loc, _Method->Name, Protos), Receiver(std::move(_Receiver)), Method(std::move(_Method)) {}
};

// struct field selection like 'struct.field'
class SelectExprAST : public LvalueExprAST {
public:
	std::unique_ptr<ExprAST> Struct;
	std::unique_ptr<IdentExprAST> Field;
	const char* FieldName = nullptr;
	unsigned FieldIndex = (unsigned)(-1);
	SelectExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Struct, std::unique_ptr<IdentExprAST> _Field) :
		LvalueExprAST(Loc), Struct(std::move(_Struct)), Field(std::move(_Field))
		{
			FieldName = Field->Name.c_str();
			if (Struct->ft && Struct->ft->type) {
				if (auto struct_type = llvm::dyn_cast<llvm::StructType>(Struct->ft->type)) {
					// for regular method call (having regular struct or interface as receiver)
					// 'getSelect()' in parser.cc creates a MethodExprAST (see above)
					// here we handle compiler built-in methods and regular struct fields
					if (Struct->ft->type_attr & A_thread) {
						if (!strcmp(FieldName, "wait")) {
							FieldIndex = 0;
							ft = Struct->ft->elem_type;
						} else if (!strcmp(FieldName, "kill")) {
							FieldIndex = 1;
							ft = void_type;
						} else {
							errs() << Struct->Loc << ": threads do not have a method '" << FieldName << "'\n";
							ft = nullptr;
						}
					} else if (MapValue* mv = map_string_get(Struct->ft->fields, FieldName)) {
						FieldIndex = *(unsigned*)((char*)mv + mv->offset);
						char* adr = (char*)mv + mv->offset + 4;
						memcpy(&ft, adr, sizeof(void*));
					} else {
						llvm::StringRef struct_name = struct_type->hasName() ?
							struct_type->getName() :
							"<anonymous>";
						errs() << Struct->Loc << ": struct type '" << struct_name << "' has no field named '"
						       << FieldName << "'\n";
						ft = nullptr;
					}
				} else if (Struct->ft->type_attr & A_string) {
					if (!strcmp(FieldName, "size")) {
						FieldIndex = 0;
						ft = size_type;
					} else if (!strcmp(FieldName, "len")) {
						FieldIndex = 1;
						ft = size_type;
					} else {
						errs() << Struct->Loc << ": strings do not have a property '" << FieldName << "'\n";
						ft = nullptr;
					}
				} else if ((Struct->ft->type_attr & A_complex) && Struct->ft->type == llvm_c32_type) {
					if (!strcmp(FieldName, "real")) {
						FieldIndex = 0;
						ft = f32_type;
					} else if (!strcmp(FieldName, "imag")) {
						FieldIndex = 1;
						ft = f32_type;
					} else {
						errs() << Struct->Loc << ": c32 objects do not have a property '" << FieldName << "'\n";
						ft = nullptr;
					}
				} else if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(Struct->ft->type)) {
					if (!strcmp(FieldName, "size")) {
						FieldIndex = 0;
						ft = size_type;
					} else if (!strcmp(FieldName, "order")) {
						FieldIndex = 1;
						ft = integer_type;
					} else if (!strcmp(FieldName, "dim")) {
						auto fntype = llvm::FunctionType::get(llvm_size_type, { llvm_int_type }, false);
						ft = new_FullType(fntype, 0);
						ft->Protos = int_int_proto;
					} else {
						errs() << Struct->Loc << ": arrays do not have a property '" << FieldName << "'\n";
						ft = nullptr;
					}
				} else {
					errs() << Struct->Loc << ": LHS of '.' must be a struct (not " << *Struct->ft->type << ")\n";
					ft = nullptr;
				}
			} else {
				errs() << Struct->Loc << ": LHS of '.' has no defined type\n";
				ft = nullptr;
			}
		}
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false, bool constref = false) override;
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	llvm::Value* codegen_complex(llvm::Value* target = nullptr);
	llvm::Value* codegen_thread_wait(llvm::Value* control_block, llvm::Value* thread_handle);
	llvm::Value* codegen_thread_kill(llvm::Value* control_block, llvm::Value* thread_handle);
	VariableExprAST* getBase() override {
		if (auto lval = dynamic_cast<LvalueExprAST*>(Struct.get()))
			return lval->getBase();
		return nullptr;
	}
};

extern std::unique_ptr<ExprAST> getSelect(SourceLocation Loc, std::unique_ptr<ExprAST> LHS, std::unique_ptr<IdentExprAST> Ident);

// IndexExprAST - Expressions like x[2] or y["key"]
class IndexExprAST : public LvalueExprAST {
public:
	std::unique_ptr<ExprAST> Field, Index;
	llvm::Type* ml_elem_type = nullptr;
	int num_dims_to_strip_from_val = 0;
	bool const_ref;
	IndexExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Field_,
	             std::unique_ptr<ExprAST> Index_, bool const_ref = false) :
		LvalueExprAST(Loc), Field(std::move(Field_)), Index(std::move(Index_)), const_ref(const_ref)
		{
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
				llvm::Type* elem_type = array_type->getElementType();
				if (elem_type == Field->ft->elem_type->type) {
					*ft = *Field->ft->elem_type;
				} else {
					*ft = *Field->ft;
					ft->type = elem_type;
				}
				return;
			} else if (auto a_type = llvm::dyn_cast<llvm::PointerType>(Field->ft->type)) {
				if (Field->ft->type_attr & A_map) {
					ft = &Field->ft->elem_type[1];
					return;
				} else {
					errs() << Loc << ": invalid index expression\n";
					ft = nullptr;
				}
			} else if (Field->ft->type == llvm_vec_type) {
				ft = Field->ft->elem_type;
				return;
			}
			errs() << Index->Loc << ": index for non array expression " << *Field->ft << ' ' << Field->ft->type_attr << "\n";
			ft->type = nullptr;
		}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false, bool constref = false) override;
	std::vector<llvm::Value*> _getAllocSize(llvm::Type** el_ty = nullptr) override;
	VariableExprAST* getBase() override {
		if (auto lval = dynamic_cast<LvalueExprAST*>(Field.get()))
			return lval->getBase();
		return nullptr;
	}
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "index", ind);
		Field->dump(indent(out, ind) << "Field:", ind + 1);
		Index->dump(indent(out, ind) << "Index:", ind + 1);
		return out;
	}
#endif
};

class ListExprAST : public ExprAST {
public:
	std::vector<std::unique_ptr<ExprAST>> Elements; // x, y, z in type{x, y, z}
	ListExprAST(SourceLocation Loc, std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	            volvoxc::FullType* _ft = nullptr) :
		ExprAST(_ft, Loc), Elements(std::move(_Elements)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	unsigned getNumElements() { return Elements.size(); }
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "Expression List: " << Elements.size(), ind);
		for (const auto &Element : Elements)
			Element->dump(indent(out, ind + 1), ind + 1);
		return out;
	}
#endif
};

class ExprListIterator {
	ListExprAST* list = nullptr;
	bool struct_err = false;
	TokenKind kind = TokenKind('[');
	bool explicit_order = false;
public:
	std::vector<std::unique_ptr<ExprAST>> Dims; // Dims.size() = order of tensor - 1 for vector, 2 for matrix, ...
	                                            // Dims[i] = dimension of tensor in level 'i'
	std::vector<unsigned> LitDims; // maximum used index in literal for each level
	std::unique_ptr<ExprAST> Cap;
	std::unique_ptr<ExprAST> Init;
	std::vector<std::unique_ptr<ExprAST>> prepare_list(std::vector<std::unique_ptr<ExprAST>> Elems, unsigned depth);
	std::vector<ExprAST*> valid_exprs;
	ExprListIterator(std::vector<std::unique_ptr<ExprAST>> _Dims) : Dims(std::move(_Dims)) {
		if (Dims.size()) {
			explicit_order = true;
		} else {
			explicit_order = false;
			Dims.push_back(nullptr);
		}
	}
	bool struct_error() const { return struct_err; }
};

class AggregateExprAST : public ListExprAST {
public:
	llvm::Type* key_type = nullptr;
	unsigned key_type_attr = 0;
	std::vector<ExprAST*> valid_exprs; // to find a common element type
	std::vector<unsigned> LitDims; // maximum used index in literal for each level
	AggregateExprAST(SourceLocation Loc, llvm::Type* key_type,
	                 unsigned key_type_attr = 0,
	                 std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                 std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {},
	                 volvoxc::FullType* el_type = nullptr) :
		ListExprAST(Loc, std::move(_Elements)), key_type(key_type),
		key_type_attr(key_type_attr), valid_exprs(std::move(_valid_exprs)), LitDims(std::move(_LitDims))
		{
			if (el_type)
				ft->elem_type = el_type;
			else
				ft->elem_type = getCommonType(valid_exprs);
		}
	AggregateExprAST(SourceLocation Loc, volvoxc::FullType* _ft, llvm::Type* key_type,
	                 unsigned key_type_attr = 0,
	                 std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                 std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {}) :
		ListExprAST(Loc, std::move(_Elements), _ft), key_type(key_type), LitDims(std::move(_LitDims)),
		key_type_attr(key_type_attr), valid_exprs(std::move(_valid_exprs)) {}
	std::pair<volvoxc::FullType*,std::vector<std::function<llvm::Value*(llvm::Value*)>>> getArrayConv(
		ListExprAST* List, llvm::Type* elem_type = nullptr, unsigned elem_attr = 0);
};

// map keys / array indices
union AggregateKey {
	uint64_t Uint;
	int64_t Int;
	double Double;
	float Float;
	llvm::Constant* String = nullptr;
};

class MapExprAST : public ListExprAST {
public:
	std::vector<ExprAST*> keys;
	std::vector<ExprAST*> values;
	MapExprAST(SourceLocation Loc, volvoxc::FullType* map_ft, std::vector<std::unique_ptr<ExprAST>> _Elements = {});
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
};

class SetExprAST : public ListExprAST {
public:
	SetExprAST(SourceLocation Loc, volvoxc::FullType* set_ft, std::vector<std::unique_ptr<ExprAST>> _Elements = {});
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
};

class VecExprAST : public SetExprAST {
public:
	VecExprAST(SourceLocation Loc, volvoxc::FullType* set_ft, std::vector<std::unique_ptr<ExprAST>> _Elements = {}) :
		SetExprAST(Loc, set_ft, std::move(_Elements)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
};

class FixedArrayExprAST : public AggregateExprAST {
public:
	std::vector<std::unique_ptr<ExprAST>> Dims; // known at run time
	std::vector<SourceLocation> LenLocs;
	FixedArrayExprAST(SourceLocation Loc,
	                  std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                  std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {},
	                  volvoxc::FullType* el_type = nullptr,
	                  std::vector<std::unique_ptr<ExprAST>> _Dims = {}, std::vector<SourceLocation> LenLocs = {}) :
		AggregateExprAST(Loc, llvm_size_type, 0, std::move(_Elements), std::move(_valid_exprs),
		                 std::move(_LitDims), el_type), Dims(std::move(_Dims)), LenLocs(LenLocs)
		{
			// the AggregateExprAST constructor should have determined the element type if not
			// given - we check this:
			if (ft && ft->elem_type) {
				unsigned order = (unsigned)Dims.size();
				ft->type = ft->elem_type->type;
				for (unsigned i = 0; i < order; i++)
					ft->type = llvm::ArrayType::get(ft->type, 0);
			} else {
				errs() << Loc << ": unable to determine element type of fixed array\n";
				ft = nullptr;
			}
		}
	FixedArrayExprAST(SourceLocation Loc, volvoxc::FullType* _ft,
	                  std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                  std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {},
	                  std::vector<std::unique_ptr<ExprAST>> _Dims = {}, std::vector<SourceLocation> LenLocs = {}) :
		AggregateExprAST(Loc, _ft, llvm_size_type, 0, std::move(_Elements), std::move(_valid_exprs),
		                 std::move(_LitDims)), Dims(std::move(_Dims)), LenLocs(LenLocs) {}
	llvm::Value* getArrayLitVal(llvm::ArrayType* initializer_type, ListExprAST* List);
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "Fixed Array size: " << Elements.size(), ind);
		for (const auto &Element : Elements)
			Element->dump(indent(out, ind + 1), ind + 1);
		return out;
	}
#endif
};

#define SZ_OPCODE 4
/// UnaryExprAST - Expression class for a unary operator (-x, !e)
class UnaryExprAST : public ExprAST {
	char Opcode[SZ_OPCODE] = { 0, 0, 0, 0 };
	std::unique_ptr<ExprAST> Operand;
public:
	UnaryExprAST(SourceLocation Loc, const char* Op, std::unique_ptr<ExprAST> _Operand)
		: ExprAST(_Operand->ft->type, _Operand->ft->type_attr, Loc), Operand(std::move(_Operand)) {
		strlcpy(Opcode, Op, SZ_OPCODE); 
	}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "unary" << Opcode, ind);
		Operand->dump(out, ind + 1);
		return out;
	}
#endif
};

class ThreadExprAST : public ExprAST {
	std::unique_ptr<CallExprAST> Call;
public:
	ThreadExprAST(SourceLocation Loc, std::unique_ptr<CallExprAST> _Call)
		: ExprAST(new_FullType(*lex.source_stack.front().module->type_table.get_full("__thread"), A_thread), Loc),
		  Call(std::move(_Call))
		{
			// new_FullType() only makes a shallow copy so we must not destroy the map "fields"
			ft->elem_type = Call->ft;
		}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	llvm::Function* get_thread_wrapper(bool have_target);
	llvm::StructType* args_type = nullptr;
	bool do_sret; // 1 if result address is passes as 1st arg, 0 otherwise
	bool use_eventfd = false;
	bool use_pipe = false;
	unsigned arg_offs = 0;
	unsigned arg_offs0 = 0;
	unsigned n_args = 0;
	llvm::Type* ret_typ = nullptr;
	size_t ret_sz = 0;
	llvm::Value* refcount_adr = nullptr;
	llvm::Value* eventfd_or_piperead_adr = nullptr;
	llvm::Value* pipewrite_adr = nullptr;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "task", ind);
		Call->dump(out, ind + 1);
		return out;
	}
#endif
};

/// postfix expressions (c++, b--) - similar to UnaryExprAST above but operand must be an lvalue
class PostfixExprAST : public ExprAST {
	char Opcode[SZ_OPCODE] = { 0, 0, 0, 0 };
	std::unique_ptr<LvalueExprAST> Operand;
public:
	PostfixExprAST(SourceLocation Loc, const char* Op, std::unique_ptr<LvalueExprAST> _Operand)
		: ExprAST(_Operand->ft->type, _Operand->ft->type_attr, Loc), Operand(std::move(_Operand)) {
		strlcpy(Opcode, Op, SZ_OPCODE); 
	}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "postfix" << Opcode, ind);
		Operand->dump(out, ind + 1);
		return out;
	}
#endif
};

// Expression class for a unary '&' operator in rvalues
// usually to call C functions like f(void*) as f(&x)
//
class ReferenceExprAST : public LvalueExprAST {
public:
	std::unique_ptr<LvalueExprAST> Operand;
	ReferenceExprAST(SourceLocation Loc, std::unique_ptr<LvalueExprAST> _Operand, bool is_optional = false)
		: LvalueExprAST(Loc), Operand(std::move(_Operand)) {
		if (Operand->ft->type)
			// get address from expression as 'voidptr' to call C-functions "f(&x)"
			ft = voidptr_type;
		else
			// declare reference "&r := x"
			ft = new_FullType(*Operand->ft, A_ptrref | (is_optional ? A_optional : 0));
	}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override {
		auto pair = Operand->codegen_ref(false);
		llvm::Value* ptr;
		if (auto struct_type = llvm::dyn_cast<llvm::StructType>(pair.second->getType()))
			ptr = Builder->CreateExtractValue(pair.second, struct_type->getNumElements() - 1);
		else if (pair.second->getType()->isPointerTy())
			ptr = pair.second;
		else {
			errs() << Loc << ": cannot get address of expression\n";
			return nullptr;
		}
		return handle(target, Builder->CreatePointerCast(ptr, llvm_ptr_type));
	}
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false, bool constref = false) override {
		auto pair = Operand->codegen_ref(silent_fail, false);
		ft->type = pair.first;
		return pair;
	}
	VariableExprAST* getBase() override { return nullptr; }
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "unary &", ind);
		Operand->dump(out, ind + 1);
		return out;
	}
#endif
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ReferencableExprAST {
	// it's declared "Referencable" to allow "&b = &c = ..."
public:
	std::unique_ptr<ExprAST> LHS, RHS;
	const char* err_msg = nullptr;
	char Op[SZ_OPCODE] = { 0, 0, 0, 0 };
	OpClass opclass = OpNormal;
	ReferenceExprAST* LREF = nullptr;
	BinaryExprAST(SourceLocation Loc, const char* _Op, std::unique_ptr<ExprAST> _LHS,
	              std::unique_ptr<ExprAST> _RHS, std::tuple<llvm::Type*, unsigned, bool, OpClass,
	              const char*> res_t = { llvm::Type::getVoidTy(Context), 0, false, OpDeclAssign, nullptr })
		: ReferencableExprAST(std::get<0>(res_t), std::get<1>(res_t), Loc, std::get<2>(res_t)),
		  LHS(std::move(_LHS)), RHS(std::move(_RHS)), err_msg(std::get<4>(res_t)), opclass(std::get<3>(res_t)),
		  LREF(dynamic_cast<ReferenceExprAST*>(LHS.get()))
		{
			strlcpy(Op, _Op, SZ_OPCODE);
			if (opclass == OpDeclAssign)
				LHS->ft = RHS->ft;
			if (opclass == OpRange && ft && ft->type) {
				auto limits_type_name = lex.get_type_name(ft->type, (bool)(ft->type_attr & A_signed));
				if (limits_type_name) {
#define RANGE_PREFIX "__range_"
#define RANGE_PREFIX_SIZE ARRAY_SIZE(RANGE_PREFIX) /* including terminating 0 */
					size_t name_sz = ARRAY_SIZE(RANGE_PREFIX) + strlen(limits_type_name);
					auto range_type_name = (char*)alloca(name_sz);
					strlcpy(range_type_name, RANGE_PREFIX, name_sz);
					strlcpy(range_type_name + (RANGE_PREFIX_SIZE - 1), limits_type_name, name_sz-(RANGE_PREFIX_SIZE - 1));
					ft = lex.get_full_type(range_type_name);
					if (ft)
						return;
				} else
					ft = nullptr;
				errs() << Loc << ": cannot create range from types " << *LHS->ft->type << " and " << *RHS->ft->type << "\n";
			}
		}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	llvm::Value* codegen_atomic_Xassign(llvm::Type* typ, llvm::Value* val);
	llvm::Value* codegen_atomic_CmpExchange(llvm::Type* typ, llvm::Value* ptr);
	llvm::Value* codegen_ternary(llvm::Value* target = nullptr);
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(
		bool silent_fail = false, bool constref = false) override;
	bool needs_target() override { return (opclass == OpAssign || opclass == OpModAssign) && LHS->ft && LHS->ft->type && LHS->ft->type->isSized() && TheModule->getDataLayout().getTypeAllocSize(LHS->ft->type) == 0; }
	llvm::Value* alloc_size() override {
		if (opclass == OpAssign || opclass == OpModAssign)
			return LHS->alloc_size();
		return ((ExprAST*)this)->alloc_size();
	}
	std::pair<llvm::Type*,std::unique_ptr<std::vector<llvm::Value*>>> codegen_dims() override {
		if (opclass == OpAssign || opclass == OpModAssign)
			return LHS->codegen_dims();
		return ((ExprAST*)this)->codegen_dims();
	}
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "binary" << Op, ind);
		LHS->dump(indent(out, ind) << "LHS:", ind + 1);
		RHS->dump(indent(out, ind) << "RHS:", ind + 1);
		return out;
	}
#endif
};

class TypeExprAST : public ExprAST {
public:
	std::string Name;
	TypeExprAST(SourceLocation Loc, std::string TypeName, volvoxc::FullType* ft)
		: ExprAST(ft, Loc), Name(std::move(TypeName)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override {
		return llvm::Constant::getNullValue(ft->type);
	}
};

class StructExprAST : public ExprAST {
public:
	std::map<std::string, std::unique_ptr<ExprAST>> Fields;
	StructExprAST(SourceLocation Loc, volvoxc::FullType* ft, std::unique_ptr<ListExprAST> list)
		: ExprAST(ft, Loc) {
		for (auto& field: list->Elements) {
			if (auto field_val = dynamic_cast<BinaryExprAST*>(field.get())) {
				if (field_val->Op[0] == ':' && !field_val->Op[1]) {
					std::string* field_key = nullptr;
					// we are only interested in the "ident" of the LHS of "ident: value"
					// the parser might have found the ident in tables so we have to handle these cases
					// it does not seems effective to declare a common base class "NamedExprAST" to derive
					// these cases because 'VariableExprAST' is derived from 'LvalueExprAST', the others are not
					if (auto nameAST = dynamic_cast<VariableExprAST*>(field_val->LHS.get()))
						field_key = &nameAST->Name;
					else if (auto nameAST = dynamic_cast<FunctionExprAST*>(field_val->LHS.get()))
						field_key = &nameAST->Name;
					else if (auto nameAST = dynamic_cast<IdentExprAST*>(field_val->LHS.get()))
						field_key = &nameAST->Name;
					else if (auto nameAST = dynamic_cast<TypeExprAST*>(field_val->LHS.get()))
						field_key = &nameAST->Name;
					else
						errs() << field_val->LHS->Loc << " field name expected\n";
					if (field_key) {
						auto insert = Fields.try_emplace(*field_key, std::move(field_val->RHS));
						if (!insert.second)
							errs() << field_val->LHS->Loc << ": field '" << field_key << "' already initialized\n";
					}
					continue;
				}
				errs() << field->Loc << ": initializer with ':' expected - not '" << field_val->Op << "'\n";
			}
			errs() << field->Loc << ": binary expression as initializer expected\n";
		}
	}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	bool needs_target() override {
		if (!ft) {
			errs() << Loc << ":undetermined expression\n";
			return false;
		}
		return ft->type_attr & A_constructor;
	}
};

class DefaultConstructorCall : public ExprAST {
public:
	std::unique_ptr<VariableExprAST> Var;
	DefaultConstructorCall(SourceLocation Loc, std::unique_ptr<VariableExprAST> _Var)
		: ExprAST(void_type, Loc), Var(std::move(_Var)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
};

inline void dump_branch_parts(std::vector<unsigned>* v) {
	if (!v) {
		errs() << " - no levels\n";
		return;
	}
	errs() << " - levels: " << v->size() << "\n";
	for (auto p: *v) {
		if (p & (1U << 31))
			errs() << "  else: ";
		else
			errs() << "  then: ";
		errs() << ((p & 0x7fff000) >> 16) << " " << (p & 0xffff) << "\n";
	}
}

class BranchExprAST : public ExprAST {
public:
	// branches, end-kinds
	std::vector<std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>> Then, Else;
	unsigned max_brk_level = 0; // if >1 this expr must be considered as brk-like for
	                   // variable validation
protected:
	VarTable then_locals_table, else_locals_table;
	TokenKind if_kind = (TokenKind)0;
	llvm::BasicBlock* StackRestoreBB0;
	llvm::Function* TheFunction = nullptr;
public:
	const char* errmsg; // postponed: result type is just void if branch last values do not match
	                    // however if a result value is needed by a consumer this message is used
	bool is_elif_branch = false; // true indicates that no further syntactic nesting is done
	std::unique_ptr<ExprAST> Cond;
	std::set<std::string> merged_vars; // constructor has not to be called
	bool always_return = false;
	BranchExprAST(SourceLocation Loc, llvm::Type* type, unsigned type_attr,
	              bool is_unknown_type, const char* errmsg,
	              std::vector<std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>> _Then,
	              std::vector<std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>> _Else,
	              VarTable _then_locals_table, VarTable _else_locals_table, unsigned max_brk_level,
	              std::set<std::string> _merged_vars,
	              std::unique_ptr<ExprAST> _Cond = nullptr, TokenKind if_kind = (TokenKind)0,
	              bool always_return = false)
	: ExprAST(type, type_attr, Loc, is_unknown_type), Then(std::move(_Then)), Else(std::move(_Else)),
	  then_locals_table(std::move(_then_locals_table)), max_brk_level(max_brk_level),
	  merged_vars(std::move(_merged_vars)),
	  else_locals_table(std::move(_else_locals_table)), 
	  Cond(std::move(_Cond)), if_kind(if_kind),
	  always_return(always_return), errmsg(errmsg) {}
	std::tuple<llvm::Value*, llvm::Instruction*, int> createCondBranch(
		std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>& bBranch, bool isElse);
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
};

enum CTcond_t : uint8_t {
	CTcond_false = 0,
	CTcond_true,
	CTcond_undef
};

/// IfExprAST - Expression class for if/then/else.
class IfExprAST : public BranchExprAST {

public:
	IfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Cond,
	          std::vector<std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>> _Then,
	          std::vector<std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>> _Else,
	          VarTable _then_locals_table, VarTable _else_locals_table, unsigned max_brk_level,
	          std::set<std::string> _merged_vars,
	          std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*> res_t, TokenKind if_kind = tok_if,
	          bool always_return = false)
		: BranchExprAST(Loc, std::get<0>(res_t),
		                std::get<1>(res_t), std::get<2>(res_t), std::get<4>(res_t), std::move(_Then),
		                std::move(_Else), std::move(_then_locals_table), std::move(_else_locals_table),
		                max_brk_level, std::move(_merged_vars), std::move(_Cond), if_kind, always_return)
		{
			// this is a little bit of a hack to make arrays work. Conversions can only handle SingleValueTypes but 'merge_values()' in codegen.cc is more powerful
			if (Then[0].first.size() && Then[0].first.back()->ft && Then[0].first.back()->ft->type && !Then[0].first.back()->ft->type->isSingleValueType() && !Then[0].first.back()->ft->type->isVoidTy()
			    && Else[0].first.size() && Else[0].first.back()->ft && Else[0].first.back()->ft->type && !Else[0].first.back()->ft->type->isSingleValueType() && !Else[0].first.back()->ft->type->isVoidTy())
				ft = new_FullType(*Then[0].first.back()->ft);
			if (!ft->type)
				ft = new_FullType(llvm::Type::getVoidTy(Context), 0);
		}
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "if", ind);
		Cond->dump(indent(out, ind) << "Cond:", ind + 1);
		Then[0].first[0]->dump(indent(out, ind) << "Then:", ind + 1);
		if (Else.size())
			Else[0].first[0]->dump(indent(out, ind) << "Else:", ind + 1);
		return out;
	}
#endif
};

enum new_var_kind : uint8_t {
	new_var_none = 0,
	new_var_created,
	existing_var_returned,
	setter_method_returned,
	generic_lvalue_returned
};

/// ForExprAST - Expression class for for/in.
class ForExprAST : public BranchExprAST {
	std::unique_ptr<ExprAST> Iterator = nullptr;
	llvm::Value* limit = nullptr;
	llvm::Value* approx_limit = nullptr; // for float
	llvm::Value* ptr_storage = nullptr; // when iterating over array with non-reference
	                                    // value variable there's still an unnamed
	                                    // control variable pointing to the current elem
	llvm::Value* iterator = nullptr;
	llvm::Value* iterator_ref = nullptr;
	llvm::Type* iterator_type = nullptr;
	llvm::Type* ElType = nullptr;
	llvm::Value* Ptr = nullptr;
	std::unique_ptr<ExprAST> Key = nullptr, Value = nullptr;
	FullVar* KeyFV = nullptr;
	LvalueExprAST* KeyLval = nullptr;
	FullVar* ValueFV = nullptr;
	LvalueExprAST* ValueLval = nullptr;
	llvm::Value* ValueRef = nullptr;
	llvm::Type* ValueType = nullptr;
	llvm::Value* KeyRef = nullptr;
	llvm::Type* KeyType = nullptr;
	volvoxc::FullType* KeyFT = nullptr;
	volvoxc::FullType* ValueFT = nullptr;
	llvm::Value* Step = nullptr;
	std::string KeyName, ValueName;
	llvm::Align rvalue_align;
	new_var_kind new_Key, new_Value;
	bool descending;

public:
	ForExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Iterator, VarTable _locals_table,
	           VarTable else_locals_table, unsigned max_brk_level, std::set<std::string> _merged_vars, std::unique_ptr<ExprAST> _Key,
	           std::unique_ptr<ExprAST> _Value, std::string _KeyName, std::string _ValueName,
	           std::vector<std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>> _Body,
	           std::vector<std::pair<std::vector<std::unique_ptr<ExprAST>>,BreakDescription>> _Else,
	           FullVar* ValueFV, FullVar* KeyFV = nullptr, volvoxc::FullType* ValueFT = nullptr,
	           volvoxc::FullType* KeyFT = nullptr,
	           new_var_kind new_Key = new_var_none, new_var_kind new_Value = new_var_none, bool descending = false)
		: BranchExprAST(Loc, llvm::Type::getVoidTy(Context), 0, false, nullptr, std::move(_Body),
		                std::move(_Else), std::move(_locals_table),
		                std::move(else_locals_table), max_brk_level, std::move(_merged_vars), nullptr, tok_for),
		  Iterator(std::move(_Iterator)), Key(std::move(_Key)), Value(std::move(_Value)),
		  KeyFV(KeyFV), ValueFV(ValueFV), KeyName(std::move(_KeyName)), ValueName(std::move(_ValueName)),
		  ValueFT(ValueFT), KeyFT(KeyFT), new_Key(new_Key), new_Value(new_Value), descending(descending) {}
	bool PrepareIterator();
	llvm::Value* CreateCondition(bool at_end = false);
	bool SetupLoop();
	bool Iterate();
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "for " << KeyName << "," << ValueName, ind);
		Then[0].first[0]->dump(indent(out, ind) << "Body:", ind + 1);
		return out;
	}
#endif
};
