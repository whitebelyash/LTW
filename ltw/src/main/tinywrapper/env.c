//
// Created by maks on 12.07.2025.
//
#include "env.h"
#include <stdlib.h>
#include "libraryinternal.h"

INTERNAL bool env_istrue(const char* name) {
    const char* env = getenv(name);
    return env != NULL && *env == '1';
}