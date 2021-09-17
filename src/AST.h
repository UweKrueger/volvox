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
	LiteralExprAST(const Token& tok, SourceLocation Loc = CurLoc) : ExprAST(tok.key, A_const, Loc), Val(tok.Val) {}
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		switch (type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			if (type_attr | A_signed)
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
	std::string Name;

public:
	VariableExprAST(SourceLocation Loc, const std::string &Name)
		: ExprAST(type_table.get_full(Name.c_str()), Loc), Name(Name) {}
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
		: Operand(std::move(Operand)) {
		strcpy(Opcode, Op); 
	}
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "unary" << Opcode, ind);
		Operand->dump(out, ind + 1);
		return out;
	}
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
	char Op[4];
	std::unique_ptr<ExprAST> LHS, RHS;

public:
	BinaryExprAST(SourceLocation Loc, const char* _Op, std::unique_ptr<ExprAST> LHS,
				  std::unique_ptr<ExprAST> RHS)
		: ExprAST(LHS->type, LHS->type_attr, Loc), LHS(std::move(LHS)), RHS(std::move(RHS)) {
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
	std::vector<std::string> Args;
	std::vector<llvm::Type*> ArgTypes;
	std::vector<unsigned> ArgAttribs;
	bool IsOperator;
	int Line;

public:
	std::string Name;
	llvm::Type* RetType;
	unsigned type_attr;
	PrototypeAST(SourceLocation Loc, const std::string &Name,
				 std::vector<std::string> Args, bool IsOperator = false,
				 llvm::Type* RetType = llvm::Type::getDoubleTy(TheContext), unsigned type_attr = 0, std::vector<llvm::Type*> ArgTypes = {})
		: Name(Name), Args(std::move(Args)), IsOperator(IsOperator),
		  Line(Loc.Line), RetType(RetType), type_attr(type_attr), ArgTypes(ArgTypes) {}
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
			type = FI->second->RetType;
			type_attr = FI->second->type_attr;
		} else {
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
	std::unique_ptr<ExprAST> Cond, Then, Else;

public:
	IfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Cond,
			  std::unique_ptr<ExprAST> Then, std::unique_ptr<ExprAST> Else)
		: ExprAST(llvm::Type::getDoubleTy(TheContext), 0, Loc), Cond(std::move(Cond)), Then(std::move(Then)),
		  Else(std::move(Else)) {}
	llvm::Value *codegen() override;
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "if", ind);
		Cond->dump(indent(out, ind) << "Cond:", ind + 1);
		Then->dump(indent(out, ind) << "Then:", ind + 1);
		Else->dump(indent(out, ind) << "Else:", ind + 1);
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
			   std::unique_ptr<ExprAST> Body)
		: VarName(VarName), Start(std::move(Start)), End(std::move(End)),
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
		std::unique_ptr<ExprAST> Body)
		: VarNames(std::move(VarNames)), Body(std::move(Body)) {}
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
	std::unique_ptr<PrototypeAST> Proto;
	std::unique_ptr<ExprAST> Body;

	FunctionAST(std::unique_ptr<PrototypeAST> Proto,
				std::unique_ptr<ExprAST> Body)
		: Proto(std::move(Proto)), Body(std::move(Body)) {}
	llvm::Function *codegen();
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) {
		indent(out, ind) << "FunctionAST\n";
		++ind;
		indent(out, ind) << "Body:";
		return Body ? Body->dump(out, ind) : out << "null\n";
	}
};
