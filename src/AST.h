#pragma once
#include "../include/volvox.hh"
#include "global.h"

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//


/// ExprAST - Base class for all expression nodes.

class InterfaceExprAST : public ExprAST {
	std::unique_ptr<ExprAST> expr;
	llvm::Value* rttype = nullptr;
public:
	InterfaceExprAST(std::unique_ptr<ExprAST> _expr) :
		ExprAST(_expr->ft, _expr->Loc, _expr->is_unknown_type), expr(std::move(_expr)) {}
	llvm::Value* codegen_raw();
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
	llvm::Value* codegen_raw() { return val; }
};

// empty parameter list in calls like "f()"
class EmptyExprAST : public ExprAST {
public:
	EmptyExprAST(SourceLocation Loc = CurLoc) : ExprAST(Loc) {}
	llvm::Value* codegen_raw() { return nullptr; }
};

// Class for all literals - 1.2, 3u, "str"
class LiteralExprAST : public ExprAST {

public:
	union LitValue Val;
	LiteralExprAST(const Token& tok, SourceLocation Loc = CurLoc) : ExprAST(tok.key, A_const |
		  (((tok.int_type.ID == llvm::Type::IntegerTyID &&
		     tok.int_type.is_signed) || tok.kind == tok_ptr_lit) ? A_signed : 0), Loc, tok.is_unknown_type), Val(tok.Val) {}
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
			errs() << "internal compiler error: unhandled literal type '" << *ft->type << "'\n";
			return out;
		}
	}
#endif
	llvm::Value *codegen_raw() override;
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes), as well as if it is an operator.
class PrototypeAST {

public:
	std::vector<std::string> Args;
	std::vector<volvoxc::FullType*> ArgTypes;
	std::vector<llvm::Type*> LLVMArgTypes; // to get LLVM function type
	std::vector<SourceLocation> ArgPos;
	volvoxc::FullType* RetType;
	SourceLocation retLoc;
	llvm::FunctionType* FT;
	bool IsVarArgs;
	bool IsOperator;
	llvm::GlobalValue::LinkageTypes link_type;
	int Line;
	std::string Name;
	PrototypeAST(SourceLocation Loc, const std::string &Name,
	             std::vector<std::string> Args, SourceLocation retLoc = CurLoc, bool IsOperator = false,
	             volvoxc::FullType* RetType_ = nullptr, std::vector<volvoxc::FullType*> ArgTypes = {},
	             std::vector<llvm::Type*> LLVMArgTypes = {}, std::vector<SourceLocation> _ArgPos = {},
	             bool IsVarArgs = false)
		: Name(Name), Args(Args), IsOperator(IsOperator), retLoc(retLoc),
		  Line(Loc.Line), RetType(RetType_ ? RetType_ : void_type), ArgTypes(ArgTypes),
		  ArgPos(_ArgPos), LLVMArgTypes(LLVMArgTypes), IsVarArgs(IsVarArgs) {
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

// Expressions that can the the LHS of an assignmen: `a = 1`, `b[3] = 4.5`, `s.a = 9`
class LvalueExprAST : public ExprAST {

public:
	std::string Name;
	LvalueExprAST(SourceLocation Loc, std::string Name = "") : ExprAST(Loc), Name(Name) {}
	// get a reference to the value
	// if this is an rvalue and silent_fail=true then the llvm::Type is returned
	// but the llvm::Value is NULL
	virtual std::pair<llvm::Type*,llvm::Value*> codegen_ref(bool silent_fail = false) = 0;
	llvm::Value* codegen_raw() override;
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public LvalueExprAST {

public:
	std::pair<FullVar*, bool> full_var; // and if it's global
	VariableExprAST(SourceLocation Loc, const std::string &Name)
		: LvalueExprAST(Loc, Name), full_var(lookup_var(Name.c_str())) {
		if (full_var.first) {
			dbgs() << "found variable " << Name << " type " << *full_var.first->ft.type << '\n';
			ft = &full_var.first->ft;
		}
		// if the variable name has not found in the database we don't generate
		// an error message here because this VariableExprAST could be the LHS of
		// an initialization e.g. `a := 42`
	}
	const std::string &getName() const { return Name; }
	// create reference to this variable - second result is the storage_type
	std::pair<llvm::Type*,llvm::Value*> codegen_ref(bool silent_fail = false) override;
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
		ft->proto = Proto;
	}
	const std::string &getName() const { return Name; }
	llvm::Value *codegen_raw() override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		return ExprAST::dump(out << Name, ind);
	}
#endif
};

// IndexExprAST - Expressions like x[2] or y["key"]
class IndexExprAST : public LvalueExprAST {

public:
	std::unique_ptr<ExprAST> Field, Index;
	llvm::Type* ml_elem_type = nullptr;
	llvm::Type* deepest_elem_type = nullptr;
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
			} else {
				errs() << Index->Loc << ": index for non array expression\n";
				ft->type = nullptr;
			}
		}
	std::tuple<uint64_t,llvm::Value*,llvm::Value*> getMLIdxOffset(
		llvm::Type* elem_type, std::vector<llvm::Value*>& Idxs,
		llvm::Value* Dims, int idx_idx, int dim_idx);
	llvm::Value *codegen_raw() override;
	std::pair<llvm::Type*,llvm::Value*> codegen_ref(bool silent_fail = false) override;
	llvm::Value* codegen_ref0(std::vector<llvm::Value*>& Idxs, llvm::Type*& ml_field_type);
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
	llvm::Value *codegen_raw() override;
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
	ListExprAST* list;
	bool struct_err = false;
	TokenKind kind = TokenKind('[');
	bool explicit_order;
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
	llvm::Type* key_type;
	unsigned key_type_attr;
	std::vector<ExprAST*> valid_exprs;
	std::vector<std::function<llvm::Value*(llvm::Value*)>> Elem_convs;
	std::vector<unsigned> LitDims; // maximum used index in literal for each level
	AggregateExprAST(SourceLocation Loc, llvm::Type* key_type,
	                 unsigned key_type_attr = 0,
	                 std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                 std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {},
	                 volvoxc::FullType* el_type = nullptr) :
		ListExprAST(Loc, std::move(_Elements)), key_type(key_type),
		key_type_attr(key_type_attr), valid_exprs(std::move(_valid_exprs)), LitDims(std::move(_LitDims))
		{
			auto convs = getArrayConv(this, el_type ? el_type->type : nullptr, el_type ? el_type->type_attr : 0);
			if (!convs.first) {
				ft = nullptr;
				errs() << "internal problem when creating AggregateExprAST\n";
			}
			ft->elem_type = convs.first;
			Elem_convs = convs.second;
		}
	AggregateExprAST(SourceLocation Loc, volvoxc::FullType* _ft, llvm::Type* key_type,
	                 unsigned key_type_attr = 0,
	                 std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                 std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {}) :
		ListExprAST(Loc, std::move(_Elements), _ft), key_type(key_type), LitDims(std::move(_LitDims)),
		key_type_attr(key_type_attr), valid_exprs(std::move(_valid_exprs))
		{
			auto convs = getArrayConv(this, ft->elem_type->type, ft->elem_type->type_attr);
			if (!convs.first) {
				ft = nullptr;
				errs() << "internal problem creating AggregateExprAST\n";
			}
			else
				Elem_convs = convs.second;
		}
	std::pair<volvoxc::FullType*,std::vector<std::function<llvm::Value*(llvm::Value*)>>> getArrayConv(
		ListExprAST* List, llvm::Type* elem_type = nullptr, unsigned elem_attr = 0);
};

// map keys / array indices
union AggregateKey {
	uint64_t Uint;
	int64_t Int;
	double Double;
	float Float;
	llvm::Constant* String;
};

class MapExprAST : public AggregateExprAST {
public:
	std::vector<AggregateKey> Keys;
	MapExprAST(SourceLocation Loc, llvm::Type* key_type, std::vector<AggregateKey> _Keys = {},
	           volvoxc::FullType* elem_type = nullptr, std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	           std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {}, unsigned key_type_attr = 0) :
		AggregateExprAST(Loc, key_type, key_type_attr,
		                 std::move(_Elements), std::move(_valid_exprs), std::move(_LitDims), elem_type), Keys(_Keys) {}
};

class FixedArrayExprAST : public AggregateExprAST {
	uint64_t iter_idx;
public:
	std::vector<std::unique_ptr<ExprAST>> Dims; // known at run time
	std::vector<SourceLocation> LenLocs;
	FixedArrayExprAST(SourceLocation Loc,
	                  std::vector<std::unique_ptr<ExprAST>> _Elements = {},
	                  std::vector<ExprAST*> _valid_exprs = {}, std::vector<unsigned> _LitDims = {},
	                  volvoxc::FullType* el_type = nullptr,
	                  std::vector<std::unique_ptr<ExprAST>> _Dims = {}, std::vector<SourceLocation> LenLocs = {}) :
		AggregateExprAST(Loc, llvm::Type::getInt64Ty(Context), 0, std::move(_Elements), std::move(_valid_exprs),
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
		AggregateExprAST(Loc, _ft, llvm::Type::getInt64Ty(Context), 0, std::move(_Elements), std::move(_valid_exprs),
		                 std::move(_LitDims)), Dims(std::move(_Dims)), LenLocs(LenLocs) {}
	llvm::Value* getArrayLitVal(llvm::ArrayType* initializer_type, ListExprAST* List);
	llvm::Value* codegen_raw() override;
#ifndef NDEBUG
	llvm::raw_ostream &dump(llvm::raw_ostream &out, int ind) override {
		ExprAST::dump(out << "Fixed Array size: " << Elements.size(), ind);
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
	llvm::Value *codegen_raw() override;
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
	              std::unique_ptr<ExprAST> _RHS, BinOpConvSet conv = {})
		: ExprAST(conv.compat.res_type, conv.compat.res_attr, Loc,
		          _RHS->is_unknown_type && _LHS->is_unknown_type),
		  LHS(std::move(_LHS)), RHS(std::move(_RHS)), conv(conv) {
		strcpy(Op, _Op);
		if (strcmp(Op, ":=")) {
			if (LHS->ft->type && !LHS->ft->type->isSingleValueType())
				ft = LHS->ft;
			else if (RHS->ft->type && !RHS->ft->type->isSingleValueType())
				ft = RHS->ft;
			else if (conv.ideal.res_type && (!conv.compat.res_type || conv.ideal.res_type == llvm_bool_type)
			         && strcmp(Op, "=")) { // '=' means assignment by default - compare if bool is expected
				dbgs() << "setting expression type to " << *conv.ideal.res_type << '\n';
				ft->type = conv.ideal.res_type;
				ft->type_attr = conv.ideal.res_attr;
			}
		} else {
			LHS->ft = RHS->ft;
		}
		if (!strcmp(Op, "|") || !strcmp(Op, "&") || !strcmp(Op, "><")) {
			if (!ft->type)
				ft->type = LHS->ft->type;
		}
	}
	llvm::Value *codegen_raw() override;
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
		: ExprAST(Callee_->ft->proto->RetType, Loc), Callee(std::move(Callee_)), Args(std::move(Args)) {}
	llvm::Value *codegen_raw() override;
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
	std::vector<std::unique_ptr<ExprAST>> Then, Else;
	BinOpConvSet conv;

public:
	int ThenEndKind, ElseEndKind; // maybe tok_else, tok_end, tok_return, ...

	IfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> _Cond,
	          std::vector<std::unique_ptr<ExprAST>> _Then, std::vector<std::unique_ptr<ExprAST>> _Else,
	          int ThenEndKind, int ElseEndKind, BinOpConvSet conv = {})
		: ExprAST(_Else.size() ? conv.compat.res_type : llvm::Type::getVoidTy(Context), conv.compat.res_attr, Loc,
		          _Else.size() && _Then.back()->is_unknown_type & _Else.back()->is_unknown_type),
		  Cond(std::move(_Cond)), Then(std::move(_Then)), Else(std::move(_Else)), ThenEndKind(ThenEndKind),
		  ElseEndKind(ElseEndKind), conv(conv)
		{}
	llvm::Value *codegen_raw() override;
	llvm::Value* createCondBranch(llvm::BasicBlock *MergeBB, bool isElse);
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
	llvm::Value *codegen_raw() override;
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
