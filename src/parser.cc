#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
/// token the parser is looking at.  getNextToken reads another token from the
/// lexer and updates CurTok with its results.
Lexer lex;
Token CurTok;

FVListElem* anon_fullvars = nullptr;
FVListElem** anon_fullvars_end = &anon_fullvars;
extern llvm::ExitOnError ExitOnErr;

Token getNextToken(eXpect expect) { return CurTok = lex.gettok(expect); }
Token purgeLine() { return CurTok = lex.purge_line(); }

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static inline int GetTokPrecedence() {
	return (CurTok.kind < 0 && CurTok.kind > tok_last_op) ? (-CurTok.kind) << 8 : -256;
}

// same as above but take into account that some operators are right binding
static inline int NextTokPrecedence() {
	int prec = GetTokPrecedence();
	// assignments and the invisible operator are right binding
	if (CurTok.kind == tok_assign || CurTok.kind == tok_)
		prec++;
	return prec;
}

static bool Expect(int tok, eXpect expect = eNone) {
	bool res = CurTok.kind == tok;
	if (res) {
		getNextToken(expect);
	} else {
		errs() << "unexpected '" << CurTok.str() << "' - expected '" << Token::tokName(tok) << "'\n";
	}
	return res;
}

static void Eat(int tok, eXpect expect = eNone) {
	if (CurTok.kind == tok) {
		getNextToken(expect);
	}
}

static std::unique_ptr<ExprAST> ParseExpression();

volvox::FullType* ParseType(bool allow_attribute, eXpect expect) {
	unsigned attribs = 0;
	while (CurTok.kind != tok_identifier) {
		if (allow_attribute) {
			switch (CurTok.kind) {
			case tok_atomic:
				attribs |= A_atomic;
				break;
			case tok_shared:
				attribs |= A_shared;
				break;
			case tok_iso:
				attribs |= A_iso;
				break;
			case tok_const:
				attribs |= A_const;
				break;
			}
		} else {
			if (CurTok.kind == tok_packed)
				attribs |= A_packed;
		}
		if (attribs)
			getNextToken();
		switch (CurTok.kind) {
		case '[': {
			dbgs() << "parsing indexed type\n";
			getNextToken();
			int64_t dim = -1;
			llvm::ConstantInt* Dim = nullptr;
			if (CurTok.kind == ']') {
				getNextToken();
			} else {
				if (auto e = ParseExpression()) {
					auto Vdim = e->codegen();
					if (!Vdim) {
						errs() << "cannot generate code for index\n";
						return nullptr;
					}
					if (Dim = llvm::dyn_cast<llvm::ConstantInt>(Vdim)) {
						dim = Dim->getSExtValue();
					} else {
						errs() << "dimension must be constant int\n";
						return nullptr;
					}
				} else {
					errs() << "cannot parse dimension expression\n";
					return nullptr;
				}
				if (!Expect(']')) {
					errs() << "'[' expected\n";
					return nullptr;
				}
				if (dim < 0) {
					errs() << "dimension must be a positive int (not " << dim << "\n";
					return nullptr;
				}
			} 
			auto elem_type = ParseType();
			if (!elem_type)
				return nullptr;
			llvm::Type* array_type;
			if (dim >= 0) {
				array_type = llvm::ArrayType::get(elem_type->type, dim);
			} else {
				llvm::Type* ptr = llvm::PointerType::get(elem_type->type, 0);
				array_type = llvm::StructType::get(ptr, llvm_int_type);
			}
			return new_FullType(array_type, 0, nullptr, dim, elem_type);
		}
			break;
		case '{': {
			// struct type
			getNextToken();
			std::vector<std::string> FieldNames;
			std::vector<volvox::FullType*> FieldTypes;
			std::vector<llvm::Type*> LLVMFieldTypes;
			for (;;) {
				if (CurTok.kind != tok_identifier) {
					errs () << "Unexpected '" << CurTok.str() << "' in struct declaration - field name expected\n";
					return nullptr;
				}
				FieldNames.push_back(IdentifierStr);
				getNextToken(eComma);
				auto type = ParseType(true);
				if (!type) {
					errs() << "Unexpected '" << CurTok.str() << "' in struct declaration - type name expected\n";
					return nullptr;
				}
				FieldTypes.push_back(type);
				LLVMFieldTypes.push_back(type->type);
				getNextToken();
				if (CurTok.kind == '}')
					break;
				Eat(',', eBinOp);
			}
			getNextToken();
			llvm::Type* struct_type = llvm::StructType::get(Context, LLVMFieldTypes, (bool)(attribs & A_packed));
			MapNode* fields = map_string_new_map();
			for (int i=0; i<FieldNames.size(); i++) {
				MapNode* new_node = map_string_tag_insert(&fields, FieldNames[i].c_str(), i, MapValue{ .src_ptr = FieldTypes[i] }, 0, false);
				if (!new_node) {
					errs() << "Duplicate field name '" << FieldNames[i] << "' in struct declaration\n";
					return nullptr;
				}
			}
				
			return new_FullType(struct_type, attribs, nullptr /*DIType*/, FieldNames.size(), (volvox::FullType*)fields);
		}
			break;
		case '&':
			do {
				attribs = (attribs & 0xffff) | ((attribs & 0xffff0000) + 0x10000);
				getNextToken(eBinOp);
			} while (CurTok.kind == '&');
			if (CurTok.kind == tok_identifier)
				break;
			// else fallthough to error
		default:
			errs() << "Unexpected '" << CurTok.str() << "' - type name expected\n";
			return nullptr;
		}
		getNextToken(eBinOp);
	}
	auto type = type_table.get_full(IdentifierStr.c_str());
	if (!type) {
		errs() << "Unknown type '" << IdentifierStr << "'\n";
		return nullptr;
	}
	//if (type.type_attr)
	//	attribs |= A_signed;
	return type;
}

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(eBinOp); // consume the number
	return Result;
}

static std::unique_ptr<ExprAST> ParseStringExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(eBinOp); // consume the string
	return Result;
}

static std::unique_ptr<ExprAST> ParsePointerExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(eBinOp); // consume the pointer
	return Result;
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
	getNextToken(); // eat (.
	auto V = ParseExpression();
	if (!V)
		return nullptr;

	Eat(')', eBinOp);
	return V;
}

static std::vector<std::unique_ptr<ExprAST>> SplitExprList(std::unique_ptr<ExprAST> Arg) {
	std::vector<std::unique_ptr<ExprAST>> Args;
	// If we have an EmptyExprAST just return the empty list
	if (auto empty_expr = dynamic_cast<EmptyExprAST*>(Arg.get())) {
		return Args;
	}
	// The arguments are parsed as a tree of binary expressions (Op=',') where
	// all objects are in the leaves.
	// Due to operator precedence rules the tree is stricly left-heavy and can be
	// processed right to left without the need for recursions. We just have to iterate
	// through the binary nodes and front-push each right leave (RHS) to the Args list.
	while (auto bin_expr = dynamic_cast<BinaryExprAST*>(Arg.get())) {
		if (bin_expr->Op[0] == ',') {
			Args.insert(Args.begin(), std::move(bin_expr->RHS));
			Arg = std::move(bin_expr->LHS);
		} else {
			break;
		}
	}
	Args.insert(Args.begin(), std::move(Arg));
	return Args;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
	std::string IdName = IdentifierStr;

	SourceLocation LitLoc = CurLoc;

	getNextToken(eBinOp); // eat identifier.
	// first try to find a function with this name
	auto F = FunctionProtos.find(IdName);
	if (F != FunctionProtos.end()) {
		return std::make_unique<FunctionExprAST>(LitLoc, IdName, F->second.get());
	}
	
	return std::make_unique<VariableExprAST>(LitLoc, IdName);
}

static std::unique_ptr<ExprAST> ParseAggregateExpr() {
	bool is_dynamic;
	AggregateKind kind;
	TokenKind closing;
	switch (CurTok.kind) {
	case '{':
		is_dynamic = true;
		kind = AnyDyn;
		closing = (TokenKind)'}';
		break;
	case '[':
		is_dynamic = false;
		kind = AnyFixed;
		closing = (TokenKind)']';
		break;
	default:
		errs() << "AggregateExpr: unexpected '" << CurTok.str() << "' (expected '{' or '[')\n";
		return nullptr;
	}
	SourceLocation loc = CurLoc;
	getNextToken(); // eat '{'/'['
	if (CurTok.kind == closing) {
		getNextToken(eBinOp);
		// TODO: parse type and given elements
		return std::make_unique<AggregateExprAST>(loc, kind);
	}
	if (auto Elem = ParseExpression()) {
		Expect(closing, eBinOp);
		auto Elems = SplitExprList(std::move(Elem));
		if (auto bin_expr = dynamic_cast<BinaryExprAST*>(Elems[0].get())) {
			if (bin_expr->Op[0] == ':') { // struct or map
				if (auto ident = dynamic_cast<VariableExprAST*>(bin_expr->LHS.get())) {
					if (is_dynamic) {
						kind = Array;
						
					} else {
						kind = Struct;
					}
				} else if (auto key = dynamic_cast<LiteralExprAST*>(bin_expr->LHS.get())) {
					kind = is_dynamic ? Map : FixedArray;
				} else {
					errs() << "AggregateExpr: illegal expression before ':'\n";
					return nullptr;
				}
			}
		}
		return std::make_unique<AggregateExprAST>(loc, /* kind */ FixedArray, std::move(Elems));
	} else {
		errs() << "AggregateExpr: unexpected '" << CurTok.str() << "' (expected expression)\n";
		return nullptr;
	}
}

static std::pair<std::vector<std::unique_ptr<ExprAST>>, int> ParseExprList();

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
static std::unique_ptr<ExprAST> ParseIfExpr() {
	SourceLocation IfLoc = CurLoc;

	getNextToken(); // eat the if.

	// condition - expect bool.
	auto Cond = ParseExpression();
	if (!Cond)
		return nullptr;

	if (CurTok.kind != tok_then) {
		errs() << "expected then\n";
		return nullptr;
	}
	getNextToken(); // eat the then

	auto Then = ParseExprList();
	if (Then.second != tok_else)
		getNextToken();
	if (CurTok.kind != tok_else) {
		errs() << "expected else\n";
		return nullptr;
	}
	getNextToken();

	auto Else = ParseExprList();
	if (Else.second == tok_end)
		getNextToken(eBinOp);
	return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then.first),
	                                   std::move(Else.first), Then.second, Else.second);
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
	getNextToken(); // eat the for.

	if (CurTok.kind != tok_identifier) {
		errs() << "expected identifier after for\n";
		return nullptr;
	}

	std::string IdName = IdentifierStr;
	getNextToken(eBinOp); // eat identifier.

	if (CurTok.kind != tok_assign) {
		errs() << "expected '=' after for\n";
		return nullptr;
	}
	getNextToken(); // eat '='.

	auto Start = ParseExpression();
	if (!Start)
		return nullptr;
	if (CurTok.kind != ',') {
		errs() << "expected ',' after for start value\n";
		return nullptr;
	}
	getNextToken();

	auto End = ParseExpression();
	if (!End)
		return nullptr;

	// The step value is optional.
	std::unique_ptr<ExprAST> Step;
	if (CurTok.kind == ',') {
		getNextToken();
		Step = ParseExpression();
		if (!Step)
			return nullptr;
	}

	if (CurTok.kind != tok_in) {
		errs() << "expected 'in' after for\n";
		return nullptr;
	}
	getNextToken(); // eat 'in'.

	auto Body = ParseExpression();
	if (!Body)
		return nullptr;

	return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
	                                    std::move(Step), std::move(Body));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
///   ::= ifexpr
///   ::= forexpr
///   ::= varexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
	switch (CurTok.kind) {
	case tok_eof:
		errs() << "EOF when expecting an expression\n";
		exit(1);
	case tok_identifier:
		return ParseIdentifierExpr();
	case tok_number:
		return ParseNumberExpr();
	case tok_str_lit:
		return ParseStringExpr();
	case tok_ptr_lit:
		return ParsePointerExpr();
	case '(':
		return ParseParenExpr();
	case ')':
		return std::make_unique<EmptyExprAST>();
	case '{':
	case '[':
		return ParseAggregateExpr();
	case tok_if:
		return ParseIfExpr();
	case tok_for:
		return ParseForExpr();
	default:
		errs() << "unknown token '" << CurTok.kind << "' '" << CurTok.str() << "' when expecting an expression\n";;
		purgeLine();
		return nullptr;
	}
}

/// unary
///   ::= primary
///   ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary() {
	// If the current token is not an operator, it must be a primary expr.
	if (CurTok.kind != tok_unary || CurTok.kind == '(' || CurTok.kind == ',')
		return ParsePrimary();
	
	// If this is a unary operator, read it.
	std::string Op = IdentifierStr;
	getNextToken();
	if (auto Operand = ParseUnary())
		return std::make_unique<UnaryExprAST>(Op.c_str(), std::move(Operand));
	return nullptr;
}

/// binoprhs
///   ::= ('+' unary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();
		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (TokPrec < ExprPrec) {
			return LHS;
		}
		// Okay, we know this is a binop.
		std::string BinOp = IdentifierStr;
		SourceLocation BinLoc = CurLoc;
		getNextToken(); // eat binop
		// Parse the unary expression after the binary operator.
		auto RHS = ParseUnary();
		if (!RHS)
			return nullptr;

		// If BinOp binds less tightly with RHS than the operator after RHS, let
		// the pending operator take RHS as its LHS.
		if (TokPrec < NextTokPrecedence()) {
			RHS = ParseBinOpRHS(TokPrec, std::move(RHS));
			if (!RHS)
				return nullptr;
		}
		// Merge LHS/RHS.
		// save types befor objects are moved
		auto LHS_type = LHS->ft->type;
		auto LHS_attr = LHS->ft->type_attr;
		auto LHS_is_unknown_type = LHS->is_unknown_type;
		auto RHS_type = RHS->ft->type;
		auto RHS_attr = RHS->ft->type_attr;
		auto RHS_is_unknown_type = RHS->is_unknown_type;
		if (inside_function && BinOp == ":=") {
			if (auto VarL = dynamic_cast<VariableExprAST*>(LHS.get())) {
				auto type_descr = MakeType(RHS_type, RHS_attr & A_signed, RHS_is_unknown_type);
				llvm::Type* type = std::get<0>(type_descr);
				bool is_signed = std::get<2>(type_descr);
				FullVar fv = {
					.ft = *RHS->ft
				};
				fv.ft.type = type;
				fv.ft.type_attr = is_signed ? 1U : 0U;

				if (!locals_table.back().insert(VarL->Name.c_str(), fv)) {
					errs() << "variable " << VarL->Name << " already exists in current scope\n";
					return nullptr;
				}
			} else {
				errs() << "left operand of \":=\" must be a variable\n";
				return nullptr;
			}
		} else if (LHS_type && LHS_type->isFunctionTy() && (BinOp[0] == '(' || BinOp[0] == '\0')) {
			auto Args = SplitExprList(std::move(RHS));
			LHS = std::make_unique<CallExprAST>(LHS->Loc, std::move(LHS), std::move(Args));
			if (verbosity >= 4)
				dbgs() << "create call expr " << *dynamic_cast<CallExprAST*>(LHS.get())->Callee->ft->type << "\n";
			continue;
		} else if (LHS_type && LHS_type->isAggregateType() && BinOp[0] == '[') {
			LHS = std::make_unique<IndexExprAST>(LHS->Loc, std::move(LHS), std::move(RHS));
			continue;
		}
		LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp.c_str(), std::move(LHS), std::move(RHS),
		                                      convBinOp(LHS_type, RHS_type, LHS_attr, RHS_attr,
		                                                LHS_is_unknown_type, RHS_is_unknown_type, BinOp.c_str()));
	}
}

/// expression
///   ::= unary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression() {
	auto LHS = ParseUnary();
	if (!LHS)
		return nullptr;

	return ParseBinOpRHS(0, std::move(LHS));
}

static std::pair<std::unique_ptr<ExprAST>, int> ParseExprOrReturn() {
	while (CurTok.kind == ';')
		getNextToken();
	auto kind = CurTok.kind;
	if (kind == tok_return || kind == tok_else || kind == tok_end) {
		if (kind == tok_return) {
			getNextToken(eColon);
			if (CurTok.kind == ';') 
				return { nullptr, kind };
			else
				return { ParseExpression(), kind };
		}
		else
			return { nullptr, kind };
	} else {
		return { ParseExpression(), 0 };
	}
}

static std::pair<std::vector<std::unique_ptr<ExprAST>>, int> ParseExprList() {
	std::vector<std::unique_ptr<ExprAST>> expr_list;
	int end_kind = 0;
	while (!end_kind) {
		auto expr = ParseExprOrReturn();
		end_kind = expr.second;
		if (expr.first) {
			if (!end_kind)
				if (auto I = dynamic_cast<IfExprAST*>(expr.first.get()))
					if (I->ThenEndKind == tok_return && I->ElseEndKind == tok_return)
						end_kind = tok_leave;
			expr_list.push_back(std::move(expr.first));
		}
	}
	return { std::move(expr_list), end_kind };
}

/// prototype
///   ::= id '(' id* ')'
///   ::= binary LETTER number? (id, id)
///   ::= unary LETTER (id)
static std::unique_ptr<PrototypeAST> ParsePrototype() {
	std::string FnName;

	SourceLocation FnLoc = CurLoc;

	unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
	unsigned BinaryPrecedence = 30;
	std::vector<std::string> ArgNames;
	std::vector<volvox::FullType*> ArgTypes;
	std::vector<llvm::Type*> LLVMArgTypes;
	bool is_method;
	bool isVarArgs = false;

	switch (CurTok.kind) {
	case '(': {
		is_method = true;
		unsigned attribs = 0;
		getNextToken();
		if (CurTok.kind != tok_identifier) {
			errs() << "Unexpected '" << CurTok.str() << "' in method prototype - receiver name expected\n";
			return nullptr;
		}
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		auto type = ParseType(true);
		if (!type->type) {
			errs() << "Unexpected '" << CurTok.str() << "' in method prototype - type name expected\n";
			return nullptr;
		}
		ArgTypes.push_back(type);
		LLVMArgTypes.push_back(type->type);
		getNextToken();
		Expect(')');
	}
	default:
		is_method = false;
	}
	switch (CurTok.kind) {
	case tok_identifier:
		FnName = IdentifierStr;
		Kind = 0;
		getNextToken();
		break;
	case tok_unary:
		getNextToken();
		if (!isascii(CurTok.kind)) {
			errs() << "Expected unary operator\n";
			return nullptr;
		}
		FnName = "unary";
		FnName += (char)CurTok.kind;
		Kind = 1;
		getNextToken();
		break;
	case tok_colon: // ... not really
		getNextToken();
		if (!isascii(CurTok.kind)) {
			errs() << "Expected binary operator\n";
			return nullptr;
		}
		FnName = "binary";
		FnName += (char)CurTok.kind;
		Kind = 2;
		getNextToken();

		// Read the precedence if present.
		if (CurTok.kind == tok_number) {
			if (CurTok.Val.Int < 1 || CurTok.Val.Int > 100) {
				errs() << "Invalid precedence: must be 1..100\n";
				return nullptr;
			}
			BinaryPrecedence = (unsigned)CurTok.Val.Int;
			getNextToken();
		}
		break;
	default:
		errs() << "Expected function name in prototype\n";
		return nullptr;
	}
	if (!Expect('(')) return nullptr;
	if (CurTok.kind == ')')
		goto noargs;
	for (;;) {
		if (CurTok.kind != tok_identifier) {
			if (CurTok.kind == tok_ellipsis) {
				isVarArgs = true;
				getNextToken();
				if (CurTok.kind != ')') {
					errs() << "Unexpected '" << CurTok.str() << "' after '...' - ')' expected\n";
					return nullptr;
				}
				else
					break;
			}
			errs() << "Unexpected '" << CurTok.str() << "' in function arg list - arg name expected\n";
			return nullptr;
		}	
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		auto type = ParseType(true);
		if (!type) {
			errs() << "Unexpected '" << CurTok.str() << "' in function arg list - type name expected\n";
			return nullptr;
		}
		ArgTypes.push_back(type);
		LLVMArgTypes.push_back(type->type);
		getNextToken();
		if (CurTok.kind == ')')
			break;
		Eat(',');
	}
noargs:
	Eat(')', eColon); //getNextToken(); // eat ')'.
	// parse return type(s)
	volvox::FullType* RetType = nullptr;
	while (CurTok.kind != ';') {
		auto type = ParseType(true);
		if (!type) {
			errs() << "error parsing return type of function prototype\n";
			return nullptr;
		} else if (RetType) {
			errs() << "functions returning multiple objecs is not implemented, yet\n";
			return nullptr;
		}
		RetType = type;
		getNextToken(eColon);
	}
	// getNextToken();
	// Verify right number of names for operator.
	if (Kind && ArgNames.size() != Kind) {
		errs() << "Invalid number of operands for operator\n";
		return nullptr;
	}

	return std::make_unique<PrototypeAST>(FnLoc, FnName, ArgNames, Kind != 0, RetType, ArgTypes, LLVMArgTypes, isVarArgs);
}

/// definition ::= 'fn' prototype expression
std::unique_ptr<FunctionAST> ParseDefinition() {
	getNextToken(); // eat fn.
	auto Proto = ParsePrototype();
	prompt_indent++;
	if (!Proto)
		return nullptr;
	auto sz = Proto->Args.size();
	// initialize local vars lookup table with function arguments
	for (int i=0; i<sz; i++) {
		FullVar fv = {
			.ft = *Proto->ArgTypes[i]
		};
		bool is_new = locals_table.back().insert(Proto->Args[i].c_str(), fv);
		if (!is_new) {
			errs() << "duplicat function arg '" << Proto->Args[i] << "'\n";
			return nullptr;
		}
	}
	auto ProtoRef = Proto.get();
	FunctionProtos[Proto->getName()] = std::move(Proto);
	std::pair<std::vector<std::unique_ptr<ExprAST>>, int> Elist = ParseExprList();
	prompt_indent = 0;
	return std::make_unique<FunctionAST>(ProtoRef, std::move(Elist.first), Elist.second);
}

std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	SourceLocation FnLoc = CurLoc;
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
				errs() << "cannot evalute expression\n";
				return nullptr;
			} else {
				errs() << "Could not deduce type of expression\n";
				if (E->ft)
					E->ft->dump();
				return nullptr;
			}
		}
		if (comp_mode == comp_jit) {
#if LLVM_VERSION_MAJOR >= 12
			ExitOnErr(TheJIT->addModule(
				          llvm::orc::ThreadSafeModule(std::move(TheModule), TS_Context)));
#else
			TheJIT->addModule(std::move(TheModule));
#endif
			InitializeModuleAndPassManager();
		}
		// Make an anonymous proto.
		volvox::FullType* TheType = type_table.get_full("bool");
		auto Proto = std::make_unique<PrototypeAST>(FnLoc, "__anon_expr",
		                                            std::vector<std::string>(),
		                                            false, TheType);
		std::vector<std::unique_ptr<ExprAST>> GlobalExprList;
		// E->ft->dump();
		if (last_shadow_restorer) {
			auto restorer_proto = FunctionProtos.find(last_shadow_restorer);
			if (restorer_proto == FunctionProtos.end()) {
				errs() << "could not find restorer '" << last_shadow_restorer << "'\n";
			} else {
				auto restorer = std::make_unique<FunctionExprAST>(FnLoc, last_shadow_restorer, restorer_proto->second.get());
				auto restorer_call = std::make_unique<CallExprAST>(FnLoc, std::move(restorer), std::move(std::vector<std::unique_ptr<ExprAST>>()));
				GlobalExprList.push_back(std::move(restorer_call));
			}
		}
		if (E->ft->type->isVoidTy()) {
			GlobalExprList.push_back(std::move(E));
			GlobalExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(true))));
		} else {
			llvm::Constant* rttype_ptr = getRtType(E->ft);
			if (E->ft->type->isAggregateType()) {
				// pass by reference
				E = std::make_unique<UnaryExprAST>("&", std::move(E));
			}
			if (auto V = dynamic_cast<VariableExprAST*>(E.get()))
				if (auto ty = llvm::dyn_cast<llvm::IntegerType>(E->ft->type))
					if (ty->getBitWidth() == 64)
						E = std::make_unique<UnaryExprAST>("&", std::move(E));
			std::string mangled_println = "_ZN6volvox7printlnEPKcPKNS_6RtTypeEz";
			auto println_proto = FunctionProtos.find(mangled_println);
			if (println_proto == FunctionProtos.end()) {
				errs() << "Fatal error: could not find 'println' function\n";
				return nullptr;
			}
			auto volvox_println = std::make_unique<FunctionExprAST>(FnLoc, mangled_println, println_proto->second.get());
			std::vector<std::unique_ptr<ExprAST>> PrintArgs;
			bool is_string = E->ft->type->isPointerTy();
			if (is_string)
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string(E->ft->type->isPointerTy() ? "\"" : "")))));
			else
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
			PrintArgs.push_back(std::move(std::make_unique<ConstExprAST>(rttype_ptr)));
			// println requires parameters for width, precision and flags - pass 0s (and signed bit) to get defaults
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(0LL))));
			PrintArgs.push_back(std::move(E));
			if (is_string)
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token(std::string("\"")))));
			else
				PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
			PrintArgs.push_back(std::move(std::make_unique<LiteralExprAST>(Token((void*)0))));
			auto print_call = std::make_unique<CallExprAST>(FnLoc, std::move(volvox_println), std::move(PrintArgs));
			GlobalExprList.push_back(std::move(print_call));
		}
		if (last_shadow_saver) {
			auto saver_proto = FunctionProtos.find(last_shadow_saver);
			if (saver_proto == FunctionProtos.end()) {
				errs() << "could not find saver '" << last_shadow_saver << "\n";
			} else {
				auto saver = std::make_unique<FunctionExprAST>(FnLoc, last_shadow_saver, saver_proto->second.get());
				auto saver_call = std::make_unique<CallExprAST>(FnLoc, std::move(saver), std::move(std::vector<std::unique_ptr<ExprAST>>()));
				GlobalExprList.push_back(std::move(saver_call));
				GlobalExprList.push_back(std::move(std::make_unique<LiteralExprAST>(Token(true))));
			}
		}
		auto ProtoRef = Proto.get();
		FunctionProtos[Proto->getName()] = std::move(Proto);
		auto tmp_function = std::make_unique<FunctionAST>(ProtoRef, std::move(GlobalExprList), tok_return);
		return tmp_function;
	}
	return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> ParseExtern() {
	getNextToken(); // eat extern.
	return ParsePrototype();
}
