#include <stdlib.h>

#include "chunk.h"
#include "memory.h"
#include "vm.h"

void initChunk(Chunk* chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  // constants is a struct of type ValueArray
  // initValueArray function defined in value.h
  initValueArray(&chunk->constants);
}

/**
 * deallocate all the memory from a chunk
 * and zero out the fields
 */
void freeChunk(Chunk* chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  FREE_ARRAY(int, chunk->lines, chunk->capacity);
  freeValueArray(&chunk->constants);
  initChunk(chunk);
}

/**
 * Append a byte to the end of a chunk
 * 
 * chunk - a sequence of bytecode
 * byte - each instruction has a 1-byte operation code
 */
void writeChunk(Chunk* chunk, uint8_t byte, int line) {
    // If we don't have capacity for the new byte then grow the array
    if (chunk->capacity < chunk->count + 1) {
      int oldCapacity = chunk->capacity;
      chunk->capacity = GROW_CAPACITY(oldCapacity);
      // args: type, pointer to array, oldcount, new count
      chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);
      chunk->lines = GROW_ARRAY(int, chunk->lines, oldCapacity, chunk->capacity);
    }
  
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

/**
 * Add value to the constants pool array 
 * and return its index
 */
int addConstant(Chunk* chunk, Value value) {
  push(value);
  // constants is a ValueArray struct (defined in value.h)
  writeValueArray(&chunk->constants, value);
  pop();
  // return index where the constant was appended so we can locate it later
  return chunk->constants.count - 1;
}
