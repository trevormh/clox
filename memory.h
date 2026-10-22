#ifndef clox_memory_h
#define clox_memory_h

#include "common.h"
#include "object.h"

/**
 * Macro to allocate new chunk of memory like malloc()
 * Exists so callers don't have to determine the size
 * based on the type each time.
 */
#define ALLOCATE(type, count) \
    (type*)reallocate(NULL, 0, sizeof(type) * (count))

// wrapper around reallcoate that resizes an array down to 0 bytes
#define FREE(type, pointer) reallocate(pointer, sizeof(type), 0)

/**
 * macro to calculate a new capacity based on a given current capacity
 *
 * If the capacity is less than 8 elements (not bytes or bits) then
 * use 8 otherwise double what the capacity is.
 */
#define GROW_CAPACITY(capacity) \
    ((capacity) < 8 ? 8 : (capacity) * 2)

/**
 * Macro to assist with reallocating memory
 *
 * calculates the "size of the array's element type"....the old array size and new size (in bytes)
 * casts the response to the correct type
 */
#define GROW_ARRAY(type, pointer, oldCount, newCount)     \
    (type*)reallocate(pointer, sizeof(type) * (oldCount), sizeof(type) * (newCount))

/**
 * Macro to assist with freeing memory from an array
 *
 * Passes 0 to the reallocate function for the new size, which
 * frees the memory from the provided pointer
 */
#define FREE_ARRAY(type, pointer, oldCount) \
    reallocate(pointer, sizeof(type) * (oldCount), 0)

// void* means it returns a pointer to memory of unknown type
void* reallocate(void* pointer, size_t oldSize, size_t newSize);
void markObject(Obj* object); // garbage collector helper
void markValue(Value value); // garbage collector helper
void collectGarbage();
void freeObjects();

#endif
