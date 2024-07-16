#ifndef clox_chunk_h
#define clox_chunk_h
#include "value.h"
#include "common.h"
#include "scanner.h"

typedef enum {
  OP_CONSTANT,
  OP_CONSTANT_INT,
  OP_CONSTANT_FLOAT,
  OP_CONSTANT_STRING,
  OP_NIL,
  OP_TRUE,
  OP_FALSE,
  OP_POP,
  OP_GET_LOCAL,
  OP_SET_LOCAL,
  OP_SET_GLOBAL,
  OP_GET_GLOBAL,
  OP_DEFINE_GLOBAL_INT,
  OP_DEFINE_GLOBAL_FLOAT,
  OP_DEFINE_GLOBAL_STRING,
  OP_DEFINE_GLOBAL,
  OP_GET_GLOBAL_INT,
  OP_GET_GLOBAL_FLOAT,
  OP_GET_GLOBAL_STRING,
  OP_SET_GLOBAL_INT,
  OP_SET_GLOBAL_FLOAT,
  OP_SET_GLOBAL_STRING,
  OP_GET_UPVALUE,
  OP_SET_UPVALUE,
  OP_GET_PROPERTY,
  OP_SET_PROPERTY,
  OP_GET_SUPER,
  OP_EQUAL,
  OP_GREATER,
  OP_LESS,
  OP_ADD,
  OP_SUBTRACT,
  OP_MULTIPLY,
  OP_DIVIDE,
  OP_ADD_INT,
  OP_SUBTRACT_INT,
  OP_MULTIPLY_INT,
  OP_DIVIDE_INT,
  OP_ADD_FLOAT,
  OP_SUBTRACT_FLOAT,
  OP_MULTIPLY_FLOAT,
  OP_DIVIDE_FLOAT,
  OP_NOT,
  OP_NEGATE_INT,
  OP_NEGATE_FLOAT,
  OP_PRINT,
  OP_JUMP,
  OP_JUMP_IF_FALSE,
  OP_LOOP,
  OP_CALL,
  OP_INVOKE,
  OP_SUPER_INVOKE,
  OP_CLOSURE,
  OP_CLOSE_UPVALUE,
  OP_RETURN,
  OP_CLASS,
  OP_INHERIT,
  OP_METHOD,
  OP_TYPE_ERROR,
  OP_CHECK_INT,
  OP_CHECK_FLOAT,
  OP_CHECK_STRING,
  OP_RUNTIME_ERROR,
} OpCode;

typedef struct {        //chunk ds to hold bytecodes
  int count;            //no. of bytecode instructions currently in use
  int capacity;         //no. of elements in memory we allocated
  uint8_t* code;        //pointer to the array of bytecode instructions
  int* lines;           //array to store line numbers for debugging
  ValueArray constants; //array to store constants
  TokenType* type;
} Chunk;

void initChunk(Chunk* chunk);                          //initialize a chunk
void freeChunk(Chunk* chunk);                          //free a chunk
void writeChunk(Chunk* chunk, uint8_t byte, int line); //add a chunk
int addConstant(Chunk* chunk, Value value);            //add a const

#endif