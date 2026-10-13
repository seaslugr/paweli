#include <string.h>

#include "native_signatures.h"
#include "type.h"

static void add(signature_table_t *table, const char *name, type_t *params, size_t param_count, bool has_return, type_t return_type);

static void add(signature_table_t *table, const char *name, type_t *params, size_t param_count, bool has_return, type_t return_type) {
	signature_table_add(table, name, strlen(name), params, param_count, return_type, has_return);
}

void native_signatures_init(signature_table_t *globals, signature_table_t *stdlib_members) {
	type_t params[2];

	// print() with no arguments. A fixed parameter cannot say "any type",
	// so the compiler handles the one-argument form itself (see
	// compile_bare_call()).
	add(globals, "print", NULL, 0, true, type_void());

	params[0] = type_bool();
	add(globals, "assert", params, 1, true, type_void());
	params[0] = type_bool();
	params[1] = type_string();
	add(globals, "assert", params, 2, true, type_void());

	params[0] = type_int();
	add(stdlib_members, "abs", params, 1, true, type_int());

	params[0] = type_int();
	params[1] = type_int();
	add(stdlib_members, "max", params, 2, true, type_int());
	add(stdlib_members, "min", params, 2, true, type_int());

	params[0] = type_int();
	add(stdlib_members, "sqrt", params, 1, true, type_float());
	params[0] = type_float();
	add(stdlib_members, "sqrt", params, 1, true, type_float());

	params[0] = type_string();
	add(stdlib_members, "length", params, 1, true, type_int());

	params[0] = type_string();
	add(stdlib_members, "to_upper", params, 1, true, type_string());

	params[0] = type_string();
	params[1] = type_string();
	add(stdlib_members, "concat", params, 2, true, type_string());
	add(stdlib_members, "split", params, 2, true, type_array_of(type_string()));

	params[0] = type_int();
	add(stdlib_members, "to_string", params, 1, true, type_string());
	params[0] = type_float();
	add(stdlib_members, "to_string", params, 1, true, type_string());
	params[0] = type_bool();
	add(stdlib_members, "to_string", params, 1, true, type_string());

	params[0] = type_string();
	add(stdlib_members, "to_int", params, 1, true, type_int());

	add(stdlib_members, "read_line", NULL, 0, true, type_string());
}
