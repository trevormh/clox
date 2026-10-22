#include "value.h"
#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"

void initValueArray(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void writeValueArray(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = GROW_CAPACITY(oldCapacity);
        array->values = GROW_ARRAY(Value, array->values, oldCapacity, array->capacity);
    }

    array->values[array->count] = value;
    array->count++;
}

void freeValueArray(ValueArray* array) {
    FREE_ARRAY(Value, array->values, array->capacity);
    initValueArray(array);
}

void printValue(Value value) {
#ifdef NAN_BOXING
    if (IS_BOOL(value)) {
        printf(AS_BOOL(value) ? "true" : "false");
    } else if (IS_NIL(value)) {
        printf("nil");
    } else if (IS_NUMBER(value)) {
        printf("%g", AS_NUMBER(value));
    } else if (IS_OBJ(value)) {
        printObject(value);
    }
#else
    switch (value.type) {
        case VAL_BOOL:
            printf(AS_BOOL(value) ? "true" : "false");
            break;
        case VAL_NIL: printf("nil"); break;
        case VAL_NUMBER: printf("%g", AS_NUMBER(value)); break;
        case VAL_OBJ: printObject(value); break;  // strings and other objecs
    }
#endif
}

bool valuesEqual(Value a, Value b) {
#ifdef NAN_BOXING
    // IEEE 754 mandates that NaN values are not equal to themselves
    // convert them back to numbers and let the C compiler handle generating proper instructions for the CPU
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        return AS_NUMBER(a) == AS_NUMBER(b);
    }
    return a == b;
#else
    if (a.type != b.type) return false;  // if the Values have different types, they are definitely not equal
    /**
     * Use a switch statement instead of memcmp because padding and
     * different-sized union fields, a Value contains unused bits.
     * It's possible 2 equal Values actually differ in memory that isn't used.
     *
     * Reminder: Value is a union struct, which allow us to define a struct
     * with overlapping memory, so the size of it is the size of its largest field.
     * A Value struct initliazed with a boolean will occupy the same amount of
     * memory as one initialized with a number.
     * This means that boolean will have more unused bits than number, so
     * it could have different values in those unused bits.
     */
    switch (a.type) {
        case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
        case VAL_NIL: return true;
        case VAL_NUMBER: return AS_NUMBER(a) == AS_NUMBER(b);
        // case VAL_OBJ: {
        //     ObjString* aString = AS_STRING(a);
        //     ObjString* bString = AS_STRING(b);
        //     bool string_lengths_equal = aString->length == bString->length;
        //     bool string_vals_equal = memcmp(aString->chars, bString->chars, aString->length) == 0;
        //     return string_lengths_equal && string_vals_equal;
        // }
        /**
         * After implementing string interning we can now take advantage
         * of the fact that two strings at different memory addresses
         * must have different contents.
         */
        case VAL_OBJ: return AS_OBJ(a) == AS_OBJ(b);
        default: return false;  // Unreachable.
    }
#endif
}
