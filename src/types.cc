#include "../include/volvox.hh"
#include "global.h"

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
	
static std::nullptr_t AutoErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type, const char* reason) {
	return Error(Loc, "Cannot automatically convert %s to %s (%s)",
	             type_table.get_name(expr_type), type_table.get_name(desired_type), reason);
}

static std::nullptr_t ExplicitErr(SourceLocation Loc, llvm::Type* expr_type, llvm::Type* desired_type, const char* reason) {
	return Error(Loc, "Cannot convert %s to %s (%s)",
	             type_table.get_name(expr_type), type_table.get_name(desired_type), reason);
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
						return AutoErr(Loc, expr_type, desired_type, "signed->unsigned");
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
							return AutoErr(Loc, expr_type, desired_type, "would truncate upper bits");
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
							return AutoErr(Loc, expr_type, desired_type, "would truncate upper bits");
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
							return AutoErr(Loc, expr_type, desired_type, "would truncate/reinterpret upper bits");
					else
						return [=](llvm::Value* v) { return Builder->CreateIntCast(v, desired_type, false, "expandstmp"); };
		}
	}
	return NoConversion;		
}

// compute the conversion functions for binary Operators
std::tuple<std::function<llvm::Value*(llvm::Value*)>,
           std::function<llvm::Value*(llvm::Value*)>,
           std::function<llvm::Value*(llvm::Value*)>>
calc_conv(llvm::Type* left_type, llvm::Type* right_type, llvm::Type* desired_type,
          unsigned left_attr, unsigned right_attr, unsigned desired_attr,
          const char* Op, SourceLocation Loc = CurLoc)
{
	if (desired_type) {
	} else {
		// no result type known - deduce from "biggest" operand
		if (left_type->isIntegerTy()) {
			unsigned left_bitwidth = left_type->getIntegerBitWidth();
			if (right_type->isIntegerTy()) {
				unsigned right_bitwidth = right_type->getIntegerBitWidth();
				if (left_attr & A_signed)
					if (right_attr & A_signed)
						// signed # signed
						if (left_bitwidth == right_bitwidth)
							return { NoConversion, NoConversion, NoConversion };
						else if (left_bitwidth > right_bitwidth)
							return { NoConversion,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, true, "expandstmp"); },
								NoConversion };
						else
							return {
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, true, "expandstmp"); },
								NoConversion, NoConversion };
					else
						// signed # unsigned
						if (left_bitwidth > right_bitwidth)
							return { NoConversion,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, false, "expandstmp"); },
								NoConversion };
						else
							return { AutoErr(Loc, left_type, right_type, "would truncate/reinterpret upper bits"), nullptr, nullptr };
				else
					if (right_attr & A_signed)
						// unsigned # signed
						if (left_bitwidth < right_bitwidth)
							return {
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, false, "expandstmp"); },
								NoConversion,
								NoConversion };
						else
							return { AutoErr(Loc, right_type, left_type, "would truncate/reinterpret upper bits"), nullptr, nullptr };
					else
						// unsigned # unsigned
						if (left_bitwidth == right_bitwidth)
							return { NoConversion, NoConversion, NoConversion };
						else if (left_bitwidth > right_bitwidth)
							return { NoConversion,
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, left_type, true, "expandstmp"); },
								NoConversion };
						else
							return {
								[=](llvm::Value* v) { return Builder->CreateIntCast(v, right_type, true, "expandstmp"); },
								NoConversion, NoConversion };
			}
		}
	}
	return { nullptr, nullptr, nullptr };
}

