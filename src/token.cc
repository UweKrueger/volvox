/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "../include/volvox.hh"

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
	MapNode* res = map_string_insert(&keyword_toks, tokenstr, val, 0, &replace);
	if (replace) {
		errs() << "internal error: map entry for keyword \"" << tokenstr << " already exists\n";
		abort();
	}
}

void init_token_map() {
	// first fix some exceptions in token array
	// tokens[tok_end - 1 - tok_1st_keyword] = ".";
	tokens[tok_invisible - 1 - tok_1st_keyword] = "<invisible operator>";
	tokens[tok_reverse_in - 1 - tok_1st_keyword] = "~in";
	tokens[tok_not_in - 1 - tok_1st_keyword] = "!in";
	// now fill token map with those tokens that correspond to ASCII-keywords (not operators)
	for (int token = tok_1st_keyword + 1; token < tok_last_keyword; token++) {
		insert_token_into_map(token);
	}
	insert_token_into_map(tok_thread);
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
	Tristate sign;
	if (**s_ptr == '-')
		sign = true;
	else if (**s_ptr == '+')
		sign = false;
	char* endptr;
	errno = 0;
	Tristate int_conversion_out_of_range; 
	Tristate float_conversion_out_of_range;
	char* endptr_f;
	double f = 0;
	Tristate is_flt;
	unsigned bitw = 0;
	Tristate is_signed;
	bool is_hex;
	if (sign.undecided())
		is_hex = **s_ptr == '0' && tolower(*(*s_ptr+1)) == 'x';
	else
		is_hex = *(*s_ptr+1) == '0' && tolower(*(*s_ptr+2)) == 'x';
	if (sign.undecided() && **s_ptr != '.' || !sign.undecided() && *(*s_ptr + 1) != '.') {
		errno = 0;
		if (sign) { // negative - so parse as signed
			Val.Int = strtoll(*s_ptr, &endptr, 0);
		} else {
			Val.Uint = strtoull(*s_ptr, &endptr, 0);
		}
		if (errno == ERANGE)
			int_conversion_out_of_range = true;
		else if (!errno)
			int_conversion_out_of_range = false;
		else {
			goto unknown_err;
		}
		int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = 32, .is_signed = true };
	} else {
		endptr = *s_ptr;
	}
	// try to parse same number as float
	// if (!sign.undecided())
	// 	is_signed = true;
	char* eptr;
	if (*endptr == '.' && *(endptr+1) == '.' && *(endptr+2) != '.') {
		// don't parse as float if range operator is seen, e.g. '2..7' (but do for '2...7' which is '2. .. 7')
		*s_ptr = endptr;
	} else {
		errno = 0;
		f = strtod(*s_ptr, &endptr_f);
		if (errno == 0 && (endptr_f > endptr || int_conversion_out_of_range)) {
			if (*endptr == '.' && endptr_f - endptr == 1 && isalpha(*endptr_f)) {
				if (int_conversion_out_of_range)
					goto overflow;
				is_flt = false; // method call on int literal
			} else {
				is_flt = true;
			}
		} else if (errno == ERANGE)
			goto overflow;
		else if (errno) {
			goto unknown_err;
		}
		if (is_flt) {
			*s_ptr = endptr_f;
		} else {
			*s_ptr = endptr;
		}
	}
	// handle explicit typed numeric tokens
	eptr = *s_ptr;
	if (is_flt)
		is_signed = false;
	// we try to interpret directly following letters as explicit type specifiers
	// if this is not possible we leave the line ptr to allow the letters to be
	// interpreted as identifier
	do {
		char t = tolower(*eptr);
		switch (t) {
		case 'f':
			if (is_flt.is_false())
				goto flt_inconsistent;
			is_flt = true;
			if (bitw)
				goto bitwidth_inconsistent;
			bitw = 32;
			break;
		case 'i':
			if (is_flt.is_false())
				goto flt_inconsistent;
			is_flt = true;
			is_signed = true; // "signed" indicates imaginary for floats
			break;
		case 'u':
			if (is_signed || is_flt)
				goto signed_inconsistent;
			is_signed = false;
			if (is_flt)
				goto flt_inconsistent;
			is_flt = false;
			break;
		case 'l':
			if (is_flt)
				goto flt_inconsistent;
			if (bitw)
				goto bitwidth_inconsistent;
			if (t == tolower(*(eptr+1))) {
				eptr++;
				bitw = 128;
				errs() << CurLoc << ": 128 bit integers not supported, yet\n";
				kind = TokenKind(tok_error);
				return;
			} else {
				bitw = 64;
			}
			is_flt = false;
			break;
		case 'h':
			if (is_flt)
				goto flt_inconsistent;
			if (bitw)
				goto bitwidth_inconsistent;
			if (t == tolower(*(eptr+1))) {
				eptr++;
				bitw = 8;
			} else {
				bitw = 16;
			}
			is_flt = false;
			break;
		case 'z':
			if (is_flt)
				goto flt_inconsistent;
			if (bitw)
				goto bitwidth_inconsistent;
			bitw = target_bits;
			is_flt = false;
			break;
		case 'd':
			if (bitw)
				goto bitwidth_inconsistent;
			if (is_flt)
				bitw = 64;
			else {
				is_flt = false;
				bitw = 32;
			}
			break;
		default:
			goto end_loop;
		}
		eptr++;
	} while (true);
end_loop:
	if (is_flt) {
		Val.Float = f;
		if (bitw == 32) {
			int_type = { .ID = llvm::Type::FloatTyID, .is_signed = is_signed }; // signed means imaginary not support
		} else {
			int_type = { .ID = llvm::Type::DoubleTyID, .is_signed = is_signed };
		}
	} else {
		int_type = { .ID = llvm::Type::IntegerTyID, .BitWidth = bitw ? bitw : 32, .is_signed = !is_signed.is_false() };
	}
	is_unknown_type = !bitw && (is_signed.undecided() || is_flt);
	if (!is_flt && !is_signed.is_false() && !sign && Val.Int < 0)
		// no '-' sign, thus parsed as unsigned - if we have a negative value it's due to overflow
		// we forbid this for decimal but allow it for hexadecimal numbers
		if (!is_hex)
			goto overflow;
	*s_ptr = eptr;
	if (!is_unknown_type) {
		// checks for number fitting in data type
		if (is_flt) {
			if (bitw == 32) {
				f = (f<0) ? -f : f;
				if (f>FLT_MAX)
					goto overflow;
			}
		} else {
			if (is_signed.is_false()) {
				switch (bitw) {
				case 8:
					if (Val.Uint > UCHAR_MAX)
						goto overflow;
					break;
				case 16:
					if (Val.Uint > USHRT_MAX)
						goto overflow;
					break;
				case 32:
				default:
					if (Val.Uint > UINT_MAX)
						goto overflow;
					break;
				case 64:
					if (Val.Uint > ULLONG_MAX) // should be impossible, but keep for completeness
						goto overflow;
					break;
				}
			} else {
				switch (bitw) {
				case 8:
					if (Val.Int < CHAR_MIN || Val.Int > CHAR_MAX)
						goto overflow;
					break;
				case 16:
					if (Val.Int < SHRT_MIN || Val.Int > SHRT_MAX)
						goto overflow;
					break;
				case 32:
				default:
					if (Val.Int < INT_MIN || Val.Int > INT_MAX)
						goto overflow;
					break;
				case 64:
					if (Val.Int < LLONG_MIN || Val.Int > LLONG_MAX)
						goto overflow;
					break;
				}
			}
		}
	}
	return;
flt_inconsistent:
	errs() << CurLoc << ": data type suffix of numeric literal inconclusive (float vs. integer)\n";
	goto err_ret;
signed_inconsistent:
	errs() << CurLoc << ": data type suffix of numeric literal inconclusive (signedness)\n";
	goto err_ret;
bitwidth_inconsistent:
	errs() << CurLoc << ": data type suffix of numeric literal inconclusive (bit width)\n";
	goto err_ret;
overflow:
	errs() << CurLoc << ": numeric literal value is not representable in supposed data type\n";
err_ret:
	kind = TokenKind(tok_error);
	*s_ptr = eptr;
	return;
unknown_err:
	int save_err = errno;
	// since we definitely have a digit and have set base to 0 we should never get here
	errs() << CurLoc << ": unexpected error while parsing number literal: " << strerror(save_err) << "\n";
	abort();
}
				
Token::Token(const std::string& str, char Closing)
	: kind(Closing == '\'' ? tok_number
	       : Closing == '"' ? tok_str_lit : tok_part_str_lit) {
	if (kind == tok_number) {
		const char* seq_start = str.c_str();
		const char* seq = seq_start;
		uint32_t codepoint = utf8_decode(&seq);
		if (*seq || codepoint == (uint32_t)(-1)) {
			kind = TokenKind(tok_error);
			return;
		}
		bool is_ASCII = (int8_t)*seq_start >= 0;
		Val.Uint = codepoint;
		unsigned bits = is_ASCII ? 8 : 32;
		int_type = { .ID = llvm::Type::IntegerTyID,
		             .BitWidth = bits,
		             .is_signed = false };
		return;
	}
	if (!llvm_string_type) {
		string_type = lex.get_full_type("string");
		if (!string_type) {
			errs() << CurLoc << ": internal error - string literal used before string type is declared\n";
			exit(1);
		}
		llvm_string_type = string_type->type;
	}
	auto llvmtype = llvm_string_type;
	gen_type = { .ID = (VOLVOX_TypeID)llvmtype->getTypeID(), .SubclassData = ((genType*)llvmtype)->SubClassData() };
	Val.CStr = (char*)malloc(str.size() + 1);
	memcpy(Val.CStr, str.data(), str.size());
	Val.CStr[str.size()] = '\0';
	Val.Len = str.size();
}

Token::Token(void* ptr) : kind(tok_ptr_lit) {
	Val.Ptr = ptr;
	auto llvmtype = llvm_ptr_type;
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
			auto llvmtype = llvm_ptr_type;
			gen_type = { .ID = (VOLVOX_TypeID)llvmtype->getTypeID(),
			             .SubclassData = ((genType*)llvmtype)->SubClassData()
			};
		}
	} else {
		kind = TokenKind(_kind);
	}
}
