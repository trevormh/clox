#include <stdlib.h>

#include "compiler.h"
#include "memory.h"
#include "vm.h"

// debugging for garbage collector
#ifdef DEBUG_LOG_GC
    #include "debug.h"
    #include <stdio.h>
#endif

/**
 * Garbage collection is triggered based on the vm.nextGC size,
 * which is a threshold of how many bytes the VM has allocated.
 * Once it reaches that size a GC cycle happens.
 *
 * After collection the threshold is adjusted based on how much
 * free memory remains. This variable is used as the grow factor
 * for how much to adjust the vm.nextGC threshold
 */
#define GC_HEAP_GROW_FACTOR 2

/**
 * handles all dynamic memory management
 *
 * The two size arguments control which operation to perform
 *  old size   new size         operation
 *  ---------------------------------------
 *    0        non-zero         Allocate new block
 *  non-zero    0               Free allocation (release entire block)
 *  non-zero    < old size      Shrink existing allocation
 *  non-zero    > old size      Grow existing allocation
 */
void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
    vm.bytesAllocated += newSize - oldSize;
    /**
     * collect garbage when we call to acquire more memory.
     * This function is also called to free/shrink an allocation, so
     * guard the GC call with an if statement
     */
    if (newSize > oldSize) {
#ifdef DEBUG_STRESS_GC
        collectGarbage();
#endif
        // check if threshold of memory consumption to run GC has been reached
        if (vm.bytesAllocated > vm.nextGC) {
            collectGarbage();
        }
    }

    if (newSize == 0) {
        free(pointer);
        return NULL;
    }

    void* result = realloc(pointer, newSize);  // resize the memory block
    if (result == NULL) exit(1);               // allocation failed, ex not enough memory
    return result;
}

/**
 * garbage collector helper.
 * marking an object means it's reachable by the VM and should not
 * be collected during a GC cyle.
 */
void markObject(Obj* object) {
    /**
     * A Lox Value that is of type Obj will always have a valid pointer.
     * Some places will call this directly from code, and in those
     * places an object is optional.
     */
    if (object == NULL) return;
    /**
     * The reference graph can be cyclical
     * Skip over already marked objects so we don't re-add the same
     * gray objects and get stuck in a loop
     */
    if (object->isMarked) return;

    // log object being marked for garbage collection
    // This way we can see what the mark phase is doing
#ifdef DEBUG_LOG_GC
    printf("%p mark ", (void*)object);
    printValue(OBJ_VAL(object));
    printf("\n");
#endif

    object->isMarked = true;

    /**
     * gray objects are ones we've already traversed and know are reachable.
     * we've already marked them, but don't have an easy way to find them, so
     * we'll add them to a worklist stack to keep track of them.
     */
    if (vm.grayCapacity < vm.grayCount + 1) {
        vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
        // Use realloc instead of the reallocate() function we created to prevent
        // growing the stack during GC, causing it to recursively start a new GC cycle
        vm.grayStack = (Obj**)realloc(vm.grayStack, sizeof(Obj*) * vm.grayCapacity);
        if (vm.grayStack == NULL) exit(1);
    }

    vm.grayStack[vm.grayCount++] = object;
}

// garbage collector helper
void markValue(Value value) {
    /**
     * Check if value is an object, because some values, like numbers, Booleans and nil
     * are stored directly inline and don't require heap allocation.
     */
    if (IS_OBJ(value)) markObject(AS_OBJ(value));
}

// garbage collector helper
static void markArray(ValueArray* array) {
    for (int i = 0; i < array->count; i++) {
        markValue(array->values[i]);
    }
}

/**
 * garbage collector helper
 * black objects are ones that have been marked as reachable and processed, not to be picked up by the GC
 */
static void blackenObject(Obj* object) {
#ifdef DEBUG_LOG_GC
    printf("%p blacken ", (void*)object);
    printValue(OBJ_VAL(object));
    printf("\n");
#endif
    // Each object kind has different fields that might reference other objects,
    // so we need a specific blob of code for each type.
    // A black object is any object whose isMarked field is set and that is no longer in the gray stack.
    switch (object->type) {
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)object;
            markValue(bound->receiver);
            markObject((Obj*)bound->method);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            markObject((Obj*)klass->name);  // keep the class's string name alive
            markTable(&klass->methods);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)object;
            markObject((Obj*)closure->function);
            for (int i = 0; i < closure->upvalueCount; i++) {
                markObject((Obj*)closure->upvalues[i]);
            }
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            markObject((Obj*)function->name);
            markArray(&function->chunk.constants);  // trace the constant table
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            markObject((Obj*)instance->klass);
            markTable(&instance->fields);
            break;
        }
        case OBJ_UPVALUE:
            // the closed over upvalue is no longer on the stack
            // need to make sure we trace the references to it from the upvalue
            markValue(((ObjUpvalue*)object)->closed);
            break;
        // strings and native functions contain no references
        case OBJ_NATIVE:
        case OBJ_STRING:
            break;
    }
}

static void freeObject(Obj* object) {
    // log what memory we're freeing up for garbage collector debugging
#ifdef DEBUG_LOG_GC
    printf("%p free type %d\n", (void*)object, object->type);
#endif

    switch (object->type) {
        case OBJ_BOUND_METHOD:
            /**
             * Only free the object itself, not its properties.
             * The GC will pick them up.
             * This ensures that a handle to a method keeps the receiver
             * in memory so `this` can still find the bound object later.
             */
            FREE(ObjBoundMethod, object);
            break;
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            freeTable(&klass->methods);
            FREE(ObjClass, object);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)object;
            // free the upvalues array cthat contains pointers to the upvalues
            FREE_ARRAY(ObjUpvalue*, closure->upvalues, closure->upvalueCount);
            /**
             * Free only ObjClosure, not the function itself in the closure.
             * There could be multiple closures that all reference the
             * same function
             */
            FREE(ObjClosure, object);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            freeChunk(&function->chunk);
            FREE(ObjFunction, object);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            // The instance owns its fields table, need to also free it.
            // Don't free the entries in the table, because there  may be references
            // to those objects which the GC will take care of
            freeTable(&instance->fields);
            FREE(ObjInstance, object);
            break;
        }
        case OBJ_NATIVE:
            FREE(ObjNative, object);
            break;
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
            FREE_ARRAY(char, string->chars, string->length + 1);
            FREE(ObjString, object);
            break;
        }
        case OBJ_UPVALUE:
            FREE(ObjUpvalue, object);
            break;
    }
}

/**
 * garbage collector helper
 * roots are values the VM can access without having to traverse through other references.
 * We need to "mark" all the roots, which indicates they're reachable by the VM.
 * Later we'll traverse all the roots and mark everything else that can be reached.
 * Anything unreachable will be cleared by the GC.
 */
static void markRoots() {
    // local vars on the stack are a main source of roots
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
        markValue(*slot);
    }

    // The VM maintains a separate stack of CallFrames, each which contains a pointer
    // to the function being called. These closures need to be kept around too
    for (int i = 0; i < vm.frameCount; i++) {
        markObject((Obj*)vm.frames[i].closure);
    }

    // the open upvalue list is another set the VM can directly reach
    for (ObjUpvalue* upvalue = vm.openUpvalues; upvalue != NULL; upvalue = upvalue->next) {
        markObject((Obj*)upvalue);
    }

    // other main source of roots are global variables
    // check the globals hash table
    markTable(&vm.globals);
    // the compiler allocates memory for literals and the constants table
    // need to treat these as roots too
    markCompilerRoots();
     // Each time an instance is constructed the VM looks for the init() method.
     // the initString stores the interned string "init" for this purpose.
     // Mark it so it doesn't get picked up by the GC
    markObject((Obj*)vm.initString);
}

/**
 * garbage collector helper.
 * keep pulling out gray objects, traverse their references, then mark them black
 */
static void traceReferences() {
    while (vm.grayCount > 0) {
        Obj* object = vm.grayStack[--vm.grayCount];
        blackenObject(object);
    }
}

/**
 * garbage collector helper.
 * All reachable references have been marked as black,
 * anything remaining can be reclaimed.
 */
static void sweep() {
    Obj* previous = NULL;
    Obj* object = vm.objects;
    // walk all the objects on the heap
    while (object != NULL) {
        // Any marked objects are black
        // we want to keep these, continue onward
        if (object->isMarked) {
            object->isMarked = false;  // clear for next collection cycle
            previous = object;
            object = object->next;
        } else {
            // found an unmarked object
            Obj* unreached = object;  // unlink it
            object = object->next;
            // If we're not at the end of the list connect the previous
            // object to the next one, cutting out white object
            if (previous != NULL) {
                previous->next = object;
            } else {
                vm.objects = object;
            }

            freeObject(unreached);
        }
    }
}

void collectGarbage() {
    // log when the GC starts running
#ifdef DEBUG_LOG_GC
    printf("-- gc begin\n");
    size_t before = vm.bytesAllocated;
#endif

    markRoots();
    traceReferences();
    tableRemoveWhite(&vm.strings);
    sweep();

    /**
     * After sweep() runs the bytesAllocated has been reduced, so we
     * know how many live bytes remain.
     * Adjust the threshold of the next GC based on that.
     */
    vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;

#ifdef DEBUG_LOG_GC
    printf("-- gc end\n");
    printf("   collected %zu bytes (from %zu to %zu) next at %zu\n",
           before - vm.bytesAllocated, before, vm.bytesAllocated,
           vm.nextGC);
#endif
}

void freeObjects() {
    Obj* object = vm.objects;
    while (object != NULL) {
        Obj* next = object->next;
        freeObject(object);
        object = next;
    }
    free(vm.grayStack);
}
