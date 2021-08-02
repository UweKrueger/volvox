
class PrototypeAST;
class ExprAST;

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum Token {
	tok_eof = -1,

	// commands
	tok_def = -2,
	tok_extern = -3,

	// primary
	tok_identifier = -4,
	tok_number = -5,

	// control
	tok_if = -6,
	tok_then = -7,
	tok_else = -8,
	tok_for = -9,
	tok_in = -10,

	// operators
	tok_binary = -11,
	tok_unary = -12,

	// var definition
	tok_var = -13
};

struct DebugInfo {
	llvm::DICompileUnit *TheCU;
	llvm::DIType *DblTy;
	std::vector<llvm::DIScope *> LexicalBlocks;

	void emitLocation(ExprAST *AST);
	llvm::DIType *getDoubleTy();
};

extern DebugInfo KSDbgInfo;

struct SourceLocation {
	int Line;
	int Col;
};

enum CompModes {
	comp_jit,
	comp_obj,
	comp_dbg
};

extern CompModes comp_mode;
extern std::string getTokName(int Tok);
extern SourceLocation CurLoc;
extern SourceLocation LexLoc;
extern int advance();
extern std::string IdentifierStr; // Filled in if tok_identifier
extern double NumVal;             // Filled in if tok_number
extern int gettok();
