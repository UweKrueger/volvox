#pragma once

#include "map.h"
#include <inttypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfoMetadata.h>

// Type Attributes
#define A_signed (1U<<0) // also used for imaginary, string
#define A_const  (1U<<1)
#define A_shared (1U<<2)
#define A_iso    (1U<<3)
#define A_atomic (1U<<4)
#define A_packed (1U<<5)

namespace volvox {

#if defined (_MSC_VER)
#define PACK(s) __pragma(pack(push,1)) s __pragma(pack(pop))
#else
#define PACK(s) s __attribute__((__packed__))
#endif

	PACK(struct gen_val_type_t {
		llvm::Type::TypeID ID : 8; // base type
		unsigned SubclassData : 24;
	});

	struct FullType {
		llvm::Type* type; // used by compiler
		unsigned type_attr; // signed, atomic, shared, iso, ref, num_indices
		uint64_t num_fields; // or #elements for fixed sized arrays
		const char* type_name; // maybe NULL for anonymous types
		llvm::DIType* ditype;
		union {
			FullType* elem_type; // for array or tuples
			MapNode* fields;
		};
	};

	/* Named types can be kept in a map using the name as key.
	   anonymous types must be kept too - in a way that allows freeing
	   them when not needed anymore.
	   This can be done in a single linked list */

	struct FTListElem {
		FTListElem* next;
		FullType ft;
	};

	/* The runtime type system has no LLVM infrastructure available
	   so it is a somewhat stripped down version of the above */

	struct RtStructField;

	PACK(struct RtType {
		union {
			struct {
				llvm::Type::TypeID ID : 8; // base type
				unsigned SubclassData : 24;
			};
			unsigned key;
		};
		unsigned type_attr;
		uint64_t num_fields;
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
