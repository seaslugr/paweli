#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct obj obj_t;

typedef enum {
	VAL_NULL,
	VAL_BOOL,
	VAL_INT,
	VAL_FLOAT,
	VAL_OBJ,
} value_type_t;

typedef struct {
	value_type_t type;
	union {
		bool boolean;
		int64_t integer;
		double number;
		obj_t *obj;
	} as;
} value_t;

static inline value_t value_null() {
	value_t value;

	value.type = VAL_NULL;
	value.as.integer = 0;
	return value;
}

static inline value_t value_bool(bool b) {
	value_t value;

	value.type = VAL_BOOL;
	value.as.boolean = b;
	return value;
}

static inline value_t value_int(int64_t i) {
	value_t value;

	value.type = VAL_INT;
	value.as.integer = i;
	return value;
}

static inline value_t value_float(double d) {
	value_t value;

	value.type = VAL_FLOAT;
	value.as.number = d;
	return value;
}

static inline value_t value_obj(obj_t *obj) {
	value_t value;

	value.type = VAL_OBJ;
	value.as.obj = obj;
	return value;
}

static inline bool value_is_null(value_t value) {
	return value.type == VAL_NULL;
}

static inline bool value_is_bool(value_t value) {
	return value.type == VAL_BOOL;
}

static inline bool value_is_int(value_t value) {
	return value.type == VAL_INT;
}

static inline bool value_is_float(value_t value) {
	return value.type == VAL_FLOAT;
}

static inline bool value_is_obj(value_t value) {
	return value.type == VAL_OBJ;
}

static inline bool value_as_bool(value_t value) {
	return value.as.boolean;
}

static inline int64_t value_as_int(value_t value) {
	return value.as.integer;
}

static inline double value_as_float(value_t value) {
	return value.as.number;
}

static inline obj_t *value_as_obj(value_t value) {
	return value.as.obj;
}

/* Truthiness for conditions. null and false are falsey. Everything
   else is truthy, including 0 and 0.0. */
static inline bool value_is_falsey(value_t value) {
	return value_is_null(value) || (value_is_bool(value) && !value_as_bool(value));
}

bool value_equals(value_t a, value_t b);
void value_print(value_t value, FILE *out);
const char *value_type_name(value_t value);
