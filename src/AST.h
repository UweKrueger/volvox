#pragma once
#include "../include/volvox.hh"
#include "global.h"

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//


/// ExprAST - Base class for all expression nodes.

// Class for all literals - 1.2, 3u, "str"
class LiteralExprAST : public ExprAST {
protected:
	union LitValue Val;

public:
	LiteralExprAST(const Token& tok, SourceLocation Loc = CurLoc) : ExprAST(tok.key, A_const |
		  ((tok.int_type.ID == llvm::Type::IntegerTyID &&
		    tok.int_type.is_signed) ? A_signed : 0), Loc), Val(tok.Val) {}
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		switch (type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			if (type_attr & A_signed)
				return ExprAST::dump(out << Val.Int, ind);
			else
				return ExprAST::dump(out << Val.Uint, ind);
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return ExprAST::dump(out << Val.Float, ind);
		case llvm::Type::PointerTyID:
			return ExprAST::dump(out << Val.Str, ind);
		default:
			fprintf(stderr, "internal compiler error: unhandled literal type %d\n", type->getTypeID());
			return out;
		}
	}
	llvm::Value *codegen() override;
};


/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {

public:
	std::string Name;
	std::pair<FullType*, bool> full_type; // and if it's global
	VariableExprAST(SourceLocation Loc, const std::string &Name)
		: ExprAST(Loc), Name(Name), full_type(lookup_var(Name.c_str())) {
		if (full_type.first) {
			type = full_type.first->type;
			type_attr = full_type.first->type_attr;
		} else {
			type = nullptr;
			type_attr = 0;
		}
	}
	const std::string &getName() const { return Name; }
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		return ExprAST::dump(out << Name, ind);
	}
};

/// UnaryExprAST - Expression class for a unary operator.
class UnaryExprAST : public ExprAST {
	char Opcode[4];
	std::unique_ptr<ExprAST> Operand;

public:
	UnaryExprAST(const char* Op, std::unique_ptr<ExprAST> Operand)
		: ExprAST(Operand->type, Operand->type_attr), Operand(std::move(Operand)) {
		strcpy(Opcode, Op); 
	}
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "unary" << Opcode, ind);
		Operand->dump(out, ind + 1);
		return out;
	}
};

// conversions that have to be applied to Operands of binary Operator to make them compatible
struct BinOpConv {
	std::function<llvm::Value*(llvm::Value*)> LHS;
	std::function<llvm::Value*(llvm::Value*)> RHS;
	llvm::Type* res_type;
	unsigned res_attr;
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
                              const char* Op, SourceLocation Loc = CurLoc);

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
	
public:
	char Op[4];
	std::unique_ptr<ExprAST> LHS, RHS;
	BinOpConvSet conv;
	BinaryExprAST(SourceLocation Loc, const char* _Op, std::unique_ptr<ExprAST> LHS,
	              std::unique_ptr<ExprAST> RHS, BinOpConvSet conv, llvm::Type* desired_type = nullptr, unsigned desired_attrib = 0)
		: ExprAST(conv.compat.res_type, conv.compat.res_attr, Loc, desired_type, desired_attrib), LHS(std::move(LHS)), RHS(std::move(RHS)), conv(conv) {
		if (!desired_type && _Op[0] != '=' || desired_type && desired_type == llvm::Type::getInt1Ty(*Context.getContext())) {
			if (conv.ideal.res_type == llvm::Type::getInt1Ty(*Context.getContext())) {
				type = conv.ideal.res_type;
				type_attr = 0;
			}
		}
		strcpy(Op, _Op);
	}
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "binary" << Op, ind);
		LHS->dump(indent(out, ind) << "LHS:", ind + 1);
		RHS->dump(indent(out, ind) << "RHS:", ind + 1);
		return out;
	}
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes), as well as if it is an operator.
class PrototypeAST {

public:
	std::vector<std::string> Args;
	std::vector<llvm::Type*> ArgTypes;
	std::vector<unsigned> ArgAttribs;
	std::vector<std::pair<llvm::Type*, unsigned>> RetTypes;
	bool IsOperator;
	int Line;
	std::string Name;
	PrototypeAST(SourceLocation Loc, const std::string &Name,
	             std::vector<std::string> Args, bool IsOperator = false,
	             std::vector<std::pair<llvm::Type*, unsigned>> RetTypes = {}, std::vector<llvm::Type*> ArgTypes = {}, std::vector<unsigned> ArgAttribs = {})
		: Name(Name), Args(std::move(Args)), IsOperator(IsOperator),
		  Line(Loc.Line), RetTypes(RetTypes), ArgTypes(ArgTypes), ArgAttribs(ArgAttribs) {}
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

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
	std::string Callee;
	std::vector<std::unique_ptr<ExprAST>> Args;

public:
	CallExprAST(SourceLocation Loc, const std::string &Callee,
	            std::vector<std::unique_ptr<ExprAST>> Args)
		: ExprAST(nullptr, 0, Loc), Callee(Callee), Args(std::move(Args)) {
		auto FI = FunctionProtos.find(Callee);
		if (FI != FunctionProtos.end()) {
			if (FI->second->RetTypes.size() == 0) {
				type = llvm::Type::getVoidTy(*Context.getContext());
				type_attr = 0;
			} else if(FI->second->RetTypes.size() == 1) {
				type = FI->second->RetTypes[0].first;
				type_attr = FI->second->RetTypes[0].second;
			} else {
				LogError("call of function %s() returning %d objects is not implemented, yet", Callee.c_str(), FI->second->RetTypes.size());
			}
		} else {
			// constructors have no return value so failure is signaled by type == nullptr
			LogError("call to undeclared function %s()", Callee.c_str());
		}
	}
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "call " << Callee, ind);
		for (const auto &Arg : Args)
			Arg->dump(indent(out, ind + 1), ind + 1);
		return out;
	}
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
		: ExprAST(_Then.back()->type, _Then.back()->type_attr, Loc),
		  is_void(_Then.back()->type == llvm::Type::getVoidTy(*Context.getContext()) || !_Else.size()
		          || _Else.back()->type != _Then.back()->type
		          || (_Else.back()->type_attr & A_signed) != (_Then.back()->type_attr & A_signed)),
		  Cond(std::move(Cond)), Then(std::move(_Then)), Else(std::move(_Else)), ThenEndKind(ThenEndKind),
		  ElseEndKind(ElseEndKind)
		{
			if (is_void) {
				printf("void IfExpr: %p %p %u %u %s %s\n", Then.back()->type, Else.back()->type,
				       Then.back()->type_attr, Else.back()->type_attr,
				       type_table.get_name(Then.back()->type, Then.back()->type_attr & A_signed),
				       type_table.get_name(Else.back()->type, Else.back()->type_attr & A_signed));
				type = llvm::Type::getInt1Ty(*Context.getContext());
				type_attr = 0;
			}
		}
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "if", ind);
		Cond->dump(indent(out, ind) << "Cond:", ind + 1);
		Then[0]->dump(indent(out, ind) << "Then:", ind + 1);
		if (Else.size())
			Else[0]->dump(indent(out, ind) << "Else:", ind + 1);
		return out;
	}
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
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "for", ind);
		Start->dump(indent(out, ind) << "Cond:", ind + 1);
		End->dump(indent(out, ind) << "End:", ind + 1);
		Step->dump(indent(out, ind) << "Step:", ind + 1);
		Body->dump(indent(out, ind) << "Body:", ind + 1);
		return out;
	}
};

/// VarExprAST - Expression class for var/in
class VarExprAST : public ExprAST {
	std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
	std::unique_ptr<ExprAST> Body;

public:
	VarExprAST(
		std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames,
		std::unique_ptr<ExprAST> Body, SourceLocation Loc = CurLoc)
		: ExprAST(Loc), VarNames(std::move(VarNames)), Body(std::move(Body)) {}
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "var", ind);
		for (const auto &NamedVar : VarNames)
			NamedVar.second->dump(indent(out, ind) << NamedVar.first << ':', ind + 1);
		Body->dump(indent(out, ind) << "Body:", ind + 1);
		return out;
	}
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
	~FunctionAST() = default;
};
