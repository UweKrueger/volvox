/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#pragma once
#include "../include/volvox.hh"
#include "global.h"

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
					.type = llvm::Type::getInt8PtrTy(Context),
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
	llvm::Value* rttype = nullptr;
public:
	InterfaceExprAST(std::unique_ptr<ExprAST> _expr) :
		ExprAST(interface_type, _expr->Loc, false), expr(std::move(_expr)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr);
};

class ConstExprAST : public ExprAST {
	llvm::Constant* val;
public:
	ConstExprAST(llvm::Constant* val) : val(val) {
		if (!val)
			errs() << "ConstExprAST: no valid value\n";
		else
			ft->type = val->getType();
	}
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
	std::string Name;
	ModuleExprAST(SourceLocation Loc, std::string _Name) : ExprAST(Loc), Name(std::move(_Name)) {}
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
			tok.Val.Ptr = nullptr;
		}
	}
	~LiteralExprAST() {
		if (ft->type->getTypeID() == llvm::Type::PointerTyID && !(ft->type_attr & A_signed))
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

inline bool is_cfn(std::vector<std::unique_ptr<PrototypeAST>>* Proto) {
	return Proto && (*Proto).size() == 1 && ((*Proto)[0]->getName().c_str()[0] != '_' || (*Proto)[0]->getName().c_str()[1] != 'Z');
}

inline bool is_ccfn(std::vector<std::unique_ptr<PrototypeAST>>* Proto) {
	return Proto && (*Proto).size() >= 1 && (*Proto)[0]->getName().c_str()[0] == '_' && (*Proto)[0]->getName().c_str()[1] == 'Z';
}

// Expressions that can the the LHS of an assignmen: `a = 1`, `b[3] = 4.5`, `s.a = 9`
class LvalueExprAST : public ExprAST {
	std::pair<llvm::Type*,llvm::Value*> ref_cache = { nullptr, nullptr };
public:
	std::string Name;
	LvalueExprAST(SourceLocation Loc, std::string Name = "") : ExprAST(Loc), Name(Name) {}
	// get a reference to the value
	// if this is an rvalue and silent_fail=true then the llvm::Type is returned
	// but the llvm::Value is NULL
	virtual std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false) = 0;
	std::pair<llvm::Type*,llvm::Value*> codegen_ref(bool silent_fail = false) {
		if (ref_cache.first) {
			if (!ref_cache.second && !silent_fail)
				errs() << Loc << ": cannot get reference\n";
			return ref_cache;
		}
		return codegen_ref_(silent_fail);
	}
	std::pair<llvm::Type*,std::unique_ptr<std::vector<llvm::Value*>>> codegen_dims() override;
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	virtual VariableExprAST* getBase() = 0;
	virtual llvm::Value* ref2val(std::pair<llvm::Type*,llvm::Value*> ref) {
		if (ref.second && ref.first->isSized() && TheModule->getDataLayout().getTypeAllocSize(ref.first) > 0)
			return Builder->CreateLoad(ref.first, ref.second, Name.c_str());
		else
			return nullptr;
	}
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public LvalueExprAST {

public:
	FullVar* full_var; // and if it's global
	VariableExprAST(SourceLocation Loc, const std::string &Name)
		: LvalueExprAST(Loc, Name), full_var(lookup_var(Name.c_str())) {
		if (full_var) {
			ft = &full_var->ft;
			if (ft->type_attr & A_untyped)
				is_unknown_type = true;
		}
		// if the variable name has not found in the database we don't generate
		// an error message here because this VariableExprAST could be the LHS of
		// an initialization e.g. `a := 42`
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
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false) override;
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
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false) override {
		return { ft->type, ref };
	}
	VariableExprAST* getBase() override { return var; }
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
public:
	std::vector<FnArg> fn_args;
	const char* name = "*";
	std::unique_ptr<ExprAST> Callee;
	std::vector<std::unique_ptr<ExprAST>> Args;
	PrototypeAST* Proto = nullptr;
	CallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Callee_,
	            std::vector<std::unique_ptr<ExprAST>> Args = {});
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	bool needs_target() override { return Proto && (Proto->IsStructRet || (Proto->visibility & A_constructor)); }
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
	bool already_returned = false; // both branches of 'if ... else ...' end with 'return'
	volvoxc::FullType* receiver_ft;
	llvm::Function* TheFunction;
	llvm::BasicBlock* BB;
	unsigned ArgIdx;
	volvoxc::FullType* ret_ft;
	llvm::Value* this_ret_ptr = nullptr;
	llvm::Value* RetVal = nullptr;
	llvm::Value* InterRetVal = nullptr;
public:
	bool prepare_codegen();
	bool process_body(std::vector<std::unique_ptr<ExprAST>>& thisBody);
	llvm::Function* finish_codegen(bool finishModule = false, bool getNewModule = false);
	llvm::Function* cleanup_codegen();
	PrototypeAST* Proto = nullptr;
	std::string unmangledName;
	std::vector<std::unique_ptr<ExprAST>> Body;
	int EndKind = 0;
	int return_val_idx = -1;
	FunctionAST(PrototypeAST* Proto,
	            std::vector<std::unique_ptr<ExprAST>> Body, int EndKind, std::string unmName, int return_val_idx = -1)
		: Proto(Proto), Body(std::move(Body)), EndKind(EndKind), unmangledName(std::move(unmName)), return_val_idx(return_val_idx) {}
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
	int selected_proto = 0; // should be set by call expr
	FunctionExprAST(SourceLocation Loc, const std::string &Name, std::vector<std::unique_ptr<PrototypeAST>>* Protos)
		: ExprAST(Loc), Name(Name) {
		ft = new_FullType((*Protos)[0]->FT, 0);
		ft->Protos = Protos;
	}
	// function references are created by a pseudo call expression (to be able to match the signature)
	FunctionExprAST(CallExprAST* call)
		: ExprAST(*call->Callee), Name("fn") {
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

class MethodExprAST : public FunctionExprAST {
public:
	std::unique_ptr<ExprAST> Receiver;
	std::unique_ptr<IdentExprAST> Method;
	MethodExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Receiver, std::unique_ptr<IdentExprAST> _Method, std::vector<std::unique_ptr<PrototypeAST>>* Protos)
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
					MapValue* mv = map_string_get(Struct->ft->fields, FieldName);
					if (mv) {
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
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false) override;
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	VariableExprAST* getBase() override {
		if (auto lval = dynamic_cast<LvalueExprAST*>(Struct.get()))
			return lval->getBase();
		return nullptr;
	}
};

// IndexExprAST - Expressions like x[2] or y["key"]
class IndexExprAST : public LvalueExprAST {

public:
	std::unique_ptr<ExprAST> Field, Index;
	llvm::Type* ml_elem_type = nullptr;
	int num_dims_to_strip_from_val = 0;
	IndexExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Field_,
	             std::unique_ptr<ExprAST> Index_) :
		LvalueExprAST(Loc), Field(std::move(Field_)), Index(std::move(Index_))
		{
			if (auto array_type = llvm::dyn_cast<llvm::ArrayType>(Field->ft->type)) {
				llvm::Type* elem_type = array_type->getElementType();
				if (elem_type == Field->ft->elem_type->type)
					*ft = *Field->ft->elem_type;
				else {
					*ft = *Field->ft;
					ft->type = array_type->getElementType();
				}
				return;
			} else if (auto a_type = llvm::dyn_cast<llvm::PointerType>(Field->ft->type)) {
				// if (Field->ft->type_attr & A_map) {
					ft = &Field->ft->elem_type[1];
					return;
				// }
			}
			errs() << Index->Loc << ": index for non array expression " << *Field->ft << ' ' << Field->ft->type_attr << "\n";
			ft->type = nullptr;
		}
	std::tuple<uint64_t,llvm::Value*,llvm::Value*> getMLIdxOffset(
		llvm::Type* elem_type, std::vector<llvm::Value*>& Idxs,
		llvm::Value* Dims, int idx_idx, int dim_idx);
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false) override;
	llvm::Value* codegen_ref0(std::vector<llvm::Value*>& Idxs, llvm::Type*& ml_field_type);
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
				errs() << "undefined element type\n";
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

/// UnaryExprAST - Expression class for a unary operator (-x, !e)
class UnaryExprAST : public ExprAST {
	char Opcode[8] = { 0, 0, 0, 0 };
	std::unique_ptr<ExprAST> Operand;
public:
	UnaryExprAST(SourceLocation Loc, const char* Op, std::unique_ptr<ExprAST> _Operand)
		: ExprAST(_Operand->ft->type, _Operand->ft->type_attr, Loc), Operand(std::move(_Operand)) {
		strcpy(Opcode, Op); 
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

class TaskExprAST : public ExprAST {
	std::unique_ptr<CallExprAST> Call;
public:
	TaskExprAST(SourceLocation Loc, std::unique_ptr<CallExprAST> _Call)
		: ExprAST(_Call->ft->type, _Call->ft->type_attr, Loc), Call(std::move(_Call)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
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
	char Opcode[4] = { 0, 0, 0, 0 };
	std::unique_ptr<LvalueExprAST> Operand;
public:
	PostfixExprAST(SourceLocation Loc, const char* Op, std::unique_ptr<LvalueExprAST> _Operand)
		: ExprAST(_Operand->ft->type, _Operand->ft->type_attr, Loc), Operand(std::move(_Operand)) {
		strcpy(Opcode, Op); 
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

// Expression class for a unary '&' operator
class ReferenceExprAST : public LvalueExprAST {
public:
	std::unique_ptr<LvalueExprAST> Operand;
	ReferenceExprAST(SourceLocation Loc, std::unique_ptr<LvalueExprAST> _Operand, bool is_optional = false)
		: LvalueExprAST(Loc), Operand(std::move(_Operand)) {
		if (Operand->ft->type) {
			// get address from expression as 'voidptr' to call C-functions "f(&x)"
			ft = voidptr_type;
		} else {
			// declare reference "&r := x"
			ft = new_FullType(*Operand->ft);
			ft->type_attr |= A_ptrref;
			if (is_optional)
				ft->type_attr |= A_optional;
		}
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
		return handle(target, Builder->CreatePointerCast(ptr, llvm::Type::getInt8PtrTy(Context)));
	}
	std::pair<llvm::Type*,llvm::Value*> codegen_ref_(bool silent_fail = false) override {
		auto pair = Operand->codegen_ref(silent_fail);
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
class BinaryExprAST : public ExprAST {
	
public:
	std::unique_ptr<ExprAST> LHS, RHS;
	const char* err_msg = nullptr;
	char Op[4] = { 0, 0, 0, 0 };
	OpClass opclass = OpNormal;
	BinaryExprAST(SourceLocation Loc, const char* _Op, std::unique_ptr<ExprAST> _LHS,
	              std::unique_ptr<ExprAST> _RHS, std::tuple<llvm::Type*, unsigned, bool, OpClass,
	              const char*> res_t = { llvm::Type::getVoidTy(Context), false, false, OpDeclAssign, nullptr })
		: ExprAST(std::get<0>(res_t), std::get<1>(res_t), Loc,
		          std::get<2>(res_t)),
		  LHS(std::move(_LHS)), RHS(std::move(_RHS)), err_msg(std::get<4>(res_t)), opclass(std::get<3>(res_t))
		{
			strcpy(Op, _Op);
			if (opclass == OpDeclAssign)
				LHS->ft = RHS->ft;
		}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	llvm::Value* codegen_atomic_Xassign(llvm::Value* ptr, llvm::Value* val);
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

enum CTcond_t : uint8_t {
	CTcond_false = 0,
	CTcond_true,
	CTcond_undef
};

/// IfExprAST - Expression class for if/then/else.
class IfExprAST : public ExprAST {
	std::unique_ptr<ExprAST> Cond;
	std::vector<std::unique_ptr<ExprAST>> Then, Else;
	const char* errmsg = nullptr;
	TokenKind if_kind = (TokenKind)0;
	VarTable then_locals_table;
	VarTable else_locals_table;

public:
	int ThenEndKind, ElseEndKind; // maybe tok_else, tok_end, tok_return, ...
	bool always_return = false;

	IfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Cond,
	          std::vector<std::unique_ptr<ExprAST>> _Then, std::vector<std::unique_ptr<ExprAST>> _Else,
	          int ThenEndKind, int ElseEndKind, VarTable _then_locals_table, VarTable _else_locals_table,
	          std::tuple<llvm::Type*, unsigned, bool, OpClass, const char*> res_t, TokenKind if_kind = tok_if, bool always_return = false)
		: ExprAST(_Else.size() ? std::get<0>(res_t) : llvm::Type::getVoidTy(Context), std::get<1>(res_t), Loc, std::get<2>(res_t)),
		  errmsg(std::get<4>(res_t)), Cond(std::move(_Cond)), Then(std::move(_Then)), Else(std::move(_Else)), ThenEndKind(ThenEndKind),
		  ElseEndKind(ElseEndKind), then_locals_table(std::move(_then_locals_table)), else_locals_table(std::move(_else_locals_table)), if_kind(if_kind),
		  always_return(always_return)
		{
			// this is a little bit of a hack to make arrays work. Conversions can only handle SingleValueTypes but 'merge_values()' in codegen.cc is more powerful
			if (Then.size() && Then.back()->ft && Then.back()->ft->type && !Then.back()->ft->type->isSingleValueType() && !Then.back()->ft->type->isVoidTy()
			    && Else.size() && Else.back()->ft && Else.back()->ft->type && !Else.back()->ft->type->isSingleValueType() && !Else.back()->ft->type->isVoidTy())
				ft = new_FullType(*Then.back()->ft);
			if (!ft->type)
				ft = new_FullType(llvm::Type::getVoidTy(Context), 0);
		}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
	std::pair<llvm::Value*, llvm::Instruction*> createCondBranch(llvm::BasicBlock *MergeBB, bool isElse = false);
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "if", ind);
		Cond->dump(indent(out, ind) << "Cond:", ind + 1);
		Then[0]->dump(indent(out, ind) << "Then:", ind + 1);
		if (Else.size())
			Else[0]->dump(indent(out, ind) << "Else:", ind + 1);
		return out;
	}
#endif
};

class IteratorAST {
public:
	std::unique_ptr<ExprAST> Cond, Iterate, Init, Key, Value;
	bool skip_1st_check = false;
	IteratorAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Iterate, bool skip_1st_check, std::unique_ptr<ExprAST> Init, std::unique_ptr<ExprAST> Key, std::unique_ptr<ExprAST> Value)
		: Cond(std::move(Cond)), Iterate(std::move(Iterate)), skip_1st_check(skip_1st_check), Init(std::move(Init)), Key(std::move(Key)), Value(std::move(Value)) {}
	virtual ~IteratorAST() = default;
};

/// ForExprAST - Expression class for for/in.
class ForExprAST : public ExprAST {
	std::string VarName;
	std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
	ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Start,
	           std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step,
	           std::unique_ptr<ExprAST> Body, SourceLocation Loc = CurLoc)
		: ExprAST(Loc), VarName(VarName), Start(std::move(Start)), End(std::move(End)),
		  Step(std::move(Step)), Body(std::move(Body)) {}
	llvm::Value* codegen_raw(llvm::Value* target = nullptr) override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "for", ind);
		Start->dump(indent(out, ind) << "Cond:", ind + 1);
		End->dump(indent(out, ind) << "End:", ind + 1);
		Step->dump(indent(out, ind) << "Step:", ind + 1);
		Body->dump(indent(out, ind) << "Body:", ind + 1);
		return out;
	}
#endif
};
