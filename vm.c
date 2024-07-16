#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "value.h"
#include "object.h"
#include "memory.h"
#include "vm.h"

VM vm;
static Value clockNative(int argCount, Value* args) {
  return INT_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

void printStack(VM* vm){
  printf("Printing stack: ");
  for(Value* slot = vm->stack; slot< vm->stackTop; slot++){
      printValue(*slot);
      printf("  ");
  }
  printf("\n");
}

static void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame* frame = &vm.frames[i];
    ObjFunction* function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }
  resetStack();
}

static void defineNative(const char* name, NativeFn function) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

void initVM() {
  resetStack();
  vm.objects = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;
  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;
  initTable(&vm.globals);
  initTable(&vm.strings);
  for (int i = 0; i < STACK_MAX; i++) {
        vm.stack[i] = NIL_VAL;
    }
  vm.initString = copyString("init", 4);
  defineNative("clock", clockNative);
}

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  vm.initString = NULL;
  freeObjects();
}

void push(Value value) {
    *vm.stackTop = value;
    vm.stackTop++;
}

Value pop() {
    vm.stackTop--;
    return *vm.stackTop;
}

static Value peek(int distance) {
  return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
  if (argCount != closure->function->arity) {
    runtimeError("Expected %d arguments but got %d.",
        closure->function->arity, argCount);
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}


static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        vm.stackTop[-argCount - 1] = bound->receiver;
        return call(bound->method, argCount);
      }
      case OBJ_CLASS: {
        ObjClass* klass = AS_CLASS(callee);
        vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
        Value initializer;
        if (tableGet(&klass->methods, vm.initString, &initializer)) {
          return call(AS_CLOSURE(initializer), argCount);
        } else if (argCount != 0) {
          runtimeError("Expected 0 arguments but got %d.", argCount);
          return false;
        }
        return true;
      }
      case OBJ_CLOSURE:
        return call(AS_CLOSURE(callee), argCount);
      case OBJ_NATIVE: {
        NativeFn native = AS_NATIVE(callee);
        Value result = native(argCount, vm.stackTop - argCount);
        vm.stackTop -= argCount + 1;
        push(result);
        return true;
      }
      default:
        break; // Non-callable object type.
    }
  }
  runtimeError("Can only call functions and classes.");
  return false;
}
static bool invokeFromClass(ObjClass* klass, ObjString* name,int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }
  return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString* name, int argCount) {
  Value receiver = peek(argCount);
  if (!IS_INSTANCE(receiver)) {
    runtimeError("Only instances have methods.");
    return false;
  }
  ObjInstance* instance = AS_INSTANCE(receiver);
  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    vm.stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }
  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass* klass, ObjString* name) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }
  ObjBoundMethod* bound = newBoundMethod(peek(0),AS_CLOSURE(method));
  pop();
  push(OBJ_VAL(bound));
  return true;
}

static ObjUpvalue* captureUpvalue(Value* local) {
  ObjUpvalue* prevUpvalue = NULL;
  ObjUpvalue* upvalue = vm.openUpvalues;
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }
  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjUpvalue* createdUpvalue = newUpvalue(local);
  createdUpvalue->next = upvalue;
  if (prevUpvalue == NULL) {
    vm.openUpvalues = createdUpvalue;
  } else {
    prevUpvalue->next = createdUpvalue;
  }
  return createdUpvalue;
}

static void closeUpvalues(Value* last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue* upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

static void defineMethod(ObjString* name) {
  Value method = peek(0);
  ObjClass* klass = AS_CLASS(peek(1));
  tableSet(&klass->methods, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
  ObjString* b = AS_STRING(peek(0));
  ObjString* a = AS_STRING(peek(1));
  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';
  ObjString* result = takeString(chars, length);
  pop();
  pop();
  push(OBJ_VAL(result));
}

static InterpretResult run() {
  CallFrame* frame = &vm.frames[vm.frameCount - 1];
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2,(uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
// #define BINARY_OP(valueType, op) \
//     do { if (!IS_OBJ(peek(0)) || !IS_OBJ(peek(1))) { runtimeError("From BINARY_OP_PRINT. Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; } \
//       char b = AS_OBJ(pop()); char a = AS_OBJ(pop()); push(valueType(a op b)); } while (false) ;
#define BINARY_OP_INT(valueType, op) \
    do { if (!IS_INT(peek(0)) || !IS_INT(peek(1))) { runtimeError("From BINARY_OP_INT. Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; } \
      double b = AS_INT(pop()); double a = AS_INT(pop()); push(valueType(a op b)); } while (false) ;
#define BINARY_OP_FLOAT(valueType, op) \
    do { if (!IS_FLOAT(peek(0)) || !IS_FLOAT(peek(1))) { runtimeError("From BINARY_OP_FLOAT. Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; } \
      double b = AS_FLOAT(pop()); double a = AS_FLOAT(pop()); push(valueType(a op b)); \
    } while (false)
  for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    printStack(&vm);
    disassembleInstruction(&frame->closure->function->chunk,(int)(frame->ip - frame->closure->function->chunk.code)); 
#endif

    uint8_t instruction;
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT: {
        printf("OP_CONSTANT\n");
        Value constant = READ_CONSTANT();
        printf("%s", constant);
        push(constant);
        printf("Pushed constant: %s", constant);
        break;
      }
      case OP_CONSTANT_INT: {
        printf("OP_CONSTANT_INT\n");
        int value = AS_INT(READ_CONSTANT());
        push(INT_VAL(value));
        //printf("Pushed constant: ");
        //printValue(INT_VAL(value));
        printf("\n");
        break;
      }
      case OP_CONSTANT_FLOAT: {
        printf("OP_CONSTANT_FLOAT\n");
        double value = AS_FLOAT(READ_CONSTANT());
        push(FLOAT_VAL(value));
        //printf("Pushed constant: ");
        //printValue(FLOAT_VAL(value));
        printf("\n");
        break;
      }
      case OP_CONSTANT_STRING: {
        printf("OP_CONSTANT_STRING\n");
        ObjString* string = AS_STRING(READ_CONSTANT());
        push(OBJ_VAL(string));
        if ((string)->type == VAL_OBJ) {
            runtimeError("Expected a string");
            break;
        }
        break;
      }
      case OP_NIL: push(NIL_VAL); break;
      case OP_TRUE: push(BOOL_VAL(true)); break;
      case OP_FALSE: push(BOOL_VAL(false)); break;
      case OP_POP: pop(); break;
      case OP_GET_LOCAL: {
        uint8_t slot = READ_BYTE();
        push(frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL: {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peek(0);
        break;
      }
      case OP_GET_GLOBAL: {
        ObjString* name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, name, &value)) {
            runtimeError("Undefined variable '%s'.", name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }
        //printf("After tableGet, value type: %d\n", value.type);
        if (value.type == VAL_FLOAT) {
            //printf("Float value: %f\n", value.as.float_val);
        }
        push(value);
        break;
      }
   case OP_SET_GLOBAL: {
        printf("OP_SET_GLOBAL\n");
        // ObjString* name = READ_STRING();
        // printf("%s", name);
        // if (tableSet(&vm.globals, name, peek(0))) {
        //   printf("value being deleted is: ", name);
        //   tableDelete(&vm.globals, name); // [delete]
        //   runtimeError("Undefined variable '%s'.", name->chars);
        //   return INTERPRET_RUNTIME_ERROR;
        // }
        break;
      }
      case OP_DEFINE_GLOBAL: {
        printf("entered OP_DEFINE_GLOBAL\n");
        printf("OP_DEFINE_GLOBAL\n");
        ObjString* name = READ_STRING();
        printf("Defining global: %s = ", name->chars);
        printValue(peek(0));
        printf("\n");
        tableSet(&vm.globals, name, peek(0));
        pop();
        break;
      }
      case OP_GET_GLOBAL_INT: {
        printf("OP_GET_GLOBAL_INT");
        ObjString* name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, name, &value)) {
            runtimeError("Undefined variable '%s'.", name->chars);
            break;
        }
        if (!IS_INT(value)) {
            runtimeError("Expected int value for variable '%s'.", name->chars);
            break;
        }
        push(value);
        break;
    }
      case OP_GET_GLOBAL_FLOAT: {
        ObjString* name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, name, &value)) {
            runtimeError("Undefined variable '%s'.", name->chars);
            break;
        }
        if (!IS_FLOAT(value)) {
            runtimeError("Expected float value for variable '%s'.", name->chars);
            break;
        }
        push(value);
        break;
    }
      case OP_GET_GLOBAL_STRING: {
        ObjString* name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, name, &value)) {
            runtimeError("Undefined variable '%s'.", name->chars);
            break;
        }
        if (!IS_STRING(value)) {
            runtimeError("Expected string value for variable '%s'.", name->chars);
            break;
        }
        push(value);
        break;
    }
      case OP_DEFINE_GLOBAL_INT: {
          printf("OP_DEFINE_GLOBAL_INT\n");
          ObjString* name = READ_STRING();
          Value value = pop();
          // if (!IS_INT(value)) {
          //   runtimeError("Cannot assign non-integer value to int variable.");
          //   return INTERPRET_RUNTIME_ERROR;
          // }
          tableSet(&vm.globals, name, value);
          break;
      }
      case OP_DEFINE_GLOBAL_FLOAT: {
        printf("OP_DEFINE_GLOBAL_FLOAT\n");
          ObjString* name = READ_STRING();
          Value value = pop();
          if (!IS_FLOAT(value)) {
            runtimeError("Cannot assign non-float value to float variable.");
            return INTERPRET_RUNTIME_ERROR;
          }
          tableSet(&vm.globals, name, value);
          break;
      }
      case OP_DEFINE_GLOBAL_STRING: {
        printf("OP_DEFINE_GLOBAL_STRING\n");
          ObjString* name = READ_STRING();
          Value value = pop();
          if (!IS_STRING(value)) {
            runtimeError("Cannot assign non-string value to string variable.");
            return INTERPRET_RUNTIME_ERROR;
          }
          tableSet(&vm.globals, name, value);
          break;
      }
      case OP_SET_GLOBAL_INT: {
          ObjString* name = READ_STRING();
          Value value = pop();
          if (!IS_INT(value)) {
              runtimeError("Expected int value for variable '%s'.", name->chars);
              break;
          }
          tableSet(&vm.globals, name, value);
          break;
      }
      case OP_SET_GLOBAL_FLOAT: {
          ObjString* name = READ_STRING();
          Value value = pop();
          if (!IS_FLOAT(value)) {
              runtimeError("Expected float value for variable '%s'.", name->chars);
              break;
          }
          tableSet(&vm.globals, name, value);
          break;
      }
      case OP_SET_GLOBAL_STRING: {
        ObjString* name = READ_STRING();
        Value value = pop();
        if (!IS_STRING(value)) {
            runtimeError("Expected string value for variable '%s'.", name->chars);
            break;
        }
        tableSet(&vm.globals, name, value);
        break;
    }
      case OP_GET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        push(*frame->closure->upvalues[slot]->location);
        break;
      }
      case OP_SET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peek(0);
        break;
      }
      case OP_GET_PROPERTY:{
        if (!IS_INSTANCE(peek(0))) {
          runtimeError("Only instances have properties.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjInstance* instance = AS_INSTANCE(peek(0));
        ObjString* name = READ_STRING();
        Value value;
        if (tableGet(&instance->fields, name, &value)) {
          pop(); // Instance.
          push(value);
          break;
        }
        if (!bindMethod(instance->klass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
        }
      case OP_SET_PROPERTY: {
        if (!IS_INSTANCE(peek(1))) {
          runtimeError("Only instances have fields.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjInstance* instance = AS_INSTANCE(peek(1));
        tableSet(&instance->fields, READ_STRING(), peek(0));
        Value value = pop();
        pop();
        push(value);
        break;
      }
      case OP_GET_SUPER: {
        ObjString* name = READ_STRING();
        ObjClass* superclass = AS_CLASS(pop());  
        if (!bindMethod(superclass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_EQUAL: {
        Value b = pop();
        Value a = pop();
        push(BOOL_VAL(valuesEqual(a, b)));
        break;
      }
     case OP_GREATER: {
        if(IS_INT(peek(0))) {BINARY_OP_INT(BOOL_VAL, >); break;}
        else if(IS_FLOAT(peek(0))) {BINARY_OP_FLOAT(BOOL_VAL, >); break;}
      }  
     case OP_LESS: { 
        if(IS_INT(peek(0))) {BINARY_OP_INT(BOOL_VAL, <); break;}
        else if(IS_FLOAT(peek(0))) {BINARY_OP_FLOAT(BOOL_VAL, <); break;}
      }
     case OP_ADD: concatenate(); break;
     case OP_ADD_INT: BINARY_OP_INT(INT_VAL, +); break;
     case OP_SUBTRACT_INT: BINARY_OP_INT(INT_VAL, -); break;
     case OP_MULTIPLY_INT: BINARY_OP_INT(INT_VAL, *); break;
     case OP_DIVIDE_INT: BINARY_OP_INT(INT_VAL, /); break;
     case OP_ADD_FLOAT: BINARY_OP_FLOAT(FLOAT_VAL, +);
     case OP_SUBTRACT_FLOAT: BINARY_OP_FLOAT(FLOAT_VAL, -);
     case OP_MULTIPLY_FLOAT: BINARY_OP_FLOAT(FLOAT_VAL, *);
     case OP_DIVIDE_FLOAT: BINARY_OP_FLOAT(FLOAT_VAL, /);
     case OP_NOT: push(BOOL_VAL(isFalsey(pop()))); break;
     case OP_NEGATE_INT:
        if (!IS_INT(peek(0))) {
          runtimeError("Operand must be a number.");
          return INTERPRET_RUNTIME_ERROR;
        }
        push(INT_VAL(-AS_INT(pop()))); 
        break;
     case OP_NEGATE_FLOAT:
        if (!IS_FLOAT(peek(0))) {
          runtimeError("Operand must be a number.");
          return INTERPRET_RUNTIME_ERROR;
        }
        push(FLOAT_VAL(-AS_FLOAT(pop())));
        break;
     case OP_PRINT: {
        Value value = pop();
        //printf("About to print value of type: %d\n", value.type);
        printValue(value);
        printf("\n");
        break;
      }
     case OP_JUMP: {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        break;
      }
     case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0))) frame->ip += offset;
        break;
      }
     case OP_LOOP: {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        break;
      }
     case OP_CALL: {
        int argCount = READ_BYTE();
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
     case OP_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        if (!invoke(method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
     case OP_SUPER_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass* superclass = AS_CLASS(pop());
        if (!invokeFromClass(superclass, method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
     case OP_CLOSURE: {
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = newClosure(function);
        push(OBJ_VAL(closure));
        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t index = READ_BYTE();
          if (isLocal) {
            closure->upvalues[i] =
                captureUpvalue(frame->slots + index);
          } else {
            closure->upvalues[i] = frame->closure->upvalues[index];
          }
        }
        break;
      }
     case OP_CLOSE_UPVALUE:{
        closeUpvalues(vm.stackTop - 1);
        pop();
        break;
      }
     case OP_RETURN: {
        return INTERPRET_OK;
        Value result = pop();
        closeUpvalues(frame->slots);
        vm.frameCount--;
        if (vm.frameCount == 0) {
          pop();
          return INTERPRET_OK;
        }
        vm.stackTop = frame->slots;
        push(result);
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
     case OP_CLASS: {
        push(OBJ_VAL(newClass(READ_STRING())));
        break;
      }
     case OP_INHERIT: {
        Value superclass = peek(1);
        if (!IS_CLASS(superclass)) {
          runtimeError("Superclass must be a class.");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjClass* subclass = AS_CLASS(peek(0));
        tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
        pop();
        break;
      }
     case OP_METHOD:{
        defineMethod(READ_STRING());
        break;
      }
     case OP_TYPE_ERROR:{
        runtimeError("Type mismatch");
        return INTERPRET_RUNTIME_ERROR;
      }
     case OP_RUNTIME_ERROR: {}
        runtimeError("An error occurred");
        return INTERPRET_RUNTIME_ERROR;
    }
  }
}

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP

void hack(bool b) {
  run();
  if (b) hack(false);
}

InterpretResult interpret(const char* source) {
  printf("Interpreting...");
  ObjFunction* function = compile(source);
  if (function == NULL) return INTERPRET_COMPILE_ERROR;
  push(OBJ_VAL(function));
  ObjClosure* closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  call(closure, 0);
  return run();
}
