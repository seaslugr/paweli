#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paw_internal.h"

static void put_u64_le(uint8_t *buf, uint64_t v);
static uint64_t get_u64_le(const uint8_t *buf);

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

	// The bounds check never subtracts offset or size from total_u, so a
	// corrupt trailer cannot underflow into a value that looks valid.
	max_payload_region = total_u - PAW_EXE_TRAILER_SIZE;
	if (size > max_payload_region || offset > max_payload_region - size) {
		return false;
	}

	*out_offset = offset;
	*out_size = size;
	return true;
}

bool paw_build_executable(const char *output_path, const char *stub_path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source) {
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
	ok = paw_write_payload(out, entry_script, entry_filename, entry_source, modules, module_count, edges, edge_count, strip_source);
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
