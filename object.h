#ifndef clox_object_h
#define clox_object_h

#include "chunk.h"
#include "common.h"
#include "table.h"
#include "value.h"

// Macro to extract the object type tag from a given Value
#define OBJ_TYPE(value) (AS_OBJ(value)->type)

// Macro to check if an Obj* is of ObjFunction*, ObjString*, etc...
// before casting "down" ("generic" Obj* to ObjString*)
// Takes a Value, not raw obj* because most of the code in the VM works with Values
#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define IS_CLASS(value) isObjType(value, OBJ_CLASS)
#define IS_CLOSURE(value) isObjType(value, OBJ_CLOSURE)
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define IS_INSTANCE(value) isObjType(value, OBJ_INSTANCE)
#define IS_NATIVE(value) isObjType(value, OBJ_NATIVE)
#define IS_STRING(value) isObjType(value, OBJ_STRING)

// Macros to assist with converting between lox runtime Value to a C pointer type
#define AS_BOUND_METHOD(value) ((ObjBoundMethod*)AS_OBJ(value))
#define AS_CLASS(value) ((ObjClass*)AS_OBJ(value))
#define AS_CLOSURE(value) ((ObjClosure*)AS_OBJ(value))
#define AS_FUNCTION(value) ((ObjFunction*)AS_OBJ(value))
#define AS_INSTANCE(value) ((ObjInstance*)AS_OBJ(value))
#define AS_NATIVE(value) (((ObjNative*)AS_OBJ(value))->function)
#define AS_STRING(value) ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value) (((ObjString*)AS_OBJ(value))->chars)

typedef enum {
    OBJ_BOUND_METHOD,
    OBJ_CLASS,
    OBJ_CLOSURE,
    OBJ_FUNCTION,
    OBJ_INSTANCE,
    OBJ_NATIVE,
    OBJ_STRING,
    OBJ_UPVALUE,
} ObjType;

struct Obj {
    ObjType type;
    bool isMarked;  // flag to mark var as reachable, i.e. not a candidate for garbage collection
    struct Obj* next;
};

typedef struct {
    // functions are first class, so they need to be actual lox objects
    Obj obj;           // functions need the same Obj header that all objects share
    int arity;         // number of parameters the function expects
    int upvalueCount;  // upvalues are local variables defined in an enclosing function
    Chunk chunk;       // the body of the function
    ObjString* name;
} ObjFunction;

// Native function takes argcount and pointer to the first argument on the stack
// Value is the return type
// *NativeFn is a pointer to the function
typedef Value (*NativeFn)(int argCount, Value* args);

typedef struct {
    Obj obj;
    NativeFn function;  // pointer to C function that implements the behavior
} ObjNative;

/**
 * Struct to define a string object.
 * This is stored in a separate heap-allocated array so we
 * only set aside as much memory as needed for the string.
 */
struct ObjString {
    Obj obj;  // Obj is a struct itself
    int length;
    char* chars;
    uint32_t hash;  // cache the hash so we don't have to compute it every time
};

typedef struct ObjUpvalue {
    Obj obj;
    Value* location;  // closed-over local variables don't live on the stack, they're in the heap
    // the value of the upvalue will be set here when it's closed, and then
    // the location property will point to it for the VM to read.
    Value closed;
    struct ObjUpvalue* next;
} ObjUpvalue;

// wraps functions to handle variables in closures
typedef struct {
    Obj obj;
    ObjFunction* function;
    // pointer to an array of pointers, i.e. each index of the array contains pointers
    // different closures may have different numbers of upvalues, so it needs to be a dynamic array
    ObjUpvalue** upvalues;
    int upvalueCount;
} ObjClosure;

typedef struct {
    Obj obj;
    ObjString* name;
    // hash table to store class methods
    // keys are the method names and values are an ObjClosure object for the body
    Table methods;
} ObjClass;

// Object to store instance of a class
// enables binding methods/properties at runtime 
typedef struct {
    Obj obj;
    ObjClass* klass;
    Table fields;
} ObjInstance;

// Wrapper for methods to track the instance that it was accessed from
// The bound object can later be called like a function
typedef struct {
    Obj obj;
    /**
     * The receiver is the object which the method was accessed on. Ex:
     * var jane = Person();
     * jane.name = "Jane"; <--- jane is the receiver
     */
    Value receiver;
    ObjClosure* method;
} ObjBoundMethod;

ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method);
ObjClass* newClass(ObjString* name);
ObjClosure* newClosure(ObjFunction* function);

ObjFunction* newFunction();
ObjInstance* newInstance(ObjClass* klass);
ObjNative* newNative(NativeFn function);
ObjString* takeString(char* chars, int length);
ObjString* copyString(const char* chars, int length);
ObjUpvalue* newUpvalue(Value* slot);
void printObject(Value value);

/**
 * isObjType isn't declared as part of the IS_STRING macro,
 * because if we passed it something that has side effects,
 * like pop(), it would be evaluated twice.
 *
 * Ex: IS_STRING(POP())
 * Would be evaluated as: IS_OBJ(POP()) && AS_OBJ(POP())->type == type;
 * which pops 2 values off the stack
 */
static inline bool isObjType(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#endif
