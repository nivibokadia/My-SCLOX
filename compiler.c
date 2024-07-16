#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "value.h"
#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "scanner.h"
#include "vm.h"
#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
  ValueType currentType;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  // =
  PREC_OR,          // or
  PREC_AND,         // and
  PREC_EQUALITY,    // == !=
  PREC_COMPARISON,  // < > <= >=
  PREC_TERM,        // + -
  PREC_FACTOR,      // * /
  PREC_UNARY,       // ! -
  PREC_CALL,        // . ()
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
    char* name;
    ValueType type;
} Symbol;

typedef struct {
    Symbol* symbols;
    int count;
    int capacity;
} SymbolTable;

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  TokenType type;
  Token name;
  int depth;
  bool isCaptured;
} Local;

typedef struct VM{
  Table globals;
  TokenType type;
  Token name;
} global;

typedef struct {
  uint8_t index;
  bool isLocal;
} Upvalue;

typedef enum {
  TYPE_FUNCTION,
  TYPE_INITIALIZER,
  TYPE_METHOD,
  TYPE_SCRIPT
} FunctionType;

typedef struct Compiler {
  Value* slots;
  int stackTop;
  struct Compiler* enclosing;
  ObjFunction* function;
  FunctionType type;
  Local locals[UINT8_COUNT];
  int localCount;
  Upvalue upvalues[UINT8_COUNT];
  int scopeDepth;
} Compiler;

typedef struct ClassCompiler {
  struct ClassCompiler* enclosing; 
  bool hasSuperclass;
} ClassCompiler;

Parser parser;
Compiler* current = NULL;
ClassCompiler* currentClass = NULL;
VM vm;
SymbolTable symbolTable;

static void errorAt(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end \n"); exit(TOKEN_ERROR);
    } else if (token->type == TOKEN_ERROR) {
      exit(TOKEN_ERROR);
    } else {
        fprintf(stderr, " at '%.*s'\n", token->length, token->start);
        exit(TOKEN_ERROR);
    }
    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
    return;
}

static void error(const char* message) {
  printf("Error: %s\n", message);
  errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

void initSymbolTable() {
    symbolTable.symbols = NULL;
    symbolTable.count = 0;
    symbolTable.capacity = 0;
}

void addSymbol(const char* name, ValueType type, int scope, int line) {
    if (symbolTable.count + 1 > symbolTable.capacity) {
        int oldCapacity = symbolTable.capacity;
        symbolTable.capacity = GROW_CAPACITY(oldCapacity);
        symbolTable.symbols = GROW_ARRAY(Symbol, symbolTable.symbols, oldCapacity, symbolTable.capacity);
    }
    Symbol* symbol = &symbolTable.symbols[symbolTable.count++];
    symbol->name = strdup(name);
    symbol->type = type;
}

ValueType getSymbolType(const char* name) {
    for (int i = 0; i < symbolTable.count; i++) {
        if (strcmp(symbolTable.symbols[i].name, name) == 0) {
            return symbolTable.symbols[i].type;
        }
    }
    error("Invalid symbol type");
    return -1;
}

static Chunk* currentChunk() {
    if (current == NULL) {
        return NULL;
    }
    if (current->function == NULL) {
        return NULL;
    }
    return &current->function->chunk;
}

static void advance() {
  printf("advancing\n");
  parser.previous = parser.current;
  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR) break;
    errorAtCurrent(parser.current.start);
  }
  printf("scanning token: %d, lexeme: %.*s\n", parser.current.type, parser.current.length, parser.current.start);
}

static void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }
  errorAtCurrent(message);
}

static bool check(TokenType type) {
  return parser.current.type == type;
}

static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

static void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    printf("Entered emitBytes: %d, %d\n", byte1, byte2);
    emitByte(byte1);
    emitByte(byte2);
    printf("Left emitBytes\n");
}


static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);
  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");
  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

static int emitJump(uint8_t instruction) {
  int offset = currentChunk()->count - 2;
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return offset;
}

static void emitReturn() {
  printf("entered EmitReturn\n");
  if (current->type == TYPE_INITIALIZER) {
    emitBytes(OP_GET_LOCAL, 0);
  } else {
    emitByte(OP_NIL);
  }
  emitByte(OP_RETURN);
  printf("left emitReturn\n");
}

static uint8_t makeConstant(Value value) {
    printf("Entered makeConstant\n");
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }
    printf("Left makeConstant\n");
    return (uint8_t)constant;
}

static uint8_t makeConstantInt(Value value) {
    //printf("Entered makeConstantInt\n");
    int constant = addConstant((currentChunk()), value);
    //printf("Constant index: %d\n", constant);
    if (constant > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }
    //printf("Left makeConstantInt\n");
    return (uint8_t)constant;
}

static uint8_t makeConstantFloat() {
    double floatValue = strtod(parser.previous.start, NULL);
    return makeConstant(FLOAT_VAL(floatValue));
}

static uint8_t makeConstantString() {
    //printf("Entered makeConstantString\n");
    return makeConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void emitConstant(Value value) {
    //printf("Entered emitConstant\n");
    //printf("Value type: %d\n", value.type);
    switch (value.type) {
        case VAL_INT:
            printf("Emitting integer constant\n");
            emitBytes(OP_CONSTANT_INT, makeConstantInt(value));
            break;
        case VAL_FLOAT:
            printf("Emitting float constant\n");
            emitBytes(OP_CONSTANT_FLOAT, makeConstantFloat(value));
            break;
        case VAL_OBJ:
            printf("Emitting object constant\n");
            if (AS_OBJ(value)->type == OBJ_STRING) {
                printf("Object is a string\n");
                emitBytes(OP_CONSTANT_STRING, makeConstantString(value));
            } else {
                printf("Unsupported object type\n");
                error("Unsupported object type for constant");
            }
            break;
        default:
            printf("Unsupported value type\n");
            error("Unsupported value type for constant");
            break;
    }
    printf("Left emitConstant\n");
}

void patchJump(int offset) {
  int jump = currentChunk()->count - offset - 2;
  if (jump > UINT16_MAX) error("Too much code to jump over.");
  currentChunk()->code[offset] = (jump >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler* compiler, FunctionType type) {
    //printf("Initializing compiler\n");
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->function = newFunction();
    current = compiler;
    if (type != TYPE_SCRIPT) {
        current->function->name = copyString(parser.previous.start,parser.previous.length);
    }
    Local* local = &current->locals[current->localCount++];
    local->depth = 0;
    local->isCaptured = false;
    if (type != TYPE_FUNCTION) {
        local->name.start = "this";
        local->name.length = 4;
    } else {
        local->name.start = "";
        local->name.length = 0;
    }
    //printf("Compiler initialized, function: %p\n", (void*)compiler->function);
}

static ObjFunction* endCompiler() {
    //printf("entered endCompiler\n");
    emitReturn();
    ObjFunction* function = current->function;
    #ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
    disassembleChunk(currentChunk(), function->name != NULL ? function->name->chars : "<script>");
    }
    #endif
    current = current->enclosing;
    return function;
    //freeSymbolTable(symbolTable);
    //printf("left endCompiler\n");
}

static void beginScope() {
  current->scopeDepth++;
}

static void endScope() {
  current->scopeDepth--;
  while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
    if (current->locals[current->localCount - 1].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
    current->localCount--;
  }
}

static void expression();
static void statement();
static void declaration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

static uint8_t identifierConstant(Token* name) {
  //printf("Adding identifier to constant: %.*s\n", name->length, name->start);
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

static bool isGlobal(Token* name) {
    //printf("Checking if '%.*s' is global\n", name->length, name->start);
    ObjString* globalName = copyString(name->start, name->length);
    Value value;
    bool result = tableGet(&vm.globals, globalName, &value);
    //printf("Global check result: %s\n", result ? "true" : "false");
    return result;
}

static int resolveLocal(Compiler* compiler, Token* name) {
  //printf("Resolving variable: '%.*s'\n", name->length, name->start);
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    //printf("local var\n");
    Local* local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      if (local->depth == -1) {
        //error("Can't read local variable in its own initializer.");
      }
      //printf("Resolved local variable '%.*s' at index %d\n", name->length, name->start, i);
      return i;
    }
  }
    //printf("Unresolved variable '%.*s'\n", name->length, name->start);
    return -1;
}

static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
  int upvalueCount = compiler->function->upvalueCount;
  for (int i = 0; i < upvalueCount; i++) {
    Upvalue* upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  if (upvalueCount == UINT8_COUNT) {
    error("Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
  if (compiler->enclosing == NULL) return -1;

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false);
  }
  return -1;
}

static void addLocal(Token name) {
    if (current->localCount == UINT8_COUNT) {
        error("Too many local variables in function.");
        return;
    }
    Local* local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = -1;
    local->isCaptured = false;
    local->type = parser.previous.type;
    //printf("Added local variable '%.*s' at index %d\n", name.length, name.start, current->localCount - 1);
}

static void declareVariable() {
    if (current->scopeDepth == 0) return;
    Token* name = &parser.previous;
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break;
        }      
        if (identifiersEqual(name, &local->name)) {
            error("Already a variable with this name in this scope.");
        }
    }
    addLocal(*name);
}

static const char* typeToString(ValueType type) {
    switch (type) {
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_OBJ: return "string";
        default: return "unknown";
    }
}

static uint8_t parseVariable(const char* errorMessage) {
    consume(TOKEN_IDENTIFIER, errorMessage);
    return identifierConstant(&parser.previous);
}

static void markInitialized() {
    if (current->scopeDepth == 0) return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
    printf("Marked variable '%.*s' as initialized at depth %d\n",current->locals[current->localCount - 1].name.length,
           current->locals[current->localCount - 1].name.start, current->scopeDepth);
}

static void defineVariable(uint8_t global) {
  TokenType type = parser.current.type;
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }
  //printf("Defining variable with index: %d\n", global);
  if(type == TOKEN_INT){
    emitBytes(OP_DEFINE_GLOBAL, global);
  } else if (type == TOKEN_FLOAT){
    emitBytes(OP_DEFINE_GLOBAL, global);
  } else if (type == TOKEN_STRING){
    emitBytes(OP_DEFINE_GLOBAL, global);
  } else {
    emitBytes(OP_DEFINE_GLOBAL, global);
  }
}

static uint8_t argumentList() {
  uint8_t argCount = 0;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      expression();
      if (argCount == 255) {
        error("Can't have more than 255 arguments.");
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  return argCount;
}

static void and_(bool canAssign) {
  int endJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJump(endJump);
}

static void checkTypes(TokenType declaredType, TokenType expressionType) {
    switch (declaredType) {
        case TOKEN_INT:
            if (expressionType != TOKEN_INT_LITERAL) {
                error("Type mismatch: expected int, got non-int.");
            }
            break;
        case TOKEN_FLOAT:
            if (expressionType != TOKEN_FLOAT_LITERAL && expressionType != TOKEN_INT_LITERAL) {
                error("Type mismatch: expected float, got non-numeric.");
            }
            break;
        case TOKEN_STRING:
            if (expressionType != TOKEN_STRING_LITERAL) {
                error("Type mismatch: expected string, got non-string.");
            }
            break;
        default:
            error("Unknown type in type check.");
    }
}

static void emitBinaryOp(TokenType operatorType, OpCode intOpcode, OpCode floatOpcode) {
  if (IS_INT(current->slots[current->stackTop - 1]) && IS_INT(current->slots[current->stackTop - 2])) {
    emitByte(intOpcode);
  } else if (IS_FLOAT(current->slots[current->stackTop - 1]) && IS_FLOAT(current->slots[current->stackTop - 2])) {
    emitByte(floatOpcode);
  } else {
    error("Operands must be two integers or two floats.");
  }
}

const char* valueTypeToString(ValueType type) {
    switch (type) {
        case VAL_BOOL:   return "boolean";
        case VAL_NIL:    return "nil";
        case VAL_INT:    return "integer";
        case VAL_FLOAT:  return "float";
        case VAL_OBJ:    return "object";
        default:         return "unknown";
    }
}
    
static void binary(bool canAssign) {
    printf("Entering binary function. parser.currentType: %d\n", parser.currentType);
    TokenType operatorType = parser.previous.type;
    ParseRule* rule = getRule(operatorType);
    ValueType leftType = parser.currentType;
    parsePrecedence((Precedence)(rule->precedence + 1));
    ValueType rightType = parser.currentType;
    if (leftType != rightType) {
            error("Operands must be of compatible types.");
            return;
    }
    switch (operatorType) {
        case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
        case TOKEN_GREATER:       emitByte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS:          emitByte(OP_LESS); break;
        case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS:
            printf("entered token-plus\n");
            printf("left: %d , right: %d\n", leftType, rightType);
            if (leftType == VAL_FLOAT && rightType == VAL_FLOAT) {
                printf("float enter in tokens\n");
                emitByte(OP_ADD_FLOAT);
            } else if (leftType == VAL_INT && rightType == VAL_INT) {
                printf("int enter in tokens\n");
                emitByte(OP_ADD_INT);
            } else if (leftType == VAL_OBJ && rightType == VAL_OBJ) {
                printf("obj enter in tokens\n");
                emitByte(OP_ADD);
            } else {
                printf("Type mismatch: Cannot add %s and %s.\n", 
                      valueTypeToString(leftType), valueTypeToString(rightType));
                emitByte(OP_TYPE_ERROR);  // Emit a type error opcode
                return;  // Stop compilation of this expression
            }
            break;
        case TOKEN_MINUS:
            printf("entered token-minus\n");
            if (leftType == VAL_FLOAT && rightType == VAL_FLOAT) {
                emitByte(OP_SUBTRACT_FLOAT);
            } else if (leftType == VAL_INT && rightType == VAL_INT) {
                emitByte(OP_SUBTRACT_INT);
            } else {
              printf("Both the variables are not the same type.\n");
              exit(1);
            }
            break;
          case TOKEN_STAR:
            printf("entered token-star\n");
            printf("leftType: %d, rightType: %d\n", leftType, rightType);
            if (leftType == VAL_FLOAT && rightType == VAL_FLOAT) {
                printf("star-float\n");
                emitByte(OP_MULTIPLY_FLOAT);
            } else if (leftType == VAL_INT && rightType == VAL_INT) {
                printf("star-int\n");
                emitByte(OP_MULTIPLY_INT);
            } else {
              printf("Both the variables are not the same type.\n");
              exit(1);
            }
            break;
          case TOKEN_SLASH:
            printf("entered token-slash\n");
            if (leftType == VAL_FLOAT && rightType == VAL_FLOAT) {
                emitByte(OP_DIVIDE_FLOAT);
            } else if (leftType == VAL_INT && rightType == VAL_INT) {
                emitByte(OP_DIVIDE_INT);
            } else {
              printf("Both the variables are not the same type.\n");
              exit(1);
            }
            break;
        default: return; 
    }
    parser.currentType = leftType;
}

static void call(bool canAssign) {
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static void dot(bool canAssign) {
  consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
  uint8_t name = identifierConstant(&parser.previous);
  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(OP_SET_PROPERTY, name);
  } else if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCount = argumentList();
    emitBytes(OP_INVOKE, name);
    emitByte(argCount);
  } else {
    emitBytes(OP_GET_PROPERTY, name);
  }
}

static void literal(bool canAssign) {
  switch (parser.previous.type) {
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
    default: return;
  }
}

static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void integer_(bool canAssign) {
    long value = strtol(parser.previous.start, NULL, 10);
    if (value > INT_MAX || value < INT_MIN) {
        error("Integer literal is too large.");
    } else {
        parser.currentType = VAL_INT;
        emitConstant(INT_VAL((int)value));
    }
}

static void floating_(bool canAssign) {
    double value = strtod(parser.previous.start, NULL);
   if(strchr(parser.previous.start, '.') != NULL){
      parser.currentType = VAL_FLOAT;
      emitConstant(FLOAT_VAL((float)value));
   }
}

static ValueType getVariableType(Token* name) {
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (identifiersEqual(name, &local->name)) {
            return local->type;
        }
    }
    ObjString* nameString = copyString(name->start, name->length);
    Value value;
    if (tableGet(&vm.globals, nameString, &value)) {
        if (IS_INT(value)) return VAL_INT;
        if (IS_FLOAT(value)) return VAL_FLOAT;
        if (IS_STRING(value)) return VAL_OBJ;
        if (IS_BOOL(value)) return VAL_BOOL;
        if (IS_NIL(value)) return VAL_NIL;    
        error("Unknown global variable type.");
        return VAL_NIL;
    }
    error("Undefined variable.");
    return VAL_NIL;
}

static void or_(bool canAssign) {
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP);
  patchJump(elseJump);
  emitByte(OP_POP);
  parsePrecedence(PREC_OR);
  patchJump(endJump);
}

static void string(bool canAssign) {
    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
    parser.currentType = VAL_OBJ;  // Assuming VAL_OBJ is used for strings
}

// static ValueType getExpressionType() {
//     switch (parser.previous.type) {
//         case TOKEN_INT:
//             return VAL_INT;
//         case TOKEN_FLOAT:
//             return VAL_FLOAT;
//         case TOKEN_STRING:
//             return VAL_OBJ;
//         case TOKEN_TRUE:
//         case TOKEN_FALSE:
//             return VAL_BOOL;
//         case TOKEN_NIL:
//             return VAL_NIL;
//         case TOKEN_IDENTIFIER:
//             return getVariableType(&parser.previous);
//         case TOKEN_LEFT_PAREN:
//             expression();
//             consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
//             return getExpressionType();
//         case TOKEN_PLUS:
//                {
//                 ValueType left = getExpressionType();
//                 ValueType right = getExpressionType();
//                 if (left == VAL_FLOAT || right == VAL_FLOAT) {
//                     return VAL_FLOAT;
//                 } else if (left == VAL_INT || right == VAL_INT){
//                   return VAL_INT;
//                 } else {
//                   return VAL_OBJ;
//                 }
//             }
//         case TOKEN_MINUS:
//         case TOKEN_SLASH:
//         case TOKEN_STAR:
//             {
//                 ValueType left = getExpressionType();
//                 ValueType right = getExpressionType();
//                 if (left == VAL_FLOAT || right == VAL_FLOAT) {
//                     return VAL_FLOAT;
//                 }
//                 return VAL_INT;
//             }
//         case TOKEN_BANG:
//         case TOKEN_BANG_EQUAL:
//         case TOKEN_EQUAL_EQUAL:
//         case TOKEN_GREATER:
//         case TOKEN_GREATER_EQUAL:
//         case TOKEN_LESS:
//         case TOKEN_LESS_EQUAL: 
//             return VAL_BOOL;
//         default:
//             error("Unable to determine expression type.");
//             return VAL_NIL;
//     }
// }

static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name);
  TokenType type = name.type;
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg = identifierConstant(&name);
    switch (type) {
      case VAL_INT:
        getOp = OP_GET_GLOBAL_INT;
        setOp = OP_SET_GLOBAL_INT;
        break;
      case VAL_FLOAT:
        getOp = OP_GET_GLOBAL_FLOAT;
        setOp = OP_SET_GLOBAL_FLOAT;
        break;
      case VAL_OBJ:
        getOp = OP_GET_GLOBAL_STRING;
        setOp = OP_SET_GLOBAL_STRING;
        break;
      default:
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
        break;
    }
  }
  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, (uint8_t)arg);
  } else {
    emitBytes(getOp, (uint8_t)arg);
  }
}

static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

static Token syntheticToken(const char* text) {
  Token token;
  token.start = text;
  token.length = (int)strlen(text);
  return token;
}

static void super_(bool canAssign) {
  if (currentClass == NULL) {
    error("Can't use 'super' outside of a class.");
  } else if (!currentClass->hasSuperclass) {
    error("Can't use 'super' in a class with no superclass.");
  }
  consume(TOKEN_DOT, "Expect '.' after 'super'.");
  consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
  uint8_t name = identifierConstant(&parser.previous);
  namedVariable(syntheticToken("this"), false);
  if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCount = argumentList();
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_SUPER_INVOKE, name);
    emitByte(argCount);
  } else {
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_GET_SUPER, name);
  }
}

static void this_(bool canAssign) {
  if (currentClass == NULL) {
    error("Can't use 'this' outside of a class.");
    return;
  }
  variable(false);
} 

static void unary(bool canAssign) {
  TokenType operatorType = parser.previous.type;
  parsePrecedence(PREC_UNARY);
  switch (operatorType) {
    case TOKEN_BANG: emitByte(OP_NOT); break;
    case TOKEN_MINUS: 
        emitByte(OP_NEGATE_INT); break;
      }
}

ParseRule rules[] = {
  [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
  [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
  [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COLON]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
  [TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
  [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
  [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
  [TOKEN_INT]           = {integer_,   NULL,   PREC_NONE},
  [TOKEN_FLOAT]         = {floating_,   NULL,   PREC_NONE},
  [TOKEN_STRING_LITERAL]= {string,   NULL,   PREC_NONE},
  [TOKEN_INT_LITERAL]   = {integer_,   NULL,   PREC_NONE},
  [TOKEN_FLOAT_LITERAL] = {floating_,   NULL,   PREC_NONE},
  [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
  [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
  [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
  [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
  [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
  [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
  //[TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};


static ParseRule* getRule(TokenType type) {
    return &rules[type];
}

static void parsePrecedence(Precedence precedence) {
    //printf("entered parsefunc\n");
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);
    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }
    if (canAssign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
    //printf("Exiting parsePrecedence\n");
}

static void expression() {
  //printf("Entered expression\n");
  parsePrecedence(PREC_ASSIGNMENT);
  //printf("Exiting expression\n");
}

static void block() {
    beginScope();
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
    endScope();
}

static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current->function->arity++;
      if (current->function->arity > 255) {
        errorAtCurrent("Can't have more than 255 parameters.");
      }
      uint8_t constant = parseVariable("Expect parameter name.");
      defineVariable(constant);
    } while (match(TOKEN_COMMA));
  }
//< parameters
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  block();

  ObjFunction* function = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void method() {
  consume(TOKEN_IDENTIFIER, "Expect method name.");
  uint8_t constant = identifierConstant(&parser.previous);
  FunctionType type = TYPE_METHOD;
  if (parser.previous.length == 4 &&
      memcmp(parser.previous.start, "init", 4) == 0) {
    type = TYPE_INITIALIZER;
  }
  function(type);
  emitBytes(OP_METHOD, constant);
}

static void classDeclaration() {
  consume(TOKEN_IDENTIFIER, "Expect class name.");
  Token className = parser.previous;
  uint8_t nameConstant = identifierConstant(&parser.previous);
  declareVariable();
  emitBytes(OP_CLASS, nameConstant);
  defineVariable(nameConstant);
  ClassCompiler classCompiler;
  classCompiler.hasSuperclass = false;
  classCompiler.enclosing = currentClass;
  currentClass = &classCompiler;

  if (match(TOKEN_LESS)) {
    consume(TOKEN_IDENTIFIER, "Expect superclass name.");
    variable(false);
    
    if (identifiersEqual(&className, &parser.previous)) {
      error("A class can't inherit from itself.");
    }

    beginScope();
    addLocal(syntheticToken("super"));
    defineVariable(0);
    namedVariable(className, false);
    emitByte(OP_INHERIT);
    classCompiler.hasSuperclass = true;
  }
  namedVariable(className, false);
  consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    method();
  }
  consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
  emitByte(OP_POP);
  if (classCompiler.hasSuperclass) {
    endScope();
  }
  currentClass = currentClass->enclosing;
}

static void funDeclaration() {
  uint8_t global = parseVariable("Expect function name.");
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

static TokenType parseType() {
    if (parser.current.type == TOKEN_INT) return VAL_INT;
    if (parser.current.type == TOKEN_FLOAT) return VAL_FLOAT;
    if (parser.current.type == TOKEN_STRING) return OBJ_STRING;
    return VAL_NIL;
}

static void varDeclaration() {
  //printf("Entering varDeclaration\n");
  printf("Current token: %d, lexeme: %.*s\n", parser.current.type, parser.current.length, parser.current.start);
  TokenType type = parser.previous.type;
  if (!check(TOKEN_IDENTIFIER)) {
    error("Expect variable name.");
    return;
  }
  uint8_t global = parseVariable("Expect variable name");
  //advance();
  if (match(TOKEN_EQUAL)) {
    //printf("Found '=', parsing expression\n");
    expression();
    //checkTypes(type, parser.previous.type);
  } else {
    emitByte(OP_NIL);
  }
  //printf("About to consume semicolon\n");
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  //printf("Semicolon consumed\n");
  defineVariable(global);
  addSymbol(parser.previous.start , parser.currentType, current->scopeDepth , parser.previous.line);
  if (type != TOKEN_ERROR) {
    //storeVariableType(global, type);
  }
  printf("Exiting varDeclaration\n");
}

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void forStatement() {
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
  if (match(TOKEN_SEMICOLON)) {
    // No initializer.
  } else if (match(TOKEN_INT) || match(TOKEN_FLOAT) || match(TOKEN_STRING)) {
    // Variable declaration
    varDeclaration();
  } else {
    expressionStatement();
  }
  int loopStart = currentChunk()->count;
  int exitJump = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }
  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(loopStart);
    loopStart = incrementStart;
    patchJump(bodyJump);
  }
  statement();
  emitLoop(loopStart);
  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }
  endScope();
}

static void ifStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Pop the condition value
    statement();
    int elseJump = emitJump(OP_JUMP);
    patchJump(thenJump);
    emitByte(OP_POP); // Pop the condition value
    if (match(TOKEN_ELSE)) statement();
    patchJump(elseJump);
}

static void printStatement() {
    printf("entered printStatement\n");
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void returnStatement() {
  if (current->type == TYPE_SCRIPT) {
    error("Can't return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON)) {
    emitReturn();
  } else {
    if (current->type == TYPE_INITIALIZER) {
      error("Can't return a value from an initializer.");
    }
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
    emitByte(OP_RETURN);
  }
}

static void whileStatement() {
  int loopStart = currentChunk()->count;
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();
  emitLoop(loopStart);
  patchJump(exitJump);
  emitByte(OP_POP);
}

static void synchronize() {
  parser.panicMode = false;
  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON) return;
    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_INT:
      case TOKEN_FLOAT:
      case TOKEN_STRING:
      case TOKEN_INT_LITERAL:
      case TOKEN_FLOAT_LITERAL:
      case TOKEN_STRING_LITERAL:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
        return;
      default:
        ; // Do nothing.
    }
    advance();
  }
}

static void declaration() {
  printf("enter declaration");
  if (match(TOKEN_CLASS)) {
    classDeclaration();
  } else if (match(TOKEN_FUN)) {
    funDeclaration();
  } else if (match(TOKEN_INT) || match(TOKEN_FLOAT) || match(TOKEN_STRING)) {
    printf("entered varDeclaration\n");
    varDeclaration();
  } else {
    printf("Entering statement\n");
    statement();
  }
  if (parser.panicMode) synchronize();
}

static void statement() {
  printf("Entered statement \n");
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_FOR)) {
    printf("token for");
    forStatement();
  } else if (match(TOKEN_IF)) {
    printf("token if");
    ifStatement();
  } else if (match(TOKEN_RETURN)) {
    returnStatement();
  } else if (match(TOKEN_WHILE)) {
    printf("token while");
    whileStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

const char* getTypeString(ValueType type) {
    switch(type) {
        case VAL_BOOL: return "BOOL";
        case VAL_NIL: return "NIL";
        case VAL_INT: return "INT";
        case VAL_FLOAT: return "FLOAT";
        case VAL_OBJ: return "OBJ";
        default: return "UNKNOWN";
    }
}

void printSymbolTable(SymbolTable table) {
    printf("Symbol Table:\n");
    printf("-------------\n");
    printf("Total Symbols: %d\n", table.count);
    printf("-------------\n");
    for (int i = 0; i < table.count; i++) {
        Symbol* symbol = &table.symbols[i];
        printf("Name: %s | Type: %s" ,symbol->name, getTypeString(symbol->type));
    printf("-------------\n");
  }
}
ObjFunction* compile(const char* source) {
  printf("In the compiler function...");
  initScanner(source);
  Compiler compiler;
  initCompiler(&compiler, TYPE_SCRIPT);
  parser.hadError = false;
  parser.panicMode = false;
  advance();
  while (!match(TOKEN_EOF)) {
    printf("Entering declaration...\n");
    declaration();
    printf("Scope depth after declaration: %d\n", current->scopeDepth);
  }
  printSymbolTable(symbolTable);
  ObjFunction* function = endCompiler();
  return parser.hadError ? NULL : function;
}

void markCompilerRoots() {
  Compiler* compiler = current;
  while (compiler != NULL) {
    markObject((Obj*)compiler->function);
    compiler = compiler->enclosing;
  }
}

