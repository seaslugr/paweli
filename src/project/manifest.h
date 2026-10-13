#pragma once

#include <stdbool.h>

// The manifest in a project's root directory. `paweli init` writes
// one. `paweli build`, `run`, and `test` with no file argument read
// one from the current directory. Parent directories are not searched.
#define PROJECT_MANIFEST_FILENAME "project.pawproj"

// Each field is a heap-allocated C string, or NULL before a successful
// read.
typedef struct {
	char *name;
	char *version;
	char *entry;
} project_manifest_t;

/* True if a manifest exists at path and can be opened. It is not
   parsed. A caller uses this to tell "no project here" from "broken
   manifest", which project_manifest_read() reports. */
bool project_manifest_exists(const char *path);

/* Reads and parses path into *out. The manifest uses paweli's lexer.
   It holds `string <field> = "<value>";` declarations in any order,
   for exactly the three fields below, each once. Returns false for an
   unreadable file, a syntax error, an unknown field, a repeated field,
   or a missing field. A diagnostic is printed in every case except a
   file that cannot be opened. On failure *out is all NULL and safe to
   pass to project_manifest_free(). */
bool project_manifest_read(const char *path, project_manifest_t *out);

void project_manifest_free(project_manifest_t *manifest);

/* Scaffolds a project in dir, which must exist: writes the manifest
   and a starter main.pw. name becomes the manifest's name field. With
   a NULL name it is taken from dir's last path component, or from the
   current directory's name when dir is ".". Returns false, with an
   error printed, if dir already has a manifest. An existing main.pw is
   left as it is. */
bool project_init(const char *dir, const char *name);
