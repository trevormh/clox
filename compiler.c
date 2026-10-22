#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
/**
 * The scanner headerfile has a call to scanToken which immediately
 * start scanning when it loads.
 * The scanToken function only produces one token per call, so
 * when this loads we'll have one token ready to be processed
 * by the compiler.
 */
#include "scanner.h"

#ifdef DEBUG_PRINT_CODE
    #include "debug.h"
#endif

typedef struct {
    Token current;
    Token previous;
    bool hadError;
    /**
     * We want to avoid error cascades, causing the compiler to
     * spew out meaningless errors after the first one.
     * panicMode is a flag to track when it's in this state.
     */
    bool panicMode;
} Parser;

/**
 * These are all of lox's precedence levels from lowest to highest.
 *
 * Since C implicitly gives successively larger numbers for enums,
 * this means that PREC_OR is numerically larger than PREC_ASSIGNMENT.
 */
typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

/**
 * declare ParseFn as a type representing a pointer to a function
 * that returns void
 * ...basically allow us to call ParseFn to dynamically call
 * functions by name.
 * Ex:
 * *ParseFn binary; <- reference to binary function
 * binary(canAssign); <- call it
 */
typedef void (*ParseFn)(bool canAssign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

// Track which block each local var belongs to so that we
// know which locals to discard when a block ends.
typedef struct {
    Token name;
    // depth tracks where a local var was declared.
    // 0 is global scope, 1 is the first top level, 2 is inside that, etc...
    int depth;
    bool isCaptured;  // is this a closed over variable? (an upvalue)
} Local;

typedef struct {
    uint8_t index;
    bool isLocal;
} Upvalue;

/**
 * The top level of a lox program is treated as if it's in a function.
 * This enum lets the compiler tell when it's in the top level code
 * or an actual function body.
 */
typedef enum {
    TYPE_FUNCTION,
    TYPE_INITIALIZER,  // class constructor...."init"
    TYPE_METHOD,
    TYPE_SCRIPT
} FunctionType;

// Struct to keep track of local variables that are in scope at each point of compilation.
// The local vars are ordered in the array in the order their declarations appear in code.
typedef struct Compiler {
    /**
     * The compiler treates top-level code as an implicit function,
     * so all functions are technically nested.
     * The "enclosing" member is a linked list that points all
     * the way back to the root compiler at the top level.
     */
    struct Compiler* enclosing;
    ObjFunction* function;  // reference to the function object being built
    FunctionType type;
    Local locals[UINT8_COUNT];  // UINT8_COUNT sets hard limit on # of locals that can be stored
    int localCount;             // how many locals are in scope
    /**
     * Array to store upvalues, which are local variables defined
     * in an enclosing function, aka closure variables.
     * The values added to this array are the indices where the variable
     * can be found in an enclosing function's locals array
     */
    Upvalue upvalues[UINT8_COUNT];
    int scopeDepth;  // number of blocks surrounding hte current bit of code being compiled
} Compiler;

// nested classes are allowed. this represents the current, innermost class being compiled
typedef struct ClassCompiler {
    // ClassCompiler forms a linked list from the current innermost class being compiled
    // out through all of the enclosing classes
    struct ClassCompiler* enclosing;
    bool hasSuperclass;  // tracks if we declared the `super` local var in an inheriting class
} ClassCompiler;

Parser parser;

// Current lexical environment, i.e. scope
Compiler* current = NULL;
// nested classes are allowed, this represents the current, innermost class being compiled
ClassCompiler* currentClass = NULL;

/**
 * Functions live in separate chunks.
 * The "top level" of a lox program is imperative code and will
 * also be treated as a chunk we need to compile into.
 * Lox treats top level code as a "main" function and all other
 * code in their own respective function chunks.
 *
 * The current chunk is always the chunk owned by the
 * function we’re in the middle of compiling.
 */
static Chunk* currentChunk() {
    return &current->function->chunk;
}

static void errorAt(Token* token, const char* message) {
    // Continue if we're already in panic mode to prevent additional
    // but meaningless errors from being returned.
    if (parser.panicMode) return;
    parser.panicMode = true;

    // print where the error occurred
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message) {
    // parser.previous is the previous token
    errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
    errorAt(&parser.current, message);
}

// Scan a single token and set it as the current value in the parser
static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.current.start);
    }
}

/**
 * This function is similar to advance in that it reads the next
 * token, but also validates it's the expected type and reports
 * an error if it's not.
 */
static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    errorAtCurrent(message);
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

/**
 * If the current token matches the given type we
 * consume it and return true
 */
static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

// append a byte to the chunk, which the VM will consume
static void emitByte(uint8_t byte) {
    // send prev token's line # to assist with runtime errors
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

/**
 * Helper function to return to the start of a loop
 * where the condition is re-evaluated again
 * after an iteration completes.
 *
 * Receives an offset in bytes for how far back in
 * the bytecode the VM should go to.
 *
 * Ex:
 * while (something) {
 *      // code in loop body
 *      // <-- evaluated all the statements, we're now at the end of the body
 * }
 *
 * need to go back to the point in the
 * bytestream where the condition (something) was evaluated so it can
 * be re-evaluated, thus creating a loop
 */
static void emitLoop(int loopStart) {
    // OP_LOOP instructs the VM to subtract the current bytestream position
    // from the offset we'll calculate, returning us back to the start.
    emitByte(OP_LOOP);

    // Calculate how far back in the bytestream to go
    // the +2 accounts for the OP_LOOP instructions operands
    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body too large.");

    /**
     * The tldr is that we treat 2 individual bytecode slots
     * as 1 so we can store a larger number spread out across
     * 2 bytes.
     * See patchJump() for a longer explanation, also implemented there.
     */
    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    /**
     * We need a
     * Emit 2 bytes for the jump offset operand.
     * 1 byte can store up to the int of 255, but if we combine them together
     * we can treat it as a single block of 16 bits, which stores up to 65535.
     */
    emitByte(0xff);  // this is the placeholder byte we return the position of
    emitByte(0xff);  // 0xff is 1 byte
    // return the placeholder byte so we know how far back to patch
    return currentChunk()->count - 2;
}

static void emitReturn() {
    // check if we're compiling a constructor
    if (current->type == TYPE_INITIALIZER) {
        // The class instance will be on slot 0 of the function's locals slot,
        // which is located on the VM's stack.
        emitBytes(OP_GET_LOCAL, 0);
    } else {
        /**
         * Lox allows for implicit returns
         * The VM expects every return statement to have a
         * value on the stack, so it becomes "return nil".
         */
        emitByte(OP_NIL);
    }
    // implicit return, user supplied no return statement for a function
    emitByte(OP_RETURN);
}

/**
 * Add constant to the constants pool array and return its index
 *
 * Constants are the values for operands
 * Ex: !4,  "!"" is the operation and 4 is the constant
 */
static uint8_t makeConstant(Value value) {
    // the addConstant function adds the value to the constants pool and returns the index
    int index = addConstant(currentChunk(), value);
    // we defined the max items we can store in the stack stack (STACK_MAX) to be 256 items
    // the OP_CONSTANT instruction uses a single byte for the index operand, we can store and load only up to 256
    if (index > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }

    return (uint8_t)index;
}

/**
 * Constants are the values for operands
 * Ex: 4!,  "!"" is the operation and 4 is the constant
 *
 * First add the value to the constant's table
 * then emit the bytecode
 */
static void emitConstant(Value value) {
    emitBytes(OP_CONSTANT, makeConstant(value));
}

/**
 * Replaces the temporary patch operand now that we've
 * compiled the statement and know how far to jump
 *
 * Ex:
 * if (something) {
 *      // We added a placeholder in the bytestream
 *      // for how many bytes this block occupies.
 * }
 * ....we're now here after the block
 * A placeholder was inserted in the bytestream just before
 * entering the block, now we go back and patch it with the
 * actual offset of it.
 */
static void patchJump(int offset) {
    // get the current offset after compiling the if block
    // -2 to adjust for the bytecode for the jump offset itself.
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    /**
     * We added 2 placeholder bytes (8 bits each) to indicate how many bytes of
     * an offset to skip. 1 byte can store an integer up to 255.
     * If we treat both of those 8 bit slots as a single slot, we now have 16
     * bits to work with, which can store a max number of 65535.
     *
     * Ex: we need to skip 1500 bytes of code, in binary that's 10111011100.
     * That's too big to store in a single 8 bit slot.
     * Instead we'll split across both slots to become:
     * 00000101 11011100
     *
     * We only care about the 1st 8 bit slot, which is 00000101.
     * We can use the bit shift operator ">>" to specify how many
     * bits to "remove". "00000101 11011100 >> 8" becomes 00000000 00000101
     *
     * Even though we shifted, the jump variable is still a 32-bit integer
     * in C's memory. It might have "junk" data in the higher positions
     * so we use & with 0xff to clean it up
     *
     *   00000000 00000101 <-- result of jump >> 8 for 1500
     * & 00000000 11111111 <--- value of 0xff
     * -----------------------
     *   00000000 00000101 <--- (1500 >> 8) & 0xff
     *
     * Now for jump & 0xff;
     *   00000101 11011100 <--- 1500 in binary
     * & 00000000 11111111 <--- value of 0xff
     * ---------------------
     *   00000000 11011100 <---- 1500 & 0xff
     *
     * (jump >> 8) & 0xff; = 00000101
     *  jump & 0xff; = 11011100
     */
    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

// Each instance of a Compiler type represents a function.
static void initCompiler(Compiler* compiler, FunctionType type) {
    /**
     * Since we treat the top level domain as a function, we're
     * always technically in a function and need a way to link
     * to the enclosing function.
     */
    compiler->enclosing = current;
    compiler->function = NULL;  // initialize to null for garbage collection purposes
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->function = newFunction();
    current = compiler;  // current scope

    // Set the function's name.
    // TYPE_SCRIPT means we're in the top level body of a script
    // and not inside a user-defined function
    if (type != TYPE_SCRIPT) {
        // parser.previous points to the function name in the source code.
        // Copy the name of the string instead in case it's freed up when
        // the code completes compiling.
        current->function->name = copyString(parser.previous.start, parser.previous.length);
    }

    // the compiler's locals array keeps track of which stack slots are associated with which local variables.
    // give the compiler index 0 for the VM to use, it will assign the function to execute there
    Local* local = &current->locals[current->localCount++];
    local->depth = 0;
    local->isCaptured = false;
    if (type != TYPE_FUNCTION) {
        /**
         * When a `this` keyword is encountered it needs to be defined somewhere for the VM to look up.
         * The `this` keyword for methods will be stored in stack slot 0.
         */
        local->name.start = "this";
        local->name.length = 4;
    } else {
        local->name.start = "";  // We give it an empty name so that the user can't write an identifier that refers to it
        local->name.length = 0;
    }
}

/**
 * All functions are represented as Compiler objects and we also treat the
 * top level of a lox program as an implicit function, so it gets its
 * own Compiler object.
 *
 * When a new function is created (Compiler object) we set an enclosing property
 * on it pointing to the current scope.
 * For the top level "implicit main" function, enclosing is initialized to NULL.
 * When we declare any functions its parent is the top level
 * "implicit main" function, and if any nested functions are declared then
 * its enclosing is the previous function we just declared.
 *
 * The purpose of this function is to "exit" the function by
 * restoring the current Compiler object back to the parent scope,
 * and then return the function we just created.
 */
static ObjFunction* endCompiler() {
    emitReturn();
    ObjFunction* function = current->function;

// Print out the chunk's bytecode when debug mode is enabled and there was no error
#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        // user defined functions have names, but the implicit top level code "functions" does not
        disassembleChunk(currentChunk(), function->name != NULL ? function->name->chars : "<script>");
    }
#endif
    // now restore the current compiler to the parent
    current = current->enclosing;
    return function;
}

static void beginScope() {
    // 0 is global, 1 is top level, 2 is nested inside 1, etc...
    current->scopeDepth++;
}

static void endScope() {
    current->scopeDepth--;

    // Remove local variables from the stack when we exit a scope
    while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
        // If the local is a captured upvalue add it to the heap
        if (current->locals[current->localCount - 1].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            // variable isn't closed over, no need to keep it around
            emitByte(OP_POP);
        }
        current->localCount--;
    }
}

/**
 * A few forward declarations due to some calls in
 * other functions. In this file instead of header because
 * they're static functions.
 *
 * Ex: getRule is defined after binary, but binary calls it.
 */
static void expression();
static void statement();
static void declaration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

/**
 * Identifiers are names of functions, variables, classes, etc...
 *
 * Makes a copy of the identifier and adds it to the constants table
 * and return its index.
 */
static uint8_t identifierConstant(Token* name) {
    ObjString* identifier_idx = copyString(name->start, name->length);
    return makeConstant(OBJ_VAL(identifier_idx));
}

// Helper to check if 2 vars, functions, etc.. share the same name
static bool identifiersEqual(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static void addLocal(Token name) {
    if (current->localCount == UINT8_COUNT) {
        error("Too many local variables in function.");
        return;
    }

    // allocate new stack slot for variable
    Local* local = &current->locals[current->localCount++];
    local->name = name;
    // Sentinal to indicate the variable is "uninitialized"
    // Will be updated to current scope when the value is assigned
    local->depth = -1;
    local->isCaptured = false;
}

/**
 * Find the index of a variable name in a compiler object's locals array
 * and return it
 */
static int resolveLocal(Compiler* compiler, Token* name) {
    // Walk list of local vars in scope and check if the identifiers match
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            // -1 depth means a variable isn't defined yet, and if we encounter one
            // we're in its own initializer, which is not valid.
            // Ex: can't do var a = a;
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }

    return -1;  // not found
}

/**
 * Upvalues are local vars defined in an enclosing function, i.e. a closure.
 *
 * Each compiler object (function) keeps an array of upvalue structs to
 * track the closed-over variables. When an upvalue is identified by
 * resolveUpvalue, this is called to add the upvalue.
 *
 * index is the value where it can be found in the enclosing function's local var slot
 */
static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
    int upvalueCount = compiler->function->upvalueCount;

    // check if the function already has an upvalue for the variable so we don't create duplicates
    for (int i = 0; i < upvalueCount; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) {
            return i;
        }
    }

    if (upvalueCount == UINT8_COUNT) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index = index;
    // return the index where we added the upvalue for the closed-over function
    // to add to its upvalues array so the VM knows where to look at runtime.
    // It will be used as the OP_GET_UPVALUE and OP_SET_UPVALUE operands
    return compiler->function->upvalueCount++;
}

/**
 * Find a local variable defined in an enclosing scope, aka closure
 *
 * This function looks for local variable declared in any
 * surrounding functions, aka upvalues.
 * This is called after failing to resolve a local var in
 * the current function's scope, so we know it isn't in
 * the current compiler (function) object
 *
 * Returns the "upvalue index", which is the index in the
 * array of upvalues attached to the function, otherwise
 * returns -1 if not found.
 */
static int resolveUpvalue(Compiler* compiler, Token* var_name) {
    // global scope, no upvalues here...
    if (compiler->enclosing == NULL) return -1;

    // try to resolve the identifier as a local variable in the enclosing compiler
    int local_idx = resolveLocal(compiler->enclosing, var_name);
    if (local_idx != -1) {
        return addUpvalue(compiler, (uint8_t)local_idx, true);
    }

    /**
     * Since functions can nest many layers deep and be returned by their enclosing functions,
     * we need a way to capture variables defined before their enclosing scope is popped from the stack.
     * Ex:
     * fun outer() {
     *    var x = "value";
     *    fun middle(){
     *      fun inner() {
     *          print x;
     *      }
     *      return inner;
     *    }
     *    return middle;
     * }
     *
     * We need to add x to both inner and middle's upvalues arrays.
     * We do this by recursively calling this function until we get all the way
     * to where x is defined in outer, adding x to outer's locals array.
     * Then on the returning iteration (in middle) we reach this chunk where
     * x does exist in the enclosing scope (outer), so it gets added to middle's upvalues.
     * Then on inner we check middle's upvalues and find it there, then add it to inner's upvalues.
     */
    int upvalue = resolveUpvalue(compiler->enclosing, var_name);
    if (upvalue != -1) {
        compiler->enclosing->locals[local_idx].isCaptured = true;
        return addUpvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

/**
 * Declaring a variable adds it to the scope, but it's
 * not ready for use until it's defined,
 * i.e. assign a value if it has an initializer
 *
 * Declaring adds a local variable to the compiler's list
 * of variables in the current scope.
 */
static void declareVariable() {
    // Skip globals. They are late bound (resolved at runtime)
    if (current->scopeDepth == 0) return;

    Token* name = &parser.previous;
    // Shadowing check.
    // Start at the end of the local vars array and work backwards
    // checking that no other variables in the current scope share the name
    // of the variable we're declaring.
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error("Already a variable with this name in this scope.");
        }
    }
    addLocal(*name);
}

/**
 * Global variables are looked up by name at runtime by the VM.
 * A string is too big to stuff into the bytecode stream, so instead
 * we store it in the constant's table and put its index in the stream
 *
 * Local variables
 */
static uint8_t parseVariable(const char* errorMessage) {
    // consume checks the next token matches TOKEN_IDENTIFIER
    // and advances the parser for declareVariable to handle it
    consume(TOKEN_IDENTIFIER, errorMessage);

    /**
     * Declaring a variable only applies to locals.
     * Adds a Local instance with the vars name and
     * depth to the Compiler struct's locals array.
     */
    declareVariable();

    // Exit the function if we're in a local scope.
    // Local vars aren't looked up by name so there's no need to
    // put them in the constant's table and return a "dummy index"
    if (current->scopeDepth > 0) return 0;

    // Add global variable to constant's table and return its index
    return identifierConstant(&parser.previous);
}

static void markInitialized() {
    /**
     * Function declarations....
     * We mark variables as initialized after they're fully defined
     * so it can't be accessed inside its own initializer, but
     * functions can be referenced inside their own body.
     */
    if (current->scopeDepth == 0) return;

    // mark variable as initialized by updating its depth from -1 to the current scope depth
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

/**
 * Defining a variable marks it ready for use.
 *
 * This function is called last in varDeclaration
 * after the variable name has already been parsed
 * and added to the constants table.
 *
 * global is the index in the constants table where
 * the variable name can be found.
 */
static void defineVariable(uint8_t global) {
    /**
     * Local vars values live on the stack, unlike globals.
     * They have already been initialized and the value will
     * be sitting on the stack ready for us to use.
     */
    if (current->scopeDepth > 0) {
        markInitialized();
        return;
    }
    /**
     * Still need to emit globals though, because they can
     * be accessed in any scope. We're not getting its
     * value from the stack.
     */
    emitBytes(OP_DEFINE_GLOBAL, global);
}

/**
 * Process arguments for a function call.
 * Each value gets left on the stack in prep
 * for the call.
 */
static uint8_t argumentList() {
    uint8_t argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            expression();
            // Each single-byte operand in the bytestream can only go up to 255
            if (argCount == 255) {
                error("Can't have more than 255 arguments.");
            }
            argCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return argCount;
}

static void and_(bool canAssign) {
    /**
     * At the point this is called, the left-hand side expression has already been compiled.
     * That means at runtime, its value will be on top of the stack.
     * If that value is falsey, then we know the entire and must be false,
     * so we skip the right operand and leave the left-hand side value
     * as the result of the entire expression.
     */
    int endJump = emitJump(OP_JUMP_IF_FALSE);

    /**
     * When the left half is false the VM will skip over this,
     * leaving the value to be come the result of the expression
     */
    emitByte(OP_POP);
    parsePrecedence(PREC_AND);

    patchJump(endJump);
}

// binary operations contain a single operator like 3 + 4 or 5 / 2
static void binary(bool canAssign) {
    /**
     * At this point we've already consumed the entire left hand
     * operand of the expression. The previous token will
     * be the operator. Ex: "4 +" if we're parsing "4 + 2"
     *
     * When run, the VM will execute the left and right operand code,
     * in that order, leaving their values on the stack.
     */
    TokenType operatorType = parser.previous.type;
    ParseRule* rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (operatorType) {
        // The expression a != b has the same semantics as !(a == b)
        // Instead of a dedicated OP_NOT_EQUAL instruction for the VM,
        // it can output an OP_EQUAL followed by an OP_NOT.
        case TOKEN_BANG_EQUAL: emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL: emitByte(OP_EQUAL); break;
        case TOKEN_GREATER: emitByte(OP_GREATER); break;
        // a <= b is the same as !(a > b)
        case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS: emitByte(OP_LESS); break;
        case TOKEN_LESS_EQUAL: emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS: emitByte(OP_ADD); break;
        case TOKEN_MINUS: emitByte(OP_SUBTRACT); break;
        case TOKEN_STAR: emitByte(OP_MULTIPLY); break;
        case TOKEN_SLASH: emitByte(OP_DIVIDE); break;
        default: return;  // Unreachable.
    }
}

static void call(bool canAssign) {
    uint8_t argCount = argumentList();  // The argumentList function gets left on the stack
    emitBytes(OP_CALL, argCount);
}

// class property access
// Ex: eclair.filling = "pastry creme";
static void dot(bool canAssign) {
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
    uint8_t nameIdx = identifierConstant(&parser.previous);

    /**
     * If there's an = sign that means it's a setter, but we don't always
     * allow an equals sign to be compiled.
     * Ex: a + b.c = 3
     * If dot() parsed the `= 3` we would incorrectly interpret the
     * code as if the user had written `a + (b.c = 3)`
     *
     * The problem is the = side of the expression has much lower
     * precedence than the . part. The parser may call dot() in a context that
     * is too high (ex: PREC_TERM). To avoid this we only parse and
     * compile if canAssign is true, which was defined in parsePrecedence
     * as this:
     * bool canAssign = precedence <= PREC_ASSIGNMENT;
     *
     */
    if (canAssign && match(TOKEN_EQUAL)) {
        expression();
        emitBytes(OP_SET_PROPERTY, nameIdx);

        /**
         * Lox supports accessing methods without calling them immediately.
         * Ex: var a_method = some_instance.my_method;
         * a_method();
         *
         * This is slow, so we don't always want to process method access and
         * calling separately if the user performs it in one step.
         * Ex: var a_method = some_instance.method();
         *
         * Look for the left parenthesis so we can process it in one step
         * if the user accesses + calls the method.
         */
    } else if (match(TOKEN_LEFT_PAREN)) {
        uint8_t argCount = argumentList();
        emitBytes(OP_INVOKE, nameIdx);
        emitByte(argCount);
    } else {
        emitBytes(OP_GET_PROPERTY, nameIdx);
    }
}

static void literal(bool canAssign) {
    switch (parser.previous.type) {
        case TOKEN_FALSE: emitByte(OP_FALSE); break;
        case TOKEN_NIL: emitByte(OP_NIL); break;
        case TOKEN_TRUE: emitByte(OP_TRUE); break;
        default: return;  // Unreachable.
    }
}

static void grouping(bool canAssign) {
    // We assume the initial "(" has already been consumed when this function is called.
    // Recursively call expression to compile the expression between parenthesis
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

// compile number literals, TOKEN_NUMBER
static void number(bool canAssign) {
    // We assume the token for the number literal has already been consumed and is stored in previous
    double value = strtod(parser.previous.start, NULL);  // convert string to double
    emitConstant(NUMBER_VAL(value));
}

static void or_(bool canAssign) {
    /**
     * In an or expression, if the left side is truthy then
     * we skip over the right operand. When the left side
     * is falsey then it does a tiny jump over the
     * next statement.
     *
     * Remember - This all gets compiled linearly, then the
     * VM runs through it.
     * The VM determines which to skip over, not the compiler.
     */

    // left side was falsey, continue evaluating right hand expression
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    // If left hand side is truthy we don't have to evaluate the right,
    // so in that case we always want to jump
    int endJump = emitJump(OP_JUMP);

    // patch for the OP_JUMP_IF_FALSE
    patchJump(elseJump);
    // pop left if it was falsey, we don't need it anymore. We will
    // next be evaluating the RHS which will become the new value
    emitByte(OP_POP);

    // parse the right hand operator if the left was falsey
    parsePrecedence(PREC_OR);
    patchJump(endJump);
}

static void string(bool canAssign) {
    // The +1 and -2 are to trim the leading and trailing quotation marks
    // -2 because of the null terminating character
    const char* start_of_string = parser.previous.start + 1;
    int length = parser.previous.length - 2;
    emitConstant(OBJ_VAL(copyString(start_of_string, length)));
}

// get or set a value for a variable
static void namedVariable(Token name, bool canAssign) {
    /**
     * Local variables are added to the bytecode stream and
     * will be directly added to the stack by the VM.
     *
     * Global variables need to be added to the constants table
     * and its index added to the bytestream.
     */
    uint8_t getOp, setOp;
    int local_idx = resolveLocal(current, &name);
    if (local_idx != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
        // Check enclosing scope for local variables, aka upvalues
    } else if ((local_idx = resolveUpvalue(current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        // global variable, add to constants table
        local_idx = identifierConstant(&name);
        // OP_GET_GLOBAL instruction will tell the VM to pull that value from
        // the constants pool and then gets the value from the globals hash table.
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    // set variable (Not declare, i.e. x = 5 instead of var x = 5)
    if (canAssign && match(TOKEN_EQUAL)) {
        expression();
        emitBytes(setOp, (uint8_t)local_idx);
    } else {
        // get variable
        emitBytes(getOp, (uint8_t)local_idx);
    }
}

// get value assigned to a local variable
static void variable(bool canAssign) {
    // parser.previous is the var's name.
    // canAssign indicates whether the compiler should look for a
    // following '=' operator and parse as a setter.
    namedVariable(parser.previous, canAssign);
}

/**
 * helper function to add a token that doesn't technically exist.
 *
 * when setting up class inheritance we need to reference the superclass
 * by setting a local variable so users can type `super.` inside methods.
 *
 * The inheritance setup is done during class creation,  not when it encounters
 * `super.` in a method for the first time, so there's no `super` token to parse.
 * This function allows us to create one and push it onto the stack so
 * the runtime can look up the superclass.
 */
static Token syntheticToken(const char* text) {
    Token token;
    token.start = text;
    token.length = (int)strlen(text);
    return token;
}

static void super_(bool canAssign) {
    if (currentClass == NULL) {
        error("Can't use 'super' outside of a class.");
    } else if (!currentClass->hasSuperclass) {
        error("Can't use 'super' in a class with no superclass.");
    }
    /**
     * Lox suppoerts getting access to a super method without invoking it.
     * Ex: var closure = super.parent_method; // where parent_method is a function in the parent class
     * closure();
     *
     * when the compiler hits a super token we consume the '.' and look for the name.
     */
    consume(TOKEN_DOT, "Expect '.' after 'super'.");
    consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
    // methods are looked up dynamically, store the it in the constatns table
    // just like we do for property access expression
    uint8_t parent_method = identifierConstant(&parser.previous);

    /**
     * In order to acecss a superclass method on the current instance
     * the runtime needs both the receiver and superclass.
     *
     * first look up the receiver (this) and push to the stack. It tells
     * the VM which object to run the method on (the child class)
     */
    namedVariable(syntheticToken("this"), false);

    // check if the method is being invoked
    if (match(TOKEN_LEFT_PAREN)) {
        /**
         * If the method is being invoked immediately then use the
         * super invoke instruction, which combines the behavior of
         * OP_GET_SUPER and OP_CALL. 
         * This bypasses binding the method (in OP_GET_SUPER) because 
         * we'd be calling it anyway, speeding it up.
         */
        uint8_t argCount = argumentList(); // evaluate args and push them onto the stack
        namedVariable(syntheticToken("super"), false);
        emitBytes(OP_SUPER_INVOKE, parent_method);
        emitByte(argCount);
    } else {
        // no call yet, just accessing the method

        // look up superclass and push it on top of the stack so we can
        // access the parent (super) method
        namedVariable(syntheticToken("super"), false);
        emitBytes(OP_GET_SUPER, parent_method);
    }
}

/**
 * we treat the `this` keyword as a lexically scoped local var.
 * By the time this is called the the `this` token has been consumed and is
 * stored in the prvious token (which variable() references)
 */
static void this_(bool canAssign) {
    if (currentClass == NULL) {
        error("Can't use 'this' outside of a class.");
        return;
    }
    /**
     * variable() uses canAssign to indicate whether the compiler should look for
     * a '=' operator and parse as a setter.
     * You can't assign to `this`, so false is provided.
     */
    variable(false);
}

// Unary expressions are those with a single operator like -5 or 3!
// FOr now this is only handling negation
static void unary(bool canAssign) {
    // The leading "-" token has been consumed and is sitting in parser.previous
    TokenType operatorType = parser.previous.type;

    // Compile the operand.
    parsePrecedence(PREC_UNARY);

    // Emit the operator instruction after the expression has been compiled.
    // This is due to the VM being a stack, where it's applied to the expression.
    switch (operatorType) {
        case TOKEN_BANG: emitByte(OP_NOT); break;
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;
        default: return;  // Unreachable.
    }
}

/**
 * This is a table of functions to call based on the expression and precedence.
 * It is an array of ParseRule structs, so the 2nd column can be
 * accessed like rules[0]->infix.
 *
 * The "indices" map directly to the TokenType enum we defined in the scanner.
 * Ex: TOKEN_LEFT_BRACE is index #2 here and was also defined #2 in the TokenType enum.
 *
 * The first column represents the function to compile a prefix expression.
 * Ex: (3....) or -5, the tokens would be ( and -, respectively
 * - many of these are NULL because you can't start an expression
 * with "}" or "else"
 *
 * The 2nd column represents the function to compile an infix expression.
 * Ex: 4 + 2
 *
 * The 3rd column is the precedence of an infix expression that uses
 * that token as an operator.
 * Ex: 5 / 3 and 4 * 2 are of Precedence PREC_FACTOR
 *
 */
ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, dot, PREC_CALL},
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
    [TOKEN_BANG] = {unary, NULL, PREC_NONE},
    [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_STRING] = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {NULL, NULL, PREC_NONE},
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, or_, PREC_OR},
    [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
    [TOKEN_SUPER] = {super_, NULL, PREC_NONE},
    [TOKEN_THIS] = {this_, NULL, PREC_NONE},
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},
    [TOKEN_WHILE] = {NULL, NULL, PREC_NONE},
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

/**
 * This function starts at the current token and parses any
 * expression at the given precedence level or higher.
 *
 * Ex: we're pasing -a.b + c
 *
 * If we call parsePrecedence(PREC_ASSIGNMENT), then it will
 * parse the entire expression because + has higher precedence
 * than assignment.
 *
 * If instead we call parsePrecedence(PREC_UNARY), it will compile
 * the -a.b and stop there. It doesn’t keep going through the + because
 * the addition has lower precedence than unary operators.
 */
static void parsePrecedence(Precedence precedence) {
    // Read next token and look up its ParseRule
    advance();
    // we just advanced so the token we actually want is the previous one
    // getRule returns a pointer to the ParseRule function
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    // In Pratt parsing, the parser calls getRule(token.type)->prefix
    // only when parsing the start of an expression so there should
    // always be a prefix rule.
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }

    // The only time we allow an assignment is when parsing an assignment expression
    // or top-level expression like in an expression statement.
    // Ex: Can't perform a * b = 3;
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    /**
     * Now we look for an infix parser for the next token.
     * If we find one, it means the prefix expression we
     * already compiled might be an operand for it.
     *
     * Ex: 3 + 4 * 5
     * 3 would be captured at this point by the prefixRule() call above.
     * It calls the binary function and parse.previous.type is now "+"
     * and the current expression it's parsing is now 4 * 5.
     * The binary function calls this again, so now the prefixRule() call
     * would capture 4 as the prefix and we reach this point again
     * where precedence is PREC_FACTOR
     */
    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }

    // If the = doesn’t get consumed as part of the expression,
    // nothing else is going to consume it.
    if (canAssign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
}

/**
 * Return the row from the rules at a given index.
 *
 * Each type in the TokenType enum from the scanner maps to the same
 * index in the ParseRule table.
 */
static ParseRule* getRule(TokenType type) {
    return &rules[type];
}

static void expression() {
    // parse the lowest precedence level, which subsumes all
    // of the higher precedence expressions too.
    parsePrecedence(PREC_ASSIGNMENT);
}

static void block() {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

/**
 * Helper function to compile function body.
 * Leaves the resulting function object on top of the stack.
 */
static void function(FunctionType type) {
    /**
     * Create new compiler struct, which contains the
     * ObjFunction, array for locals, etc...
     * Pass it to initCompiler, which sets up some values on
     * the ObjFunction like initializing memory, initializes
     * the chunk, etc...
     */
    Compiler compiler;
    initCompiler(&compiler, type);
    beginScope();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    // handle function params
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            uint8_t constant = parseVariable("Expect parameter name.");
            defineVariable(constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block();

    ObjFunction* function = endCompiler();
    uint8_t index = makeConstant(OBJ_VAL(function));
    emitBytes(OP_CLOSURE, index);

    // add any upvalues
    for (int i = 0; i < function->upvalueCount; i++) {
        /**
         * The OP_CLOSURE instruction has a 2 byte encoding for upvalues
         * If the first byte is 1 it captures a local variable in the enclosing function.
         * If 0 that means the variable is also an upvalue in the enclosing functions
         * They're both upvalues to the enclosed function, but it tells the VM where to look when fetching/setting values.
         */
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);  // local slot or upvalue index to capture
    }
}

static void method() {
    consume(TOKEN_IDENTIFIER, "Expect method name.");
    uint8_t nameIdx = identifierConstant(&parser.previous);

    FunctionType type = TYPE_METHOD;
    // if we're compiling an instance constructor, i.e. method named "init", then
    // the class instance should be returned
    if (parser.previous.length == 4 && memcmp(parser.previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }
    // function() compiles the params list + body, then emits the opcode to create
    // an ObjClosure and leaves it on top of the stack for the VM to find.
    function(type);
    emitBytes(OP_METHOD, nameIdx);
}

static void classDeclaration() {
    consume(TOKEN_IDENTIFIER, "Expect class name.");  // we've arleady consumed the "class" identifier, next should be its name
    Token className = parser.previous;
    // add it to the surrounding function's constants table
    uint8_t nameConstantIdx = identifierConstant(&parser.previous);
    declareVariable();  // declaring variable adds it to the scope

    emitBytes(OP_CLASS, nameConstantIdx);
    /**
     * Declaring variable adds it to the scope, but we can't use it
     * until it's defined.
     * For classes, need to define before the body so that
     * can refer to the containing class inside its methods.
     */
    defineVariable(nameConstantIdx);

    // lox supports nested classes
    // link the current nested class to the enclosing one
    ClassCompiler classCompiler;
    classCompiler.hasSuperclass = false;
    classCompiler.enclosing = currentClass;
    currentClass = &classCompiler;

    // inheritence is defined by `<` symbol. Ex: class Cruller < Doughnut {
    if (match(TOKEN_LESS)) {
        consume(TOKEN_IDENTIFIER, "Expect superclass name.");
        // treat superclass name as a variable, put it on the stack.
        // variable() uses the most recently consumed token as the name
        // where as namedVariable does the same but with the name as an arg
        bool canAssign = false;
        variable(canAssign);

        if (identifiersEqual(&className, &parser.previous)) {
            error("A class can't inherit from itself.");
        }

        // create a new scope so when we load the superclass on
        // the stack it'll be a local variable instead of global.
        // It also ensures if we declare 2 classes in the same scope, each has
        // its own local slot to store its superclass.
        beginScope();
        // name the superclass var "super" for the same we reason we use "this".
        // "super" is reserved, which guarantees no naming collisions with user defined vars
        char* superClasssName = "super";
        addLocal(syntheticToken(superClasssName));
        defineVariable(0);

        // load the subclass onto the stack
        namedVariable(className, false);
        emitByte(OP_INHERIT);
        classCompiler.hasSuperclass = true;
    }

    /**
     * put the className on top of the stack so when we move onto compiling
     * methods next, the VM is easily able to find the class's name to
     * bind the methods to.
     * When we execute each OP_METHOD instruction the stack has the method's
     * closure on top with the class right under it.
     */
    namedVariable(className, false);

    // compile body
    consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");
    // Lox doesn’t have field declarations, so anything before the closing brace at the end of the class body must be a method
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        method();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");

    /**
     * pop the class name off the stack
     * We're done compiling the class so there will be
     * no more methods that need access to its name
     */
    emitByte(OP_POP);

    // since we opened a local scope for the superclass variable, we need to close it.
    if (classCompiler.hasSuperclass) {
        endScope();
    }

    currentClass = currentClass->enclosing;  // restore the enclosing class
}

static void funDeclaration() {
    /**
     * Functions are first-class values in Lox.
     * We parse them just like any variable, at the top level
     * they will be global variable and inside a block or function
     * they are created as local variables.
     */
    // return value `global` is its index in the constant's table
    uint8_t global = parseVariable("Expect function name.");
    markInitialized();
    function(TYPE_FUNCTION);  // compile the param list and block body
    defineVariable(global);
}

/**
 * global variables are looked up by name at runtime.
 * We store the name in the constants table instead of in the bytecode stream.
 */
static void varDeclaration() {
    /**
     * Global variables are added to the constant's table and its index
     * is added to the bytestream to be looked up at runtime.
     *
     * Local variables are not added to the constants table.
     * parseVariable returns a "dummy" index for them
     */
    uint8_t index = parseVariable("Expect variable name.");

    // now handle the value of the variable if we're assigning it (initializer)
    // after a match is found it advances the parser once again so we're now on the var's value
    if (match(TOKEN_EQUAL)) {
        /**
         * The call to match advances past the equal sign, and
         * expression calls parsePrecidence, which advances again.
         * ex: var a = 3;
         * After match is called we're now on the 3
         * then parsePrecidence advances to the ';' so when it calculates
         * the prefix rule it checks previous which is actually 3
         */
        expression();
    } else {
        // declaring without assigning a value
        emitByte(OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    /**
     * Defining a local var means marking it ready for use (change depth from -1 to current depth)
     * Add the variable name's index last.
     * At runtime we execute the initilizer first, then this
     * instruction stores the value for later use.
     */
    defineVariable(index);
}

/**
 * An expression statment is an expression followed by a semicolon
 * ex:
 * brunch = "quiche";
 * eat(brunch);
 *
 * semantically, an expression statements evaluates the
 * expression and discards the result.
 * The compiler encodes this behaveior by compiling the expression
 * and emitting an OP_POP instruction.
 */
static void expressionStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);
}

static void forStatement() {
    beginScope();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    if (match(TOKEN_SEMICOLON)) {
        // No initializer.
    } else if (match(TOKEN_VAR)) {
        varDeclaration();
    } else {
        expressionStatement();
    }
    consume(TOKEN_SEMICOLON, "Expect ';'.");

    // Loop start is just before the middle of the 3 loop params, the condition expression
    int loopStart = currentChunk()->count;

    int exitJump = -1;
    // Check if the optional condition clause is present (the middle of the 3 for loop args)
    // If the next char isn't a semicolon we know there's a condition expression
    if (!match(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        // Jump out of the loop if the condition is false.
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);  // Condition.
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    // check if increment clause was provided
    if (!match(TOKEN_RIGHT_PAREN)) {
        /**
         * Lexically the increment clause appears before the body, but
         * is executed after it (ex: i++ will run after the body is evaluated)
         *
         * Since the compiler makes a single pass we cant compile the increment clause later.
         * Instead the VM jumps over the increment clause, runs the loop body and then
         * jumps back to the increment clause, then goes to the body for the next iteration.
         */
        int bodyJump = emitJump(OP_JUMP);  // tell the VM to skip over the increment condition
        int incrementStart = currentChunk()->count;
        expression();      // compile the increment expression
        emitByte(OP_POP);  // only evaluated it for its side effect, discard it
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        // VM will take us back to the start of the main loop, right before the condition expression
        emitLoop(loopStart);
        /**
         * Update loopStart to the point where the increment expression happens.
         * We'll later emit an instruction to jump back to this position to run the
         * increment after the main body completes, i.e. an iteration.
         */
        loopStart = incrementStart;
        patchJump(bodyJump);
    }

    statement();  // loop body
    // after the body completes an iteration jump back to the condition expression
    // to see if we should continue iterating
    emitLoop(loopStart);

    /**
     * Just before this, we compiled code for the VM to go back to the
     * start of the loop, which we defined as the middle of the 3 for
     * loop params, the condition expression.
     *
     * Now that we've compiled the full loop and know how long it is, patch
     * the size with the loop's full length, so if the condition evaluates
     * to false the VM knows how far to skip in the bytecode stream.
     */
    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP);  // Condition.
    }

    endScope();
}

static void ifStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    // The evaluated expression will be left on top of the stack
    // and we'll use to determine if we need to skip the if block.
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    // OP_JUMP_IF_FALSE has an operand for how much to offset the ip
    // if the condition is false, i.e. how many bytes to skip
    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    /**
     * The VM doesn't pop the value of the if condition result (true/fase)
     * from the top of the stack because other logical operatores use
     * the OP_JUMP_IF_FALSE instruction too.
     *
     * When the condition is truthy we pop it right before
     * entering the body of the if statement.
     */
    emitByte(OP_POP);
    // compile the body of the if statement, aka "then branch"
    statement();

    /**
     * When the if condition evaluates to true the body
     * is entered and evaluated, but at the end we need to
     * skip over an else block if there is one.
     */
    int elseJump = emitJump(OP_JUMP);

    // patch the placeholder for the size of the "if body".
    // thenJump is the index of the instruction to patch
    patchJump(thenJump);
    /**
     * If the if condition was false we need to pop the value of the if
     * condition (true/false) at the beginning of the else branch
     */
    emitByte(OP_POP);

    if (match(TOKEN_ELSE)) statement();
    patchJump(elseJump);
}

static void printStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

// when the compiler sees a return keyword, it goes here
static void returnStatement() {
    // returns aren't allowed at top-level code, i.e. not in a function
    if (current->type == TYPE_SCRIPT) {
        error("Can't return from top-level code.");
    }

    // return expression is optional, so check for a semicolon to tell if a value was provided
    // A semicolon means implicit return, i.e. return;
    if (match(TOKEN_SEMICOLON)) {
        emitReturn();
    } else {
        if (current->type == TYPE_INITIALIZER) {
            error("Can't return a value from an initializer.");
        }

        // return value provided, crunch the expression to get it
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
        emitByte(OP_RETURN);
    }
}

static void whileStatement() {
    /**
     * Reminder that these are compiled linearly and all
     * in the bytestream.
     * The VM determines which items to skip over.
     */

    // Get the current bytecode offset so we can return to
    // this position in the bytecode stream for looping.
    int loopStart = currentChunk()->count;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    // Evaluate the "condition" of the while statement to run
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    // Skip over the body if the expression we just evaluated is false.
    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    /**
     * If this is reached by the VM we're now in the body of the while loop.
     * Pop the value from the condition from the stack so then we can evaluate
     */
    emitByte(OP_POP);
    // Evaluate the body of the while loop.
    statement();
    // The loopstart instruction tells us how far back to jump
    // i.e. back to the start of the loop body
    emitLoop(loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);
}

/**
 * If we hit a compile error while parsing the
 * previous statement, we enter panic mode.
 *
 * We skip tokens indiscriminately until we reach
 * something that looks like a statement boundary.
 *
 * We recognize the boundary by looking for a preceding
 * token that can end a statement, like a semicolon.
 *
 * Or we’ll look for a subsequent token that begins a statement,
 * usually one of the control flow or declaration keywords.
 */
static void synchronize() {
    parser.panicMode = false;

    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;

            default:;  // Do nothing.
        }

        advance();
    }
}

static void declaration() {
    if (match(TOKEN_CLASS)) {
        classDeclaration();
    } else if (match(TOKEN_FUN)) {
        funDeclaration();
    } else if (match(TOKEN_VAR)) {
        varDeclaration();
    } else {
        statement();
    }

    if (parser.panicMode) synchronize();
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_IF)) {
        ifStatement();
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
    } else {
        expressionStatement();
    }
}

ObjFunction* compile(const char* source) {
    initScanner(source);  // set the starting point for scanning the user's code
    Compiler compiler;
    /**
     * Top level code in lox is treated as if it's wrapped
     * in an implicit "main" function.
     * TYPE_SCRIPT is an enum of FunctionType, which indicates it's top-level code
     */
    initCompiler(&compiler, TYPE_SCRIPT);

    parser.hadError = false;
    parser.panicMode = false;

    advance();

    // compile delcarations until the end of the source file
    while (!match(TOKEN_EOF)) {
        declaration();
    }

    /**
     * Lox is imperative, so not all code has to live in a function.
     * We'll treat the top level code if it was implicitly wrapped in
     * a main, so the compiler result is a "function" which we pass
     * to the VM
     */
    ObjFunction* function = endCompiler();
    // Don't return the compiled code to the VM if there were errors
    return parser.hadError ? NULL : function;
}

/**
 * Helper function for garbage collector
 * Check the ObjFunction it's compiling into for "roots", which are values
 * that can be accessed without having to traverse through other references
 */
void markCompilerRoots() {
    Compiler* compiler = current;
    while (compiler != NULL) {
        markObject((Obj*)compiler->function);
        compiler = compiler->enclosing;
    }
}
