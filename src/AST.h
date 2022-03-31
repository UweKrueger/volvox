#pragma once
#include "../include/volvox.hh"
#include "global.h"

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//


/// ExprAST - Base class for all expression nodes.

class ConstExprAST : public ExprAST {
	llvm::Constant* val;
public:
	ConstExprAST(llvm::Constant* val) : val(val) {
		ft->type = val->getType();
	}
	llvm::Value* codegen() { return val; }
};

// Class for all literals - 1.2, 3u, "str"
class LiteralExprAST : public ExprAST {

public:
	union LitValue Val;
	LiteralExprAST(const Token& tok, SourceLocation Loc = CurLoc) : ExprAST(tok.key, A_const |
		  (((tok.int_type.ID == llvm::Type::IntegerTyID &&
		     tok.int_type.is_signed) || tok.kind == tok_ptr_lit) ? A_signed : 0), Loc, tok.is_unknown_type, nullptr, 0, true), Val(tok.Val) {}
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
				return ExprAST::dump(out << Val.Str, ind);
		default:
			eprt("internal compiler error: unhandled literal type %d\n", ft->type->getTypeID());
			return out;
		}
	}
#endif
	llvm::Value *codegen() override;
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes), as well as if it is an operator.
class PrototypeAST {

public:
	std::vector<std::string> Args;
	std::vector<volvox::FullType*> ArgTypes;
	std::vector<llvm::Type*> LLVMArgTypes; // to get LLVM function type
	volvox::FullType* RetType;
	llvm::FunctionType* FT;
	bool IsVarArgs;
	bool IsOperator;
	int Line;
	std::string Name;
	PrototypeAST(SourceLocation Loc, const std::string &Name,
	             std::vector<std::string> Args, bool IsOperator = false,
	             volvox::FullType* RetType_ = nullptr, std::vector<volvox::FullType*> ArgTypes = {},
	             std::vector<llvm::Type*> LLVMArgTypes = {}, bool IsVarArgs = false)
		: Name(Name), Args(Args), IsOperator(IsOperator),
		  Line(Loc.Line), RetType(RetType_ ? RetType_ : void_type), ArgTypes(ArgTypes), LLVMArgTypes(LLVMArgTypes), IsVarArgs(IsVarArgs) {
		FT = llvm::FunctionType::get(RetType->type, LLVMArgTypes, IsVarArgs);
	}
	llvm::Function *codegen();
	const std::string &getName() const { return Name; }

	bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
	bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

	char getOperatorName() const {
		assert(isUnaryOp() || isBinaryOp());
		return Name[Name.size() - 1];
	}

	int getLine() const { return Line; }
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {

public:
	std::string Name;
	std::pair<FullVar*, bool> full_var; // and if it's global
	VariableExprAST(SourceLocation Loc, const std::string &Name)
		: ExprAST(Loc), Name(Name), full_var(lookup_var(Name.c_str())) {
		if (full_var.first) {
			ft = new_FullType(full_var.first->ft); // TODO: don't create a new instance (?)
		} else {
			auto F = FunctionProtos.find(Name);
			if (F != FunctionProtos.end()) {
				ft->type = F->second->FT;
				dprt("got function for name %s %u\n", Name.c_str(), ft->type->getTypeID());
				// TODO: unify handling of full_var and FunctionProtos - maybe even 1 single database
				full_var = { new_FullVar((llvm::Value *)F->second.get(), ft->type, 0), true };
				is_compile_time_const = true;
			}
		}
	}
	const std::string &getName() const { return Name; }
	llvm::Value *codegen() override;
	llvm::Value *codegen_ref(); // create reference to this variable
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		return ExprAST::dump(out << Name, ind);
	}
#endif
};

/// FunctionExprAST - classic named functions (not function pointers)
class FunctionExprAST : public ExprAST {

public:
	std::string Name;
	FunctionExprAST(SourceLocation Loc, const std::string &Name, PrototypeAST* Proto)
		: ExprAST(Loc), Name(Name) {
		ft = new_FullType(Proto->FT, 0);
		dprt("Function: %u\n", ft->type->getTypeID());
		ft->proto = Proto;
	}
	const std::string &getName() const { return Name; }
	llvm::Value *codegen() override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		return ExprAST::dump(out << Name, ind);
	}
#endif
};

// IndexExprAST - Expressions like x[2] or y["key"]
class IndexExprAST : public ExprAST {

public:
	std::unique_ptr<ExprAST> Field, Index;
	IndexExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Field,
	             std::unique_ptr<ExprAST> Index,
	             llvm::Type* desired_type = nullptr,
	             unsigned desired_attrib = 0)
		: ExprAST(*Field) {}
	llvm::Value* codegen() override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "index", ind);
		Field->dump(indent(out, ind) << "Field:", ind + 1);
		Index->dump(indent(out, ind) << "Index:", ind + 1);
		return out;
	}
#endif
};

enum AggregateKind {
	// fixed size kinds - [ ... ]
	FixedArray,
	Struct,
	FixedMatrix,
	FixedVector,
	Interval,
	AnyFixed,
	// dynamic size aggregates - { ... }
	Array,
	Vector,
	Map,
	Matrix,
	AnyDyn
};

class AggregateExprAST : public ExprAST {
	std::vector<std::unique_ptr<ExprAST>> Elements;
	AggregateKind kind;
public:
	AggregateExprAST(SourceLocation Loc, AggregateKind k,
	                 std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                 unsigned type_attr = 0, volvox::FullType* el_type = nullptr) :
		ExprAST(nullptr, type_attr, Loc), Elements(std::move(_Elements)),
		kind(k)
		{
			if (kind == FixedArray) {
				if (el_type)
					ft->elem_type = el_type;
				else
					ft->elem_type = MakeType(Elements[0]->ft, Elements[0]->is_unknown_type);
				if (ft->elem_type)
					eprt("Have Elem type %u %u\n", ft->elem_type->type->getTypeID(), ft->elem_type->type_attr);
				ft->type = llvm::ArrayType::get(ft->elem_type->type, Elements.size());
				ft->num_fields = Elements.size();
				// TODO... nrows = Elements.size();
				is_compile_time_const = true;
				// dprt("CTC: true, nrows: %d\n", nrows);
				for (auto& e: Elements)
					if (!e->is_compile_time_const) {
						is_compile_time_const = false;
						break;
					}
			}
		}
	const char* KindName();
	llvm::Value* codegen() override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "aggregate type " << KindName(), ind);
		for (const auto &Element : Elements)
			Element->dump(indent(out, ind + 1), ind + 1);
		return out;
	}
#endif
};

/// UnaryExprAST - Expression class for a unary operator.
class UnaryExprAST : public ExprAST {
	char Opcode[4];
	std::unique_ptr<ExprAST> Operand;

public:
	UnaryExprAST(const char* Op, std::unique_ptr<ExprAST> Operand)
		: ExprAST(Operand->ft->type, Operand->ft->type_attr), Operand(std::move(Operand)) {
		strcpy(Opcode, Op); 
	}
	llvm::Value *codegen() override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "unary" << Opcode, ind);
		Operand->dump(out, ind + 1);
		return out;
	}
#endif
};

// conversions that have to be applied to Operands of binary Operator to make them compatible
struct BinOpConv {
	std::function<llvm::Value*(llvm::Value*)> LHS;
	std::function<llvm::Value*(llvm::Value*)> RHS;
	llvm::Type* res_type;
	unsigned res_attr;
	bool is_unknown_type;
	const char* err_msg;
};

// there are two sets of conversions. E.g. `u16 * u16(u8) -> u16` works, but could overflow
// ideal would be `u32(u16) * u32(u8) -> u32`, which will never overflow but is only
// feasible if an `u32` is desired
struct BinOpConvSet {
	BinOpConv compat; // conversion (only one side) to make operands match
	BinOpConv ideal; // conversions to best prevent overflow/precision loss
};

extern BinOpConvSet convBinOp(llvm::Type* left_type, llvm::Type* right_type,
                              unsigned left_attr, unsigned right_attr,
                              bool left_is_unknown_type, bool right_is_unknown_type,
                              const char* Op, SourceLocation Loc = CurLoc);

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
	
public:
	char Op[4];
	std::unique_ptr<ExprAST> LHS, RHS;
	BinOpConvSet conv;
	BinaryExprAST(SourceLocation Loc, const char* _Op, std::unique_ptr<ExprAST> _LHS,
	              std::unique_ptr<ExprAST> _RHS, BinOpConvSet conv = {},
	              llvm::Type* desired_type = nullptr, unsigned desired_attrib = 0)
		: ExprAST(conv.compat.res_type, conv.compat.res_attr, Loc, desired_type, desired_attrib,
		          _RHS->is_unknown_type && _LHS->is_unknown_type,
		          _RHS->is_compile_time_const && (_LHS->is_compile_time_const || !strcmp(Op, ":"))),
		  LHS(std::move(_LHS)), RHS(std::move(_RHS)), conv(conv) {
		if (!desired_type && _Op[0] != '=' || desired_type && desired_type == llvm::Type::getInt1Ty(*Context.getContext())) {
			if (conv.ideal.res_type == llvm::Type::getInt1Ty(*Context.getContext())) {
				ft->type = conv.ideal.res_type;
				ft->type_attr = 0;
			}
		}
		strcpy(Op, _Op);
	}
	llvm::Value *codegen() override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "binary" << Op, ind);
		LHS->dump(indent(out, ind) << "LHS:", ind + 1);
		RHS->dump(indent(out, ind) << "RHS:", ind + 1);
		return out;
	}
#endif
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
public:
	std::unique_ptr<ExprAST> Callee;
	std::vector<std::unique_ptr<ExprAST>> Args;

	CallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Callee_,
	            std::vector<std::unique_ptr<ExprAST>> Args = {})
		: ExprAST(*Callee_->ft->proto->RetType, Loc), Callee(std::move(Callee_)), Args(std::move(Args)) {}
	llvm::Value *codegen() override;
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

/// IfExprAST - Expression class for if/then/else.
class IfExprAST : public ExprAST {
	std::unique_ptr<ExprAST> Cond;
	bool is_void; // no consistent result in branches -> will return bool
	std::vector<std::unique_ptr<ExprAST>> Then, Else;

public:
	int ThenEndKind, ElseEndKind; // maybe tok_else, tok_end, tok_return, ...

	IfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Cond,
	          std::vector<std::unique_ptr<ExprAST>> _Then, std::vector<std::unique_ptr<ExprAST>> _Else,
	          int ThenEndKind, int ElseEndKind)
		: ExprAST(_Then.back()->ft->type, _Then.back()->ft->type_attr, Loc),
		  is_void(_Then.back()->ft->type == llvm::Type::getVoidTy(*Context.getContext()) || !_Else.size()
		          || _Else.back()->ft->type != _Then.back()->ft->type
		          || (_Else.back()->ft->type_attr & A_signed) != (_Then.back()->ft->type_attr & A_signed)),
		  Cond(std::move(Cond)), Then(std::move(_Then)), Else(std::move(_Else)), ThenEndKind(ThenEndKind),
		  ElseEndKind(ElseEndKind)
		{
			if (is_void) {
				dprt("void IfExpr: %p %p %u %u %s %s\n", Then.back()->ft->type, Else.back()->ft->type,
				       Then.back()->ft->type_attr, Else.back()->ft->type_attr,
				       type_table.get_name(Then.back()->ft->type, Then.back()->ft->type_attr & A_signed),
				       type_table.get_name(Else.back()->ft->type, Else.back()->ft->type_attr & A_signed));
				ft->type = llvm::Type::getInt1Ty(*Context.getContext());
				ft->type_attr = 0;
			}
		}
	llvm::Value *codegen() override;
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
	bool skip_1st_check;
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
	llvm::Value *codegen() override;
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

/// FunctionAST - This class represents a function definition itself.
class FunctionAST {
public:
	PrototypeAST* Proto;
	std::vector<std::unique_ptr<ExprAST>> Body;
	int EndKind;
	
	FunctionAST(PrototypeAST* Proto,
	            std::vector<std::unique_ptr<ExprAST>> Body, int EndKind)
		: Proto(Proto), Body(std::move(Body)), EndKind(EndKind) {}
	llvm::Function *codegen();
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
