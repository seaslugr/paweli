#pragma once

#include "signature.h"

/* Fills globals with the signatures of print and assert, and
   stdlib_members with the signature of every stdlib.* native, so calls
   to them are type-checked like calls to user functions. Free both
   with signature_table_free(). Natives are never mangled. One
   obj_native_t backs every signature of a name, and a call always
   looks it up by its plain name.

   length, push, and pop are not registered here. They work on an array
   of any element type, which a fixed signature cannot say. The
   compiler handles those three names itself. */
void native_signatures_init(signature_table_t *globals, signature_table_t *stdlib_members);
