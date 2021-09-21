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
Token getNextToken(bool expectBinary) { return CurTok = lex.gettok(expectBinary); }
Token purgeLine() { return CurTok = lex.purge_line(); }

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static inline int GetTokPrecedence() {
	return (CurTok.kind < 0 && CurTok.kind > tok_last_op) ? (-CurTok.kind) << 8 : -256;
}

/// LogError* - These are little helper functions for error handling.
std::unique_ptr<ExprAST> LogErrorGen(const char *Str, va_list ap) {
	fprintf(stderr, "Error: ");
	vfprintf(stderr, Str, ap);
	fprintf(stderr, "\n");
	return nullptr;
}

std::unique_ptr<ExprAST> LogError(const char *Str, ...) {
	va_list ap;
	va_start(ap, Str);
	LogErrorGen(Str, ap);
	va_end(ap);
	return nullptr;
}

static std::unique_ptr<PrototypeAST> LogErrorP(const char *Str, ...) {
	va_list ap;
	va_start(ap, Str);
	LogErrorGen(Str, ap);
	va_end(ap);
	return nullptr;
}

static bool Expect(int tok, bool expectBinary = false) {
	bool res = CurTok.kind == tok;
	if (res) {
		getNextToken(expectBinary);
	} else {
		LogErrorP("unexpected `%s` - expected `%s`", CurTok.str().c_str(), Token::tokName(tok).c_str());
	}
	return res;
}

static void Eat(int tok, bool expectBinary = false) {
	if (CurTok.kind == tok) {
		getNextToken(expectBinary);
	}
}

std::pair<llvm::Type*, unsigned> ParseType() {
	unsigned attribs = 0;
	while (CurTok.kind != tok_identifier) {
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
		case '&':
			do {
				attribs = (attribs & 0xffff) | ((attribs & 0xffff0000) + 0x10000);
				getNextToken(true);
			} while (CurTok.kind == '&');
			if (CurTok.kind == tok_identifier)
				break;
			// else fallthough to error
		default:
			LogErrorP("Unexpected `%s` - type name expected", CurTok.str().c_str());
			return { nullptr, 0 };
		}
		getNextToken(true);
	}
	auto type = type_table.get_full(IdentifierStr.c_str());
	if (!type.first) {
		LogErrorP("Unknown type `%s` %p", IdentifierStr.c_str(), type);
		return { nullptr, 0 };
	}
	if (type.second)
		attribs |= A_signed;
	return { type.first, attribs };
}

static std::unique_ptr<ExprAST> ParseExpression(llvm::Type* desired_type = nullptr,
                                                unsigned desired_attrib = 0u );

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr(llvm::Type* desired_type = nullptr,
                                                unsigned desired_attrib = 0u) {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(true); // consume the number
	return std::move(Result);
}

static std::unique_ptr<ExprAST> ParseStringExpr() {
	auto Result = std::make_unique<LiteralExprAST>(CurTok);
	getNextToken(true); // consume the string
	return std::move(Result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr(llvm::Type* desired_type = nullptr,
                                               unsigned desired_attrib = 0u) {
	getNextToken(); // eat (.
	auto V = ParseExpression();
	if (!V)
		return nullptr;

	if (CurTok.kind != ')')
		return LogError("expected ')'");
	getNextToken(true); // eat ).
	return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr(llvm::Type* desired_type = nullptr,
                                                    unsigned desired_attrib = 0u) {
	std::string IdName = IdentifierStr;

	SourceLocation LitLoc = CurLoc;

	getNextToken(true); // eat identifier.

	if (CurTok.kind != '(') { // Simple variable ref.
		auto var_expr = std::make_unique<VariableExprAST>(LitLoc, IdName);
		if (!var_expr->type) // variable name not found
			return nullptr;
		return var_expr;
	}

	// Call.
	getNextToken(); // eat (
	std::vector<std::unique_ptr<ExprAST>> Args;
	if (CurTok.kind != ')') {
		if (auto Arg = ParseExpression()) {
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
		} else {
			return nullptr;
		}
	}
	// Eat the ')'.
	Expect(')', true);
	auto call_expr = std::make_unique<CallExprAST>(LitLoc, IdName, std::move(Args));
	if (!call_expr->type) // Used to signal failure, e.g. IdName was not found
		return nullptr;
	return call_expr;
}

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
static std::unique_ptr<ExprAST> ParseIfExpr(llvm::Type* desired_type = nullptr,
                                            unsigned desired_attrib = 0u) {
	SourceLocation IfLoc = CurLoc;

	getNextToken(); // eat the if.

	// condition.
	auto Cond = ParseExpression();
	if (!Cond)
		return nullptr;

	if (CurTok.kind != tok_then)
		return LogError("expected then");
	getNextToken(); // eat the then

	auto Then = ParseExpression();
	if (!Then)
		return nullptr;

	if (CurTok.kind != tok_else)
		return LogError("expected else");

	getNextToken();

	auto Else = ParseExpression();
	if (!Else)
		return nullptr;

	return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then),
	                                   std::move(Else));
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
	getNextToken(); // eat the for.

	if (CurTok.kind != tok_identifier)
		return LogError("expected identifier after for");

	std::string IdName = IdentifierStr;
	getNextToken(true); // eat identifier.

	if (CurTok.kind != tok_assign)
		return LogError("expected '=' after for");
	getNextToken(); // eat '='.

	auto Start = ParseExpression();
	if (!Start)
		return nullptr;
	if (CurTok.kind != ',')
		return LogError("expected ',' after for start value");
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

	if (CurTok.kind != tok_in)
		return LogError("expected 'in' after for");
	getNextToken(); // eat 'in'.

	auto Body = ParseExpression();
	if (!Body)
		return nullptr;

	return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
	                                    std::move(Step), std::move(Body));
}

/// varexpr ::= 'var' identifier ('=' expression)?
//                    (',' identifier ('=' expression)?)* 'in' expression
static std::unique_ptr<ExprAST> ParseVarExpr() {
	getNextToken(); // eat the var.

	std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

	// At least one variable name is required.
	if (CurTok.kind != tok_identifier)
		return LogError("expected identifier after var");

	while (true) {
		std::string Name = IdentifierStr;
		getNextToken(true); // eat identifier.

		// Read the optional initializer.
		std::unique_ptr<ExprAST> Init = nullptr;
		if (CurTok.kind == tok_assign) {
			getNextToken(); // eat the '='.

			Init = ParseExpression();
			if (!Init)
				return nullptr;
		}

		VarNames.push_back(std::make_pair(Name, std::move(Init)));

		// End of var list, exit loop.
		if (CurTok.kind != ',')
			break;
		getNextToken(); // eat the ','.

		if (CurTok.kind != tok_identifier)
			return LogError("expected identifier list after var");
	}

	// At this point, we have to have 'in'.
	if (CurTok.kind != tok_in)
		return LogError("expected 'in' keyword after 'var'");
	getNextToken(); // eat 'in'.

	auto Body = ParseExpression();
	if (!Body)
		return nullptr;

	return std::make_unique<VarExprAST>(std::move(VarNames), std::move(Body));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
///   ::= ifexpr
///   ::= forexpr
///   ::= varexpr
static std::unique_ptr<ExprAST> ParsePrimary(llvm::Type* desired_type = nullptr,
                                             unsigned desired_attrib = 0u) {
	switch (CurTok.kind) {
	default:
		return LogError("unknown token %d '%s' when expecting an expression", CurTok.kind, CurTok.str().c_str());
	case tok_identifier:
		return ParseIdentifierExpr();
	case tok_number:
		return ParseNumberExpr();
	case tok_str_lit:
		return ParseStringExpr();
	case '(':
		return ParseParenExpr();
	case tok_if:
		return ParseIfExpr();
	case tok_for:
		return ParseForExpr();
	case tok_var:
		return ParseVarExpr();
	}
}

/// unary
///   ::= primary
///   ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary(llvm::Type* desired_type = nullptr,
                                           unsigned desired_attrib = 0u) {
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
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
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
		int NextPrec = GetTokPrecedence();
		if (TokPrec < NextPrec) {
			RHS = ParseBinOpRHS(TokPrec + ((CurTok.kind == tok_assign) ? -1 : 1), std::move(RHS));
			if (!RHS)
				return nullptr;
		}
		// Merge LHS/RHS.
		LHS = std::make_unique<BinaryExprAST>(BinLoc, BinOp.c_str(), std::move(LHS),
		                                      std::move(RHS));
	}
}

/// expression
///   ::= unary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression(llvm::Type* desired_type, unsigned desired_attrib) {
	auto LHS = ParseUnary(desired_type, desired_attrib);
	if (!LHS)
		return nullptr;

	return ParseBinOpRHS(0, std::move(LHS));
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
	std::vector<llvm::Type*> ArgTypes;
	std::vector<unsigned> ArgAttribs;
	bool is_method;

	switch (CurTok.kind) {
	case '(': {
		is_method = true;
		unsigned attribs = 0;
		getNextToken();
		if (CurTok.kind != tok_identifier) {
			return LogErrorP("Unexpected `%s` in method prototype - receiver name expected", CurTok.str().c_str());
		}
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		auto type = ParseType();
		if (!type.first) {
			return LogErrorP("Unexpected `%s` in method prototype - type name expected", CurTok.str().c_str());
		}
		ArgTypes.push_back(type.first);
		ArgAttribs.push_back(type.second);
		FullType full_type = {
			.type = type.first,
			.type_attr = type.second
		};
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
		if (!isascii(CurTok.kind))
			return LogErrorP("Expected unary operator");
		FnName = "unary";
		FnName += (char)CurTok.kind;
		Kind = 1;
		getNextToken();
		break;
	case tok_colon: // ... not really
		getNextToken();
		if (!isascii(CurTok.kind))
			return LogErrorP("Expected binary operator");
		FnName = "binary";
		FnName += (char)CurTok.kind;
		Kind = 2;
		getNextToken();

		// Read the precedence if present.
		if (CurTok.kind == tok_number) {
			if (CurTok.Val.Int < 1 || CurTok.Val.Int > 100)
				return LogErrorP("Invalid precedence: must be 1..100");
			BinaryPrecedence = (unsigned)CurTok.Val.Int;
			getNextToken();
		}
		break;
	default:
		return LogErrorP("Expected function name in prototype");
	}
	if (!Expect('(')) return nullptr;
	if (CurTok.kind == ')')
		goto noargs;
	for (;;) {
		if (CurTok.kind != tok_identifier)
			return LogErrorP("Unexpected `%s` in function arg list - arg name expected\n", CurTok.str().c_str());
		ArgNames.push_back(IdentifierStr);
		getNextToken();
		auto type = ParseType();
		if (!type.first) {
			return LogErrorP("Unexpected `%s` in function arg list - type name expected\n", CurTok.str().c_str());
		}
		ArgTypes.push_back(type.first);
		ArgAttribs.push_back(type.second);
		FullType full_type = {
			.type = type.first,
			.type_attr = type.second
		};
		getNextToken();
		if (CurTok.kind == ')')
			break;
		Eat(',', true);
	}
noargs:
	getNextToken(true); // eat ')'.
	// parse return type(s)
	std::vector<std::pair<llvm::Type*, unsigned>> RetTypes;
	while (CurTok.kind != ';') {
		auto type = ParseType();
		if (!type.first)
			return LogErrorP("error parsing return type of function prototype");
		RetTypes.push_back(type);
		getNextToken(true);
	}
	getNextToken();
	// Verify right number of names for operator.
	if (Kind && ArgNames.size() != Kind)
		return LogErrorP("Invalid number of operands for operator");

	return std::make_unique<PrototypeAST>(FnLoc, FnName, ArgNames, Kind != 0, RetTypes, ArgTypes, ArgAttribs);
}

/// definition ::= 'fn' prototype expression
std::unique_ptr<FunctionAST> ParseDefinition() {
	getNextToken(); // eat fn.
	auto Proto = ParsePrototype();
	if (!Proto)
		return nullptr;
	auto sz = Proto->Args.size();
	// initialize local vars lookup table with function arguments
	for (int i=0; i<sz; i++) {
		FullType ft = {
			.type = Proto->ArgTypes[i],
			.type_attr = Proto->ArgAttribs[i]
		};
		bool is_new = locals_table.insert(Proto->Args[i].c_str(), ft);
		if (!is_new) {
			LogError("duplicat function arg \"%s\"\n", Proto->Args[i].c_str());
			return nullptr;
		}
	}
	auto E = ParseExpression();
	if (E)
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	return nullptr;
}

/// toplevelexpr ::= expression
std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	SourceLocation FnLoc = CurLoc;
	if (auto E = ParseExpression()) {
		// Make an anonymous proto.
		char* name = nullptr;
		auto Proto = std::make_unique<PrototypeAST>(FnLoc, "__anon_expr",
		                                            std::vector<std::string>(),
		                                            false, (std::vector<std::pair<llvm::Type*, unsigned>>){ { E->type, E->type_attr } });
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	}
	return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> ParseExtern() {
	getNextToken(); // eat extern.
	return ParsePrototype();
}
