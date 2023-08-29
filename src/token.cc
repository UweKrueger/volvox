/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

#undef TOKEN
#undef TOKEN_INV
#undef TOKENS
#undef TOKBEGIN
#undef TOKEND
#define TOKEN(a) #a,
#define TOKBEGIN tokstr_beg
#define TOKEND tokstr_end
#include "token.def"

const char* tokens[] = {
	TOKENS
};

MapNode* keyword_toks = nullptr;

void insert_token_into_map(int token) {
	MapValue val = { .i32 = token };
	const char* tokenstr = tokens[token - 1 - tok_1st_keyword];
	MapNode* replace = nullptr;
	MapNode* res = map_string_insert(&keyword_toks, tokenstr, val, 0, replace);
	if (replace) {
		errs() << "internal error: map entry for keyword \"" << tokenstr << " already exists\n";
		abort();
	}
}

void init_token_map() {
	// first fix two exceptions in token array
	// tokens[tok_end - 1 - tok_1st_keyword] = ".";
	tokens[tok_invisible - 1 - tok_1st_keyword] = "<invisible operator>";
	// now fill token map with those tokens that correspond to ASCII-keywords (not operators)
	for (int token = tok_1st_keyword + 1; token < tok_last_keyword; token++) {
		insert_token_into_map(token);
	}
	insert_token_into_map(tok_task);
}

std::string Token::str() const {
	if (kind == tok_number) {
		switch (int_type.ID) {
		case llvm::Type::IntegerTyID:
			if (int_type.BitWidth == 1)
				if (Val.Uint & 1UL)
					return "true";
				else
					return "false";
			else if (int_type.is_signed)
				return std::to_string(Val.Int);
			else
				return std::to_string(Val.Uint);
		case llvm::Type::HalfTyID:
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID:
			return std::to_string(Val.Float);
		default:
			errs() << "internal compiler error: cannot print numeric literal of TypeID " << int_type.ID << "\n";
			return "";
		}
	}
	if (kind == tok_str_lit)
		return std::string(Val.CStr, Val.Len);
	if (kind < tok_last_op)
		return tokens[kind - 1 - tok_1st_keyword];
	if (kind > 0)
		IdentifierStr = (char)kind;
	return IdentifierStr;
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, TokenKind kind) {
	if (kind > 0)
		return out << '\'' << (char)kind << '\'';
	if (kind < tok_last_keyword)
		return out << '"' << tokens[kind - 1 - tok_1st_keyword] << '"';
	return out << '<' << tokens[kind - 1 - tok_1st_keyword] << '>';
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& out, Token& tok) {
	if (tok.kind > 0 || tok.kind < tok_last_keyword)
		return out << TokenKind(tok.kind);
	return out << tok.str();
}

Token::Token(char** s_ptr) : kind(tok_number) {
	is_unknown_type = true;
	while (isspace(**s_ptr))
		++(*s_ptr);
	bool sign = **s_ptr == '-';
	char* endptr;
	errno = 0;
	if (!sign && **s_ptr != '.' || sign && *(*s_ptr + 1) != '.') {
		if (sign) {
			Val.Int = strtoll(*s_ptr, &endptr, 0);
		} else {
			Val.Uint = strtoull(*s_ptr, &endptr, 0);
		}
		int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 32, .is_signed = true };
		if (errno != 0) {
			Val.Int = errno;
			// purge rest of line
			while (**s_ptr)
				++(*s_ptr);
			errs() << CurLoc << ": cannot parse numeric token: " << strerror(Val.Int) << '\n';
			return;
		}
	} else {
		endptr = *s_ptr;
	}
	// try to parse same number as float
	char* endptr_f;
	double f = strtod(*s_ptr, &endptr_f);
	if (errno == 0 && endptr_f > endptr) {
		if (*endptr == '.' && endptr_f - endptr == 1) {
			if (isalpha(*endptr_f)) {
				int i = 1;
				while (isalnum(*(endptr_f + i)))
					i++;
				if (*(endptr_f + i) == '(') {
					// method call on integer literal - discard parsed float
					*s_ptr = endptr;
					return;
				}
			}
		}
		gen_type = { .ID = VOLVOX_DoubleTyID };
		Val.Float = f;
		*s_ptr = endptr_f;
	} else {
		*s_ptr = endptr;
	}
	// handle explicit typed numeric tokens
	char t = tolower(**s_ptr);
	unsigned long bits;
	if (t == 'f' || t == 'i' || t == 'u') {
		is_unknown_type = false;
		++*s_ptr;
		if (isdigit(**s_ptr)) {
			bits = strtoul(*s_ptr, s_ptr, 10);
		} else {
			bits = 32; // default for int, uint, float
		}
		switch (t) {
		case 'f':
			switch (bits) {
			case 16: // not really supported, yet
				gen_type = { .ID = VOLVOX_BFloatTyID };
				break;
			case 32:
				gen_type = { .ID = VOLVOX_FloatTyID };
				break;
			case 64:
				gen_type = { .ID = VOLVOX_DoubleTyID };
				break;
			default:
				errs() << "unsupported bit size " << bits << " for float literal\n";
			}
			break;
		case 'i':
		case 'u':
			if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
				errs() << "unsupported bit size " << bits << " for integer literal\n";
			int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = (unsigned)bits, .is_signed = (t == 'i') };
			break;
		}
	}
	if (gen_type.ID == VOLVOX_IntegerTyID && int_type.is_signed && !is_unknown_type && !sign && Val.Int < 0)
		// TODO: further checks for bit sizes
		errs() << Val.Uint << " exceeds maximum maximum possible signed value\n";
}
				
Token::Token(const std::string& str) : kind(tok_str_lit) {
	auto llvmtype = llvm::Type::getInt8PtrTy(Context);
	gen_type = { .ID = (VOLVOX_TypeID)llvmtype->getTypeID(), .SubclassData = ((genType*)llvmtype)->SubClassData() };
	Val.CStr = (char*)malloc(str.size() + 1);
	memcpy(Val.CStr, str.data(), str.size());
	Val.CStr[str.size()] = '\0';
	Val.Len = str.size();
}

Token::Token(void* ptr) : kind(tok_ptr_lit) {
	Val.Ptr = ptr;
	auto llvmtype = llvm::Type::getInt8PtrTy(Context);
	gen_type = { .ID = (VOLVOX_TypeID)llvmtype->getTypeID(),
	             .SubclassData = ((genType*)llvmtype)->SubClassData()
	};
}

Token::Token(int _kind) {
	// handle built-in values
	if (_kind >= tok_false && _kind <= tok_nil) {
		switch (_kind) {
		case tok_false:
		case tok_true:
			kind = tok_number;
			Val.Uint = (_kind == tok_true) ? 1UL : 0UL;
			int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 1, .is_signed = false };
			break;
		default:
			kind = tok_ptr_lit;
			Val.Ptr = (void*)0;
			auto llvmtype = llvm::Type::getInt8PtrTy(Context);
			gen_type = { .ID = (VOLVOX_TypeID)llvmtype->getTypeID(),
			             .SubclassData = ((genType*)llvmtype)->SubClassData()
			};
		}
	} else {
		kind = TokenKind(_kind);
	}
}
