#include "../include/volvox.hh"
#include "global.h"
#include "AST.h"

//===----------------------------------------------------------------------
// type conversion handling
//===----------------------------------------------------------------------

std::nullptr_t Error(SourceLocation Loc, const char *Str, ...) {
	fprintf(stderr, "Error (%d, %d)", Loc.Line, Loc.Col);
	va_list ap;
	va_start(ap, Str);
	vfprintf(stderr, Str, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	return nullptr;
}
	
std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                              unsigned expr_attr, unsigned desired_attr, const char* reason) {
	return Error(Loc, "Cannot automatically convert %s to %s (%s)",
	             type_table.get_name((llvm::Type*)((uintptr_t)expr_type | (expr_attr & A_signed))),
	             type_table.get_name((llvm::Type*)((uintptr_t)desired_type | (desired_attr & A_signed))), reason);
}

static std::nullptr_t ExplicitErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type,
                                  unsigned expr_attr, unsigned desired_attr, const char* reason) {
	return Error(Loc, "Cannot convert %s to %s (%s)",
	             type_table.get_name((llvm::Type*)((uintptr_t)expr_type | (expr_attr & A_signed))),
	             type_table.get_name((llvm::Type*)((uintptr_t)desired_type | (desired_attr & A_signed))), reason);
}

static llvm::Value* NoConversion(llvm::Value* v) { return v; }

// Try to convert 'expr_type' to 'desired_type'
// return an error if not possible or no explicit conversion
// is requested but precision would be lost
std::function<llvm::Value*(llvm::Value*)> getConv(
	llvm::Type* expr_type, llvm::Type* desired_type, unsigned expr_attr, unsigned desired_attr,
	SourceLocation Loc = CurLoc, bool is_explicit = false)
{
	const char* reason = "";
	if (desired_type->isIntegerTy()) {
		unsigned desired_bitwidth = desired_type->getIntegerBitWidth();
		if (expr_type->isIntegerTy()) {
			unsigned expr_bitwidth = expr_type->getIntegerBitWidth();
			if (!(desired_attr & A_signed))
				if (expr_attr & A_signed)
					// signed -> unsigned
					if (!is_explicit)
						return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "signed->unsigned");
					else
						if (desired_bitwidth == expr_bitwidth)
							return NoConversion;
						else
							// design decision: make this a signed conversion so that `u64(-1) -> 0xffffffffffffffff`
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "convstmp"); };
				else
					// unsigned -> unsigned
					if (desired_bitwidth < expr_bitwidth)
						if (is_explicit)
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate upper bits");
					else
						if (desired_bitwidth == expr_bitwidth)
							return NoConversion;
						else
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "expandutmp"); };
			else
				if (expr_attr & A_signed)
					// signed -> signed
					if (desired_bitwidth < expr_bitwidth)
						if (is_explicit)
							return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, true, "expandstmp"); };
				else
					// unsigned -> signed
					if (desired_bitwidth <= expr_bitwidth)
						if (is_explicit)
							if (desired_bitwidth == expr_bitwidth)
								return NoConversion;
							else
								return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "trunctmp"); };
						else
							return AutoErr(Loc, expr_type, desired_type, expr_attr, desired_attr, "would truncate/reinterpret upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "expandstmp"); };
		}
	}
	return NoConversion;		
}

// compute the conversion functions for binary Operators
BinOpConvSet convBinOp(llvm::Type* left_type, llvm::Type* right_type, unsigned left_attr, unsigned right_attr,
                       const char* Op, SourceLocation Loc)
{
	unsigned left_bitwidth, right_bitwidth, res_bitwidth;
	// no result type known - deduce from "biggest" operand
	switch (left_type->getTypeID()) {
	case llvm::Type::IntegerTyID:
		left_bitwidth = left_type->getIntegerBitWidth();
		switch (right_type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			right_bitwidth = right_type->getIntegerBitWidth();
			if (left_attr & A_signed)
				if (right_attr & A_signed)
					// signed # signed
					if (left_bitwidth == right_bitwidth)
						return {{ nullptr, nullptr, left_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else if (left_bitwidth > right_bitwidth)
						return {{ nullptr,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, true, "expandstmp"); },
								left_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, true, "expandstmp"); },
								nullptr, right_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
				else
					// signed # unsigned
					if (left_bitwidth > right_bitwidth)
						return {{ nullptr,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, false, "expandstmp"); },
								left_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{ nullptr, nullptr, nullptr, 0, "would truncate/reinterpret upper bits" }, { nullptr, nullptr, nullptr, 0 }};
			else
				if (right_attr & A_signed)
					// unsigned # signed
					if (left_bitwidth < right_bitwidth)
						return {{
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, false, "expandstmp"); },
								nullptr, right_type, A_signed }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{ nullptr, nullptr, nullptr, 0, "would truncate/reinterpret upper bits" }, { nullptr, nullptr, nullptr, 0 }};
				else
					// unsigned # unsigned
					if (left_bitwidth == right_bitwidth)
						return {{ nullptr, nullptr, left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
					else if (left_bitwidth > right_bitwidth)
						return {{ nullptr,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, true, "expandstmp"); },
								left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
					else
						return {{
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, true, "expandstmp"); },
								nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
		case llvm::Type::HalfTyID:
			right_bitwidth = 11;
			goto right_real;
		case llvm::Type::BFloatTyID:
			right_bitwidth = 8;
			goto right_real;
		case llvm::Type::FloatTyID:
			right_bitwidth = 24;
			goto right_real;
		case llvm::Type::DoubleTyID:
			right_bitwidth = 53;
		right_real:
			if (right_bitwidth >= (left_bitwidth - (left_attr & A_signed)))
				if (left_attr & A_signed)
					return {{ [=](llvm::Value* v) { return Builder->CreateSIToFP(v, right_type, "convsrealtmp"); },
							nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
				else
					return {{ [=](llvm::Value* v) { return Builder->CreateUIToFP(v, right_type, "convurealtmp"); },
							nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else
				return {{ nullptr, nullptr, nullptr, 0, "int->float would lose precision" }, { nullptr, nullptr, nullptr, 0 }};
			break;
		default:
			return {{ nullptr, nullptr, nullptr, 0, "no known automatic conversion" }, { nullptr, nullptr, nullptr, 0 }};
		}
	case llvm::Type::HalfTyID:
		left_bitwidth = 11;
		goto left_real;
	case llvm::Type::BFloatTyID:
		left_bitwidth = 8;
		goto left_real;
	case llvm::Type::FloatTyID:
		left_bitwidth = 24;
		goto left_real;
	case llvm::Type::DoubleTyID:
		left_bitwidth = 53;
	left_real:
		switch (right_type->getTypeID()) {
		case llvm::Type::IntegerTyID:
			right_bitwidth = right_type->getIntegerBitWidth();
			if (left_bitwidth >= (right_bitwidth - (right_attr & A_signed)))
				if (right_attr & A_signed)
					return {{ nullptr, [=](llvm::Value* v) { return Builder->CreateSIToFP(v, left_type, "convsrealtmp"); },
							left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
				else
					return {{ nullptr, [=](llvm::Value* v) { return Builder->CreateUIToFP(v, left_type, "convurealtmp"); },
							left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else
				return {{ nullptr, nullptr, nullptr, 0, "int->float would lose precision" }, { nullptr, nullptr, nullptr, 0 }};
		case llvm::Type::HalfTyID:
			right_bitwidth = 11;
			goto right_real2;
		case llvm::Type::BFloatTyID:
			right_bitwidth = 8;
			goto right_real2;
		case llvm::Type::FloatTyID:
			right_bitwidth = 24;
			goto right_real2;
		case llvm::Type::DoubleTyID:
			right_bitwidth = 53;
		right_real2:
			if (right_bitwidth == left_bitwidth)
				return {{ nullptr, nullptr, left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else if (left_bitwidth > right_bitwidth)
				return {{ nullptr,
						[=](llvm::Value* v) { return Builder->CreateFPCast(v, left_type, "fpcasttmp"); },
						left_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
			else
				return {{ [=](llvm::Value* v) { return Builder->CreateFPCast(v, right_type, "fpcasttmp"); }, nullptr, right_type, 0 }, { nullptr, nullptr, nullptr, 0 }};
		default:
			return {{ nullptr, nullptr, nullptr, 0, "no known automatic conversion" }, { nullptr, nullptr, nullptr, 0 }};
		}
	default:
		return {{ nullptr, nullptr, nullptr, 0, "no known automatic conversion" }, { nullptr, nullptr, nullptr, 0 }};
	}
}

