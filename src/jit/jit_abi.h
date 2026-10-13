#pragma once

#include "jit_asm.h"

// The one place the JIT asks which C calling convention it runs under.
// Every emitter stages helper arguments in the System V registers
// (rdi, rsi, rdx, rcx, r8, r9). On Win64, jit_emit_call() moves them
// into the Microsoft x64 slots right before the call.
#if defined(_WIN32)
#define JIT_ABI_WIN64 1
#else
#define JIT_ABI_WIN64 0
#endif

// The register C code passes jit_entry's one argument (vm) in.
#define JIT_ENTRY_ARG_REG (JIT_ABI_WIN64 ? JIT_RCX : JIT_RDI)

// Win64 call frame: 32 bytes of shadow space the callee owns, then
// stack arguments 5 and 6. 48 bytes keeps rsp 16-byte aligned.
#define JIT_WIN64_CALL_FRAME_SIZE 48
#define JIT_WIN64_ARG5_OFFSET 32
#define JIT_WIN64_ARG6_OFFSET 40
