#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paw.h"
#include "compiler/chunk.h"
#include "platform/exe.h"
#include "runtime/gc.h"
#include "runtime/table.h"
#include "runtime/value.h"

#define PAW_MAGIC "PAWC"
#define PAW_MAGIC_LENGTH 4
// v2: function bodies additionally carry local_reg_eligible/
// local_reg_types/local_reg_decl_end (see writer_write_function_body()'s
// own comment). Without them, a function loaded from a .paw file or
// --bin executable compiled register allocation to nothing at all
// (select_register_locals() in jit_compile.c bails out immediately on
// local_slot_count <= 0), silently losing the JIT's entire register
// allocation feature (R1/R2a) for every distribution form except
// running straight from .pw source.
#define PAW_VERSION 2

#define PAW_FLAG_HAS_SOURCE 0x01

#define PAW_CONST_NULL 0
#define PAW_CONST_BOOL 1
#define PAW_CONST_INT 2
#define PAW_CONST_FLOAT 3
#define PAW_CONST_STRING 4
#define PAW_CONST_FUNCTION 5
#define PAW_CONST_MODULE 6

#define PAW_NO_STRING UINT32_MAX

// A self-extracting executable is a copy of the paweli binary (the
// "stub", which already contains everything needed to detect and run
// this trailer) with a .paw payload and this fixed-size trailer
// appended, in that order. The trailer sits at a known offset from
// the *end* of the file specifically so it can be found without
// knowing the stub's size up front; paw_is_self_extracting() never
// needs to know how large the copied paweli binary was.
#define PAW_EXE_MAGIC "PAWELIEX"
#define PAW_EXE_MAGIC_LENGTH 8
#define PAW_EXE_TRAILER_SIZE (PAW_EXE_MAGIC_LENGTH + 8 + 8)

// Every string, function, and module object reachable from the program's
// roots (the entry script; the module's script and its own module_obj,
// if there is a #module dependency; and any struct namespace object a
// call site embeds directly as a constant; see compile_struct_call())
// gets a stable id, assigned the first time it is seen. All three arrays
// are searched linearly by pointer identity: a compiled program's
// string/function/module count is small enough (this is a teaching-scale
// language; programs stay far below the tens of thousands of functions
// where it would matter) that a real hash table would be premature.
//
// entry_globals/module_globals are the two possible values of a
// deserialized function's ->globals, borrowed from entry_script/
// module_script themselves (see paw_write_file()); comparing a newly
// discovered function's ->globals against them is how its home file is
// determined, rather than threading a "which file am I compiling for"
// argument through every recursive call. A struct's own member
// functions are only ever reachable through a namespace object's
// members table (never through a script's own top-level globals), so
// there is no other way to know which file they belong to.
typedef struct {
	FILE *out;
	bool ok;

	table_t *entry_globals;
	table_t *module_globals;

	obj_string_t **strings;
	size_t strings_count;
	size_t strings_capacity;

	obj_function_t **functions;
	uint8_t *function_home;
	size_t functions_count;
	size_t functions_capacity;

	obj_module_t **modules;
	size_t modules_count;
	size_t modules_capacity;
} paw_writer_t;

typedef struct {
	const uint8_t *data;
	size_t size;
	size_t pos;
	bool ok;
} paw_reader_t;

static void w_u8(paw_writer_t *w, uint8_t v);
static void w_u16(paw_writer_t *w, uint16_t v);
static void w_u32(paw_writer_t *w, uint32_t v);
static void w_i32(paw_writer_t *w, int32_t v);
static void w_i64(paw_writer_t *w, int64_t v);
static void w_f64(paw_writer_t *w, double v);
static void w_bytes(paw_writer_t *w, const void *data, size_t length);
static void w_blob(paw_writer_t *w, const char *data, size_t length);
static void w_cstr(paw_writer_t *w, const char *s);

static uint8_t writer_function_home_file(const paw_writer_t *w, const obj_function_t *f);
static uint32_t writer_intern_string(paw_writer_t *w, obj_string_t *s);
static uint32_t writer_register_function(paw_writer_t *w, obj_function_t *f);
static uint32_t writer_register_module(paw_writer_t *w, obj_module_t *m);
static bool writer_write_constant(paw_writer_t *w, value_t value);
static bool writer_write_function_body(paw_writer_t *w, size_t index);
static bool writer_write_module_body(paw_writer_t *w, size_t index);
static bool paw_write_payload(FILE *out, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, obj_module_t *module_obj, obj_function_t *module_script, const char *module_local_name, size_t module_local_name_length, const char *module_filename, const char *module_source, bool strip_source);

static uint8_t r_u8(paw_reader_t *r);
static uint16_t r_u16(paw_reader_t *r);
static uint32_t r_u32(paw_reader_t *r);
static int32_t r_i32(paw_reader_t *r);
static int64_t r_i64(paw_reader_t *r);
static double r_f64(paw_reader_t *r);
static const uint8_t *r_bytes(paw_reader_t *r, size_t length);
static char *r_blob_cstr(paw_reader_t *r, size_t *out_length);

static char *read_whole_file(const char *path, size_t *out_size);
static paw_program_t *paw_load_from_buffer(char *raw, size_t raw_size, const char *display_path, vm_t *vm);

static void put_u64_le(uint8_t *buf, uint64_t v);
static uint64_t get_u64_le(const uint8_t *buf);

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

static uint8_t writer_function_home_file(const paw_writer_t *w, const obj_function_t *f) {
	return (w->module_globals != NULL && f->globals == w->module_globals) ? 1 : 0;
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

// writer_register_function() and writer_register_module() are mutually
// recursive (a function's constants can embed a struct namespace object,
// and a namespace object's members are themselves functions), and
// paweli's own struct functions can call siblings on the same struct,
// which embeds the same namespace object right back as one of its own
// constants. Both reserve their id before recursing into whatever they
// reference, so that cycle terminates instead of recursing forever.
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
		w->function_home = realloc(w->function_home, w->functions_capacity * sizeof(uint8_t));
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

static bool writer_write_function_body(paw_writer_t *w, size_t index) {
	obj_function_t *f;
	size_t i;

	f = w->functions[index];

	w_u32(w, writer_intern_string(w, f->name));
	w_u32(w, (uint32_t)f->arity);
	w_u8(w, w->function_home[index]);

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

	// See chunk.h's own comment on these three fields: local_slot_count
	// (and hence all three arrays) is 0/NULL for a chunk with no
	// register-eligible locals at all, same as finalize_local_reg_info()
	// itself leaves them for that case. Nothing to write beyond the
	// count. local_reg_decl_end goes through w_i64 (not w_u32) since it
	// legitimately needs the full 64-bit range: (size_t)-1 is its
	// "not applicable" sentinel, and truncating that to 32 bits on write
	// would silently reconstruct a different (if still practically
	// never-matching) value on read. It is not a case worth relying on
	// "probably still correct in practice" for.
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

bool paw_is_paw_file(const char *path) {
	FILE *file;
	char magic[PAW_MAGIC_LENGTH];
	bool matches;

	file = fopen(path, "rb");
	if (file == NULL) {
		return false;
	}

	matches = fread(magic, 1, PAW_MAGIC_LENGTH, file) == PAW_MAGIC_LENGTH && memcmp(magic, PAW_MAGIC, PAW_MAGIC_LENGTH) == 0;
	fclose(file);
	return matches;
}

/* Writes the whole .paw payload (everything paw_write_file() produces)
   to out, which the caller already opened in binary-write mode and
   positioned wherever the payload should start, either the very
   start of a fresh file (paw_write_file()), or right after a copied
   executable stub (paw_build_executable()). Does not open or close
   out; does not report success/failure to stderr itself, since a
   caller writing into the middle of a larger file needs to decide for
   itself what "failed" means for the whole file, not just this part.
   Returns false on any write failure. */
static bool paw_write_payload(FILE *out, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, obj_module_t *module_obj, obj_function_t *module_script, const char *module_local_name, size_t module_local_name_length, const char *module_filename, const char *module_source, bool strip_source) {
	paw_writer_t w;
	uint32_t entry_id;
	uint32_t module_script_id;
	uint32_t module_table_id;
	size_t i;
	bool ok;

	memset(&w, 0, sizeof(w));
	w.ok = true;
	w.out = out;
	w.entry_globals = entry_script->globals;
	w.module_globals = module_script != NULL ? module_script->globals : NULL;

	w_bytes(&w, PAW_MAGIC, PAW_MAGIC_LENGTH);
	w_u16(&w, PAW_VERSION);
	w_u8(&w, strip_source ? 0 : PAW_FLAG_HAS_SOURCE);
	w_u8(&w, module_obj != NULL ? 1 : 0);

	w_cstr(&w, entry_filename);
	if (!strip_source) {
		w_cstr(&w, entry_source);
	}

	if (module_obj != NULL) {
		// module_local_name is a raw byte span (module_unit_t's own
		// local_name/local_name_length), not a NUL-terminated C string,
		// unlike every other string this function writes; w_cstr()
		// would read past its end. module_obj's own name travels through
		// the generic module table below instead of being duplicated
		// here, same as every other module (a struct namespace, for
		// instance) never gets special-cased in the header.
		w_blob(&w, module_local_name, module_local_name_length);
		w_cstr(&w, module_filename);
		if (!strip_source) {
			w_cstr(&w, module_source);
		}
	}

	entry_id = writer_register_function(&w, entry_script);
	module_script_id = 0;
	module_table_id = 0;
	if (module_obj != NULL) {
		module_script_id = writer_register_function(&w, module_script);
		// Never found by the constant walk above: an imported module's
		// members are called through a name lookup (OP_GET_GLOBAL the
		// local binding, then OP_GET_PROPERTY), not an embedded constant
		// the way a struct namespace is; see compile_module_call() vs.
		// compile_struct_call() in compiler.c. So it needs registering
		// explicitly, as its own root.
		module_table_id = writer_register_module(&w, module_obj);
	}

	w_u32(&w, (uint32_t)w.strings_count);
	for (i = 0; i < w.strings_count; i++) {
		w_blob(&w, w.strings[i]->chars, w.strings[i]->length);
	}

	// Discovery (the writer_register_* calls above) is a full recursive
	// walk that completes entirely before any bytes are written, so
	// every count from here on is already final. Disk order (modules
	// before function bodies) matches what paw_load_file() needs to
	// read them back in a single forward pass: a module's members
	// reference functions by id, and a function's constants can
	// reference a module by id, so both kinds of shell (function, then
	// module) have to exist before either one's real content is filled
	// in; see paw_load_file()'s own ordering comment.
	w_u32(&w, (uint32_t)w.functions_count);

	w_u32(&w, (uint32_t)w.modules_count);
	for (i = 0; i < w.modules_count; i++) {
		if (!writer_write_module_body(&w, i)) {
			w.ok = false;
			break;
		}
	}

	for (i = 0; i < w.functions_count && w.ok; i++) {
		if (!writer_write_function_body(&w, i)) {
			w.ok = false;
			break;
		}
	}

	w_u32(&w, entry_id);
	if (module_obj != NULL) {
		w_u32(&w, module_script_id);
		w_u32(&w, module_table_id);
	}

	ok = w.ok;
	free(w.strings);
	free(w.functions);
	free(w.function_home);
	free(w.modules);

	return ok;
}

bool paw_write_file(const char *path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, obj_module_t *module_obj, obj_function_t *module_script, const char *module_local_name, size_t module_local_name_length, const char *module_filename, const char *module_source, bool strip_source) {
	FILE *out;
	bool ok;

	out = fopen(path, "wb");
	if (out == NULL) {
		fprintf(stderr, "paweli: could not open '%s' for writing\n", path);
		return false;
	}

	ok = paw_write_payload(out, entry_script, entry_filename, entry_source, module_obj, module_script, module_local_name, module_local_name_length, module_filename, module_source, strip_source);
	fclose(out);

	if (!ok) {
		fprintf(stderr, "paweli: failed to write '%s'\n", path);
		remove(path);
		return false;
	}

	return true;
}

static uint8_t r_u8(paw_reader_t *r) {
	if (r->pos + 1 > r->size) {
		r->ok = false;
		return 0;
	}
	return r->data[r->pos++];
}

static uint16_t r_u16(paw_reader_t *r) {
	uint16_t low;
	uint16_t high;

	low = r_u8(r);
	high = r_u8(r);
	return (uint16_t)(low | (high << 8));
}

static uint32_t r_u32(paw_reader_t *r) {
	uint32_t b0;
	uint32_t b1;
	uint32_t b2;
	uint32_t b3;

	b0 = r_u8(r);
	b1 = r_u8(r);
	b2 = r_u8(r);
	b3 = r_u8(r);
	return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static int32_t r_i32(paw_reader_t *r) {
	uint32_t bits;
	int32_t value;

	bits = r_u32(r);
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static int64_t r_i64(paw_reader_t *r) {
	uint64_t low;
	uint64_t high;
	uint64_t bits;
	int64_t value;

	low = r_u32(r);
	high = r_u32(r);
	bits = low | (high << 32);
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static double r_f64(paw_reader_t *r) {
	int64_t signed_bits;
	uint64_t bits;
	double value;

	signed_bits = r_i64(r);
	memcpy(&bits, &signed_bits, sizeof(bits));
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static const uint8_t *r_bytes(paw_reader_t *r, size_t length) {
	const uint8_t *ptr;

	if (r->pos + length > r->size) {
		r->ok = false;
		return NULL;
	}
	ptr = r->data + r->pos;
	r->pos += length;
	return ptr;
}

static char *r_blob_cstr(paw_reader_t *r, size_t *out_length) {
	uint32_t length;
	const uint8_t *bytes;
	char *buffer;

	length = r_u32(r);
	bytes = r_bytes(r, length);
	if (!r->ok) {
		return NULL;
	}

	buffer = malloc((size_t)length + 1);
	memcpy(buffer, bytes, length);
	buffer[length] = '\0';
	if (out_length != NULL) {
		*out_length = length;
	}
	return buffer;
}

static char *read_whole_file(const char *path, size_t *out_size) {
	FILE *file;
	long size;
	char *buffer;
	size_t bytes_read;

	file = fopen(path, "rb");
	if (file == NULL) {
		return NULL;
	}

	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);

	buffer = malloc((size_t)size);
	bytes_read = fread(buffer, 1, (size_t)size, file);
	fclose(file);

	if (bytes_read != (size_t)size) {
		free(buffer);
		return NULL;
	}

	*out_size = bytes_read;
	return buffer;
}

static void put_u64_le(uint8_t *buf, uint64_t v) {
	int i;

	for (i = 0; i < 8; i++) {
		buf[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
	}
}

static uint64_t get_u64_le(const uint8_t *buf) {
	uint64_t v;
	int i;

	v = 0;
	for (i = 0; i < 8; i++) {
		v |= ((uint64_t)buf[i]) << (8 * i);
	}
	return v;
}

/* Parses a whole .paw payload already sitting in memory (raw, raw_size
   bytes) into a running program, allocating everything on vm->heap.
   Takes ownership of raw (always frees it, on both success and
   failure). display_path is used only to name the file in error
   messages; the actual bytes may have come from a plain .paw file
   (paw_load_file()) or from a range inside a larger executable
   (paw_load_embedded()), and the caller already knows which. */
static paw_program_t *paw_load_from_buffer(char *raw, size_t raw_size, const char *display_path, vm_t *vm) {
	paw_reader_t r;
	char magic[PAW_MAGIC_LENGTH];
	uint16_t version;
	uint8_t flags;
	uint8_t has_module;
	bool has_source;
	char *entry_filename;
	char *entry_source;
	char *module_local_name;
	char *module_filename;
	char *module_source;
	uint32_t string_count;
	obj_string_t **strings_arr;
	uint32_t function_count;
	obj_function_t **functions_arr;
	uint32_t module_count;
	obj_module_t **modules_arr;
	table_t *entry_globals;
	table_t *module_globals;
	table_t *file_globals[2];
	uint32_t entry_id;
	uint32_t module_script_id;
	uint32_t module_table_id;
	obj_module_t *module_obj;
	paw_program_t *program;
	uint32_t i;
	uint32_t j;

	r.data = (const uint8_t *)raw;
	r.size = raw_size;
	r.pos = 0;
	r.ok = true;

	for (i = 0; i < PAW_MAGIC_LENGTH; i++) {
		magic[i] = (char)r_u8(&r);
	}
	version = r_u16(&r);
	flags = r_u8(&r);
	has_module = r_u8(&r);

	if (!r.ok || memcmp(magic, PAW_MAGIC, PAW_MAGIC_LENGTH) != 0) {
		fprintf(stderr, "paweli: '%s' is not a valid .paw file\n", display_path);
		free(raw);
		return NULL;
	}
	if (version != PAW_VERSION) {
		fprintf(stderr, "paweli: '%s' was built by an incompatible paweli version (file version %u, this build supports %u)\n", display_path, version, PAW_VERSION);
		free(raw);
		return NULL;
	}

	has_source = (flags & PAW_FLAG_HAS_SOURCE) != 0;

	entry_filename = r_blob_cstr(&r, NULL);
	entry_source = has_source ? r_blob_cstr(&r, NULL) : NULL;

	module_local_name = NULL;
	module_filename = NULL;
	module_source = NULL;
	if (has_module) {
		module_local_name = r_blob_cstr(&r, NULL);
		module_filename = r_blob_cstr(&r, NULL);
		module_source = has_source ? r_blob_cstr(&r, NULL) : NULL;
	}

	if (!r.ok) {
		fprintf(stderr, "paweli: '%s' is truncated or corrupt\n", display_path);
		free(raw);
		free(entry_filename);
		free(entry_source);
		free(module_local_name);
		free(module_filename);
		free(module_source);
		return NULL;
	}

	entry_globals = vm_new_file_globals(vm);
	module_globals = has_module ? vm_new_file_globals(vm) : NULL;
	file_globals[0] = entry_globals;
	file_globals[1] = module_globals;

	// entry_source/module_source may be NULL (a --strip-source build):
	// vm_track_owned_source()/vm_free() already treat a NULL source as
	// "nothing to free", and find_source_line() (diagnostic.c) already
	// treats a NULL source as "no snippet available", the same fallback
	// path a genuinely out-of-range line takes.
	vm_track_owned_source(vm, entry_filename, entry_source);
	vm->filename = entry_filename;
	vm->source = entry_source;
	if (has_module) {
		vm_track_owned_source(vm, module_filename, module_source);
	} else {
		free(module_filename);
	}

	string_count = r_u32(&r);
	strings_arr = malloc((string_count == 0 ? 1 : string_count) * sizeof(obj_string_t *));
	for (i = 0; i < string_count; i++) {
		size_t length;
		char *text;

		text = r_blob_cstr(&r, &length);
		if (!r.ok) {
			break;
		}
		strings_arr[i] = object_new_string(vm->heap, vm->strings, text, length);
		free(text);
	}

	// Shells first, exactly the way compiler_compile() itself always
	// allocates an empty obj_function_t before compiling its body (see
	// collect_signatures() in compiler.c): a function's constants can
	// reference another function or a module by id regardless of
	// declaration order, and a module's members reference functions by
	// id too, so every function and module object has to already exist
	// (even if not yet filled in) before either kind's real content is
	// read. Functions before modules specifically because a module's
	// members need function shells to point to; modules before function
	// *bodies* because a function body's constants can point back at a
	// module (a struct method embedding its own struct's namespace
	// object, see compile_struct_call() in compiler.c).
	function_count = r_u32(&r);
	functions_arr = malloc((function_count == 0 ? 1 : function_count) * sizeof(obj_function_t *));
	for (i = 0; i < function_count; i++) {
		functions_arr[i] = object_new_function(vm->heap);
	}

	module_count = r_u32(&r);
	modules_arr = malloc((module_count == 0 ? 1 : module_count) * sizeof(obj_module_t *));
	for (i = 0; i < module_count && r.ok; i++) {
		uint32_t name_id;
		uint32_t member_count;

		name_id = r_u32(&r);
		if (!r.ok || name_id >= string_count) {
			r.ok = false;
			break;
		}
		modules_arr[i] = object_new_module(vm->heap, strings_arr[name_id]);

		member_count = r_u32(&r);
		for (j = 0; j < member_count && r.ok; j++) {
			uint32_t key_id;
			uint32_t fn_id;

			key_id = r_u32(&r);
			fn_id = r_u32(&r);
			if (key_id >= string_count || fn_id >= function_count) {
				r.ok = false;
				break;
			}
			table_set(modules_arr[i]->members, strings_arr[key_id], value_obj((obj_t *)functions_arr[fn_id]));
		}
	}

	for (i = 0; i < function_count && r.ok; i++) {
		obj_function_t *f;
		uint32_t name_id;
		uint32_t code_count;
		uint8_t home_file;
		uint32_t constants_count;
		uint32_t local_slot_count;

		f = functions_arr[i];

		name_id = r_u32(&r);
		f->arity = (int)r_u32(&r);
		home_file = r_u8(&r);
		f->globals = file_globals[home_file > 1 ? 0 : home_file];
		f->filename = home_file == 1 ? module_filename : entry_filename;
		f->source = home_file == 1 ? module_source : entry_source;

		if (!r.ok || name_id == PAW_NO_STRING) {
			f->name = NULL;
		} else if (name_id < string_count) {
			f->name = strings_arr[name_id];
		} else {
			r.ok = false;
		}

		code_count = r_u32(&r);
		if (!r.ok) {
			break;
		}
		for (j = 0; j < code_count; j++) {
			chunk_write(&f->chunk, r_u8(&r), 0, 0);
		}
		for (j = 0; j < code_count; j++) {
			f->chunk.lines[j] = r_i32(&r);
		}
		for (j = 0; j < code_count; j++) {
			f->chunk.columns[j] = r_i32(&r);
		}

		constants_count = r_u32(&r);
		for (j = 0; j < constants_count; j++) {
			uint8_t tag;
			uint32_t ref;

			tag = r_u8(&r);
			switch (tag) {
			case PAW_CONST_NULL:
				chunk_add_constant(&f->chunk, value_null());
				break;
			case PAW_CONST_BOOL:
				chunk_add_constant(&f->chunk, value_bool(r_u8(&r) != 0));
				break;
			case PAW_CONST_INT:
				chunk_add_constant(&f->chunk, value_int(r_i64(&r)));
				break;
			case PAW_CONST_FLOAT:
				chunk_add_constant(&f->chunk, value_float(r_f64(&r)));
				break;
			case PAW_CONST_STRING:
				ref = r_u32(&r);
				if (ref >= string_count) {
					r.ok = false;
					break;
				}
				chunk_add_constant(&f->chunk, value_obj((obj_t *)strings_arr[ref]));
				break;
			case PAW_CONST_FUNCTION:
				ref = r_u32(&r);
				if (ref >= function_count) {
					r.ok = false;
					break;
				}
				chunk_add_constant(&f->chunk, value_obj((obj_t *)functions_arr[ref]));
				break;
			case PAW_CONST_MODULE:
				ref = r_u32(&r);
				if (ref >= module_count) {
					r.ok = false;
					break;
				}
				chunk_add_constant(&f->chunk, value_obj((obj_t *)modules_arr[ref]));
				break;
			default:
				r.ok = false;
				break;
			}
		}

		// See writer_write_function_body()'s own comment: 0 (the
		// chunk_init() default already sitting in f->chunk from
		// object_new_function()) means no register-eligible locals,
		// nothing further to read for this function.
		local_slot_count = r_u32(&r);
		f->chunk.local_slot_count = (int)local_slot_count;
		if (local_slot_count > 0) {
			f->chunk.local_reg_eligible = malloc((size_t)local_slot_count * sizeof(bool));
			f->chunk.local_reg_types = malloc((size_t)local_slot_count * sizeof(value_type_t));
			f->chunk.local_reg_decl_end = malloc((size_t)local_slot_count * sizeof(size_t));
			for (j = 0; j < local_slot_count; j++) {
				f->chunk.local_reg_eligible[j] = r_u8(&r) != 0;
			}
			for (j = 0; j < local_slot_count; j++) {
				f->chunk.local_reg_types[j] = (value_type_t)r_u8(&r);
			}
			for (j = 0; j < local_slot_count; j++) {
				f->chunk.local_reg_decl_end[j] = (size_t)r_i64(&r);
			}
		}
	}

	entry_id = r_u32(&r);
	module_script_id = 0;
	module_obj = NULL;

	if (r.ok && has_module) {
		module_script_id = r_u32(&r);
		module_table_id = r_u32(&r);
		if (!r.ok || module_table_id >= module_count) {
			r.ok = false;
		} else {
			module_obj = modules_arr[module_table_id];
			table_set(entry_globals, object_new_string(vm->heap, vm->strings, module_local_name, strlen(module_local_name)), value_obj((obj_t *)module_obj));
		}
	}

	free(strings_arr);
	free(modules_arr);
	free(module_local_name);
	free(raw);

	if (!r.ok || entry_id >= function_count || (has_module && module_script_id >= function_count)) {
		fprintf(stderr, "paweli: '%s' is truncated or corrupt\n", display_path);
		free(functions_arr);
		return NULL;
	}

	// Rooted immediately, under the same reserved "<script>" key
	// compiler_compile() itself uses (see its own comment on why: the
	// key can never collide with a real identifier or mangled name).
	// Neither script is reachable from any GC root yet. That only
	// happens once main.c's vm_run() pushes it onto the stack, and
	// main.c has to vm_run() the module script before the entry script,
	// so without this a collection triggered by running the module
	// first (very reachable under a large module, and guaranteed under
	// PAWELI_GC_STRESS) would sweep the still-unrooted entry script out
	// from under it.
	table_set(entry_globals, object_new_string(vm->heap, vm->strings, "<script>", 8), value_obj((obj_t *)functions_arr[entry_id]));
	if (has_module) {
		table_set(module_globals, object_new_string(vm->heap, vm->strings, "<script>", 8), value_obj((obj_t *)functions_arr[module_script_id]));
	}

	program = malloc(sizeof(paw_program_t));
	program->entry_script = functions_arr[entry_id];
	program->module_script = has_module ? functions_arr[module_script_id] : NULL;

	free(functions_arr);
	return program;
}

paw_program_t *paw_load_file(const char *path, vm_t *vm) {
	char *raw;
	size_t raw_size;

	raw = read_whole_file(path, &raw_size);
	if (raw == NULL) {
		fprintf(stderr, "paweli: could not read '%s'\n", path);
		return NULL;
	}

	return paw_load_from_buffer(raw, raw_size, path, vm);
}

paw_program_t *paw_load_embedded(const char *exe_path, uint64_t offset, uint64_t size, vm_t *vm) {
	FILE *file;
	char *raw;
	size_t bytes_read;

	file = fopen(exe_path, "rb");
	if (file == NULL) {
		fprintf(stderr, "paweli: could not read '%s'\n", exe_path);
		return NULL;
	}

	if (fseek(file, (long)offset, SEEK_SET) != 0) {
		fprintf(stderr, "paweli: '%s' has a corrupt embedded program\n", exe_path);
		fclose(file);
		return NULL;
	}

	raw = malloc(size == 0 ? 1 : (size_t)size);
	bytes_read = fread(raw, 1, (size_t)size, file);
	fclose(file);

	if (bytes_read != (size_t)size) {
		fprintf(stderr, "paweli: '%s' has a corrupt embedded program\n", exe_path);
		free(raw);
		return NULL;
	}

	return paw_load_from_buffer(raw, (size_t)size, exe_path, vm);
}

bool paw_is_self_extracting(const char *exe_path, uint64_t *out_offset, uint64_t *out_size) {
	FILE *file;
	long total;
	uint8_t trailer[PAW_EXE_TRAILER_SIZE];
	uint64_t total_u;
	uint64_t offset;
	uint64_t size;
	uint64_t max_payload_region;

	file = fopen(exe_path, "rb");
	if (file == NULL) {
		return false;
	}

	fseek(file, 0, SEEK_END);
	total = ftell(file);
	if (total < (long)PAW_EXE_TRAILER_SIZE) {
		fclose(file);
		return false;
	}

	fseek(file, total - (long)PAW_EXE_TRAILER_SIZE, SEEK_SET);
	if (fread(trailer, 1, PAW_EXE_TRAILER_SIZE, file) != PAW_EXE_TRAILER_SIZE) {
		fclose(file);
		return false;
	}
	fclose(file);

	if (memcmp(trailer, PAW_EXE_MAGIC, PAW_EXE_MAGIC_LENGTH) != 0) {
		return false;
	}

	total_u = (uint64_t)total;
	offset = get_u64_le(trailer + PAW_EXE_MAGIC_LENGTH);
	size = get_u64_le(trailer + PAW_EXE_MAGIC_LENGTH + 8);

	// Bounds-checked without subtracting offset/size directly from
	// total_u first, so a corrupt (as opposed to merely absent)
	// trailer can never underflow into a wildly out-of-range value
	// that then reads as "valid".
	max_payload_region = total_u - PAW_EXE_TRAILER_SIZE;
	if (size > max_payload_region || offset > max_payload_region - size) {
		return false;
	}

	*out_offset = offset;
	*out_size = size;
	return true;
}

bool paw_build_executable(const char *output_path, const char *stub_path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, obj_module_t *module_obj, obj_function_t *module_script, const char *module_local_name, size_t module_local_name_length, const char *module_filename, const char *module_source, bool strip_source) {
	FILE *stub;
	FILE *out;
	uint8_t buffer[65536];
	size_t n;
	long payload_start;
	long payload_end;
	uint8_t trailer[PAW_EXE_TRAILER_SIZE];
	bool ok;

	stub = fopen(stub_path, "rb");
	if (stub == NULL) {
		fprintf(stderr, "paweli: could not open '%s' (this build's own executable, used as the runtime stub) for reading\n", stub_path);
		return false;
	}

	out = fopen(output_path, "wb");
	if (out == NULL) {
		fprintf(stderr, "paweli: could not open '%s' for writing\n", output_path);
		fclose(stub);
		return false;
	}

	ok = true;
	while ((n = fread(buffer, 1, sizeof(buffer), stub)) > 0) {
		if (fwrite(buffer, 1, n, out) != n) {
			ok = false;
			break;
		}
	}
	fclose(stub);

	if (!ok) {
		fprintf(stderr, "paweli: failed to write '%s'\n", output_path);
		fclose(out);
		remove(output_path);
		return false;
	}

	payload_start = ftell(out);
	ok = paw_write_payload(out, entry_script, entry_filename, entry_source, module_obj, module_script, module_local_name, module_local_name_length, module_filename, module_source, strip_source);
	payload_end = ftell(out);

	if (ok) {
		memcpy(trailer, PAW_EXE_MAGIC, PAW_EXE_MAGIC_LENGTH);
		put_u64_le(trailer + PAW_EXE_MAGIC_LENGTH, (uint64_t)payload_start);
		put_u64_le(trailer + PAW_EXE_MAGIC_LENGTH + 8, (uint64_t)(payload_end - payload_start));
		ok = fwrite(trailer, 1, sizeof(trailer), out) == sizeof(trailer);
	}

	fclose(out);

	if (!ok) {
		fprintf(stderr, "paweli: failed to write '%s'\n", output_path);
		remove(output_path);
		return false;
	}

	if (!platform_make_executable(output_path)) {
		fprintf(stderr, "paweli: wrote '%s' but could not mark it executable\n", output_path);
		return false;
	}

	return true;
}

void paw_program_free(paw_program_t *program) {
	free(program);
}
