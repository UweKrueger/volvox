#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

std::string Token::tokName(int tok_kind) {
	switch (tok_kind) {
	case tok_eof:
		return "eof";
	case tok_fn:
		return "fn";
	case tok_extern:
		return "extern";
	case tok_identifier:
		return "identifier";
	case tok_number:
		return "number";
	case tok_if:
		return "if";
	case tok_then:
		return "then";
	case tok_else:
		return "else";
	case tok_for:
		return "for";
	case tok_in:
		return "in";
	case tok_unary:
		return "unary";
	case tok_var:
		return "var";
	case tok_atomic:
		return "atomic";
	case tok_shared:
		return "shared";
	case tok_iso:
		return "iso";
	case tok_const:
		return "const";
	case tok_self:
		return "self";
	}
	return std::string(1, (char)tok_kind);
}

Token::Token(char** s_ptr) : kind(tok_number) {
	while (isspace(**s_ptr))
		++(*s_ptr);
	bool sign = **s_ptr == '-';
	char* endptr;
	errno = 0;
	if (sign) {
		Val.Int = strtoll(*s_ptr, &endptr, 0);
	} else {
		Val.Uint = strtoull(*s_ptr, &endptr, 0);
	}
	int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 32, .is_signed = true };
	if (errno != 0) {
		Val.Int = errno;
		*s_ptr = endptr;
		LogError("cannot parse numeric token: %s", strerror(Val.Int));
		return;
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
		gen_type = { .ID = llvm::Type::DoubleTyID };
		Val.Float = f;
		*s_ptr = endptr_f;
	} else {
		*s_ptr = endptr;
	}
	// handle explicit typed numeric tokens
	char t = tolower(**s_ptr);
	unsigned long bits;
	if (t == 'f' || t == 'i' || t == 'u') {
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
				gen_type = { .ID = llvm::Type::HalfTyID };
				break;
			case 32:
				gen_type = { .ID = llvm::Type::FloatTyID };
				break;
			case 64:
				gen_type = { .ID = llvm::Type::DoubleTyID };
				break;
			default:
				LogError("unsupported bit size %lu for float literal", bits);
			}
			break;
		case 'i':
		case 'u':
			if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
				LogError("unsupported bit size %lu for integer literal", bits);
			int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = (unsigned)bits, .is_signed = (t == 'i') };
			break;
		}
	}
	if (gen_type.ID == llvm::Type::IntegerTyID && int_type.is_signed && !sign && Val.Int < 0)
		// TODO: further checks for bit sizes
		LogError("%u exceeds maximum maximum possible signed value");
}
				
Token::Token(const std::string& str) : kind(tok_str_lit) {
	auto llvmtype = llvm::Type::getInt8PtrTy(*Context.getContext());
	gen_type = { .ID = llvmtype->getTypeID(), .SubclassData = ((genType*)llvmtype)->SubClassData() };
	Val.Str = strdup(str.c_str());
}

