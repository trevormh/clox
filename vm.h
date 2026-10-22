#ifndef clox_vm_h
#define clox_vm_h

#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64                         // frames represent a function call, max number of nested function calls
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)  // 16384....the stack stores more than just function calls, it has locals, temporaries, arguments

/**
 * Represents a single ongoing function call and each time
 * a function is called a new one of these is created.
 *
 * Need to track where on the VM's stack a functions locals begin
 * and where the caller should resume.
 *
 * Each time a function is called one of these structs is created
 */
typedef struct {
    ObjClosure* closure;  // pointer to the closure that contains the function being called. used to look up constants and other things
    uint8_t* ip;          // ip of the caller's frame that the VM will jump to when the function returns
    /**
     * Slots is where we store a function's local values, and
     * it's a subset of the Vm.stack
     *
     * Using a ruler as analogy, inches 0-12 are the VM.stack and
     * slots might only be inches 6-8 within that same chunk for
     * a function's locals.
     */
    Value* slots;
} CallFrame;

typedef struct {
    CallFrame frames[FRAMES_MAX];  // frames are used to track function invocations.
    int frameCount;                // tracks the frame height - the number of ongoing function calls

    Value stack[STACK_MAX];  // max size of the stack is 16384 elements
    /**
     * The pointer for stackTop points to one index past the top element in the stack.
     * This allows us to point at index 0 when it's empty, and when there's
     * one item in the stack it would be at index 1.
     * Otherwise if it was empty we'd need to point at element -1, which isn't valid.
     * ....basically it points where the next value will go.
     */
    Value* stackTop;
    // hash table for storing global variables.
    Table globals;
    /**
     * list of "interned" strings, which is a collection of strings
     * that are guaranteed to be distinct. This enables us to dedupe
     * multiple of the same strings from memory and simplifies comparison
     * logic because we can compare memory addresses instead of values.
     * If two strings point to different addresses, they're distinct.
     */
    Table strings;
    // Each time an instance is constructed the VM looks for the init() method.
    // The VM has to locate the init method which is done via hash table.
    // The hashtable lookup is type ObjString*, so we can't just use the string
    // "init" because it's type char*.
    // To avoid casting every single time "init" will be interned once and re-used.
    ObjString* initString;
    ObjUpvalue* openUpvalues;  // VM owns the list of upvalues, so the head pointer goes here
    size_t bytesAllocated;  // track how much memory the VM has allocated for GC purposes
    size_t nextGC; // threshold that triggers the next GC cycle
    // Pointer to head of the list of Objects, the allocated heap objects.
    // Each newly allocated object is added here so we can track reachable objects
    // Used to free memory from unused objects.
    Obj* objects;
    /**
     * garbage collector helper
     * gray objects are those the GC has marked as reachable (i.e. don't remove), but
     * have not been fully processed.
     * We use a stack to keep track of them and manually manage the memory so we don't
     * kick off a GC cycle already in a GC cycle.
     */
    int grayCount;
    int grayCapacity;
    Obj** grayStack;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

// The “object” module is directly using the global vm variable from
// the “vm” module, so we need to expose that externally.
extern VM vm;

void initVM();
void freeVM();
InterpretResult interpret(const char* source);
void push(Value value);
Value pop();

#endif
