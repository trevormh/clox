/**
 * The flow of data goes something like:
 *
 * 1. main() - initializes VM and call either repl or readfile functions to get code from the user.
 * 
 * 2. repl()/readFile() - pass user's code to the interpret function in the VM.
 * 
 * 3. vm/interpret() - initializes a chunk, passes it and the source code to compile() in the compiler.
 * 
 * 4. compiler/compile() - initializes the scanner w/ the source code, compiles and writes bytecode to the chunk. Compiler returns bool back to interpret() indicating success/failure
 * 
 * 5. vm/interpret() - control returns from compiler, calls run() function to execute the bytecode
 * 
 * 6. vm/run() - executes the bytecode, returns InterpretResult to indicate if it ran successfully or there was an error.
 * 
 * 7. vm/interpret() - Returns InterpretResult response from run(), giving control back to repl()/readFile()
 * 
 * 8. main/repl()/readFile() - returns control back to main
 * 
 * main() - frees up VM, exit program
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "debug.h"
#include "vm.h"

static void repl() {
    char line[1024];
    for (;;) {
        printf("> ");

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        interpret(line);
    }
}

static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    // Handle errors, file might not exist or permission issues
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    // first determine how large the file is so we know how much memory we need to allocate
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    // Next allocate enough memory
    // Files on disk do not contain null terminators (the \0 at the end)
    // so we need to add an extra byte
    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }

    // read the file's contents
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }
    buffer[bytesRead] = '\0';  // add null terminator to the end

    fclose(file);
    return buffer;
}

static void runFile(const char* path) {
    char* source = readFile(path);
    InterpretResult result = interpret(source);
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) exit(65);
    if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

int main(int argc, const char* argv[]) {
    initVM();

    if (argc == 1) {
        repl();
    } else if (argc == 2) {
        runFile(argv[1]);
    } else {
        fprintf(stderr, "Usage: clox [path]\n");
        exit(64);
    }

    freeVM();
    return 0;
}