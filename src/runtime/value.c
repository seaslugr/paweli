#include <string.h>

#include "object.h"
#include "value.h"

bool value_equals(value_t a, value_t b) {
	obj_string_t *sa;
	obj_string_t *sb;

	if (a.type != b.type) {
		return false;
	}

	switch (a.type) {
		case VAL_NULL:
			return true;
		case VAL_BOOL:
			return value_as_bool(a) == value_as_bool(b);
		case VAL_INT:
			return value_as_int(a) == value_as_int(b);
		case VAL_FLOAT:
			return value_as_float(a) == value_as_float(b);
		case VAL_OBJ:
			if (value_as_obj(a)->type != OBJ_STRING || value_as_obj(b)->type != OBJ_STRING) {
				return value_as_obj(a) == value_as_obj(b);
			}
			sa = (obj_string_t *)value_as_obj(a);
			sb = (obj_string_t *)value_as_obj(b);
			return sa->length == sb->length && memcmp(sa->chars, sb->chars, sa->length) == 0;
	}

	return false;
}

void value_print(value_t value, FILE *out) {
	switch (value.type) {
		case VAL_NULL:
			fputs("null", out);
			break;
		case VAL_BOOL:
			fputs(value_as_bool(value) ? "true" : "false", out);
			break;
		case VAL_INT:
			fprintf(out, "%lld", (long long)value_as_int(value));
			break;
		case VAL_FLOAT:
			fprintf(out, "%g", value_as_float(value));
			break;
		case VAL_OBJ:
			object_print(value_as_obj(value), out);
			break;
	}
}

const char *value_type_name(value_t value) {
	switch (value.type) {
		case VAL_NULL: return "null";
		case VAL_BOOL: return "bool";
		case VAL_INT: return "int";
		case VAL_FLOAT: return "float";
		case VAL_OBJ: return object_type_name(value_as_obj(value)->type);
	}
	return "?";
}
