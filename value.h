#ifndef clox_value_h
#define clox_value_h

#include "common.h"
#include <string.h>

// Obj is the type for a struct that contains the state shared across all object types.
// Sort of like a "base class" for objects.
typedef struct Obj Obj;
typedef struct ObjString ObjString;  // payload for strings

#ifdef NAN_BOXING

    #define SIGN_BIT ((uint64_t)0x8000000000000000)
    #define QNAN ((uint64_t)0x7ffc000000000000)  // quiet NaN bits are those whose exponent bits + highest mantissa are set to 1

    // these 3 will occupy the 2 lowest mantissa bits
    #define TAG_NIL 1    // 01.
    #define TAG_FALSE 2  // 10.
    #define TAG_TRUE 3   // 11.

typedef uint64_t Value;

    #define IS_NIL(value) ((value) == NIL_VAL)

    // macro to check if a number is a double
    // every Value that is not a number will use a quiet NaN representation
    #define IS_NUMBER(value) (((value) & QNAN) != QNAN)
    #define IS_OBJ(value) \
        (((value) & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))

    #define AS_BOOL(value) ((value) == TRUE_VAL)  // lox treats all non-zero values as True
    #define AS_NUMBER(value) valueToNum(value)
    #define AS_OBJ(value) \
        ((Obj*)(uintptr_t)((value) & ~(SIGN_BIT | QNAN)))

    #define BOOL_VAL(b) ((b) ? TRUE_VAL : FALSE_VAL)
    #define FALSE_VAL ((Value)(uint64_t)(QNAN | TAG_FALSE))
    #define TRUE_VAL ((Value)(uint64_t)(QNAN | TAG_TRUE))
    #define NIL_VAL ((Value)(uint64_t)(QNAN | TAG_NIL))
    #define NUMBER_VAL(num) numToValue(num)
    #define OBJ_VAL(obj) \
        (Value)(SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(obj))

static inline double valueToNum(Value value) {
    double num;
    memcpy(&num, &value, sizeof(Value));
    return num;
}

static inline Value numToValue(double num) {
    Value value;
    memcpy(&value, &num, sizeof(double));
    return value;
}

#else

typedef enum {
    VAL_BOOL,
    VAL_NIL,
    VAL_NUMBER,
    VAL_OBJ  // used for string representation
} ValueType;

/**
 * In order to represent our data types for the VM to understand
 * we need to store the type of value, and the value itself.
 * We can use a tagged union for this, which is a type tag
 * and a payload for the value itself.
 *
 * We could use a struct with fields for each type, but it's
 * a waste of memory since a value can't be simultaneously
 * a double and a bool.
 *
 * Unions allow us to define a struct with overlapping memory
 * so the size of it is the size of its largest field.
 * i.e. a Value struct initialized with a boolean "as" value
 * will occupy the same amount of memory as one with a number value.
 */
typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj* obj;  // When a Value's type is VAL_OBJ the payload is a pointer to the heap memory
    } as;
} Value;

    /**
     * Before we can use the AS_ macros below, we need to know the
     * value contains the appropriate type.
     * Ex:
     * Value my_val = BOOL_VAL(true);  // initialize Value struct with Value.type = bool and Value.as = true
     * double number = AS_NUMBER(my_val);  // read number....it doesn't exist. We set it as type bool, and only one type can exist per instance
     *
     * These macros return true if the Value instance we're accessing has that type.
     * Anytime we call the AS_macros we need to guard it behind one of these calls first.
     */
    #define IS_BOOL(value) ((value).type == VAL_BOOL)
    #define IS_NIL(value) ((value).type == VAL_NIL)
    #define IS_NUMBER(value) ((value).type == VAL_NUMBER)
    #define IS_OBJ(value) ((value).type == VAL_OBJ)

    // Given a Value (the struct) of the right type, unwrap and return it as a raw C value.
    // Basically "casting" a lox value to a natice C value.
    #define AS_OBJ(value) ((value).as.obj)
    #define AS_BOOL(value) ((value).as.boolean)
    #define AS_NUMBER(value) ((value).as.number)

    // Macros take a C value of the appropriate type and produces a Value (struct) that
    // has the correct type tag and contains the underlying value.
    // Basically "casting" a native C value to a clox Value (struct)
    #define BOOL_VAL(value) ((Value){VAL_BOOL, {.boolean = value}})
    #define NIL_VAL ((Value){VAL_NIL, {.number = 0}})
    #define NUMBER_VAL(value) ((Value){VAL_NUMBER, {.number = value}})
    #define OBJ_VAL(object) ((Value){VAL_OBJ, {.obj = (Obj*)object}})

// end of else block for not using NaN boxing
#endif

/**
 * For small fixed-size values like integers, many instruction sets
 * store the value directly in the code stream right after the opcode.
 * These are called immediate instructions, but that doesn't work well
 * for large or variable-sized constants like strings.
 *
 * These large values will be stored in a chunk's constant
 * pool, which is a pointer to this struct.
 * The bytestream will receive the ndex where its value can be found.
 */
typedef struct {
    int capacity;
    int count;
    Value* values;
} ValueArray;

bool valuesEqual(Value a, Value b);
void initValueArray(ValueArray* array);
void writeValueArray(ValueArray* array, Value value);
void freeValueArray(ValueArray* array);
void printValue(Value value);

#endif
