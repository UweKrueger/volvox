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
#define A_signed_key (1U<<7) //for maps with integer key
#define A_rtlen  (1U<<8) // run time sized fixed array
#define A_varlen (1U<<9) // variable size array
#define A_map    (1U<<10) // llvm-type is key type

#if defined (_MSC_VER)
#define PACK(s) __pragma(pack(push,1)) s __pragma(pack(pop))
#else
#define PACK(s) s __attribute__((__packed__))

#define VOLVOX_HalfTyID HalfTyID
#define VOLVOX_BFloatTyID BFloatTyID
#define VOLVOX_FloatTyID FloatTyID
#define VOLVOX_DoubleTyID DoubleTyID
#define VOLVOX_X86_FP80TyID X86_FP80TyID
#define VOLVOX_FP128TyID FP128TyID
#define VOLVOX_PPC_FP128TyID PPC_FP128TyID
#define VOLVOX_VoidTyID VoidTyID
#define VOLVOX_LabelTyID LabelTyID
#define VOLVOX_MetadataTyID MetadataTyID
#define VOLVOX_X86_MMXTyID X86_MMXTyID
#define VOLVOX_X86_AMXTyID X86_AMXTyID
#define VOLVOX_TokenTyID TokenTyID
#define VOLVOX_IntegerTyID IntegerTyID
#define VOLVOX_FunctionTyID FunctionTyID
#define VOLVOX_PointerTyID PointerTyID
#define VOLVOX_StructTyID StructTyID
#define VOLVOX_ArrayTyID ArrayTyID
#define VOLVOX_FixedVectorTyID FixedVectorTyID
#define VOLVOX_ScalableVectorTyID ScalableVectorTyID
#define VOLVOX_TypeID TypeID
#define VOLVOX_gen_val_type_t gen_val_type_t
#define VOLVOX_RtStructField RtStructField
#define VOLVOX_RtType RtType

namespace volvox {
#endif

	// We do not want to include LLVM headers since this file
	// is part of the run time system - so we mirror the definitions
	// to have consistent ID numbers
	enum VOLVOX_TypeID {
		// PrimitiveTypes
		VOLVOX_HalfTyID = 0,  ///< 16-bit floating point type
		VOLVOX_BFloatTyID,    ///< 16-bit floating point type (7-bit significand)
		VOLVOX_FloatTyID,     ///< 32-bit floating point type
		VOLVOX_DoubleTyID,    ///< 64-bit floating point type
		VOLVOX_X86_FP80TyID,  ///< 80-bit floating point type (X87)
		VOLVOX_FP128TyID,     ///< 128-bit floating point type (112-bit significand)
		VOLVOX_PPC_FP128TyID, ///< 128-bit floating point type (two 64-bits, PowerPC)
		VOLVOX_VoidTyID,      ///< type with no size
		VOLVOX_LabelTyID,     ///< Labels
		VOLVOX_MetadataTyID,  ///< Metadata
		VOLVOX_X86_MMXTyID,   ///< MMX vectors (64 bits, X86 specific)
#if LLVM_VERSION_MAJOR >= 12
		VOLVOX_X86_AMXTyID,   ///< AMX vectors (8192 bits, X86 specific)
#endif
		VOLVOX_TokenTyID,     ///< Tokens

		// Derived types... see DerivedTypes.h file.
		VOLVOX_IntegerTyID,       ///< Arbitrary bit width integers
		VOLVOX_FunctionTyID,      ///< Functions
		VOLVOX_PointerTyID,       ///< Pointers
		VOLVOX_StructTyID,        ///< Structures
		VOLVOX_ArrayTyID,         ///< Arrays
		VOLVOX_FixedVectorTyID,   ///< Fixed width SIMD vector type
		VOLVOX_ScalableVectorTyID ///< Scalable SIMD vector type
	};
#if defined(_MSC_VER)
	typedef enum VOLVOX_TypeID VOLVOX_TypeID;
#endif
	PACK(struct VOLVOX_gen_val_type_t {
		VOLVOX_TypeID ID : 8; // base type
		unsigned SubclassData : 24;
	});
#if defined(_MSC_VER)
	typedef struct VOLVOX_gen_val_type_t VOLVOX_gen_val_type_t;
#endif

	/* The runtime type system has no LLVM infrastructure available
	   so it is a somewhat stripped down version of the above */

	struct VOLVOX_RtStructField;
#if defined(_MSC_VER)
	typedef struct VOLVOX_RtStructField VOLVOX_RtStructField;
	struct VOLVOX_RtType;
	typedef struct VOLVOX_RtType VOLVOX_RtType;
#endif

	PACK(struct VOLVOX_RtType {
		union {
			struct {
				VOLVOX_TypeID ID : 8; // base type
				unsigned SubclassData : 24;
			};
			unsigned key;
		};
		unsigned type_attr;
		uint64_t num_fields;
		uint64_t type_size;
		const char* name;
		union {
			const VOLVOX_RtType* elem_type;
			const VOLVOX_RtStructField* fields;
		};
	});

	struct VOLVOX_RtStructField {
		const char* FieldName;
		VOLVOX_RtType rttype;
	};
#if !defined (_MSC_VER)
}
#undef VOLVOX_HalfTyID
#define VOLVOX_HalfTyID volvox::HalfTyID
#undef VOLVOX_BFloatTyID
#define VOLVOX_BFloatTyID volvox::BFloatTyID
#undef VOLVOX_FloatTyID
#define VOLVOX_FloatTyID volvox::FloatTyID
#undef VOLVOX_DoubleTyID
#define VOLVOX_DoubleTyID volvox::DoubleTyID
#undef VOLVOX_X86_FP80TyID
#define VOLVOX_X86_FP80TyID volvox::X86_FP80TyID
#undef VOLVOX_FP128TyID
#define VOLVOX_FP128TyID volvox::FP128TyID
#undef VOLVOX_PPC_FP128TyID
#define VOLVOX_PPC_FP128TyID volvox::PPC_FP128TyID
#undef VOLVOX_VoidTyID
#define VOLVOX_VoidTyID volvox::VoidTyID
#undef VOLVOX_LabelTyID
#define VOLVOX_LabelTyID volvox::LabelTyID
#undef VOLVOX_MetadataTyID
#define VOLVOX_MetadataTyID volvox::MetadataTyID
#undef VOLVOX_X86_MMXTyID
#define VOLVOX_X86_MMXTyID volvox::X86_MMXTyID
#undef VOLVOX_X86_AMXTyID
#define VOLVOX_X86_AMXTyID volvox::X86_AMXTyID
#undef VOLVOX_TokenTyID
#define VOLVOX_TokenTyID volvox::TokenTyID
#undef VOLVOX_IntegerTyID
#define VOLVOX_IntegerTyID volvox::IntegerTyID
#undef VOLVOX_FunctionTyID
#define VOLVOX_FunctionTyID volvox::FunctionTyID
#undef VOLVOX_PointerTyID
#define VOLVOX_PointerTyID volvox::PointerTyID
#undef VOLVOX_StructTyID
#define VOLVOX_StructTyID volvox::StructTyID
#undef VOLVOX_ArrayTyID
#define VOLVOX_ArrayTyID volvox::ArrayTyID
#undef VOLVOX_FixedVectorTyID
#define VOLVOX_FixedVectorTyID volvox::FixedVectorTyID
#undef VOLVOX_ScalableVectorTyID
#define VOLVOX_ScalableVectorTyID volvox::ScalableVectorTyID
#undef VOLVOX_TypeID
#define VOLVOX_TypeID volvox::TypeID
#undef VOLVOX_gen_val_type_t
#define VOLVOX_gen_val_type_t volvox::gen_val_type_t
#undef VOLVOX_RtStructField
#define VOLVOX_RtStructField volvox::RtStructField
#undef VOLVOX_RtType
#define VOLVOX_RtType volvox::RtType
#endif
