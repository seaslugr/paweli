#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "compiler/chunk.h"
#include "diagnostics/diagnostic.h"
#include "stdlib/stdlib.h"
#include "vm.h"

// Extra slack allocated past the configured stack_size. run()'s overflow
// check runs once per instruction dispatch, and no single instruction
// grows the stack by more than one slot. This margin only needs to
// cover that one-instruction detection lag.
#define VM_STACK_SAFETY_MARGIN 64

static void push(vm_t *vm, value_t value);
static value_t pop(vm_t *vm);
static value_t peek(vm_t *vm, int distance);
static bool call_value(vm_t *vm, value_t callee, int arg_count);
static bool binary_op(vm_t *vm, uint8_t op);
static vm_result_t run(vm_t *vm);

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
	char message[1024];
	int i;
	call_frame_t *frame;
	size_t instruction;
	int line;
	int column;

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	line = 0;
	column = 0;
	if (vm->frame_count > 0) {
		frame = &vm->frames[vm->frame_count - 1];
		instruction = (size_t)(frame->ip - frame->function->chunk.code) - 1;
		line = frame->function->chunk.lines[instruction];
		column = frame->function->chunk.columns[instruction];
	}
	diagnostic_emit(DIAG_ERROR, code, vm->filename, vm->source, line, column, "%s", message);

	for (i = vm->frame_count - 1; i >= 0; i--) {
		frame = &vm->frames[i];
		instruction = (size_t)(frame->ip - frame->function->chunk.code) - 1;
		line = frame->function->chunk.lines[instruction];

		if (frame->function->name == NULL) {
			fprintf(stderr, "%s:%d: in <script>\n", vm->filename, line);
		} else {
			fprintf(stderr, "%s:%d: in %.*s()\n", vm->filename, line, (int)frame->function->name->length, frame->function->name->chars);
		}
	}
}

static bool call_value(vm_t *vm, value_t callee, int arg_count) {
	obj_function_t *function;
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
			frame->ip = function->chunk.code;
			frame->slots = vm->stack_top - arg_count;
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
			vm_runtime_error(vm, "vm::not-callable", "can only call functions");
			return false;
	}

	vm_runtime_error(vm, "vm::not-callable", "can only call functions");
	return false;
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

static vm_result_t run(vm_t *vm) {
	call_frame_t *frame;
	uint8_t instruction;
	uint8_t byte_a;
	uint16_t jump_offset;
	value_t a;
	value_t b;
	obj_string_t *name;
	value_t global_value;
	obj_module_t *module;

	frame = &vm->frames[vm->frame_count - 1];

	for (;;) {
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->function->chunk.constants[READ_BYTE()])
#define READ_STRING() ((obj_string_t *)value_as_obj(READ_CONSTANT()))

		if ((size_t)(vm->stack_top - vm->stack) >= vm->stack_size) {
			vm_runtime_error(vm, "vm::stack-overflow", "stack overflow");
			return VM_RUNTIME_ERROR;
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
				if (!table_get(&vm->globals, name, &global_value)) {
					vm_runtime_error(vm, "vm::undefined-variable", "undefined variable '%.*s'", (int)name->length, name->chars);
					return VM_RUNTIME_ERROR;
				}
				push(vm, global_value);
				break;
			case OP_DEFINE_GLOBAL:
				name = READ_STRING();
				table_set(&vm->globals, name, peek(vm, 0));
				pop(vm);
				break;
			case OP_GET_PROPERTY:
				name = READ_STRING();
				a = pop(vm);

				if (!value_is_obj(a) || value_as_obj(a)->type != OBJ_MODULE) {
					vm_runtime_error(vm, "vm::unsupported-member-access", "'.' access only works on modules right now");
					return VM_RUNTIME_ERROR;
				}

				module = (obj_module_t *)value_as_obj(a);
				if (!table_get(module->members, name, &global_value)) {
					vm_runtime_error(vm, "vm::undefined-module-member", "module '%.*s' has no member '%.*s'", (int)module->name->length, module->name->chars, (int)name->length, name->chars);
					return VM_RUNTIME_ERROR;
				}
				push(vm, global_value);
				break;
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

				vm->frame_count--;
				if (vm->frame_count == 0) {
					pop(vm);
					return VM_OK;
				}
				vm->stack_top = frame->slots - 1;
				push(vm, result);
				frame = &vm->frames[vm->frame_count - 1];
				break;
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

void vm_init(vm_t *vm, obj_t **objects, table_t *strings, const char *filename, const char *source, size_t stack_size, int max_frames) {
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

	table_init(&vm->globals);
	vm->objects = objects;
	vm->strings = strings;
	vm->filename = filename;
	vm->source = source;

	stdlib_install(vm);
}

void vm_free(vm_t *vm) {
	table_free(&vm->globals);
	free(vm->stack);
	free(vm->frames);
}

vm_result_t vm_run(vm_t *vm, obj_function_t *script) {
	call_frame_t *frame;

	push(vm, value_obj((obj_t *)script));

	frame = &vm->frames[vm->frame_count];
	vm->frame_count++;
	frame->function = script;
	frame->ip = script->chunk.code;
	frame->slots = vm->stack;

	return run(vm);
}
