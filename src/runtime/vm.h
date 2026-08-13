#pragma once

#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/value.h"

#define VM_DEFAULT_STACK_SIZE 4096
#define VM_DEFAULT_MAX_FRAMES 256
#define VM_MIN_STACK_SIZE 16

typedef struct {
	obj_function_t *function;
	uint8_t *ip;
	value_t *slots;
} call_frame_t;

typedef enum {
	VM_OK,
	VM_RUNTIME_ERROR,
} vm_result_t;

typedef struct vm {
	call_frame_t *frames;
	int frame_count;
	int max_frames;

	value_t *stack;
	value_t *stack_top;
	size_t stack_size;

	table_t globals;
	obj_t **objects;
	table_t *strings;

	const char *filename;
} vm_t;

/* Does not take ownership of objects or strings. Same convention as the
   compiler: the caller frees both once done. Pre-defines "print" and the
   "stdlib" module into globals, see src/stdlib/stdlib.c. Both are always
   available; no #module is needed. stack_size and max_frames configure
   the VM's value stack and call-frame depth. stack_size must be at
   least VM_MIN_STACK_SIZE. */
void vm_init(vm_t *vm, obj_t **objects, table_t *strings, const char *filename, size_t stack_size, int max_frames);
void vm_free(vm_t *vm);
vm_result_t vm_run(vm_t *vm, obj_function_t *script);

/* Exposed so native functions (in src/stdlib) can report their own type
   or arity errors. They use the same mechanism the VM itself uses. */
void vm_runtime_error(vm_t *vm, const char *format, ...);
