#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paw_internal.h"

static void w_u8(paw_writer_t *w, uint8_t v);
static void w_u16(paw_writer_t *w, uint16_t v);
static void w_u32(paw_writer_t *w, uint32_t v);
static void w_i32(paw_writer_t *w, int32_t v);
static void w_i64(paw_writer_t *w, int64_t v);
static void w_f64(paw_writer_t *w, double v);
static void w_bytes(paw_writer_t *w, const void *data, size_t length);
static void w_blob(paw_writer_t *w, const char *data, size_t length);
static void w_cstr(paw_writer_t *w, const char *s);
static uint32_t writer_function_home_file(const paw_writer_t *w, const obj_function_t *f);
static uint32_t writer_intern_string(paw_writer_t *w, obj_string_t *s);
static uint32_t writer_register_function(paw_writer_t *w, obj_function_t *f);
static uint32_t writer_register_module(paw_writer_t *w, obj_module_t *m);
static bool writer_write_constant(paw_writer_t *w, value_t value);
static bool writer_write_module_body(paw_writer_t *w, size_t index);

static void w_u8(paw_writer_t *w, uint8_t v) {
	if (fwrite(&v, 1, 1, w->out) != 1) {
		w->ok = false;
	}
}

static void w_u16(paw_writer_t *w, uint16_t v) {
	w_u8(w, (uint8_t)(v & 0xFF));
	w_u8(w, (uint8_t)((v >> 8) & 0xFF));
}

static void w_u32(paw_writer_t *w, uint32_t v) {
	w_u8(w, (uint8_t)(v & 0xFF));
	w_u8(w, (uint8_t)((v >> 8) & 0xFF));
	w_u8(w, (uint8_t)((v >> 16) & 0xFF));
	w_u8(w, (uint8_t)((v >> 24) & 0xFF));
}

static void w_i32(paw_writer_t *w, int32_t v) {
	uint32_t bits;

	memcpy(&bits, &v, sizeof(bits));
	w_u32(w, bits);
}

static void w_i64(paw_writer_t *w, int64_t v) {
	uint64_t bits;

	memcpy(&bits, &v, sizeof(bits));
	w_u32(w, (uint32_t)(bits & 0xFFFFFFFFu));
	w_u32(w, (uint32_t)((bits >> 32) & 0xFFFFFFFFu));
}

static void w_f64(paw_writer_t *w, double v) {
	uint64_t bits;
	int64_t signed_bits;

	memcpy(&bits, &v, sizeof(bits));
	memcpy(&signed_bits, &bits, sizeof(signed_bits));
	w_i64(w, signed_bits);
}

static void w_bytes(paw_writer_t *w, const void *data, size_t length) {
	if (length > 0 && fwrite(data, 1, length, w->out) != length) {
		w->ok = false;
	}
}

static void w_blob(paw_writer_t *w, const char *data, size_t length) {
	w_u32(w, (uint32_t)length);
	w_bytes(w, data, length);
}

static void w_cstr(paw_writer_t *w, const char *s) {
	w_blob(w, s, s == NULL ? 0 : strlen(s));
}

static uint32_t writer_function_home_file(const paw_writer_t *w, const obj_function_t *f) {
	size_t i;

	for (i = 0; i < w->file_globals_count; i++) {
		if (f->globals == w->file_globals[i]) {
			return (uint32_t)i;
		}
	}
	// Every function belongs to a loaded file, so this should be
	// unreachable. The entry file (0) is a safe fallback index.
	return 0;
}

static uint32_t writer_intern_string(paw_writer_t *w, obj_string_t *s) {
	size_t i;

	if (s == NULL) {
		return PAW_NO_STRING;
	}

	for (i = 0; i < w->strings_count; i++) {
		if (w->strings[i] == s) {
			return (uint32_t)i;
		}
	}

	if (w->strings_count == w->strings_capacity) {
		w->strings_capacity = w->strings_capacity < 8 ? 8 : w->strings_capacity * 2;
		w->strings = realloc(w->strings, w->strings_capacity * sizeof(obj_string_t *));
	}
	w->strings[w->strings_count] = s;
	return (uint32_t)(w->strings_count++);
}

/* writer_register_function() and writer_register_module() call each
   other. A function's constants can hold a struct namespace object,
   and that object's members are functions. A struct function that
   calls a sibling holds the same namespace object again. Both
   functions reserve their id before they recurse, so the cycle ends. */
static uint32_t writer_register_function(paw_writer_t *w, obj_function_t *f) {
	size_t index;
	size_t i;

	for (i = 0; i < w->functions_count; i++) {
		if (w->functions[i] == f) {
			return (uint32_t)i;
		}
	}

	if (w->functions_count == w->functions_capacity) {
		w->functions_capacity = w->functions_capacity < 8 ? 8 : w->functions_capacity * 2;
		w->functions = realloc(w->functions, w->functions_capacity * sizeof(obj_function_t *));
		w->function_home = realloc(w->function_home, w->functions_capacity * sizeof(uint32_t));
	}
	index = w->functions_count;
	w->functions[index] = f;
	w->function_home[index] = writer_function_home_file(w, f);
	w->functions_count++;

	writer_intern_string(w, f->name);
	for (i = 0; i < f->chunk.constants_count; i++) {
		value_t value = f->chunk.constants[i];

		if (!value_is_obj(value)) {
			continue;
		}
		if (value_as_obj(value)->type == OBJ_STRING) {
			writer_intern_string(w, (obj_string_t *)value_as_obj(value));
		} else if (value_as_obj(value)->type == OBJ_FUNCTION) {
			writer_register_function(w, (obj_function_t *)value_as_obj(value));
		} else if (value_as_obj(value)->type == OBJ_MODULE) {
			writer_register_module(w, (obj_module_t *)value_as_obj(value));
		}
	}

	return (uint32_t)index;
}

static uint32_t writer_register_module(paw_writer_t *w, obj_module_t *m) {
	size_t index;
	size_t i;
	table_entry_t *entries;

	for (i = 0; i < w->modules_count; i++) {
		if (w->modules[i] == m) {
			return (uint32_t)i;
		}
	}

	if (w->modules_count == w->modules_capacity) {
		w->modules_capacity = w->modules_capacity < 4 ? 4 : w->modules_capacity * 2;
		w->modules = realloc(w->modules, w->modules_capacity * sizeof(obj_module_t *));
	}
	index = w->modules_count;
	w->modules[index] = m;
	w->modules_count++;

	writer_intern_string(w, m->name);
	entries = m->members->entries;
	for (i = 0; i < m->members->capacity; i++) {
		if (entries[i].key == NULL) {
			continue;
		}
		writer_intern_string(w, entries[i].key);
		if (value_is_obj(entries[i].value) && value_as_obj(entries[i].value)->type == OBJ_FUNCTION) {
			writer_register_function(w, (obj_function_t *)value_as_obj(entries[i].value));
		}
	}

	return (uint32_t)index;
}

static bool writer_write_constant(paw_writer_t *w, value_t value) {
	switch (value.type) {
	case VAL_NULL:
		w_u8(w, PAW_CONST_NULL);
		return true;
	case VAL_BOOL:
		w_u8(w, PAW_CONST_BOOL);
		w_u8(w, value_as_bool(value) ? 1 : 0);
		return true;
	case VAL_INT:
		w_u8(w, PAW_CONST_INT);
		w_i64(w, value_as_int(value));
		return true;
	case VAL_FLOAT:
		w_u8(w, PAW_CONST_FLOAT);
		w_f64(w, value_as_float(value));
		return true;
	case VAL_OBJ:
		if (value_as_obj(value)->type == OBJ_STRING) {
			w_u8(w, PAW_CONST_STRING);
			w_u32(w, writer_intern_string(w, (obj_string_t *)value_as_obj(value)));
			return true;
		}
		if (value_as_obj(value)->type == OBJ_FUNCTION) {
			w_u8(w, PAW_CONST_FUNCTION);
			w_u32(w, writer_register_function(w, (obj_function_t *)value_as_obj(value)));
			return true;
		}
		if (value_as_obj(value)->type == OBJ_MODULE) {
			w_u8(w, PAW_CONST_MODULE);
			w_u32(w, writer_register_module(w, (obj_module_t *)value_as_obj(value)));
			return true;
		}
		fprintf(stderr, "paweli: internal error: cannot serialize a compiled '%s' constant\n", object_type_name(value_as_obj(value)->type));
		return false;
	}

	fprintf(stderr, "paweli: internal error: unknown constant type in .paw serialization\n");
	return false;
}

bool paw_writer_write_function_body(paw_writer_t *w, size_t index) {
	obj_function_t *f;
	size_t i;

	f = w->functions[index];

	w_u32(w, writer_intern_string(w, f->name));
	w_u32(w, (uint32_t)f->arity);
	w_u32(w, w->function_home[index]);

	w_u32(w, (uint32_t)f->chunk.count);
	w_bytes(w, f->chunk.code, f->chunk.count);
	for (i = 0; i < f->chunk.count; i++) {
		w_i32(w, f->chunk.lines[i]);
	}
	for (i = 0; i < f->chunk.count; i++) {
		w_i32(w, f->chunk.columns[i]);
	}

	w_u32(w, (uint32_t)f->chunk.constants_count);
	for (i = 0; i < f->chunk.constants_count; i++) {
		if (!writer_write_constant(w, f->chunk.constants[i])) {
			return false;
		}
	}

	// See chunk.h for these four fields. A chunk with no register-eligible
	// locals has a count of 0 and nothing more to write. decl_end and
	// dead_at are written as 64-bit values, because (size_t)-1 is their
	// "not applicable" value and must survive the round trip.
	w_u32(w, (uint32_t)f->chunk.local_slot_count);
	for (i = 0; i < (size_t)f->chunk.local_slot_count; i++) {
		w_u8(w, f->chunk.local_reg_eligible[i] ? 1 : 0);
	}
	for (i = 0; i < (size_t)f->chunk.local_slot_count; i++) {
		w_u8(w, (uint8_t)f->chunk.local_reg_types[i]);
	}
	for (i = 0; i < (size_t)f->chunk.local_slot_count; i++) {
		w_i64(w, (int64_t)f->chunk.local_reg_decl_end[i]);
	}
	for (i = 0; i < (size_t)f->chunk.local_slot_count; i++) {
		w_i64(w, (int64_t)f->chunk.local_reg_dead_at[i]);
	}

	return true;
}

static bool writer_write_module_body(paw_writer_t *w, size_t index) {
	obj_module_t *m;
	size_t i;
	size_t member_count;
	table_entry_t *entries;

	m = w->modules[index];
	w_u32(w, writer_intern_string(w, m->name));

	member_count = 0;
	entries = m->members->entries;
	for (i = 0; i < m->members->capacity; i++) {
		if (entries[i].key != NULL) {
			member_count++;
		}
	}

	w_u32(w, (uint32_t)member_count);
	for (i = 0; i < m->members->capacity; i++) {
		if (entries[i].key == NULL) {
			continue;
		}
		if (!value_is_obj(entries[i].value) || value_as_obj(entries[i].value)->type != OBJ_FUNCTION) {
			fprintf(stderr, "paweli: internal error: module member '%s' is not a compiled function\n", entries[i].key->chars);
			return false;
		}
		w_u32(w, writer_intern_string(w, entries[i].key));
		w_u32(w, writer_register_function(w, (obj_function_t *)value_as_obj(entries[i].value)));
	}

	return true;
}

/* Writes the whole .paw payload to out. The caller has opened out in
   binary mode and positioned it: at the start of a new file, or after
   a copied executable stub. It does not open or close out and prints
   no error. Returns false on any write failure. */
bool paw_write_payload(FILE *out, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source) {
	paw_writer_t w;
	uint32_t entry_id;
	uint32_t *module_script_ids;
	uint32_t *module_table_ids;
	size_t i;
	bool ok;

	memset(&w, 0, sizeof(w));
	w.ok = true;
	w.out = out;
	w.file_globals_count = module_count + 1;
	w.file_globals = malloc(w.file_globals_count * sizeof(table_t *));
	w.file_globals[0] = entry_script->globals;
	for (i = 0; i < module_count; i++) {
		w.file_globals[i + 1] = modules[i].script->globals;
	}

	w_bytes(&w, PAW_MAGIC, PAW_MAGIC_LENGTH);
	w_u16(&w, PAW_VERSION);
	w_u8(&w, strip_source ? 0 : PAW_FLAG_HAS_SOURCE);
	w_u32(&w, (uint32_t)module_count);

	w_cstr(&w, entry_filename);
	if (!strip_source) {
		w_cstr(&w, entry_source);
	}

	for (i = 0; i < module_count; i++) {
		w_cstr(&w, modules[i].filename);
		if (!strip_source) {
			w_cstr(&w, modules[i].source);
		}
	}

	// Every #module import edge in the program. A module that imports
	// another module needs its binding rebuilt too. local_name is a byte
	// span with a length and is not NUL-terminated, so w_cstr() cannot
	// write it.
	w_u32(&w, (uint32_t)edge_count);
	for (i = 0; i < edge_count; i++) {
		w_u32(&w, (uint32_t)edges[i].importer_index);
		w_u32(&w, (uint32_t)edges[i].target_index);
		w_blob(&w, edges[i].local_name, edges[i].local_name_length);
	}

	entry_id = writer_register_function(&w, entry_script);
	module_script_ids = module_count > 0 ? malloc(module_count * sizeof(uint32_t)) : NULL;
	module_table_ids = module_count > 0 ? malloc(module_count * sizeof(uint32_t)) : NULL;
	for (i = 0; i < module_count; i++) {
		module_script_ids[i] = writer_register_function(&w, modules[i].script);
		// The constant walk above never finds an imported module. Its members
		// are reached by name lookup and not through an embedded constant, as
		// a struct namespace is. So it is registered here as a root.
		module_table_ids[i] = writer_register_module(&w, modules[i].module_obj);
	}

	w_u32(&w, (uint32_t)w.strings_count);
	for (i = 0; i < w.strings_count; i++) {
		w_blob(&w, w.strings[i]->chars, w.strings[i]->length);
	}

	// Discovery is finished before any byte is written, so every count is
	// final. Function and module shells are written before function
	// bodies. A module's members name functions by id, and a function's
	// constants can name a module by id, so the reader needs both shells
	// before either one's content (see paw_load_from_buffer()).
	w_u32(&w, (uint32_t)w.functions_count);

	w_u32(&w, (uint32_t)w.modules_count);
	for (i = 0; i < w.modules_count; i++) {
		if (!writer_write_module_body(&w, i)) {
			w.ok = false;
			break;
		}
	}

	for (i = 0; i < w.functions_count && w.ok; i++) {
		if (!paw_writer_write_function_body(&w, i)) {
			w.ok = false;
			break;
		}
	}

	w_u32(&w, entry_id);
	for (i = 0; i < module_count; i++) {
		w_u32(&w, module_script_ids[i]);
		w_u32(&w, module_table_ids[i]);
	}

	ok = w.ok;
	free(w.file_globals);
	free(w.strings);
	free(w.functions);
	free(w.function_home);
	free(w.modules);
	free(module_script_ids);
	free(module_table_ids);

	return ok;
}

bool paw_write_file(const char *path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source) {
	FILE *out;
	bool ok;

	out = fopen(path, "wb");
	if (out == NULL) {
		fprintf(stderr, "paweli: could not open '%s' for writing\n", path);
		return false;
	}

	ok = paw_write_payload(out, entry_script, entry_filename, entry_source, modules, module_count, edges, edge_count, strip_source);
	fclose(out);

	if (!ok) {
		fprintf(stderr, "paweli: failed to write '%s'\n", path);
		remove(path);
		return false;
	}

	return true;
}
