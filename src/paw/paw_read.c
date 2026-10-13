#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paw_internal.h"

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

/* Parses a whole .paw payload in memory into a runnable program,
   allocated on vm->heap. Takes ownership of raw and always frees it.
   display_path only names the file in error messages. */
static paw_program_t *paw_load_from_buffer(char *raw, size_t raw_size, const char *display_path, vm_t *vm) {
	paw_reader_t r;
	char magic[PAW_MAGIC_LENGTH];
	uint16_t version;
	uint8_t flags;
	uint32_t module_count;
	bool has_source;
	char *entry_filename;
	char *entry_source;
	char **module_filenames;
	char **module_sources;
	uint32_t edge_count;
	uint32_t *edge_importer_index;
	uint32_t *edge_target_index;
	char **edge_local_names;
	size_t *edge_local_name_lengths;
	uint32_t string_count;
	obj_string_t **strings_arr;
	uint32_t function_count;
	obj_function_t **functions_arr;
	uint32_t modules_disk_count;
	obj_module_t **modules_arr;
	table_t **file_globals;
	uint32_t entry_id;
	uint32_t *module_script_ids;
	uint32_t *module_table_ids;
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
	module_count = r_u32(&r);

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

	module_filenames = module_count > 0 ? malloc(module_count * sizeof(char *)) : NULL;
	module_sources = module_count > 0 ? malloc(module_count * sizeof(char *)) : NULL;
	for (i = 0; i < module_count; i++) {
		module_filenames[i] = r_blob_cstr(&r, NULL);
		module_sources[i] = has_source ? r_blob_cstr(&r, NULL) : NULL;
	}

	// Every #module import edge. importer_index 0 is the entry file, and
	// 1..module_count is modules[importer_index - 1]. target_index is a
	// plain modules[] index, since only a module can be a target.
	edge_count = r_u32(&r);
	edge_importer_index = edge_count > 0 ? malloc(edge_count * sizeof(uint32_t)) : NULL;
	edge_target_index = edge_count > 0 ? malloc(edge_count * sizeof(uint32_t)) : NULL;
	edge_local_names = edge_count > 0 ? malloc(edge_count * sizeof(char *)) : NULL;
	edge_local_name_lengths = edge_count > 0 ? malloc(edge_count * sizeof(size_t)) : NULL;
	for (i = 0; i < edge_count; i++) {
		edge_importer_index[i] = r_u32(&r);
		edge_target_index[i] = r_u32(&r);
		edge_local_names[i] = r_blob_cstr(&r, &edge_local_name_lengths[i]);
	}

	if (!r.ok) {
		fprintf(stderr, "paweli: '%s' is truncated or corrupt\n", display_path);
		free(raw);
		free(entry_filename);
		free(entry_source);
		for (i = 0; i < module_count; i++) {
			free(module_filenames[i]);
			free(module_sources[i]);
		}
		free(module_filenames);
		free(module_sources);
		for (i = 0; i < edge_count; i++) {
			free(edge_local_names[i]);
		}
		free(edge_importer_index);
		free(edge_target_index);
		free(edge_local_names);
		free(edge_local_name_lengths);
		return NULL;
	}

	file_globals = malloc((module_count + 1) * sizeof(table_t *));
	file_globals[0] = vm_new_file_globals(vm);
	for (i = 0; i < module_count; i++) {
		file_globals[i + 1] = vm_new_file_globals(vm);
	}

	// A source may be NULL in a --strip-source build.
	// vm_track_owned_source() and the diagnostic code both accept NULL.
	vm_track_owned_source(vm, entry_filename, entry_source);
	vm->filename = entry_filename;
	vm->source = entry_source;
	for (i = 0; i < module_count; i++) {
		vm_track_owned_source(vm, module_filenames[i], module_sources[i]);
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

	// Shells first. Every function and module object must exist before any
	// content is read, since constants and members refer to them by id in
	// any order. Function shells come before modules, because a module's
	// members point at functions. Modules come before function bodies,
	// because a body's constants can point at a module.
	function_count = r_u32(&r);
	functions_arr = malloc((function_count == 0 ? 1 : function_count) * sizeof(obj_function_t *));
	for (i = 0; i < function_count; i++) {
		functions_arr[i] = object_new_function(vm->heap);
	}

	modules_disk_count = r_u32(&r);
	modules_arr = malloc((modules_disk_count == 0 ? 1 : modules_disk_count) * sizeof(obj_module_t *));
	for (i = 0; i < modules_disk_count && r.ok; i++) {
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
		uint32_t home_file;
		uint32_t constants_count;
		uint32_t local_slot_count;

		f = functions_arr[i];

		name_id = r_u32(&r);
		f->arity = (int)r_u32(&r);
		home_file = r_u32(&r);
		if (home_file > module_count) {
			// Corrupt: no file has this index. Use the entry file so nothing reads
			// out of bounds. The !r.ok check below still fails the load.
			r.ok = false;
			home_file = 0;
		}
		f->globals = file_globals[home_file];
		f->filename = home_file == 0 ? entry_filename : module_filenames[home_file - 1];
		f->source = home_file == 0 ? entry_source : module_sources[home_file - 1];

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
				if (ref >= modules_disk_count) {
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

		// 0 means no register-eligible locals and nothing more to read. The
		// chunk already holds the defaults.
		local_slot_count = r_u32(&r);
		f->chunk.local_slot_count = (int)local_slot_count;
		if (local_slot_count > 0) {
			f->chunk.local_reg_eligible = malloc((size_t)local_slot_count * sizeof(bool));
			f->chunk.local_reg_types = malloc((size_t)local_slot_count * sizeof(value_type_t));
			f->chunk.local_reg_decl_end = malloc((size_t)local_slot_count * sizeof(size_t));
			f->chunk.local_reg_dead_at = malloc((size_t)local_slot_count * sizeof(size_t));
			for (j = 0; j < local_slot_count; j++) {
				f->chunk.local_reg_eligible[j] = r_u8(&r) != 0;
			}
			for (j = 0; j < local_slot_count; j++) {
				f->chunk.local_reg_types[j] = (value_type_t)r_u8(&r);
			}
			for (j = 0; j < local_slot_count; j++) {
				f->chunk.local_reg_decl_end[j] = (size_t)r_i64(&r);
			}
			for (j = 0; j < local_slot_count; j++) {
				f->chunk.local_reg_dead_at[j] = (size_t)r_i64(&r);
			}
		}
	}

	entry_id = r_u32(&r);
	module_script_ids = module_count > 0 ? malloc(module_count * sizeof(uint32_t)) : NULL;
	module_table_ids = module_count > 0 ? malloc(module_count * sizeof(uint32_t)) : NULL;

	for (i = 0; i < module_count && r.ok; i++) {
		module_script_ids[i] = r_u32(&r);
		module_table_ids[i] = r_u32(&r);
		if (!r.ok || module_table_ids[i] >= modules_disk_count) {
			r.ok = false;
			break;
		}
	}

	// Rebuild every import edge now that the module objects are known.
	// Each importer's globals table gets each of its targets under its
	// local name, as in a compile from source.
	for (i = 0; i < edge_count && r.ok; i++) {
		if (edge_importer_index[i] > module_count || edge_target_index[i] >= module_count) {
			r.ok = false;
			break;
		}
		table_set(file_globals[edge_importer_index[i]], object_new_string(vm->heap, vm->strings, edge_local_names[i], edge_local_name_lengths[i]), value_obj((obj_t *)modules_arr[module_table_ids[edge_target_index[i]]]));
	}

	free(strings_arr);
	free(modules_arr);
	for (i = 0; i < edge_count; i++) {
		free(edge_local_names[i]);
	}
	free(edge_importer_index);
	free(edge_target_index);
	free(edge_local_names);
	free(edge_local_name_lengths);
	free(raw);

	if (!r.ok || entry_id >= function_count) {
		fprintf(stderr, "paweli: '%s' is truncated or corrupt\n", display_path);
		free(functions_arr);
		free(module_script_ids);
		free(module_table_ids);
		free(file_globals);
		free(module_filenames);
		free(module_sources);
		return NULL;
	}
	for (i = 0; i < module_count; i++) {
		if (module_script_ids[i] >= function_count) {
			fprintf(stderr, "paweli: '%s' is truncated or corrupt\n", display_path);
			free(functions_arr);
			free(module_script_ids);
			free(module_table_ids);
			free(file_globals);
			free(module_filenames);
			free(module_sources);
			return NULL;
		}
	}

	// Root every script now, under the reserved "<script>" key that
	// compiler_compile() also uses. No script is reachable from a GC root
	// until vm_run() pushes it, and the module scripts run before the
	// entry script. A collection while an early module runs would free
	// the scripts that have not run yet.
	table_set(file_globals[0], object_new_string(vm->heap, vm->strings, "<script>", 8), value_obj((obj_t *)functions_arr[entry_id]));
	for (i = 0; i < module_count; i++) {
		table_set(file_globals[i + 1], object_new_string(vm->heap, vm->strings, "<script>", 8), value_obj((obj_t *)functions_arr[module_script_ids[i]]));
	}

	program = malloc(sizeof(paw_program_t));
	program->entry_script = functions_arr[entry_id];
	program->module_count = module_count;
	program->module_scripts = module_count > 0 ? malloc(module_count * sizeof(obj_function_t *)) : NULL;
	for (i = 0; i < module_count; i++) {
		program->module_scripts[i] = functions_arr[module_script_ids[i]];
	}

	free(functions_arr);
	free(module_script_ids);
	free(module_table_ids);
	free(file_globals);
	free(module_filenames);
	free(module_sources);
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

void paw_program_free(paw_program_t *program) {
	free(program->module_scripts);
	free(program);
}
