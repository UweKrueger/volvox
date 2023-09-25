/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#pragma once

#include <inttypes.h>

// Type Attributes
#define A_signed (1U<<0) // also used for imaginary, string
#define A_const  (1U<<1)
#define A_shared (1U<<2)
#define A_unique (1U<<3)
#define A_atomic (1U<<4)
#define A_ref    (1U<<5) // function arg is reference
#define A_ptrref (1U<<6) // reference that is a represented as pointer internally
#define A_map    (1U<<7) // llvm-type is key type
#define A_signed_key (1U<<8) //for maps with integer key
#define A_fn     (1U<<9) // function (or function pointer)
#define A_packed (1U<<10) // only set for RtType, otherwise part of llvm::Type
#define A_union  (1U<<11)
#define A_string (1U<<12)
#define A_cstring (1U<<13)
#define A_interface (1U<<14)
#define A_optional (1U<<15)
// symbol visibility attributes
#define A_pub    (1U<<16)
#define A_mainvar (1U<<17) // global for LLVM
#define A_global (1U<<18) // in jit jit all main symbols are global for LLVM, so an additional flag for logical visibility is needed
#define A_inline (1U<<19)
#define A_c_api  (1U<<20)
#define A_destructor (1U<<21) // fn is destructor or type has destructor
#define A_constructor (1U<<22) // fn is default c. or type has default c.
#define A_conversion (1U<<23) // fn is "type conversion operator" (in C++ speach)
#define A_method (1U<<24)
#define A_extern (1U<<25)
#define A_merged (1U<<26) // helper flag for processing merge of then/else branches
#define A_rvalue (1U<<27) // pseudo FullVar that has not storage location
#define A_closure (1U<<28)
#define A_untyped (1U<<29)

#define SHARE_KIND_MASK (A_const|A_shared|A_unique|A_atomic)
#define VISIBILITY_MASK (A_pub|A_global|A_c_api|A_inline)
#define A_use_target A_map
#define A_globally_visible (A_const|A_shared|A_atomic|A_global|A_extern)

#if defined (_MSC_VER)
#define PACK(s) __pragma(pack(push,1)) s __pragma(pack(pop))
#else
#define PACK(s) s __attribute__((__packed__))
#endif

#ifdef __cplusplus
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
		VOLVOX_X86_AMXTyID,   ///< AMX vectors (8192 bits, X86 specific)
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
#ifndef __cplusplus
	typedef enum VOLVOX_TypeID VOLVOX_TypeID;
#endif
	PACK(struct VOLVOX_gen_val_type_t {
		VOLVOX_TypeID ID : 8; // base type
		unsigned SubclassData : 24;
	});
#ifndef __cplusplus
	typedef struct VOLVOX_gen_val_type_t VOLVOX_gen_val_type_t;
#endif

	/* The runtime type system has no LLVM infrastructure available
	   so it is a somewhat stripped down version of the above */

	struct VOLVOX_RtStructField;
#ifndef __cplusplus
	typedef struct VOLVOX_RtStructField VOLVOX_RtStructField;
	struct VOLVOX_RtType;
	typedef struct VOLVOX_RtType VOLVOX_RtType;
#endif

	/* There are actually 3 type systems:
	   1. The LLVM type system - this is used by the code geneneration engine but
	      it lacks some attributes like signedness, struct field names
	   2. volvoxc::FullType - used by the Volvox compiler
	      embedds llvm::Type and adds those attributes
	   3. volvox::RtType - the run time type
	      the run time system should not depend on libLLVM so this is a stripped
	      down stand alone version of the above systems
	*/

	struct VOLVOX_RtType;

	struct VOLVOX_RtStructField {
		const char* FieldName;
		VOLVOX_RtType* rttype;
	};

	PACK(struct VOLVOX_RtType {
		union {
			struct {
				VOLVOX_TypeID ID : 8; // base type
				unsigned SubclassData : 24;
			};
			unsigned key;
		};
		unsigned type_attr;
		union {
			size_t type_size;
			size_t* dims; // for (multi dimensional) arrays
		};
		const char* name;
		union {
			const VOLVOX_RtType* elem_type;
			const VOLVOX_RtStructField fields;
		};
	});

#ifdef __cplusplus
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

// architecture index numbers for runtime system - they differ
// from those in LLVM - to be independent from LLVM versions
// also we do not support every architecture
// however the must match the numbers defined in builtin.vx

#ifdef __cplusplus

enum OS_Type_t : uint8_t {
	OS_UnknownOS = 0,
	OS_DragonFly,
	OS_FreeBSD,
	OS_Linux,
	OS_MacOSX,
	OS_NetBSD,
	OS_OpenBSD,
	OS_Haiku,
	OS_Windows
};
	
enum CPU_Type_t : uint8_t {
	CPU_Unknown = 0,
	CPU_arm,
	CPU_aarch64,
	CPU_avr,
	CPU_x86,
	CPU_x86_64
};

#endif
