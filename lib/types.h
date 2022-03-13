#pragma once

#include "map.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace volvox {

	struct gen_val_type_t {
		llvm::Type::TypeID ID : 8; // base type
		unsigned SubclassData : 24;
	};

	struct FullStructField;

	// Type representation used by compiler - uses LLVM type system
	struct FullType {
		llvm::Type* type; // used by compiler
		unsigned type_attr; // signed, atomic, shared, iso, ref, num_indices
		const char* type_name; // maybe NULL for anonymous types
		union {
			FullType* elem_type; // element-name -> { index, FullType }
			FullStructField* fields;
		};
		llvm::DIType* ditype;
		// unsigned dimensions[...];
	};

	struct FullStructField {
		const char* FieldName;
		FullType rttype;
	};

	struct RtStructField;

	struct RtType {
		union {
			unsigned key;
			gen_val_type_t type;
		};
		unsigned type_attr;
		const char* name;
		union {
			const RtType* elem_type;
			const RtStructField* fields;
		};
		// unsigned dimensions[...];
	};

	struct RtStructField {
		const char* FieldName;
		RtType rttype;
	};

}
