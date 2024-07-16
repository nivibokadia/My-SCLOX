#include <stdlib.h>
#include <stdio.h>
#include "chunk.h"
#include "vm.h"
#include "memory.h"

void initChunk(Chunk* chunk) {
    //printf("Initializing chunk\n");
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    initValueArray(&chunk->constants);
    //printf("Chunk initialized\n");
}

void freeChunk(Chunk* chunk){
    FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    initChunk(chunk);
    FREE_ARRAY(uint8_t, chunk->code , chunk->capacity);
    FREE_ARRAY(uint8_t, chunk->lines, chunk->capacity);
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}

void writeChunk(Chunk* chunk, uint8_t byte, int line) {
    //printf("entered writeChunk \n");
    if (chunk->capacity < chunk->count + 1) {
        int oldCapacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(oldCapacity);
        chunk->code = GROW_ARRAY(uint8_t, chunk->code,
        oldCapacity, chunk->capacity);
    }
    chunk->code[chunk->count] = byte;
    chunk->count++;
    //printf("leaving writeChunk \n");
}

int addConstant(Chunk* chunk, Value value){
    push(value);
    writeValueArray(&chunk->constants, value);
    pop();
    return chunk->constants.count-1;
}