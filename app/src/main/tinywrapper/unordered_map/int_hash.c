//
// Created by maks on 04.03.2024.
//
#include "int_hash.h"
#include "../libraryinternal.h"
static size_t intmap_hash(void* key) {
    return (size_t)key;
}

static bool intmap_equals(void* v1, void* v2) {
    return v1 == v2;
}

INTERNAL unordered_map* alloc_intmap_safe() {
    return unordered_map_alloc(0, 1, intmap_hash, intmap_equals);
}

INTERNAL unordered_map* alloc_intmap() {
    unordered_map* map = alloc_intmap_safe();
    if(map == NULL) {
        printf("failed to alloc_intmap\n");
        abort();
    }
    return map;
}