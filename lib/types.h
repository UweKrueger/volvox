#pragma once

#include "map.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace volvox {

	/* The runtime type system has no LLVM infrastructure available
	   so it is a somewhat stripped down version of the above */

#if defined (_MSC_VER)
#define PACK(s) __pragma(pack(push,1)) s __pragma(pack(pop))
#else
#define PACK(s) s __attribute__((__packed__))
#endif

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
