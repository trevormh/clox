#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"

/**
 * We don't grow the hash table capacity when it's completely full,
 * instead grow it based on the load factor.
 * Load factor = # entries / capacity, i.e. 75% full
 *
 * We consider tombstones to be full buckets to prevent
 * the risk of ending up with no actual empty buckets
 * to terminate a lookup, which would result in an infinite loop.
 */
#define TABLE_MAX_LOAD 0.75

void initTable(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void freeTable(Table* table) {
    FREE_ARRAY(Entry, table->entries, table->capacity);
    initTable(table);
}

/**
 * Core function of the hash table, responsible for taking a key and
 * array of buckets and figuring out which bucket it belongs in.
 */
static Entry* findEntry(Entry* entries, int capacity, ObjString* key) {
    /**
     * Original implementation:
     * Use modulo to map the key's hash to an index within the array's bounds.
     * key->hash is a 32 bit unsigned int, so it's pretty much guaranteed to
     * be greater than capacity. That would be 4,294,967,295 indices
     * or 64 GB...(16 bytes per entry * 4,294,967,295)
     * 
     * Updated implementation:
     * Instead of using the modulo operator (%), which is slow, we can calculate
     * it for any power of two by AND-ing it with that power of two minus 1.
     * https://craftinginterpreters.com/optimization.html#slow-key-wrapping
     */
    uint32_t index = key->hash & (capacity - 1);
    Entry* tombstone = NULL;

    for (;;) {
        Entry* entry = &entries[index];

        /**
         * Tombstones help us identify deleted entries when probing the table.
         * A tombstone is found when the key is null and value is true.
         * See tableDelete function for an explanation.
         */
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) {
                /**
                 * Empty entry, key isn't present.
                 * If we already found a tombstone in a previous
                 * iteration we can use that address.
                 * Otherwise return the current address.
                 */
                return tombstone != NULL ? tombstone : entry;
            } else {
                // We found a tombstone
                // Udate it to point to the current address so
                // it can be re-used by another entry.
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (entry->key == key) {
            // We found the key.
            return entry;
        }

        /**
         * If niether conditions matched then the bucket has an entry in it but
         * with a different key, aka collision.
         * Advance to the next element.
         * This is linear linear probing.
         */
        index = (index + 1) & (capacity - 1);
    }
}

/**
 * Look up key in table by passing those two + a value output parameter.
 * If the entry exists, value will be updated to point to it.
 */
bool tableGet(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return false;

    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;

    *value = entry->value;
    return true;
}

static void adjustCapacity(Table* table, int capacity) {
    Entry* entries = ALLOCATE(Entry, capacity);
    // clear out garbage data from memory to make sure the new slots are empty
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }
    /**
     * When we chose the bucket index for each entry it was based on
     * the hash key modulo the array capacity, which means
     * entries may now end up in different buckets since
     * the array capacity has changed.
     *
     * The new buckets may have collisions, so rebuild
     * the table from scratch by re-inserting every entry.
     */
    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key == NULL) continue;

        // When found a non-empty buck in the old table,
        // insert it into the new one.
        Entry* dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }
    FREE_ARRAY(Entry, table->entries, table->capacity);
    table->entries = entries;
    table->capacity = capacity;
}

// Add key/value pair to the given hash table.
bool tableSet(Table* table, ObjString* key, Value value) {
    // make sure we have enough space in the table for the new entry
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = GROW_CAPACITY(table->capacity);
        adjustCapacity(table, capacity);
    }
    Entry* entry = findEntry(table->entries, table->capacity, key);
    bool isNewKey = entry->key == NULL;
    /**
     * Don't increase count for an existing key we overwrote,
     * and also don't increment count for tombstones.
     *
     * Tombstones are identified by a null key and true as the value.
     * If we are replacing a tombstone with a new entry, the bucket has
     * already been accounted for and the count doesn’t change.
     */
    if (isNewKey && IS_NIL(entry->value)) table->count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

// Delete an entry from the hash table
bool tableDelete(Table* table, ObjString* key) {
    if (table->count == 0) return false;

    // Find the entry.
    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;

    /**
     * When inserting into the table and we've had a collision,
     * we walk the probe sequence to find the next empty slot.
     * We know it's empty when it's null, but if we were to delete
     * an entry in a series of entries with collisions, we would
     * break that sequence like leaving removing a node from a linked
     * list without re-linking the pointer from the previous node.
     *
     * We get around this by placing a tombstone in the entry we
     * want to delete, so when we find one when probing we know to continue.
     *
     * Our tombstone is a combination of NULL key and true value, but
     * any representation could be used that doesn't confuse an empty
     * bucket with a valid entry.
     */
    entry->key = NULL;
    entry->value = BOOL_VAL(true);
    return true;
}

// Helper function for copying all of the entries of one hash table into another.
// Supports inheritance.
void tableAddAll(Table* from, Table* to) {
    for (int i = 0; i < from->capacity; i++) {
        Entry* entry = &from->entries[i];
        if (entry->key != NULL) {
            tableSet(to, entry->key, entry->value);
        }
    }
}

/**
 * To look for an itnerned string we can't use the normal tableGet() function
 * because that calls findEntry(), which has the same problem with duplicate
 * strings that interning solves, i.e. we want to compare memory addresses, not chars of a string.
 *
 * This is very similar to findEntry except this receives the raw chars instead of an ObjString,
 * but when this is called ObjString doesn't exist yet.
 * We also look at the actual strings instead of just an == comparison.
 */
ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;

    // use bitwise instead of modulo for faster calculation
    // see optimization ch for explanation https://craftinginterpreters.com/optimization.html#slow-key-wrapping
    uint32_t index = hash & (table->capacity - 1);
    for (;;) {
        Entry* entry = &table->entries[index];
        if (entry->key == NULL) {
            // Stop if we find an empty non-tombstone entry.  Tombstones are identified by null key and true value.
            // String doesn't exist in the table, the caller will add it.
            if (IS_NIL(entry->value)) return NULL;
            // we need to match lengths, hashes and "substring" matches to know if we found it in the interned strings table.
        } else if (entry->key->length == length && entry->key->hash == hash && memcmp(entry->key->chars, chars, length) == 0) {
            // We found an exaxt string match, return it.
            return entry->key;
        }
        // Continue to next index if we haven't found a match
        index = (index + 1) & (table->capacity - 1);
    }
}

/**
 * garbage collector helper to clear out strings
 */
void tableRemoveWhite(Table* table) {
    /**
     * Walk every entry in the strings table.
     * The string intern table uses only the key of each entry, basically a set not a hash map
     * If a key string object's mark bit is not set, then it's a white object
     */
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL && !entry->key->obj.isMarked) {
            tableDelete(table, entry->key);
        }
    }
}

/**
 * Helper function for garbage collector
 * Walk the provided hash table and look for "roots", which are values
 * that can be accessed without having to traverse through other references
 */
void markTable(Table* table) {
    // walk the entries array and mark each value
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        markObject((Obj*)entry->key);
        markValue(entry->value);
    }
}
