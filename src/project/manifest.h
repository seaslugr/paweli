#pragma once

#include <stdbool.h>

// project.pawproj: the manifest a paweli project keeps in its root
// directory. paweli init writes one; a bare `paweli build` (no file
// argument) reads one from the current directory instead of requiring
// a filename. Looked up by this exact name, cwd-only, no parent-
// directory search yet.
#define PROJECT_MANIFEST_FILENAME "project.pawproj"

// Every field is a plain heap-allocated C string, or NULL if the
// manifest hasn't been successfully read into this struct yet.
typedef struct {
	char *name;
	char *version;
	char *entry;
} project_manifest_t;

// True if PROJECT_MANIFEST_FILENAME's file exists at path and could be
// opened for reading. Doesn't parse it; lets a caller (build_command())
// tell "no project here" (silent, expected) apart from "a manifest is
// here but broken" (project_manifest_read() prints a real diagnostic
// for that case).
bool project_manifest_exists(const char *path);

// Reads and parses path (normally PROJECT_MANIFEST_FILENAME) into
// *out. The manifest grammar reuses paweli's own lexer: zero or more
// `string <field> = "<value>";` declarations, in any order, covering
// exactly the three fields below, each required exactly once. Returns
// false on a missing/unreadable file, a lexer/grammar error, an
// unrecognized field name, a field repeated more than once, or a
// required field never given, with a diagnostic already printed to
// stderr via diagnostic_emit() in every case except "file could not be
// opened" (see project_manifest_exists()). *out is left fully NULL'd
// on failure, safe to pass to project_manifest_free() either way.
bool project_manifest_read(const char *path, project_manifest_t *out);

void project_manifest_free(project_manifest_t *manifest);

// Scaffolds a new project in dir (normally "."), which must already
// exist: writes PROJECT_MANIFEST_FILENAME and a starter main.pw there.
// name becomes the manifest's own name field; NULL derives it from
// dir's absolute-path basename (via getcwd(), so dir must be "." or
// otherwise resolve through the current directory; see project_init()'s
// own comment in manifest.c). Fails (false, error already printed) if
// a manifest already exists in dir. main.pw is left untouched, not
// overwritten, if it already exists; everything else about init still
// happens.
bool project_init(const char *dir, const char *name);
