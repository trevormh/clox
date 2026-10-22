#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

/**
 * sizeof(type) refers to the struct we're creating memory for,
 * not the amount of memory its fields contain, like a string.
 *
 * Ex:
 * typedef struct {
 *    Obj obj; -> some fixed size
 *    int length; -> ints are usually 4 bytes
 *    char* chars; -> char* is usually 8 bytes
 *  } ObjString;
 *
 * The string might be 100k characters, but the struct
 * only stores a pointer to the starting 8 byte character.
 */
#define ALLOCATE_OBJ(type, objectType) \
    (type*)allocateObject(sizeof(type), objectType)

static Obj* allocateObject(size_t size, ObjType type) {
    Obj* object = (Obj*)reallocate(NULL, 0, size);
    object->type = type;
    object->isMarked = false;

    object->next = vm.objects;
    vm.objects = object;

    // log how much memory we just allocated for garbage collector debugging
#ifdef DEBUG_LOG_GC
    printf("%p allocate %zu for %d\n", (void*)object, size, type);
#endif
    return object;
}

ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method) {
    ObjBoundMethod* bound = ALLOCATE_OBJ(ObjBoundMethod, OBJ_BOUND_METHOD);
    /**
     * receiver is the property accessing the method. Ex:
     * var j = Person();
     * j.name = "Jane"; // j is the receiver
     */
    bound->receiver = receiver;
    bound->method = method;
    return bound;
}

ObjClass* newClass(ObjString* name) {
    // spelled with a "k" because "class" is a reserved word in C
    ObjClass* klass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
    klass->name = name;
    initTable(&klass->methods);
    return klass;
}

ObjClosure* newClosure(ObjFunction* function) {
    // Closures can have different number of upvalues so instead of hardcoding
    // a fixed array size it's dynamically sized.
    ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*, function->upvalueCount);
    for (int i = 0; i < function->upvalueCount; i++) {
        upvalues[i] = NULL;  // initialize to NULL for the garbage collector
    }
    ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalueCount = function->upvalueCount;
    return closure;
}

ObjFunction* newFunction() {
    ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
    function->arity = 0;
    function->upvalueCount = 0;
    function->name = NULL;
    initChunk(&function->chunk);
    return function;
}

// new class instance
ObjInstance* newInstance(ObjClass* klass) {
    ObjInstance* instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
    instance->klass = klass;
    initTable(&instance->fields);  // initalize the field table to an empty hash table
    return instance;
}

ObjNative* newNative(NativeFn function) {
    // wrap the C function in a pointer to ObjNative
    ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
    native->function = function;
    return native;
}

// function to create  string objects on the heap and initialize their values
static ObjString* allocateString(char* chars, int length, uint32_t hash) {
    ObjString* string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    string->length = length;
    string->chars = chars;
    string->hash = hash;
    push(OBJ_VAL(string));
    /**
     * Every time we create a new unique string we'll "intern" it
     * We’re using the table more like a hash set than a hash table.
     * The keys are the strings and those are all we care about,
     * so we just use nil for the values.
     */
    tableSet(&vm.strings, string, NIL_VAL);
    pop();
    return string;
}

//
/**
 * algorithm is called “FNV-1a”
 * Sets an initial hash value that will be modified.
 * Walks the string one character at a time and
 * uses XOR to "mix in" the current string character
 * into the hash.
 *
 * Ex:
 * hash = 10110110 (in binary, 1 byte for simplicity)
 * key[i] = 01001101 (binary, the current character)
 * - an XOR operation is now perfomed
 * hash: 1 0 1 1 0 1 1 0
 * key:  0 1 0 0 1 1 0 1
 * ----------------------
 *       1 1 1 1 1 0 1 1
 *
 * Next multiplies the modified hash by a prime number
 * to spread out the bit changes widely.
 * Using a shorter example for brevity where hash
 * is 101 and mulitplier is 3
 * hash: 1 1 0
 * mult: 0 1 1
 * ------------
 *   c(1) <-- bit carried from 3rd col....1 + 1 + 0 = 10. carry bit over
 * c(1) <-- bit carried from 3rd col... 1 + 0 + 0 + carried bit = 10. carry bit over
 *   0 0 1 1 0
 *   0 1 1 0 0 <-- just like normal long muliplication, start 2nd column mult with 0 offset
 *  +0 0 0 0 0 <-- 3rd column starts with 0 0 offset, just like long muliplication
 * ---------------
 * 0 1 0 0 1 0
 */
static uint32_t hashString(const char* key, int length) {
    /**
     * hash is a starting value called the offset basis which prevents
     * similar short strings (or empty input) from all producing
     * similar hashes.
     * 2166136261u is an unsigned int and the global standard
     * for the FNV-1a algorithm
     */
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

/**
 * Allocates a string, the caller is responsible for managing memory.
 * Ex: concatenation has already allocated the chars being passed into
 * this function, allocateString stores that in the Object->chars field,
 * no extra copy is made.
 *
 * Different from copyString because that allocates additional space for the
 * chars and copies it there.
 *
 * The compiler will mostly use copyString, the VM uses this to perform
 * string manipulations
 */
ObjString* takeString(char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    // Check if the string was interned
    ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
    if (interned != NULL) {
        FREE_ARRAY(char, chars, length + 1);
        return interned;
    }
    return allocateString(chars, length, hash);
}

// Creates a copy of the characters passed in and returns it
// rather than assuming it can take ownership of the
// chars passed in, i.e. doesn't have to manage the memory
ObjString* copyString(const char* chars, int length) {
    uint32_t hash = hashString(chars, length);

    // Check if the string is already interned before allocating space and creating it
    // This allows us to re-use the same stings in memory to help with deduplication
    // and make comparisons easier since we're checking memory addresses instead of chars.
    ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
    if (interned != NULL) return interned;

    char* heapChars = ALLOCATE(char, length + 1);  // allocate new array on heap, +1 for null terminator
    memcpy(heapChars, chars, length);
    // We need to terminate the string ourselves because the lexeme points at a
    // range of characters inside the monolithic source string and isn’t terminated.
    heapChars[length] = '\0';
    return allocateString(heapChars, length, hash);
}

ObjUpvalue* newUpvalue(Value* slot) {
    ObjUpvalue* upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
    upvalue->closed = NIL_VAL;
    upvalue->location = slot;
    upvalue->next = NULL;  // when we allocate an upvalute it's not attached to any list yet
    return upvalue;
}

static void printFunction(ObjFunction* function) {
    // Top level code does not receive a function name even though it's treaded like it's in a function
    if (function->name == NULL) {
        printf("<script>");
        return;
    }
    printf("<fn %s>", function->name->chars);
}

// print strings and other objects
void printObject(Value value) {
    switch (OBJ_TYPE(value)) {
        case OBJ_BOUND_METHOD:
            printFunction(AS_BOUND_METHOD(value)->method->function);
            break;
        case OBJ_CLASS:
            printf("%s", AS_CLASS(value)->name->chars);
            break;
        case OBJ_CLOSURE:
            printFunction(AS_CLOSURE(value)->function);
            break;
        case OBJ_FUNCTION:
            printFunction(AS_FUNCTION(value));
            break;
        case OBJ_INSTANCE:
            printf("%s instance", AS_INSTANCE(value)->klass->name->chars);
            break;
        case OBJ_NATIVE:
            printf("<native fn>");
            break;
        case OBJ_STRING:
            printf("%s", AS_CSTRING(value));
            break;
        case OBJ_UPVALUE:
            printf("upvalue");
            break;
    }
}