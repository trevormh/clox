#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "vm.h"

VM vm;

// Native function for testing
// Returns elapsed time since the program started running, in seconds.
static Value clockNative(int argCount, Value* args) {
    return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void resetStack() {
    // move the top of the stack (stackTop pointer) back
    // to the beginning of the stack array
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
}

// Callers can pass a format string followed by a number of arguments, just like they can when calling printf()
static void runtimeError(const char* format, ...) {
    va_list args;                    // va_list stuff let us pass an arbitrary number of arguments
    va_start(args, format);          // point to the first extra argument
    vfprintf(stderr, format, args);  // read the arguments in order according to their format string and print it
    va_end(args);                    // cleanus up memory
    fputs("\n", stderr);             // write new line to stderr

    // print the stack trace
    // walk the call stack from top (the most recently called function) to bottom
    // The -1 is because the IP is already sitting on the next instruction
    for (int i = vm.frameCount - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* function = frame->closure->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    resetStack();
}

/**
 * Allow users to define native C functions.
 * Params are the name it will be called with in Lox and
 * a pointer to the C function
 */
static void defineNative(const char* name, NativeFn function) {
    /**
     * Copy the function name and wrap the function
     * in an ObjNative, then push them on the stack.
     * This informs the garbage collector they're in use.
     */
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function)));
    tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    /**
     * Pop them from the stack since the globals table now holds
     * references and the GC can safely track them.
     */
    pop();
    pop();
}

void initVM() {
    resetStack();
    vm.objects = NULL;
    vm.bytesAllocated = 0;    // GC helper, how much memory the VM has allocated
    vm.nextGC = 1024 * 1024;  // threshold of triggering a GC cycle
    // gc helper stack
    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

    // initialize hash tables
    initTable(&vm.globals);
    initTable(&vm.strings);

    // copyString can trigger a GC. If the collector ran at the wrong time
    // it would read vm.initString before initializing, so zero it out first.
    vm.initString = NULL;
    vm.initString = copyString("init", 4);

    defineNative("clock", clockNative);
}

// cleanup/destructor function, called when then program is done
void freeVM() {
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.initString = NULL;
    freeObjects();
}

void push(Value value) {
    // The stackTop points to where the next element should go
    // so all we have to do is set that value and then increment
    // to the next position.
    *vm.stackTop = value;
    vm.stackTop++;
}

Value pop() {
    // opposite of push....stackTop is currently pointing at the
    // next empty position, so first decrement the top to get the
    // value that's actually at the top, then return it
    vm.stackTop--;
    return *vm.stackTop;
}

// return value from stack but don't remove it
static Value peek(int distance) {
    // distance is how far down from the top of the stack to look, where 0 is the top
    return vm.stackTop[-1 - distance];
}

// initialize the next CallFrame on the stack
static bool call(ObjClosure* closure, int argCount) {
    if (argCount != closure->function->arity) {
        runtimeError("Expected %d arguments but got %d.", closure->function->arity, argCount);
        return false;
    }

    if (vm.frameCount == FRAMES_MAX) {
        runtimeError("Stack overflow.");  // he said it!!!
        return false;
    }

    CallFrame* frame = &vm.frames[vm.frameCount++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;  // beginning of the function's bytecode
    // Slide the window to the function's slots on the stack.
    // The function will be index 0, param 1 index 1, etc...
    frame->slots = vm.stackTop - argCount - 1;
    return true;
}

// Execute a a call for things like functions
static bool callValue(Value callee, int argCount) {
    /**
     * Check the type to prevent calling a non-function like:
     * var notAFunction = 123;
     * notAFunction();
     */
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_BOUND_METHOD: {
                // pull the raw closure out of the ObjBoundMethod object
                ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                /**
                 * The compiler sets aside stack slot 0 by declaring an empty named variable.
                 * For function calls that slot is the function being called.
                 * For methods that is the receiver. Ex:
                 * j = Person("Jane");
                 * j.getName(); // j is the receiver, bind the method "getName" to it
                 *
                 * slot 0 is located at -argCount - 1
                 * The top of the stack contains all of the arguments, and just
                 * under those is the closure of the method.
                 * The -argCount skips past the arguments and the - 1 adjusts for the fact that
                 * stackTop points just past the last used stack slot.
                 */
                vm.stackTop[-argCount - 1] = bound->receiver;
                // invoke the closure by pushing a CallFrame onto the stack
                return call(bound->method, argCount);
            }
            case OBJ_CLASS: {
                // treat as constructor call. i.e. create new instance and store on the stack
                ObjClass* klass = AS_CLASS(callee);
                vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
                Value initializer;
                // check if the class has an init method and call it if it does
                if (tableGet(&klass->methods, vm.initString, &initializer)) {
                    // This pushes a new CallFrame onto the VM's frames array.
                    // any args passed to the class will be sitting on the stack above the instance
                    return call(AS_CLOSURE(initializer), argCount);
                } else if (argCount != 0) {
                    // class doesn't have init but user provided args
                    runtimeError("Expected 0 arguments but got %d.", argCount);
                    return false;
                }

                return true;
            }
            case OBJ_CLOSURE:
                return call(AS_CLOSURE(callee), argCount);
            case OBJ_NATIVE: {
                // invoke a native C function
                NativeFn native = AS_NATIVE(callee);
                Value result = native(argCount, vm.stackTop - argCount);
                vm.stackTop -= argCount + 1;
                push(result);
                return true;
            }
            default:
                break;  // Non-callable object type.
        }
    }
    runtimeError("Can only call functions and classes.");
    return false;
}

/**
 * Helper function for calling a method in one step, i.e.
 * var a = some_instance.my_method();
 *
 * This function combines the logic of how the VM implements
 * OP_GET_PROPERTY and OP_CALL instructions, in that order.
 */
static bool invokeFromClass(ObjClass* klass, ObjString* name, int argCount) {
    Value method;
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("Undefined property '%s'.", name->chars);
        return false;
    }
    // push a call to it on the vm's CallFrame stack
    return call(AS_CLOSURE(method), argCount);
}

/**
 * Invoke a method
 */
static bool invoke(ObjString* name, int argCount) {
    /**
     * receivers are objects which access the method
     * var brunch = Brunch();
     * var eggs = brunch.eggs; // brunch is the receiver
     */
    Value receiver = peek(argCount);  // method arguments are above it on the stack, peek that many slots
    if (!IS_INSTANCE(receiver)) {
        runtimeError("Only instances have methods.");
        return false;
    }
    ObjInstance* instance = AS_INSTANCE(receiver);
    /**
     * A method could declare an inner function and then
     * set it as an instance property to later be called.
     * Check if it's actually a method or if it's a field.
     */
    Value value;
    if (tableGet(&instance->fields, name, &value)) {
        // Store the field on the stack in place of the receiver,
        // which is under the argument list.
        vm.stackTop[-argCount - 1] = value;
        return callValue(value, argCount);
    }

    return invokeFromClass(instance->klass, name, argCount);
}

/**
 * receive's an instance's class and a property name to look up.
 * If the method is found it's placed on the stack and this returns true.
 */
static bool bindMethod(ObjClass* klass, ObjString* name) {
    Value method;
    // check the class's method table for the method
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("Undefined property '%s'.", name->chars);
        return false;
    }
    // grab the receiver from the top of the stack wrap and wrap the method in a bound object
    Value receiver = peek(0);
    ObjBoundMethod* bound = newBoundMethod(receiver, AS_CLOSURE(method));
    // The instance is now on top of the stack
    // Replace it with the bound object by popping hte instance and pushing thebound object
    pop();
    push(OBJ_VAL(bound));
    return true;
}

/**
 * This function handles creating new upvalues, but before doing that
 * we look for an existing one in the vm.openUpvalues linked list.
 *
 * This function creates a structure like this
 *

   vm.openUpvalues =   a <------ b <------ c
                       |         |         |
                       V         V         V
    VM stack =        [a:1, f(), b:2, g(), c:3, h() ]

 * each upvalue's next property points to the next upvalue, and its location property points to the VM stack
 */
static ObjUpvalue* captureUpvalue(Value* local) {
    ObjUpvalue* prevUpvalue = NULL;
    /**
     * start at the head of the list, which is the the upvalue closes to the top of the stack.
     * In the comment above, that would be c
     */
    ObjUpvalue* upvalue = vm.openUpvalues;
    /**
     * upvalue->location (of type Value*) points to the index on the stack where the
     * closed over variable lives.
     * local is the vm slot index of the upvalue we're trying to find.
     *
     */
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        /**
         * if upvalue->next is null then we reached the first inserted item (end of the list)
         * Ex: a <- b <- c
         * a was the first upvalue inserted so a->next is null
         */
        upvalue = upvalue->next;
    }
    // found an existing upvalue, no need to create a new one
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue* createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;

    if (prevUpvalue == NULL) {
        vm.openUpvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }

    return createdUpvalue;
}

/**
 * move value from stack to heap.
 * Upvalues are of type ObjUpvalue, which have location, next and closed properties.
 *
 * The location of an open upvalue points to where the var lives on the stack.
 *
 * We set the closed property to the same value that's on the stack,
 * then point the location to that closed property (in the heap)
 *
 * This function will close every open upvalue it can find that points to the
 * slot or any slot above it on the stack
 */
static void closeUpvalues(Value* last) {
    // walk list of open upvalues from the top of the stack to bottom
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
        ObjUpvalue* upvalue = vm.openUpvalues;
        // upvalue->location used to point to the stack.
        // copy the variable to closed into the upvalue's closed property.
        // this is how we're putting them onto the heap.
        upvalue->closed = *upvalue->location;
        // by updating the location to the closed property, the VM will now read directly
        // from the heap instead of the stack where it used to live and we don't have
        // to change the VM's functionality for the GET and SET upvalue instructions.
        upvalue->location = &upvalue->closed;
        vm.openUpvalues = upvalue->next;
    }
}

static void defineMethod(ObjString* name) {
    Value method = peek(0);                   // get method closure from top of the stack
    ObjClass* klass = AS_CLASS(peek(1));      // the class the method will be bound to is under it on the stack
    tableSet(&klass->methods, name, method);  // store the closure in the class's method table
    pop();                                    // pop the closure, leave the class to process additional methods
}

// nil and false are falsey, every other value behaves like true
static bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
    /**
     * use peek() to get the values instead of pop so they're
     * still on the stack and prevented from being picked up by the GC
     */
    ObjString* b = AS_STRING(peek(0));
    ObjString* a = AS_STRING(peek(1));

    int length = a->length + b->length;
    char* chars = ALLOCATE(char, length + 1);
    // 2 separate copy operations
    // first copy string a to the new memory we allocated
    memcpy(chars, a->chars, a->length);
    // now copy string b
    char* end_of_a = chars + a->length;
    memcpy(end_of_a, b->chars, b->length);
    chars[length] = '\0';

    ObjString* result = takeString(chars, length);
    /**
     * Now pop off the values we set earlier for b and a from the stack
     * so they can be collected by the GC
     */
    pop();
    pop();
    push(OBJ_VAL(result));
}

static InterpretResult run() {
    // The first CallFrame to use is the topmost frame from the stack.
    // The frame itself is an array that contains locals, and other
    // statements.
    CallFrame* frame = &vm.frames[vm.frameCount - 1];

// read the current instruction and move onto the next one
#define READ_BYTE() (*frame->ip++)

/**
 * Helper to read 16 bit operands (2 places in bytesream)
 * Ex: if the number 2300 was in the bytestream, its binary
 * value is 00001000 11110100
 *
 * First move the ip past the 2 bytes
 *
 * Next perform bitshift on the high byte (vm.ip[-2] << 8)
 * 00001000 becomes 00001000 00000000 (Decimal 2048)
 *
 * Next combine with the low byte (| vm.ip[-1])
 * Low byte is 11111100
 * The | operator merges 00001000 with 00001000 00000000 from the
 * previous caluclation
 *  00000000 11111100 <- low byte
 * |00001000 00000000 <- result from vm.ip[-2] << 8
 * -----------------
 *  00001000 11111100
 */
#define READ_SHORT() \
    (frame->ip += 2, \
     (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

/*
When this is called the next byte should be the index of the operand (constant).
The call to READ_BYTE() returns it and then it's fetched from the constants pool.
*/
#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])

// helper value to read strings from the constants pool
#define READ_STRING() AS_STRING(READ_CONSTANT())

/**
 * Macro to perform binary operation. Ex: 3+4, 5*2, etc...
 * Note: pay attention to order of the values popped. Ex: If we want 3-1,
 * 1 will be on the top of the stack so the "second" variable is declared first.
 *
 * C switch statements require a block scope (ex: case OP_ADD: {....})
 * to be able to make multiple declarations all in the same scope.
 * The BINARY_OP instructions do not use a block scope. ex: case OP_ADD: BINARY_OP(+); break;
 * To get around this we can use a do while loop which creates the block scope.
 * The while(false) means it only runs once, so it doesn't actually loop.
 */
#define BINARY_OP(valueType, op)                          \
    do {                                                  \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            runtimeError("Operands must be numbers.");    \
            return INTERPRET_RUNTIME_ERROR;               \
        }                                                 \
        double b = AS_NUMBER(pop());                      \
        double a = AS_NUMBER(pop());                      \
        push(valueType(a op b));                          \
    } while (false)

    for (;;) {

#ifdef DEBUG_TRACE_EXECUTION
        // show the current contents of the stack before interpreting each instruction
        // print out each value in the array starting at the bottom
        printf("          ");
        for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
            printf("[ ");
            printValue(*slot);
            printf(" ]");
        }
        printf("\n");
        /**
         * frame->ip is the current instruction, &frame->function->chunk is the starting index
         *
         * The disassembler requires the index (offset) of the opcode to print, but
         * since the vm uses pointers to track the current instruction (ip) it
         * needs to be converted back to an offset by subtracting the starting
         * "index" from the current "index"
         */
        disassembleInstruction(&frame->closure->function->chunk,
                               (int)(frame->ip - frame->closure->function->chunk.code));
#endif
        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_NIL: push(NIL_VAL); break;
            case OP_TRUE: push(BOOL_VAL(true)); break;
            case OP_FALSE: push(BOOL_VAL(false)); break;
            case OP_POP: pop(); break;
            case OP_GET_LOCAL: {
                /**
                 * Reminder: slots is just a subset of Vm.stack, not
                 * a completely different stack.
                 *
                 * The compiler put an index in the bytestream where
                 * we can find the local in the current frame's stack.
                 *
                 * Get it and push that value on the top of the vm's stack
                 * so the current expression can use it.
                 */
                uint8_t slot = READ_BYTE();
                push(frame->slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                /**
                 * Reminder: slots is just a subset of Vm.stack, not
                 * a completely different stack.
                 *
                 * The compiler put an index in the bytestream where
                 * we can find the local that we need to update.
                 *
                 * Use peek to copy the value from the top of the VM's stack
                 * and update the local value in slots.
                 */
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = peek(0);
                break;
            }
            case OP_GET_GLOBAL: {
                // Look up a global variable by name and add its value to the stack.

                // The compiler added the variable's name to the constant pool
                // and put its index in the bytestream.
                // READ_STRING performs the opposite...takes that index and
                // gets the name from the constant's pool.
                ObjString* name = READ_STRING();
                Value value;
                // look up the variable name in the globals hash table to get its value.
                if (!tableGet(&vm.globals, name, &value)) {
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                /**
                 * The compiler adds global var names to the constants table
                 * and puts the index in the bytestream after the
                 * OP_DEFINE_GLOBAL instruction.
                 * READ_STRING looks up the index in the constants table.
                 */
                ObjString* name = READ_STRING();
                // Now add the global var name to the globals hash table.
                tableSet(&vm.globals, name, peek(0));
                // we don't pop until after it's been added to the
                // hash table in case the GC picks it up first.
                pop();
                break;
            }
            case OP_SET_GLOBAL: {
                // Look up a global variable by name and assign it a value.
                // This is for re-assigning existing variables, not
                // declaring new ones. i.e. x = 5 instead of var x = 5

                ObjString* name = READ_STRING();  // get variable name from constant pool
                // update its hash table value
                if (tableSet(&vm.globals, name, peek(0))) {
                    // tableSet returns true if it's a new key, which means if this
                    // is reached it hasn't been defined yet.
                    tableDelete(&vm.globals, name);
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_GET_UPVALUE: {
                // the operand of OP_GET_UPVALUE is the index of the current function's upvalues array
                uint8_t slot = READ_BYTE();
                push(*frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                // the operand of OP_SET_UPVALUE is the index where the value should be stored
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(0);
                break;
            }
            case OP_GET_PROPERTY: {
                /**
                 * Verify the user didn't try to evaluate something that
                 * isn't a class isntance (type ObjInstance). For example, this is not valid:
                 * var obj = "not an instance";
                 */
                if (!IS_INSTANCE(peek(0))) {
                    runtimeError("Only instances have properties.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                /**
                 * when the interpreter reaches this instruction, the expression to
                 * the left of the dot has already been executed and the resulting
                 * class instance is on top of the stack.
                 * Ex: bagel.topping -> bagel is isntance on top of the stack
                 */
                ObjInstance* instance = AS_INSTANCE(peek(0));  // get class isntance from stack
                ObjString* fieldName = READ_STRING();          // read field name from the constants pool

                Value value;
                if (tableGet(&instance->fields, fieldName, &value)) {
                    pop();  // Instance.
                    push(value);
                    break;
                }

                /**
                 * Look up field on the receiver instance. Ex:
                 * j = Persion();
                 * j.name = "Jane"; // j is the receiver, need to look up name on it
                 */
                if (!bindMethod(instance->klass, fieldName)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_PROPERTY: {
                // make sure this is a class instance. Ex: not var "something".test = 123
                if (!IS_INSTANCE(peek(1))) {
                    runtimeError("Only instances have fields.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                /**
                 * When this evaluates at the top of the stack is the instance whose
                 * field is being evaluated, and above that is the value being set.
                 *
                 * Ex:
                 * class Toast {}
                 * var toast = Toast();
                 * print toast.jam = "grape"; // Prints "grape".
                 *
                 * toast is the instance, jam is the operand in the bytestream
                 * and "grape" is on top of the stack.
                 *
                 * We need to pop "grape" off the stack, pop the toast instance
                 * and then push grape back on. In other words, we remove the
                 * second element from the stack while leaving the top alone.
                 *
                 */
                ObjInstance* instance = AS_INSTANCE(peek(1));
                // set the field name from the bytestream (jam in the example)
                tableSet(&instance->fields, READ_STRING(), peek(0));
                Value value = pop();  // pop the stored value off (grape in the example)
                pop();                // pop the instance
                push(value);          // push the value back on
                break;
            }
            case OP_GET_SUPER: {
                ObjString* parent_method = READ_STRING();
                ObjClass* superclass = AS_CLASS(pop());

                /**
                 * bindMethod looks up the method in the method's table and creates
                 * an ObjBoundMethod to add the closuer to the current instance.
                 */
                if (!bindMethod(superclass, parent_method)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_EQUAL: {
                Value b = pop();
                Value a = pop();
                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_GREATER: BINARY_OP(BOOL_VAL, >); break;
            case OP_LESS: BINARY_OP(BOOL_VAL, <); break;
            case OP_ADD: {
                if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
                    concatenate();
                } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } else {
                    runtimeError("Operands must be two numbers or two strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
            case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
            case OP_DIVIDE: BINARY_OP(NUMBER_VAL, /); break;
            case OP_NOT: {
                // Get the lox value from the stack, check its falsy-ness
                // cast it back to a lox bool value and push it back on the stack
                Value v = pop();
                bool is_falsy = isFalsey(v);
                Value lox_bool_val = BOOL_VAL(is_falsy);
                push(lox_bool_val);
                break;
            }
            case OP_NEGATE: {
                if (!IS_NUMBER(peek(0))) {
                    runtimeError("Operand must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                double v = -AS_NUMBER(pop());  // need to cast to naive C value to negate it
                push(NUMBER_VAL(v));           // convert back to lox val and push back on the stack
                break;
            }
            case OP_PRINT: {
                printValue(pop());
                printf("\n");
                break;
            }
            case OP_JUMP: {
                // jump over else blocks....
                // Get the offset of how many bytes to skip over the "else" block
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                // Get the offset of how many bytes to skip if the condition evalutes to false
                // so we can skip the if statement body.
                uint16_t offset = READ_SHORT();
                // check the top of the stack to see whether the if condition is true or false
                // if false, skip over the how many bytes are specified by the next 2 bytes in the stream
                if (isFalsey(peek(0))) frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                // Instructs the VM to go back to a previous bytecode instruction
                // where the condition of a loop can be re-evaluated after an
                // iteration has completed
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                break;
            }
            case OP_CALL: {
                /**
                 * The args will be on the top of the stack with the function preceeding them.
                 * This means we can figure out where the function is on the stack
                 * by doing something like:
                 * function = top_of_stack - argCount - 1
                 *
                 * peek(argCount) performs that step of looking "into" the stack for the function
                 */
                int argCount = READ_BYTE();
                if (!callValue(peek(argCount), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                /**
                 * When callValue is successful there will be a new frame
                 * on the stack for the called function.
                 * Need to update the frame to point to it.
                 */
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            case OP_INVOKE: {
                /**
                 * Invoke instructions are used for calling a method directly after
                 * accessing it instead of in 2 steps.
                 * Instead of:
                 * var a = my_instance.my_method;
                 * a();
                 *
                 * do it in one step
                 * var a = my_instance.my_method();
                 */
                ObjString* method = READ_STRING();
                int argCount = READ_BYTE();
                if (!invoke(method, argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                // There will bea  new CallFrame on the stack, point the current frame to it
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            case OP_SUPER_INVOKE: {
                /**
                 * OP_SUPER_INVOKE is used when a superclass method is accessed
                 * and immediately called. Ex:
                 * super.parent_method("test");
                 * 
                 * instead of
                 * 
                 * var parent = super.parent_method;
                 * parent("test");
                 * 
                 * OP_SUPER_INVOKE essentially combines OP_GET_SUPER
                 * and OP_INVOKE.
                 */
                ObjString* method = READ_STRING();
                int argCount = READ_BYTE();
                ObjClass* superclass = AS_CLASS(pop());
                if (!invokeFromClass(superclass, method, argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            case OP_CLOSURE: {
                // get function from constants table and wrap it in a closure object
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure* closure = newClosure(function);
                push(OBJ_VAL(closure));
                // populate the upvalues array with the operands in the byte stream
                for (int i = 0; i < closure->upvalueCount; i++) {
                    // all upvalues are variables defined in an enclosing scope.
                    // when isLocal is 1 that means the variable was defined directly in the enclosing scope's local variables array.
                    // if isLocal is 0 that means it's also an upvalue in the enclosing scope
                    uint8_t isLocal = READ_BYTE();
                    // the next byte tells us which index that variable is located in (in either the locals or upvalues arrays)
                    uint8_t index = READ_BYTE();
                    if (isLocal) {
                        // frame->slots points to slot 0, add the index as the offset to get the slot we want to capture
                        Value* local = frame->slots + index;
                        closure->upvalues[i] = captureUpvalue(local);
                    } else {
                        // An OP_CLOSURE instruction is emitted at the end of a function declaration, which means
                        // when we're exiting that declartion, the current function is the surrounding one.
                        // That means the current function's closure is at the top of the call stack
                        // and the enclosing function already captured the upvalue.
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }
            // move local variable onto the heap
            // when we reach this instruction the variable we'rehosting is on top of the stack
            case OP_CLOSE_UPVALUE:
                closeUpvalues(vm.stackTop - 1);
                pop();
                break;
            case OP_RETURN: {
                // when a function returns a value it will be on top of the stack
                Value result = pop();  // get value from top of stack
                /**
                 * Pass the first slot of the function's stack window so we close
                 * every remaining open upvalue by the returning function.
                 */
                closeUpvalues(frame->slots);
                vm.frameCount--;

                // If that was the very last CallFrame, it means we’ve finished executing
                // the top-level code. The entire program is done.
                if (vm.frameCount == 0) {
                    pop();
                    return INTERPRET_OK;
                }

                /**
                 * slide the window back the caller.
                 *
                 * vm.stackTop points to the next free slot in the stack.
                 * frame->slots points to the start of the current function’s stack window.
                 *
                 * Setting the stackTop to the start of the current frame via frame->slots
                 * tells the VM the next free slot is back in the caller's window now, since
                 * it always points to the next available slot. It was previous index 0 of
                 * the callee's window, now it's just past the caller's window.
                 */
                vm.stackTop = frame->slots;
                push(result);  // push the return value from the previous function back onto the stack at the new location
                // vm.frameCount - 1 is the caller's frame index, switch the frame back to it
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            case OP_CLASS:
                // load string for class's name from constant table and pass to new class
                push(OBJ_VAL(newClass(READ_STRING())));
                break;
            case OP_INHERIT: {
                // The superclass and subclass are already on the stack from the compiler
                // emitting 2 OP_GET_GLOBAL instructions
                Value superclass = peek(1);
                // prevent inheritance for objects that aren't classes
                if (!IS_CLASS(superclass)) {
                    runtimeError("Superclass must be a class.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass* subclass = AS_CLASS(peek(0));

                // copy the super class's methods to the subclass
                tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
                pop();  // Subclass.
                break;
            }
            case OP_METHOD:
                defineMethod(READ_STRING());
                break;
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

InterpretResult interpret(const char* source) {
    /**
     * Pass the source code to the compiler, which returns a new ObjFunction.
     * The compiler creates chunks for each function
     */
    ObjFunction* function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    /**
     * store the function on the VM's stack.
     * The frame's slots array will be initialized to point to this.
     */
    push(OBJ_VAL(function));

    // Wrap the function returned by the compiler in a closure
    ObjClosure* closure = newClosure(function);
    // Some weird handling for the garbage collector
    pop();  // pop the function from the stack that we just added
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}
