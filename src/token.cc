#include "../include/volvox.hh"
#include "global.h"

std::string Token::tokName() const {
	switch (this->type) {
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
	case tok_binary:
		return "binary";
	case tok_unary:
		return "unary";
	case tok_var:
		return "var";
	case tok_u8:
		return "u8";
	case tok_u16:
		return "u16";
	case tok_u32:
		return "u32";
	case tok_u64:
		return "u64";
	case tok_i8:
		return "i8";
	case tok_i16:
		return "i16";
	case tok_i32:
		return "i32";
	case tok_i64:
		return "i64";
	case tok_bool:
		return "bool";
	case tok_int:
		return "int";
	case tok_uint:
		return "uint";
	case tok_usize:
		return "usize";
	case tok_ssize:
		return "ssize";
	case tok_voidptr:
		return "voidptr";
	case tok_string:
		return "string";
	case tok_self:
		return "self";
	case tok_lparen:
		return "(";
	case tok_rparen:
		return ")";
	case tok_lbrack:
		return "[";
	case tok_rbrack:
		return "]";
	case tok_lbrace:
		return "{";
	case tok_rbrace:
		return "}";
	case tok_colon:
		return ":";
	case tok_semicolon:
		return ";";
	case tok_comma:
		return ",";
	case tok_dot:
		return ".";
	case tok_space:
		return " ";
	case tok_newline:
		return "\n";
	}
	return std::string(1, (char)this->type);
}

Token::Token(char** s_ptr) : type(tok_number) {
	while (isspace(**s_ptr))
		++(*s_ptr);
	bool sign = **s_ptr == '-';
	char* endptr;
	errno = 0;
	if (sign) {
		int_val = strtoll(*s_ptr, &endptr, 0);
	} else {
		uint_val = strtoull(*s_ptr, &endptr, 0);
	}
	if (errno != 0) {
		int_val = errno;
		val_type = val_invalid;
		*s_ptr = endptr;
		return;
	}
	// try to parse same number as float
	char* endptr_f;
	double f = strtod(*s_ptr, &endptr_f);
	if (errno == 0 && endptr_f > endptr) {
		if (*endptr == '.' && endptr_f - endptr == 1) {
			if (isalpha(*endptr_f) && (*(endptr_f + 1) == '(' || isalnum(*(endptr_f + 1)))) {
				// method call on integer literal
				*s_ptr = endptr;
				val_type = (sign || this->uint_val <= INT_MAX) ? val_int : val_invalid;
				return;
			}
		}
		if (*endptr_f == 'f' || *endptr_f == 'F') {
			endptr++;
			val_type = val_f32;
		} else {
			val_type = val_f64;
		}
		float_val = f;
		*s_ptr = endptr_f;
	} else {
		*s_ptr = endptr;
	}
}
				
Token::Token(const std::string& str) : type(tok_str_lit) {						
	val_type = val_string;
	str_val = strdup(str.c_str());
}

