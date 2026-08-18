#include <stdio.h>
#include <stdlib.h>

#include "io.h"
#include "stdlib.h"

static bool stdlib_read_line(vm_t *vm, int arg_count, value_t *args, value_t *out_result);

/* Reads until '\n' or EOF, growing buffer as needed. A trailing '\r' is
   also stripped, so a CRLF-terminated line reads the same as an LF one.
   EOF with nothing read yields an empty string, not an error: there is
   no way to distinguish "empty line" from "no more input" otherwise. */
static bool stdlib_read_line(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	char *buffer;
	size_t capacity;
	size_t length;
	int c;
	obj_string_t *result;

	(void)args;

	if (!stdlib_check_arity(vm, "read_line", arg_count, 0)) {
		return false;
	}

	capacity = 128;
	buffer = malloc(capacity);
	length = 0;

	while ((c = fgetc(stdin)) != EOF && c != '\n') {
		if (length + 1 >= capacity) {
			capacity *= 2;
			buffer = realloc(buffer, capacity);
		}
		buffer[length++] = (char)c;
	}

	if (length > 0 && buffer[length - 1] == '\r') {
		length--;
	}

	result = object_new_string(vm->heap, vm->strings, buffer, length);
	free(buffer);

	*out_result = value_obj((obj_t *)result);
	return true;
}

void stdlib_register_io(vm_t *vm, obj_module_t *module) {
	stdlib_register_fn(vm, module, "read_line", stdlib_read_line);
}
