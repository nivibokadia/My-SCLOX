#include <stdio.h>
#include <string.h>
#include "object.h"
#include "memory.h"
#include "value.h"

void initValueArray(ValueArray* array) {
  array->values = NULL;
  array->capacity = 0;
  array->count = 0;
}

void writeValue(ValueArray* array, Value value) {
  //printf("Entered writeValue, count: %d, capacity: %d\n", array->count, array->capacity);
  if (array->count + 1 > array->capacity) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    //printf("Reallocating array from %d to %d\n", oldCapacity, array->capacity);
    array->values = GROW_ARRAY(Value, array->values, oldCapacity, array->capacity);
  }
  array->values[array->count] = value;
  //printf("Left writeValue, count: %d, capacity: %d\n", array->count, array->capacity);
}

void writeValueArray(ValueArray* array, Value value) {
  //printf("Entered writeValueArray, count: %d, capacity: %d\n", array->count, array->capacity);
  writeValue(array, value);
  array->count++;
  //printf("Left writeValueArray, count: %d, capacity: %d\n", array->count, array->capacity);
}

void freeValueArray(ValueArray* array) {
  FREE_ARRAY(Value, array->values, array->capacity);
  initValueArray(array);
}
        
// void printValue(Value value) {
//   printf("entered printValue\n");
// #ifdef NAN_BOXING
//   if (IS_BOOL(value)) {
//     printf(AS_BOOL(value) ? "true" : "false");
//   } else if (IS_NIL(value)) {
//     printf("nil");
//   } else if (IS_INT(value)) {
//     printf("int value\n");
//     printf("%g\n", AS_INT(value)); 
//   } else if (IS_FLOAT(value)) {
//     printf("float value\n");
//     printf("%g\n", AS_FLOAT(value)); 
//   } else if (IS_OBJ(value)) {
//     printObject(value);
//   }
// #else
//   switch (value.type) {
//     case VAL_BOOL:
//       printf(AS_BOOL(value) ? "true" : "false");
//       break;
//     case VAL_NIL: printf("nil"); break;
//     case VAL_INT: printf("%d", AS_INT(value)); break;
//     case VAL_FLOAT: printf("%g", AS_FLOAT(value)); break;
//     case VAL_OBJ: printObject(value); break;
// }
// #endif
// }

void printValue(Value value) {
  //printf("entered printValue\n");
  switch (value.type) {
    case VAL_BOOL:
      printf(AS_BOOL(value) ? "true" : "false");
      break;
    case VAL_NIL: 
      printf("nil"); 
      break;
    case VAL_INT: 
      //printf("int value\n");
      printf("%d", AS_INT(value)); 
      break;
    case VAL_FLOAT: 
      //printf("float value\n");
      printf("%g", AS_FLOAT(value)); 
      break;
    case VAL_OBJ: 
      printObject(value); 
      break;
    default:
      printf("Unknown value type");
      break;
  }
}

bool isValueType(Value value, const char* type) {
  switch (value.type) {
    case VAL_BOOL:
      return strcmp(type, "bool") == 0;
    case VAL_NIL:
      return strcmp(type, "nil") == 0;
    case VAL_INT:
      return strcmp(type, "int") == 0;
    case VAL_FLOAT:
      return strcmp(type, "float") == 0;
    case VAL_OBJ:
      return true;
    default:
      return false;
  }
}

ValueType stringToValueType(const char* type) {
    if (strcmp(type, "bool") == 0) return VAL_BOOL;
    if (strcmp(type, "nil") == 0) return VAL_NIL;
    if (strcmp(type, "int") == 0) return VAL_INT;
    if (strcmp(type, "float") == 0) return VAL_FLOAT;
    if (strcmp(type, "obj") == 0) return VAL_OBJ;
    return VAL_NIL; // Default to NIL for unknown types
}

bool valuesEqual(Value a, Value b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case VAL_BOOL:   return AS_BOOL(a) == AS_BOOL(b);
    case VAL_NIL:    return true;
    case VAL_INT:    return AS_INT(a) == AS_INT(b);
    case VAL_FLOAT:  return AS_FLOAT(a) == AS_FLOAT(b);
    case VAL_OBJ:    return AS_OBJ(a) == AS_OBJ(b);
    default:         return false; // Unreachable.
  }
}
