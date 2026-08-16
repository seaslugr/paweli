#pragma once

#include <stdbool.h>

/* Returns a malloc'd, NUL-terminated absolute path to the currently
   running executable's own file, or NULL if this platform has no
   supported way to determine it. Caller owns the result.

   Used two ways: at `paweli build --bin` time, to find the bytes to
   copy as the resulting executable's runtime stub (the running
   `paweli` binary already contains everything needed to detect and
   run an appended payload, see paw_is_self_extracting()); and at
   ordinary startup, to check whether *this* invocation's own
   executable is one of those built binaries. */
char *platform_self_exe_path(void);

/* Marks path executable. Returns false only on a genuine failure to
   set permissions. */
bool platform_make_executable(const char *path);
