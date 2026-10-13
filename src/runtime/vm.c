#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/chunk.h"
#include "diagnostics/diagnostic.h"
#include "jit/jit_dispatch.h"
#include "runtime/gc.h"
#include "stdlib/stdlib.h"
#include "vm.h"

// Extra slack allocated past the configured stack_size. run()'s overflow
// check runs once per instruction dispatch, and no single instruction
// grows the stack by more than one slot. This margin only needs to
// cover that one-instruction detection lag.
#define VM_STACK_SAFETY_MARGIN 64

static bool gc_stress_enabled(void);
static size_t frame_instruction_index(const call_frame_t *frame);
static void push(vm_t *vm, value_t value);
static value_t pop(vm_t *vm);
static value_t peek(vm_t *vm, int distance);
static bool call_value(vm_t *vm, value_t callee, int arg_count);
static bool binary_op(vm_t *vm, uint8_t op);
static bool bitwise_op(vm_t *vm, uint8_t op);
static obj_upvalue_t *capture_upvalue(vm_t *vm, value_t *local);
static void close_upvalues(vm_t *vm, value_t *last);
static vm_result_t run(vm_t *vm, int stop_frame_count, value_t *out_result);

static bool gc_stress_enabled(void) {
	const char *value;

	value = getenv("PAWELI_GC_STRESS");
	return value != NULL && value[0] != '\0';
}

/* The instruction whose line/column a diagnostic should point at: the one
   just before frame->ip, since READ_BYTE() always advances ip before an
   opcode's handler can raise an error. A freshly pushed frame (OP_CALL
   just set frame->ip = function->chunk.code, before this frame has
   dispatched anything of its own) has no such "instruction before ip".
   It falls back to the function's very first instruction instead of
   underflowing to (size_t)-1 and reading chunk.lines/columns out of
   bounds. Reachable even before an error originates in that frame: an
   error reported for a *different*, older frame still walks every
   frame's own ip to print the full backtrace (see the loop below), and
   the newest one can be this fresh. */
static size_t frame_instruction_index(const call_frame_t *frame) {
	if (frame->ip <= frame->function->chunk.code) {
		return 0;
	}
	return (size_t)(frame->ip - frame->function->chunk.code) - 1;
}

static inline void push(vm_t *vm, value_t value) {
	*vm->stack_top = value;
	vm->stack_top++;
}

static inline value_t pop(vm_t *vm) {
	vm->stack_top--;
	return *vm->stack_top;
}

static inline value_t peek(vm_t *vm, int distance) {
	return vm->stack_top[-1 - distance];
}

void vm_runtime_error(vm_t *vm, const char *code, const char *format, ...) {
	va_list args;

	va_start(args, format);
	vsnprintf(vm->error_message, sizeof(vm->error_message), format, args);
	va_end(args);

	snprintf(vm->error_code, sizeof(vm->error_code), "%s", code);
}

void vm_print_uncaught_error(vm_t *vm) {
	int i;
	call_frame_t *frame;
	size_t instruction;
	int line;
	int column;
	const char *filename;
	const char *source;

	line = 0;
	column = 0;
	filename = vm->filename;
	source = vm->source;
	if (vm->frame_count > 0) {
		frame = &vm->frames[vm->frame_count - 1];
		instruction = frame_instruction_index(frame);
		line = frame->function->chunk.lines[instruction];
		column = frame->function->chunk.columns[instruction];
		filename = frame->function->filename;
		source = frame->function->source;
	}
	diagnostic_emit(DIAG_ERROR, vm->error_code, filename, source, line, column, 0, "%s", vm->error_message);

	for (i = vm->frame_count - 1; i >= 0; i--) {
		frame = &vm->frames[i];
		instruction = frame_instruction_index(frame);
		line = frame->function->chunk.lines[instruction];

		if (frame->function->name == NULL) {
			fprintf(stderr, "%s:%d: in <script>\n", frame->function->filename, line);
		} else {
			fprintf(stderr, "%s:%d: in %.*s()\n", frame->function->filename, line, (int)frame->function->name->length, frame->function->name->chars);
		}
	}
}

static bool call_value(vm_t *vm, value_t callee, int arg_count) {
	obj_function_t *function;
	obj_closure_t *closure;
	obj_native_t *native;
	value_t result;
	call_frame_t *frame;

	if (!value_is_obj(callee)) {
		vm_runtime_error(vm, "vm::not-callable", "can only call functions");
		return false;
	}

	switch (value_as_obj(callee)->type) {
		case OBJ_FUNCTION:
			function = (obj_function_t *)value_as_obj(callee);
			if (arg_count != function->arity) {
				vm_runtime_error(vm, "vm::argument-count-mismatch", "expected %d arguments but got %d", function->arity, arg_count);
				return false;
			}
			if (vm->frame_count == vm->max_frames) {
				vm_runtime_error(vm, "vm::call-stack-overflow", "call stack overflow");
				return false;
			}
			frame = &vm->frames[vm->frame_count];
			vm->frame_count++;
			frame->function = function;
			frame->closure = NULL;
			frame->ip = function->chunk.code;
			frame->slots = vm->stack_top - arg_count;
			if (jit_maybe_compile(function)) {
				return function->jit_entry(vm);
			}
			return true;
		case OBJ_CLOSURE:
			closure = (obj_closure_t *)value_as_obj(callee);
			if (arg_count != closure->function->arity) {
				vm_runtime_error(vm, "vm::argument-count-mismatch", "expected %d arguments but got %d", closure->function->arity, arg_count);
				return false;
			}
			if (vm->frame_count == vm->max_frames) {
				vm_runtime_error(vm, "vm::call-stack-overflow", "call stack overflow");
				return false;
			}
			frame = &vm->frames[vm->frame_count];
			vm->frame_count++;
			frame->function = closure->function;
			frame->closure = closure;
			frame->ip = closure->function->chunk.code;
			frame->slots = vm->stack_top - arg_count;
			if (jit_maybe_compile(closure->function)) {
				return closure->function->jit_entry(vm);
			}
			return true;
		case OBJ_NATIVE:
			native = (obj_native_t *)value_as_obj(callee);
			if (!native->function(vm, arg_count, vm->stack_top - arg_count, &result)) {
				return false;
			}
			vm->stack_top -= arg_count + 1;
			push(vm, result);
			return true;
		case OBJ_STRING:
		case OBJ_MODULE:
		case OBJ_ARRAY:
		case OBJ_STRUCT:
		case OBJ_UPVALUE:
			vm_runtime_error(vm, "vm::not-callable", "can only call functions");
			return false;
	}

	vm_runtime_error(vm, "vm::not-callable", "can only call functions");
	return false;
}

/* Finds-or-creates the open upvalue for the stack slot at local, keeping
   vm->open_upvalues sorted deepest-first so close_upvalues() can stop
   early. Two closures created in the same scope that both capture the
   same local (see tests/closures.pw) must end up sharing one obj_upvalue_t,
   not each getting their own; otherwise a mutation through one would not
   be visible through the other, breaking capture-by-reference. */
static obj_upvalue_t *capture_upvalue(vm_t *vm, value_t *local) {
	obj_upvalue_t *prev_upvalue;
	obj_upvalue_t *upvalue;
	obj_upvalue_t *created;

	prev_upvalue = NULL;
	upvalue = vm->open_upvalues;
	while (upvalue != NULL && upvalue->location > local) {
		prev_upvalue = upvalue;
		upvalue = upvalue->next;
	}

	if (upvalue != NULL && upvalue->location == local) {
		return upvalue;
	}

	created = object_new_upvalue(vm->heap, local);
	created->next = upvalue;

	if (prev_upvalue == NULL) {
		vm->open_upvalues = created;
	} else {
		prev_upvalue->next = created;
	}

	return created;
}

/* Closes every open upvalue pointing at slot `last` or deeper (i.e.
   everything about to go out of scope, or about to be discarded by a
   returning frame), copying its value into the upvalue object itself so
   it survives the stack slot being reused. Called from OP_CLOSE_UPVALUE
   (a single local going out of scope) and OP_RETURN (the whole frame). */
static void close_upvalues(vm_t *vm, value_t *last) {
	obj_upvalue_t *upvalue;

	while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
		upvalue = vm->open_upvalues;
		upvalue->closed = *upvalue->location;
		upvalue->location = &upvalue->closed;
		vm->open_upvalues = upvalue->next;
	}
}

static bool binary_op(vm_t *vm, uint8_t op) {
	value_t b;
	value_t a;
	bool int_op;
	int64_t xi;
	int64_t yi;
	double xf;
	double yf;

	b = pop(vm);
	a = pop(vm);

	if (op == OP_ADD && value_is_obj(a) && value_as_obj(a)->type == OBJ_STRING && value_is_obj(b) && value_as_obj(b)->type == OBJ_STRING) {
		push(vm, value_obj((obj_t *)object_new_string_concat(vm->heap, vm->strings, (obj_string_t *)value_as_obj(a), (obj_string_t *)value_as_obj(b))));
		return true;
	}

	if (!value_is_int(a) && !value_is_float(a)) {
		vm_runtime_error(vm, "vm::non-numeric-operand", "left operand must be a number");
		return false;
	}
	if (!value_is_int(b) && !value_is_float(b)) {
		vm_runtime_error(vm, "vm::non-numeric-operand", "right operand must be a number");
		return false;
	}

	int_op = value_is_int(a) && value_is_int(b);

	if (int_op) {
		xi = value_as_int(a);
		yi = value_as_int(b);

		switch (op) {
			case OP_LESS: push(vm, value_bool(xi < yi)); return true;
			case OP_LESS_EQUAL: push(vm, value_bool(xi <= yi)); return true;
			case OP_GREATER: push(vm, value_bool(xi > yi)); return true;
			case OP_GREATER_EQUAL: push(vm, value_bool(xi >= yi)); return true;
			case OP_ADD: push(vm, value_int(xi + yi)); return true;
			case OP_SUBTRACT: push(vm, value_int(xi - yi)); return true;
			case OP_MULTIPLY: push(vm, value_int(xi * yi)); return true;
			case OP_DIVIDE:
				if (yi == 0) {
					vm_runtime_error(vm, "vm::division-by-zero", "division by zero");
					return false;
				}
				push(vm, value_int(xi / yi));
				return true;
			case OP_MODULO:
				if (yi == 0) {
					vm_runtime_error(vm, "vm::modulo-by-zero", "modulo by zero");
					return false;
				}
				push(vm, value_int(xi % yi));
				return true;
			default:
				vm_runtime_error(vm, "vm::unsupported-operator", "unsupported operator");
				return false;
		}
	}

	xf = value_is_int(a) ? (double)value_as_int(a) : value_as_float(a);
	yf = value_is_int(b) ? (double)value_as_int(b) : value_as_float(b);

	switch (op) {
		case OP_LESS: push(vm, value_bool(xf < yf)); return true;
		case OP_LESS_EQUAL: push(vm, value_bool(xf <= yf)); return true;
		case OP_GREATER: push(vm, value_bool(xf > yf)); return true;
		case OP_GREATER_EQUAL: push(vm, value_bool(xf >= yf)); return true;
		case OP_ADD: push(vm, value_float(xf + yf)); return true;
		case OP_SUBTRACT: push(vm, value_float(xf - yf)); return true;
		case OP_MULTIPLY: push(vm, value_float(xf * yf)); return true;
		case OP_DIVIDE: push(vm, value_float(xf / yf)); return true;
		case OP_MODULO:
			vm_runtime_error(vm, "vm::modulo-type-mismatch", "modulo requires integer operands");
			return false;
		default:
			vm_runtime_error(vm, "vm::unsupported-operator", "unsupported operator");
			return false;
	}
}

/* Bitwise/shift opcodes are int-only with no float branch at all, unlike
   binary_op()'s int/float dual path. It is kept as a separate helper
   rather than folded in there, since there is no float behavior to share.
   int64_t << or >> by a negative amount or by 64+ is undefined behavior
   in C; both are turned into a clean vm::shift-amount-out-of-range error
   instead, matching how division/modulo by zero are already handled
   rather than left as UB. A right shift of a negative left-hand side is
   left as this platform's implementation-defined (arithmetic,
   sign-extending) behavior. This is an accepted, documented pragmatism,
   the same class of choice already made for unchecked overflow on
   OP_ADD/OP_MULTIPLY elsewhere in this file. */
static bool bitwise_op(vm_t *vm, uint8_t op) {
	value_t b;
	value_t a;
	int64_t xi;
	int64_t yi;

	b = pop(vm);
	a = pop(vm);

	if (!value_is_int(a)) {
		vm_runtime_error(vm, "vm::bitwise-type-mismatch", "left operand must be an int");
		return false;
	}
	if (!value_is_int(b)) {
		vm_runtime_error(vm, "vm::bitwise-type-mismatch", "right operand must be an int");
		return false;
	}

	xi = value_as_int(a);
	yi = value_as_int(b);

	switch (op) {
		case OP_BIT_AND: push(vm, value_int(xi & yi)); return true;
		case OP_BIT_OR: push(vm, value_int(xi | yi)); return true;
		case OP_BIT_XOR: push(vm, value_int(xi ^ yi)); return true;
		case OP_SHIFT_LEFT:
			if (yi < 0 || yi >= 64) {
				vm_runtime_error(vm, "vm::shift-amount-out-of-range", "shift amount must be between 0 and 63, got %lld", (long long)yi);
				return false;
			}
			// A signed left shift is undefined behavior in C whenever the
			// mathematical result doesn't fit back in a non-negative
			// value of the same signed type (e.g. 1 << 63); shifting
			// through uint64_t and casting back sidesteps that entirely,
			// giving the plain two's-complement bit pattern this
			// operator is actually meant to produce.
			push(vm, value_int((int64_t)((uint64_t)xi << yi)));
			return true;
		case OP_SHIFT_RIGHT:
			if (yi < 0 || yi >= 64) {
				vm_runtime_error(vm, "vm::shift-amount-out-of-range", "shift amount must be between 0 and 63, got %lld", (long long)yi);
				return false;
			}
			push(vm, value_int(xi >> yi));
			return true;
		default:
			vm_runtime_error(vm, "vm::unsupported-operator", "unsupported operator");
			return false;
	}
}

static vm_result_t run(vm_t *vm, int stop_frame_count, value_t *out_result) {
	call_frame_t *frame;
	uint8_t instruction;
	uint8_t byte_a;
	uint16_t jump_offset;
	value_t a;
	value_t b;
	obj_string_t *name;
	value_t global_value;

	frame = &vm->frames[vm->frame_count - 1];

	for (;;) {
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->function->chunk.constants[READ_SHORT()])
#define READ_STRING() ((obj_string_t *)value_as_obj(READ_CONSTANT()))

		if ((size_t)(vm->stack_top - vm->stack) >= vm->stack_size) {
			vm_runtime_error(vm, "vm::stack-overflow", "stack overflow");
			return VM_RUNTIME_ERROR;
		}

		gc_note_safepoint(vm->heap);
		if (vm->gc_stress || vm->heap->bytes_allocated > vm->heap->next_gc) {
			gc_collect(vm);
		}

		instruction = READ_BYTE();

		switch (instruction) {
			case OP_CONSTANT:
				push(vm, READ_CONSTANT());
				break;
			case OP_NULL:
				push(vm, value_null());
				break;
			case OP_TRUE:
				push(vm, value_bool(true));
				break;
			case OP_FALSE:
				push(vm, value_bool(false));
				break;
			case OP_POP:
				pop(vm);
				break;
			case OP_GET_LOCAL:
				byte_a = READ_BYTE();
				push(vm, frame->slots[byte_a]);
				break;
			case OP_SET_LOCAL:
				byte_a = READ_BYTE();
				frame->slots[byte_a] = peek(vm, 0);
				break;
			case OP_GET_GLOBAL:
				name = READ_STRING();
				if (!table_get(frame->function->globals, name, &global_value)) {
					vm_runtime_error(vm, "vm::undefined-variable", "undefined variable '%.*s'", (int)name->length, name->chars);
					return VM_RUNTIME_ERROR;
				}
				push(vm, global_value);
				break;
			case OP_DEFINE_GLOBAL:
				name = READ_STRING();
				table_set(frame->function->globals, name, peek(vm, 0));
				pop(vm);
				break;
			case OP_SET_GLOBAL:
				// Leaves the assigned value on the stack (like
				// OP_SET_LOCAL), since assignment is itself an
				// expression. Always a name register_globals() already
				// proved exists, so no "undefined global" case here.
				name = READ_STRING();
				table_set(frame->function->globals, name, peek(vm, 0));
				break;
			case OP_GET_PROPERTY:
				name = READ_STRING();
				a = pop(vm);
				if (!vm_get_property(vm, name, a, &global_value)) {
					return VM_RUNTIME_ERROR;
				}
				push(vm, global_value);
				break;
			case OP_ARRAY: {
				obj_array_t *array;
				uint8_t count;
				uint8_t i;

				count = READ_BYTE();
				array = object_new_array(vm->heap);
				for (i = 0; i < count; i++) {
					object_array_push(vm->heap, array, vm->stack_top[-(int)count + i]);
				}
				vm->stack_top -= count;
				push(vm, value_obj((obj_t *)array));
				break;
			}
			case OP_GET_INDEX: {
				value_t result;

				b = pop(vm);
				a = pop(vm);
				if (!vm_get_index(vm, a, b, &result)) {
					return VM_RUNTIME_ERROR;
				}
				push(vm, result);
				break;
			}
			case OP_SET_INDEX: {
				value_t value;
				value_t index_value;
				value_t array_value;
				value_t result;

				value = pop(vm);
				index_value = pop(vm);
				array_value = pop(vm);
				if (!vm_set_index(vm, array_value, index_value, value, &result)) {
					return VM_RUNTIME_ERROR;
				}
				push(vm, result);
				break;
			}
			case OP_STRUCT: {
				obj_struct_t *instance;
				obj_string_t *type_name;
				uint8_t count;
				uint8_t i;

				type_name = READ_STRING();
				count = READ_BYTE();
				instance = object_new_struct(vm->heap, type_name, count);
				for (i = 0; i < count; i++) {
					instance->fields[i] = vm->stack_top[-(int)count + i];
				}
				vm->stack_top -= count;
				push(vm, value_obj((obj_t *)instance));
				break;
			}
			// No type or bounds check: unlike OP_GET_INDEX/OP_SET_INDEX
			// (a runtime int that could be out of range), a field index
			// here is a compile-time constant the compiler only ever
			// emits after confirming the object on the stack is an
			// instance of exactly the struct type that field belongs to.
			case OP_GET_FIELD: {
				obj_struct_t *instance;

				byte_a = READ_BYTE();
				a = pop(vm);
				instance = (obj_struct_t *)value_as_obj(a);
				push(vm, instance->fields[byte_a]);
				break;
			}
			case OP_SET_FIELD: {
				obj_struct_t *instance;
				value_t field_value;
				value_t target;

				byte_a = READ_BYTE();
				field_value = pop(vm);
				target = pop(vm);
				instance = (obj_struct_t *)value_as_obj(target);
				instance->fields[byte_a] = field_value;
				push(vm, field_value);
				break;
			}
			case OP_EQUAL:
				b = pop(vm);
				a = pop(vm);
				push(vm, value_bool(value_equals(a, b)));
				break;
			case OP_NOT_EQUAL:
				b = pop(vm);
				a = pop(vm);
				push(vm, value_bool(!value_equals(a, b)));
				break;
			case OP_LESS:
			case OP_LESS_EQUAL:
			case OP_GREATER:
			case OP_GREATER_EQUAL:
			case OP_ADD:
			case OP_SUBTRACT:
			case OP_MULTIPLY:
			case OP_DIVIDE:
			case OP_MODULO:
				if (!binary_op(vm, instruction)) {
					return VM_RUNTIME_ERROR;
				}
				break;
			case OP_BIT_AND:
			case OP_BIT_OR:
			case OP_BIT_XOR:
			case OP_SHIFT_LEFT:
			case OP_SHIFT_RIGHT:
				if (!bitwise_op(vm, instruction)) {
					return VM_RUNTIME_ERROR;
				}
				break;
			case OP_NOT:
				push(vm, value_bool(value_is_falsey(pop(vm))));
				break;
			case OP_NEGATE:
				a = pop(vm);
				if (value_is_int(a)) {
					push(vm, value_int(-value_as_int(a)));
				} else if (value_is_float(a)) {
					push(vm, value_float(-value_as_float(a)));
				} else {
					vm_runtime_error(vm, "vm::non-numeric-operand", "operand of unary '-' must be a number");
					return VM_RUNTIME_ERROR;
				}
				break;
			case OP_BIT_NOT:
				a = pop(vm);
				if (value_is_int(a)) {
					push(vm, value_int(~value_as_int(a)));
				} else {
					vm_runtime_error(vm, "vm::bitwise-type-mismatch", "operand of unary '~' must be an int");
					return VM_RUNTIME_ERROR;
				}
				break;
			case OP_DUP_N: {
				uint8_t count;
				uint8_t i;

				count = READ_BYTE();
				// stack_top[-count] is read fresh each iteration, after
				// the previous copy's push already moved stack_top.
				// That shift is exactly what keeps landing on the next
				// original element in order, without needing a separate
				// base pointer.
				for (i = 0; i < count; i++) {
					push(vm, vm->stack_top[-(int)count]);
				}
				break;
			}
			case OP_JUMP:
				jump_offset = READ_SHORT();
				frame->ip += jump_offset;
				break;
			case OP_JUMP_IF_FALSE:
				jump_offset = READ_SHORT();
				if (value_is_falsey(peek(vm, 0))) {
					frame->ip += jump_offset;
				}
				break;
			case OP_LOOP:
				jump_offset = READ_SHORT();
				frame->ip -= jump_offset;
				jit_note_loop_iteration(frame->function);
				break;
			case OP_CLOSURE: {
				obj_function_t *function;
				uint8_t upvalue_count;
				value_t closure_value;

				function = (obj_function_t *)value_as_obj(READ_CONSTANT());
				upvalue_count = READ_BYTE();
				closure_value = vm_make_closure(vm, frame, function, frame->ip, upvalue_count);
				frame->ip += (size_t)upvalue_count * 2;
				push(vm, closure_value);
				break;
			}
			case OP_GET_UPVALUE:
				byte_a = READ_BYTE();
				push(vm, *frame->closure->upvalues[byte_a]->location);
				break;
			case OP_SET_UPVALUE:
				byte_a = READ_BYTE();
				*frame->closure->upvalues[byte_a]->location = peek(vm, 0);
				break;
			case OP_CLOSE_UPVALUE:
				close_upvalues(vm, vm->stack_top - 1);
				pop(vm);
				break;
			case OP_CALL:
				byte_a = READ_BYTE();
				if (!call_value(vm, peek(vm, byte_a), byte_a)) {
					return VM_RUNTIME_ERROR;
				}
				frame = &vm->frames[vm->frame_count - 1];
				break;
			case OP_RETURN: {
				value_t result = pop(vm);

				close_upvalues(vm, frame->slots);
				vm->frame_count--;
				if (vm->frame_count == stop_frame_count) {
					// frame->slots means two different things depending
					// on how this frame was set up, so unwinding it
					// correctly needs two different formulas: an
					// ordinary call (call_value()'s OBJ_FUNCTION/
					// OBJ_CLOSURE case, always how a reentrant
					// vm_call_value() call gets here too) sets
					// frame->slots to one *past* the callee's own stack
					// slot (frame->slots - arg_count sits at the first
					// argument), so frame->slots - 1 is the callee's
					// slot. The very first frame vm_run() itself sets
					// up is different: frame->slots *is* vm->stack, the
					// callee's own slot directly, with no separate
					// "past the callee" offset, so unwinding it needs
					// frame->slots, not frame->slots - 1. That special
					// case is exactly stop_frame_count == 0: a
					// reentrant call's stop_frame_count is always the
					// frame count at the moment some already-running
					// native called vm_call_value(), which can never be
					// 0 (nothing calls a native before vm_run() has
					// already pushed at least one frame).
					vm->stack_top = stop_frame_count == 0 ? frame->slots : frame->slots - 1;
					if (out_result != NULL) {
						*out_result = result;
					}
					return VM_OK;
				}
				vm->stack_top = frame->slots - 1;
				push(vm, result);
				frame = &vm->frames[vm->frame_count - 1];
				break;
			}
			case OP_TRY_CALL:
				jump_offset = READ_SHORT();
				if (!vm_try_call(vm)) {
					frame->ip += jump_offset;
				}
				break;
			case OP_THROW: {
				obj_string_t *thrown;

				thrown = (obj_string_t *)value_as_obj(pop(vm));
				vm_runtime_error(vm, "user::thrown", "%.*s", (int)thrown->length, thrown->chars);
				return VM_RUNTIME_ERROR;
			}
			default:
				vm_runtime_error(vm, "vm::corrupt-bytecode", "corrupt bytecode: unknown opcode %d", instruction);
				return VM_RUNTIME_ERROR;
		}

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
	}
}

void vm_init(vm_t *vm, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, size_t stack_size, int max_frames) {
	if (stack_size < VM_MIN_STACK_SIZE) {
		stack_size = VM_MIN_STACK_SIZE;
	}
	if (max_frames < 1) {
		max_frames = 1;
	}

	vm->stack_size = stack_size;
	vm->stack = malloc(sizeof(value_t) * (stack_size + VM_STACK_SAFETY_MARGIN));
	vm->stack_top = vm->stack;

	vm->max_frames = max_frames;
	vm->frames = malloc(sizeof(call_frame_t) * (size_t)max_frames);
	vm->frame_count = 0;

	vm->file_globals = NULL;
	vm->file_globals_count = 0;
	vm->file_globals_capacity = 0;

	vm->owned_sources = NULL;
	vm->owned_sources_count = 0;
	vm->owned_sources_capacity = 0;

	vm->open_upvalues = NULL;

	vm->heap = heap;
	vm->strings = strings;
	vm->gc_stress = gc_stress_enabled();
	vm->filename = filename;
	vm->source = source;
}

table_t *vm_new_file_globals(vm_t *vm) {
	table_t *globals;

	globals = malloc(sizeof(table_t));
	table_init(globals);
	stdlib_install(vm, globals);

	if (vm->file_globals_count == vm->file_globals_capacity) {
		vm->file_globals_capacity = vm->file_globals_capacity < 4 ? 4 : vm->file_globals_capacity * 2;
		vm->file_globals = realloc(vm->file_globals, vm->file_globals_capacity * sizeof(table_t *));
	}
	vm->file_globals[vm->file_globals_count++] = globals;

	return globals;
}

void vm_track_owned_source(vm_t *vm, char *path, char *source) {
	if (vm->owned_sources_count == vm->owned_sources_capacity) {
		vm->owned_sources_capacity = vm->owned_sources_capacity < 4 ? 4 : vm->owned_sources_capacity * 2;
		vm->owned_sources = realloc(vm->owned_sources, vm->owned_sources_capacity * sizeof(vm_owned_source_t));
	}
	vm->owned_sources[vm->owned_sources_count].path = path;
	vm->owned_sources[vm->owned_sources_count].source = source;
	vm->owned_sources_count++;
}

void vm_free(vm_t *vm) {
	size_t i;

	for (i = 0; i < vm->file_globals_count; i++) {
		table_free(vm->file_globals[i]);
		free(vm->file_globals[i]);
	}
	free(vm->file_globals);

	for (i = 0; i < vm->owned_sources_count; i++) {
		free(vm->owned_sources[i].path);
		free(vm->owned_sources[i].source);
	}
	free(vm->owned_sources);

	free(vm->stack);
	free(vm->frames);
}

vm_result_t vm_run(vm_t *vm, obj_function_t *script) {
	call_frame_t *frame;

	push(vm, value_obj((obj_t *)script));

	frame = &vm->frames[vm->frame_count];
	vm->frame_count++;
	frame->function = script;
	frame->closure = NULL;
	frame->ip = script->chunk.code;
	frame->slots = vm->stack;

	return run(vm, 0, NULL);
}

bool vm_call_value(vm_t *vm, value_t callee, int arg_count, const value_t *args, value_t *out_result) {
	int stop_frame_count;
	int i;

	// The per-instruction stack-overflow check inside run()'s own loop
	// only runs once *between* instructions, relying on
	// VM_STACK_SAFETY_MARGIN as slack for whatever a single instruction
	// pushes, always small and fixed. This pushes callee + up to 255
	// args in one go, before run() ever gets a chance to check again,
	// so it needs its own guard rather than trusting that margin.
	if ((size_t)(vm->stack_top - vm->stack) + (size_t)(arg_count + 1) > vm->stack_size) {
		vm_runtime_error(vm, "vm::stack-overflow", "stack overflow");
		return false;
	}

	stop_frame_count = vm->frame_count;

	push(vm, callee);
	for (i = 0; i < arg_count; i++) {
		push(vm, args[i]);
	}

	if (!call_value(vm, callee, arg_count)) {
		return false;
	}

	if (vm->frame_count == stop_frame_count) {
		// call_value()'s OBJ_NATIVE case runs synchronously in C, with
		// no new frame pushed. Its result is already sitting on the
		// stack top, same as any other native call's result would be.
		*out_result = pop(vm);
		return true;
	}

	return run(vm, stop_frame_count, out_result) == VM_OK;
}

/* Thin public wrapper around close_upvalues() so JIT-generated code
   (src/jit/jit_compile.c's OP_RETURN translation) can call it as a
   plain C helper, the same way it's already called from OP_RETURN's
   and OP_CLOSE_UPVALUE's interpreted handlers above. */
void vm_close_upvalues_from(vm_t *vm, value_t *last) {
	close_upvalues(vm, last);
}

/* Thin public wrappers around binary_op()/bitwise_op() so JIT-generated
   code can fall back to them directly for anything its own inlined
   both-int/both-float fast path doesn't cover (mixed types, wrong
   types, division/modulo by zero, an out-of-range shift amount).
   Exactly the same slow path run()'s own OP_ADD..OP_SHIFT_RIGHT cases
   already call. */
bool vm_binary_op(vm_t *vm, uint8_t op) {
	return binary_op(vm, op);
}

/* Extracted from OP_GET_INDEX/OP_SET_INDEX's own case bodies (originally
   inline in run()'s switch) so both the interpreter and JIT-generated
   code (src/jit/jit_compile.c) share the exact same indexing semantics.
   A behavior-neutral refactor, not a change in what either opcode
   does. On success, writes the result through out_result and returns
   true; on failure, an error has already been reported via
   vm_runtime_error() and returns false, same convention as
   vm_binary_op()/vm_bitwise_op(). */
bool vm_get_index(vm_t *vm, value_t container, value_t index_value, value_t *out_result) {
	obj_array_t *array;
	obj_string_t *string;
	obj_string_t *ch;
	int64_t index;

	if (!value_is_int(index_value)) {
		vm_runtime_error(vm, "vm::index-not-integer", "index must be an int");
		return false;
	}
	index = value_as_int(index_value);

	if (value_is_obj(container) && value_as_obj(container)->type == OBJ_ARRAY) {
		array = (obj_array_t *)value_as_obj(container);
		if (index < 0 || (size_t)index >= array->count) {
			vm_runtime_error(vm, "vm::index-out-of-bounds", "array index %lld out of bounds for length %zu", (long long)index, array->count);
			return false;
		}
		*out_result = array->values[index];
		return true;
	}

	if (value_is_obj(container) && value_as_obj(container)->type == OBJ_STRING) {
		string = (obj_string_t *)value_as_obj(container);
		if (index < 0 || (size_t)index >= string->length) {
			vm_runtime_error(vm, "vm::index-out-of-bounds", "string index %lld out of bounds for length %zu", (long long)index, string->length);
			return false;
		}
		ch = object_new_string(vm->heap, vm->strings, string->chars + index, 1);
		*out_result = value_obj((obj_t *)ch);
		return true;
	}

	vm_runtime_error(vm, "vm::not-indexable", "'[]' access only works on arrays and strings");
	return false;
}

bool vm_set_index(vm_t *vm, value_t container, value_t index_value, value_t value, value_t *out_result) {
	obj_array_t *array;
	int64_t index;

	if (value_is_obj(container) && value_as_obj(container)->type == OBJ_STRING) {
		vm_runtime_error(vm, "vm::string-immutable", "cannot assign into a string index; strings are immutable");
		return false;
	}
	if (!value_is_obj(container) || value_as_obj(container)->type != OBJ_ARRAY) {
		vm_runtime_error(vm, "vm::not-indexable", "'[]' access only works on arrays and strings");
		return false;
	}
	if (!value_is_int(index_value)) {
		vm_runtime_error(vm, "vm::index-not-integer", "index must be an int");
		return false;
	}

	array = (obj_array_t *)value_as_obj(container);
	index = value_as_int(index_value);
	if (index < 0 || (size_t)index >= array->count) {
		vm_runtime_error(vm, "vm::index-out-of-bounds", "array index %lld out of bounds for length %zu", (long long)index, array->count);
		return false;
	}

	array->values[index] = value;
	*out_result = value;
	return true;
}

/* Thin public wrapper around capture_upvalue() for the same reason as
   vm_close_upvalues_from(). JIT-generated code (vm_make_closure()
   below) needs to call it as a plain C helper. */
obj_upvalue_t *vm_capture_upvalue(vm_t *vm, value_t *local) {
	return capture_upvalue(vm, local);
}

/* Builds a closure over function, capturing upvalue_count upvalues as
   described by descriptors: upvalue_count pairs of is_local/index
   bytes, exactly OP_CLOSURE's own variable-length bytecode encoding
   (see chunk.h). Shared between run()'s own OP_CLOSURE case (which
   passes frame->ip, still sitting at the first descriptor byte after
   READ_BYTE() consumed upvalue_count) and its JIT translation
   (src/jit/jit_compile.c, which passes a baked address into the
   function's own chunk->code. These descriptor bytes are fixed once
   compiled, same treatment as OP_CONSTANT's baked constant-pool
   addresses), so both build closures identically. Can allocate
   (object_new_closure(), and capture_upvalue() the first time a given
   local is captured) but never fails; is_local/index are always
   compile-time-valid by construction, so there's no error path to
   report, unlike every other vm_*() helper in this file. */
value_t vm_make_closure(vm_t *vm, call_frame_t *frame, obj_function_t *function, const uint8_t *descriptors, uint8_t upvalue_count) {
	obj_closure_t *closure;
	uint8_t i;
	uint8_t is_local;
	uint8_t index;

	closure = object_new_closure(vm->heap, function, upvalue_count);
	for (i = 0; i < upvalue_count; i++) {
		is_local = descriptors[i * 2];
		index = descriptors[i * 2 + 1];
		if (is_local) {
			closure->upvalues[i] = capture_upvalue(vm, &frame->slots[index]);
		} else {
			closure->upvalues[i] = frame->closure->upvalues[index];
		}
	}
	return value_obj((obj_t *)closure);
}

/* Extracted from OP_GET_PROPERTY's own case body, same reasoning as
   vm_get_index()/vm_set_index() above. Never allocates. */
bool vm_get_property(vm_t *vm, obj_string_t *name, value_t object, value_t *out_result) {
	obj_module_t *module;

	if (!value_is_obj(object) || value_as_obj(object)->type != OBJ_MODULE) {
		vm_runtime_error(vm, "vm::unsupported-member-access", "'.' access only works on modules right now");
		return false;
	}

	module = (obj_module_t *)value_as_obj(object);
	if (!table_get(module->members, name, out_result)) {
		vm_runtime_error(vm, "vm::undefined-module-member", "module '%.*s' has no member '%.*s'", (int)module->name->length, module->name->chars, (int)name->length, name->chars);
		return false;
	}
	return true;
}

/* The JIT translation of OP_CALL: the callee and its arg_count
   arguments are already sitting on vm's stack (JIT-generated code
   pushed them exactly like the interpreted opcodes that would produce
   them), same precondition OP_CALL's own interpreted handler has, so
   this needs no separate args array the way vm_call_value() (built for
   a *native* calling back into paweli with a C array of arguments)
   does. Otherwise structurally identical to vm_call_value(): call_value()
   itself already knows how to dispatch to a native (runs synchronously,
   frame_count unchanged), a JIT-compiled paweli function/closure (also
   runs synchronously to completion via its own jit_entry, frame_count
   net unchanged. See call_value()'s OBJ_FUNCTION/OBJ_CLOSURE cases
   above), or a not-yet-JIT'd one (pushes a frame and returns, needing
   run() to actually drive it). This only has to tell those last two
   cases apart the same way vm_call_value() already does, by checking
   whether frame_count actually changed. */
bool vm_jit_call(vm_t *vm, int arg_count) {
	int stop_frame_count;
	value_t callee;
	value_t result;

	stop_frame_count = vm->frame_count;
	callee = peek(vm, arg_count);

	if (!call_value(vm, callee, arg_count)) {
		return false;
	}

	if (vm->frame_count == stop_frame_count) {
		return true;
	}

	// Unlike the frame_count-unchanged case above, run()'s own
	// reentrant stop condition (OP_RETURN's handler, once frame_count
	// has unwound back to stop_frame_count) does NOT push the return
	// value back onto vm's stack. It only ever hands it back via
	// *out_result, since vm_call_value()'s own callers (a native
	// wanting a plain C value_t) never needed it on the stack too. A
	// JIT-generated OP_CALL does need it there, matching what
	// interpreted OP_CALL's own callers already see, so it has to be
	// pushed explicitly here.
	if (run(vm, stop_frame_count, &result) != VM_OK) {
		return false;
	}
	push(vm, result);
	return true;
}

bool vm_bitwise_op(vm_t *vm, uint8_t op) {
	return bitwise_op(vm, op);
}

/* The shared "attempt the call" phase behind OP_TRY_CALL, extracted out
   of run()'s own case body the same way vm_get_index()/vm_get_property()
   etc. were, and (per that same extraction) used by both run()'s
   interpreted handler and its JIT translation
   (src/jit/jit_compile.c). The try-block's 0-arg closure is already on
   the stack (peek(vm,0)), same precondition the interpreted handler
   always had.

   On success: leaves the stack exactly where it was before the closure
   was pushed (the closure itself is discarded, unlike a normal call
   which leaves the result where the callee sat), with the call's result
   pushed there instead, and returns true.

   On failure: unwinds frame_count/stack_top back to that same point,
   closes every upvalue the attempt opened, and pushes the error message
   as a string in place of a result, then returns false. The caller
   (run()'s own case, or JIT-generated code) branches on this return
   value to decide whether to fall through (success) or jump to the
   catch handler (failure); either way, something is already correctly
   sitting on the stack for the code at the landing point to use, so
   neither caller needs any further stack bookkeeping of its own.

   Structurally identical to call_value() dispatching an OBJ_CLOSURE
   then vm_jit_call()'s own sync-vs-reentrant check (see its comment).
   The try-block closure may now complete synchronously via
   jit_entry rather than needing run() to drive it, exactly like any
   other closure call since Phase (b). */
bool vm_try_call(vm_t *vm) {
	value_t *reset_stack_top;
	int stop_frame_count;
	value_t try_result;
	bool ok;
	obj_string_t *message;

	reset_stack_top = vm->stack_top - 1;
	stop_frame_count = vm->frame_count;

	ok = call_value(vm, peek(vm, 0), 0);
	if (ok) {
		if (vm->frame_count == stop_frame_count) {
			try_result = pop(vm);
		} else {
			ok = run(vm, stop_frame_count, &try_result) == VM_OK;
		}
	}

	if (ok) {
		vm->stack_top = reset_stack_top;
		push(vm, try_result);
		return true;
	}

	close_upvalues(vm, reset_stack_top);
	vm->frame_count = stop_frame_count;
	vm->stack_top = reset_stack_top;
	message = object_new_string(vm->heap, vm->strings, vm->error_message, strlen(vm->error_message));
	push(vm, value_obj((obj_t *)message));
	return false;
}
