#ifndef clox_table_h
#define clox_table_h

#include "common.h"
#include "value.h"

typedef struct {
    // Since the key is always a string we can store a
    // pointer instead of wrapping it in a Value object.
    ObjString* key;
    Value value;
} Entry;

// ratio of count/capacity is the load factor
typedef struct {
    int count;  // number of key/values currently stored
    int capacity;  // total size of the array
    // array of entries (strings)
    Entry* entries;
} Table;

void initTable(Table* table);
void freeTable(Table* table);
bool tableGet(Table* table, ObjString* key, Value* value);
bool tableSet(Table* table, ObjString* key, Value value);
bool tableDelete(Table* table, ObjString* key);
void tableAddAll(Table* from, Table* to);
ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash);
void tableRemoveWhite(Table* table);
void markTable(Table* table); // garbage collector helper

#endif
