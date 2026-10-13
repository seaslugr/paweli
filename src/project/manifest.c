#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostics/diagnostic.h"
#include "lexer/lexer.h"
#include "manifest.h"
#include "platform/platform.h"

static char *read_manifest_source(const char *path);
static bool has_path_separator(const char *name);
static char *unescape_string_literal(const char *raw, size_t raw_length);
static char *dup_string(const char *chars, size_t length);
static void manifest_error(token_t token, const char *path, const char *source, const char *code, const char *message);
static bool assign_field(project_manifest_t *out, const char *field_name, size_t field_name_length, char *value, token_t field_token, const char *path, const char *source);

static bool file_exists(const char *path) {
	FILE *file;

	file = fopen(path, "rb");
	if (file == NULL) {
		return false;
	}
	fclose(file);
	return true;
}

bool project_manifest_exists(const char *path) {
	return file_exists(path);
}

static char *read_manifest_source(const char *path) {
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

	buffer = malloc((size_t)size + 1);
	bytes_read = fread(buffer, 1, (size_t)size, file);
	buffer[bytes_read] = '\0';

	fclose(file);
	return buffer;
}

/* Decodes the same escapes as object_new_string_from_literal(). That
   function makes a GC string and needs a heap. A manifest is read
   before a heap exists, so this returns a plain malloc'd C string. raw
   does not include the quotes. */
static char *unescape_string_literal(const char *raw, size_t raw_length) {
	char *buffer;
	size_t out_len;
	size_t i;

	buffer = malloc(raw_length + 1);
	out_len = 0;

	for (i = 0; i < raw_length; i++) {
		if (raw[i] == '\\' && i + 1 < raw_length) {
			i++;
			switch (raw[i]) {
				case 'n':
					buffer[out_len++] = '\n';
					break;
				case 't':
					buffer[out_len++] = '\t';
					break;
				case 'r':
					buffer[out_len++] = '\r';
					break;
				case '\\':
					buffer[out_len++] = '\\';
					break;
				case '"':
					buffer[out_len++] = '"';
					break;
				case '0':
					buffer[out_len++] = '\0';
					break;
				default:
					buffer[out_len++] = raw[i];
					break;
			}
		} else {
			buffer[out_len++] = raw[i];
		}
	}

	buffer[out_len] = '\0';
	return buffer;
}

static char *dup_string(const char *chars, size_t length) {
	char *out;

	out = malloc(length + 1);
	memcpy(out, chars, length);
	out[length] = '\0';
	return out;
}

/* Like parser_error_at(): a TOKEN_ERROR has its own "lexer::*" code,
   and its message is in token.start and token.length. */
static void manifest_error(token_t token, const char *path, const char *source, const char *code, const char *message) {
	if (token.type == TOKEN_ERROR && token.error_code != NULL) {
		diagnostic_emit(DIAG_ERROR, token.error_code, path, source, token.line, token.column, 0, "%.*s", (int)token.length, token.start);
	} else if (token.type == TOKEN_EOF) {
		diagnostic_emit(DIAG_ERROR, code, path, source, token.line, token.column, 0, "at end of file: %s", message);
	} else {
		diagnostic_emit(DIAG_ERROR, code, path, source, token.line, token.column, (int)token.length, "at '%.*s': %s", (int)token.length, token.start, message);
	}
}

/* Takes ownership of value. It is stored in *out on success and freed
   on failure. */
static bool assign_field(project_manifest_t *out, const char *field_name, size_t field_name_length, char *value, token_t field_token, const char *path, const char *source) {
	char **slot;

	if (field_name_length == 4 && memcmp(field_name, "name", 4) == 0) {
		slot = &out->name;
	} else if (field_name_length == 7 && memcmp(field_name, "version", 7) == 0) {
		slot = &out->version;
	} else if (field_name_length == 5 && memcmp(field_name, "entry", 5) == 0) {
		slot = &out->entry;
	} else {
		manifest_error(field_token, path, source, "project::unknown-field", "unknown project field (expected 'name', 'version', or 'entry')");
		free(value);
		return false;
	}

	if (*slot != NULL) {
		manifest_error(field_token, path, source, "project::duplicate-field", "this field is already set earlier in the manifest");
		free(value);
		return false;
	}

	*slot = value;
	return true;
}

bool project_manifest_read(const char *path, project_manifest_t *out) {
	char *source;
	lexer_t lexer;
	token_t type_token;
	token_t name_token;
	token_t eq_token;
	token_t value_token;
	token_t semi_token;
	char *value;
	bool ok;

	out->name = NULL;
	out->version = NULL;
	out->entry = NULL;

	source = read_manifest_source(path);
	if (source == NULL) {
		return false;
	}

	lexer_init(&lexer, source);
	ok = true;

	for (;;) {
		type_token = lexer_next_token(&lexer);
		if (type_token.type == TOKEN_EOF) {
			break;
		}
		if (type_token.type != TOKEN_STRING) {
			manifest_error(type_token, path, source, "project::expected-field", "expected a field declaration (e.g. 'string name = \"...\";')");
			ok = false;
			break;
		}

		name_token = lexer_next_token(&lexer);
		if (name_token.type != TOKEN_IDENTIFIER) {
			manifest_error(name_token, path, source, "project::expected-field-name", "expected a field name after 'string'");
			ok = false;
			break;
		}

		eq_token = lexer_next_token(&lexer);
		if (eq_token.type != TOKEN_EQUAL) {
			manifest_error(eq_token, path, source, "project::expected-equals", "expected '=' after the field name");
			ok = false;
			break;
		}

		value_token = lexer_next_token(&lexer);
		if (value_token.type != TOKEN_STRING_LITERAL) {
			manifest_error(value_token, path, source, "project::expected-string-literal", "expected a string literal value");
			ok = false;
			break;
		}

		semi_token = lexer_next_token(&lexer);
		if (semi_token.type != TOKEN_SEMICOLON) {
			manifest_error(semi_token, path, source, "project::expected-semicolon", "expected ';' after the field value");
			ok = false;
			break;
		}

		value = unescape_string_literal(value_token.start + 1, value_token.length - 2);
		if (!assign_field(out, name_token.start, name_token.length, value, name_token, path, source)) {
			ok = false;
			break;
		}
	}

	// type_token is the TOKEN_EOF that ended the loop. Reporting at it
	// puts a missing-field error at the end of the file.
	if (ok && out->name == NULL) {
		manifest_error(type_token, path, source, "project::missing-field", "missing required field 'name'");
		ok = false;
	}
	if (ok && out->version == NULL) {
		manifest_error(type_token, path, source, "project::missing-field", "missing required field 'version'");
		ok = false;
	}
	if (ok && out->entry == NULL) {
		manifest_error(type_token, path, source, "project::missing-field", "missing required field 'entry'");
		ok = false;
	}

	free(source);

	if (!ok) {
		project_manifest_free(out);
	}
	return ok;
}

void project_manifest_free(project_manifest_t *manifest) {
	free(manifest->name);
	free(manifest->version);
	free(manifest->entry);
	manifest->name = NULL;
	manifest->version = NULL;
	manifest->entry = NULL;
}

static bool has_path_separator(const char *name) {
	for (const char *p = name; *p != '\0'; p++) {
		if (platform_is_path_separator(*p)) {
			return true;
		}
	}
	return false;
}

/* Only used when name is NULL, which is `paweli init` with no
   argument. For "." it takes the name of the current directory. For
   anything else it takes dir's last path component. Returns "project"
   if the directory cannot be read or the name is empty. */
static char *derive_name_from_dir(const char *dir) {
	char cwd_buf[4096];
	const char *base_source;
	size_t len;
	size_t last_slash;
	bool found_slash;
	size_t i;
	size_t base_len;

	if (strcmp(dir, ".") == 0) {
		if (!platform_current_dir(cwd_buf, sizeof(cwd_buf))) {
			return dup_string("project", strlen("project"));
		}
		base_source = cwd_buf;
	} else {
		base_source = dir;
	}

	len = strlen(base_source);
	while (len > 1 && platform_is_path_separator(base_source[len - 1])) {
		len--;
	}

	found_slash = false;
	last_slash = 0;
	for (i = 0; i < len; i++) {
		if (platform_is_path_separator(base_source[i])) {
			last_slash = i;
			found_slash = true;
		}
	}

	base_len = found_slash ? len - (last_slash + 1) : len;
	if (base_len == 0) {
		return dup_string("project", strlen("project"));
	}
	return dup_string(base_source + (found_slash ? last_slash + 1 : 0), base_len);
}

/* Writes value as a paweli string literal. '\' and '"' are escaped, so
   a name taken from a directory reads back correctly. */
static void write_string_field(FILE *file, const char *field_name, const char *value) {
	const char *c;

	fprintf(file, "string %s = \"", field_name);
	for (c = value; *c != '\0'; c++) {
		if (*c == '\\' || *c == '"') {
			fputc('\\', file);
		}
		fputc(*c, file);
	}
	fprintf(file, "\";\n");
}

bool project_init(const char *dir, const char *name) {
	char manifest_path[4096];
	char main_path[4096];
	char *derived_name;
	const char *final_name;
	FILE *manifest_file;
	FILE *main_file;

	// The name becomes a file name under build/. A path separator in it
	// would send the output to another directory, so it is rejected here.
	if (name != NULL && has_path_separator(name)) {
		fprintf(stderr, "paweli: project name '%s' cannot contain a path separator\n", name);
		return false;
	}

	snprintf(manifest_path, sizeof(manifest_path), "%s/%s", dir, PROJECT_MANIFEST_FILENAME);
	if (file_exists(manifest_path)) {
		fprintf(stderr, "paweli: '%s' already exists\n", manifest_path);
		return false;
	}

	derived_name = NULL;
	if (name != NULL) {
		final_name = name;
	} else {
		derived_name = derive_name_from_dir(dir);
		final_name = derived_name;
	}

	manifest_file = fopen(manifest_path, "wb");
	if (manifest_file == NULL) {
		fprintf(stderr, "paweli: could not open '%s' for writing\n", manifest_path);
		free(derived_name);
		return false;
	}
	write_string_field(manifest_file, "name", final_name);
	write_string_field(manifest_file, "version", "0.1.0");
	write_string_field(manifest_file, "entry", "main.pw");
	fclose(manifest_file);

	free(derived_name);

	snprintf(main_path, sizeof(main_path), "%s/main.pw", dir);
	if (!file_exists(main_path)) {
		main_file = fopen(main_path, "wb");
		if (main_file == NULL) {
			fprintf(stderr, "paweli: could not open '%s' for writing\n", main_path);
			return false;
		}
		fputs("fn main() {\n\tprint(\"Hello World!\");\n}\n", main_file);
		fclose(main_file);
	}

	return true;
}
