#pragma once

#include "signature.h"

/* Populates globals with print/assert's signatures, and stdlib_members
   with every stdlib.* native's signature, so the type checker can check
   calls into them exactly the way it checks calls into user functions.
   Both are ordinary signature_table_t values; free them with
   signature_table_free() like any other. Natives are never mangled.
   There is exactly one registered obj_native_t per name at runtime,
   regardless of how many signatures it type-checks against here. A
   resolved call to one of these always uses its plain registered name
   for the runtime lookup, never signature->mangled_name.

   length/push/pop are deliberately NOT registered here: they are
   polymorphic over array element type ("(T[]) -> int" for any T), which
   a fixed signature_t has no way to express. The compiler special-cases
   those three names directly instead of consulting this table. */
void native_signatures_init(signature_table_t *globals, signature_table_t *stdlib_members);
