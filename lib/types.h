#pragma once

#include "map.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfoMetadata.h>

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
		unsigned num_fields; // or #elements for fixed sized arrays
		const char* type_name; // maybe NULL for anonymous types
		llvm::DIType* ditype;
		union {
			FullType* elem_type; // for array or tuples
			MapNode* fields;
		};
		llvm::Constant* rttype;
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
		union {
			struct {
				unsigned type_attr : 16;
				unsigned num_fields : 16;
			};
			unsigned attr;
		};
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
