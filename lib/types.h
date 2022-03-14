#pragma once

#include "map.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace volvox {

	/* The compile time type system supplements the LLVM
	   type system with attributes and field names */

	// Type representation used by compiler - uses LLVM type system
	struct FullType {
		llvm::Type* type; // used by compiler
		unsigned type_attr; // signed, atomic, shared, iso, ref, num_indices
		const char* type_name; // maybe NULL for anonymous types
		union {
			FullType* elem_type; // for array or tuples
			MapNode* fields;
		};
		llvm::DIType* ditype;
	};

	/* The runtime type system has no LLVM infrastructure available
	   so it is a somewhat stripped down version of the above */

#if defined (_MSC_VER)
#define PACK(s) __pragma(pack(push,1)) s __pragma(pack(pop))
#else
#define PACK(s) s __attribute__((__packed__))
#endif

	PACK(struct gen_val_type_t {
		llvm::Type::TypeID ID : 8; // base type
		unsigned SubclassData : 24;
	});

	struct RtStructField;

	PACK(struct RtType {
		PACK(struct {
			llvm::Type::TypeID ID : 8; // base type
			unsigned SubclassData : 24;
		});
		PACK(struct {
			unsigned type_attr : 16;
			unsigned num_dimension : 16;
		});
		const char* name;
		union {
			const RtType* elem_type;
			const RtStructField* fields;
		};
		// unsigned dimensions[...];
	});

	struct RtStructField {
		const char* FieldName;
		RtType rttype;
	};

}
