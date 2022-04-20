#pragma once

#include "map.h"
#include <inttypes.h>

// Type Attributes
#define A_signed (1U<<0) // also used for imaginary, string
#define A_const  (1U<<1)
#define A_shared (1U<<2)
#define A_iso    (1U<<3)
#define A_atomic (1U<<4)
#define A_packed (1U<<5)
#define A_dirty  (1U<<6)

class PrototypeAST;

namespace volvox {

#if defined (_MSC_VER)
#define PACK(s) __pragma(pack(push,1)) s __pragma(pack(pop))
#else
#define PACK(s) s __attribute__((__packed__))
#endif

	// We do not want to include LLVM headers since this file
	// is part of the run time system - so we mirror the definitions
	// to have consistent ID numbers
	enum TypeID {
		// PrimitiveTypes
		HalfTyID = 0,  ///< 16-bit floating point type
		BFloatTyID,    ///< 16-bit floating point type (7-bit significand)
		FloatTyID,     ///< 32-bit floating point type
		DoubleTyID,    ///< 64-bit floating point type
		X86_FP80TyID,  ///< 80-bit floating point type (X87)
		FP128TyID,     ///< 128-bit floating point type (112-bit significand)
		PPC_FP128TyID, ///< 128-bit floating point type (two 64-bits, PowerPC)
		VoidTyID,      ///< type with no size
		LabelTyID,     ///< Labels
		MetadataTyID,  ///< Metadata
		X86_MMXTyID,   ///< MMX vectors (64 bits, X86 specific)
#if LLVM_VERSION_MAJOR >= 12
		X86_AMXTyID,   ///< AMX vectors (8192 bits, X86 specific)
#endif
		TokenTyID,     ///< Tokens

		// Derived types... see DerivedTypes.h file.
		IntegerTyID,       ///< Arbitrary bit width integers
		FunctionTyID,      ///< Functions
		PointerTyID,       ///< Pointers
		StructTyID,        ///< Structures
		ArrayTyID,         ///< Arrays
		FixedVectorTyID,   ///< Fixed width SIMD vector type
		ScalableVectorTyID ///< Scalable SIMD vector type
	};

	PACK(struct gen_val_type_t {
		TypeID ID : 8; // base type
		unsigned SubclassData : 24;
	});

	/* The runtime type system has no LLVM infrastructure available
	   so it is a somewhat stripped down version of the above */

	struct RtStructField;

	PACK(struct RtType {
		union {
			struct {
				TypeID ID : 8; // base type
				unsigned SubclassData : 24;
			};
			unsigned key;
		};
		unsigned type_attr;
		uint64_t num_fields;
		uint64_t type_size;
		const char* name;
		union {
			const RtType* elem_type;
			const RtStructField* fields;
		};
	});

	struct RtStructField {
		const char* FieldName;
		RtType rttype;
	};

}
