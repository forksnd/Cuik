// If you're trying to port TB on to a new platform you'll need to fill in these
// functions with their correct behavior.
#pragma once
#include <setjmp.h>

////////////////////////////////
// Exception handling
////////////////////////////////
// This is used every so often to sandbox an action, kinda
// like the C++ stuff except i don't use it as the core form
// of error handling because it makes for weird control flow
typedef struct {
    jmp_buf state;
    void* handle; // platform specific
} RestorePoint;

// returns true if it's returning to the restore point
bool tb_platform_push_restore_point(RestorePoint* r);
void tb_platform_pop_restore_point(RestorePoint* r);

////////////////////////////////
// Virtual memory management
////////////////////////////////
// This is used for JIT compiler pages or any large scale memory
// allocations.
void* tb_platform_valloc(size_t size);
void* tb_platform_valloc_guard(size_t size);
void  tb_platform_vfree(void* ptr, size_t size);

// It's either execute-read or read-write
bool tb_platform_vprotect(void* ptr, size_t size, bool execute_read);

////////////////////////////////
// General Heap allocator
////////////////////////////////
// This is used for reallocatable and smaller allocations
// compared to the large scale arenas.
void* tb_platform_heap_alloc(size_t size);
void* tb_platform_heap_realloc(void* ptr, size_t size);
void  tb_platform_heap_free(void* ptr);

////////////////////////////////
// String arena
////////////////////////////////
char* tb_platform_string_alloc(const char* str);
void  tb_platform_string_free();

////////////////////////////////
// Persistent arena allocator
////////////////////////////////
void tb_platform_arena_init();

// this persistent arena allocator is used all of the backend
// worker threads to store data until the end of compilation.
void* tb_platform_arena_alloc(size_t size);

// NOTE(NeGate): Free is supposed to free all allocations.
void tb_platform_arena_free();