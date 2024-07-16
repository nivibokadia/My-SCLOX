#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include "common.h"
#include "scanner.h"

typedef struct {
  const char* start;
  const char* current;
  int line;
} Scanner;

Scanner scanner;

void initScanner(const char* source) {
  scanner.start = source;
  scanner.current = source;
  scanner.line = 1;
}

static bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

static bool isAtEnd() {
  return *scanner.current == '\0';
}

static char advance() {
  scanner.current++;
  return scanner.current[-1];
}

static char peek() {
  return *scanner.current;
}

static char peekNext() {
  if (isAtEnd()) return '\0';
  return scanner.current[1];
}

static bool match(char expected) {
  if (isAtEnd()) return false;
  if (*scanner.current != expected) return false;
  scanner.current++;
  return true;
}

static Token makeToken(TokenType type) {
  Token token;
  token.type = type;
  token.start = scanner.start;
  token.length = (int)(scanner.current - scanner.start);
  token.line = scanner.line;
  return token;
}

static Token errorToken(const char* message) {
  Token token;
  token.type = TOKEN_ERROR;
  token.start = message;
  token.length = (int)strlen(message);
  token.line = scanner.line;
  return token;
}

static void skipWhitespace() {
  for (;;) {
    char c = peek();
    switch (c) {
      case ' ':
      case '\r':
      case '\t':
        advance();
        break;
      case '\n':
        scanner.line++;
        advance();
        break;
      case '/':
        if (peekNext() == '/') {
          while (peek() != '\n' && !isAtEnd()) advance();
        } else {
          return;
        }
        break;
      default:
        return;
    }
  }
}

static TokenType checkKeyword(int start, int length, const char* rest, TokenType type) {
  if (scanner.current - scanner.start == start + length && memcmp(scanner.start + start, rest, length) == 0) {
    return type;
  }
  return TOKEN_IDENTIFIER;
}

static TokenType identifierType() {
  switch (scanner.start[0]) {
    case 'a': return checkKeyword(1, 2, "nd", TOKEN_AND);
    case 'c': return checkKeyword(1, 4, "lass", TOKEN_CLASS);
    case 'e': return checkKeyword(1, 3, "lse", TOKEN_ELSE);
    case 'f':
      if (scanner.current - scanner.start > 1) {
        switch (scanner.start[1]) {
          case 'a': return checkKeyword(2, 3, "lse", TOKEN_FALSE);
          case 'o': return checkKeyword(2, 1, "r", TOKEN_FOR);
          case 'u': return checkKeyword(2, 1, "n", TOKEN_FUN);
          case 'l': return checkKeyword(2, 3, "oat", TOKEN_FLOAT);
        }
      }
      break;
    case 'i':
      if (scanner.current - scanner.start > 1) {
        switch (scanner.start[1]) {
          case 'f': return checkKeyword(2, 0, "", TOKEN_IF);
          case 'n': return checkKeyword(2, 1, "t", TOKEN_INT);
                    printf("Lexer identified 'int' keyword\n");
                    return TOKEN_INT;
        }
      }
      break;
    case 'n': return checkKeyword(1, 2, "il", TOKEN_NIL);
    case 'o': return checkKeyword(1, 1, "r", TOKEN_OR);
    case 'p': return checkKeyword(1, 4, "rint", TOKEN_PRINT);
    case 'r': return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
    case 's': 
      if (scanner.current - scanner.start > 1) {
        switch (scanner.start[1]) {
          case 'u': return checkKeyword(2, 3, "per", TOKEN_SUPER);
          case 't': return checkKeyword(2, 4, "ring", TOKEN_STRING); 
        }
      }
      break;
    case 't':
      if (scanner.current - scanner.start > 1) {
        switch (scanner.start[1]) {
          case 'h': return checkKeyword(2, 2, "is", TOKEN_THIS);
          case 'r': return checkKeyword(2, 2, "ue", TOKEN_TRUE);
        }
      }
      break;
    //case 'v': return checkKeyword(1, 2, "ar", TOKEN_VAR);
    case 'w': return checkKeyword(1, 4, "hile", TOKEN_WHILE);
  }
  return TOKEN_IDENTIFIER;
}

static Token identifier() {
  //printf("Entered identifier");
  while (isAlpha(peek()) || isDigit(peek())) advance();
  return makeToken(identifierType());
}

// static Token number() {
//   bool isFloat = false;
//   while (isDigit(peek())) advance();
//   if (peek() == '.' && isDigit(peekNext())) {
//     isFloat = true;
//     advance();
//     while (isDigit(peek())) advance();
//   }
//   if (peek() == 'e' || peek() == 'E') {
//     isFloat = true;
//     advance();
//     if (peek() == '-' || peek() == '+') advance();
//     if (!isDigit(peek())) {
//       return errorToken("Malformed number literal.");
//     }
//     while (isDigit(peek())) advance();
//   }
//   const char* start = scanner.start;
//   char* end;
//   if (isFloat) {
//     double value = strtod(start, &end );
//     if (start == end) {
//       return errorToken("Invalid float literal.");
//     }
//     Token token = makeToken(TOKEN_FLOAT_LITERAL);
//     token.value.float_value = value;
//     return token;
//     }else {
//     long value = strtol(start, &end, 10);
//     if (end != scanner.current) {
//       return errorToken("Invalid integer literal.");
//     }
//     if (value > INT_MAX || value < INT_MIN) {
//       return errorToken("Integer literal out of range.");
//     }
//     Token token = makeToken(TOKEN_INT_LITERAL);
//     token.value.int_value = (int)value;
//     return token;
// }
// }

static Token number() {
  while (isDigit(peek())) advance();
  if (peek() == '.' && isDigit(peekNext())) {
    advance();
    while (isDigit(peek())) advance();
    return makeToken(TOKEN_FLOAT_LITERAL);
  }
  return makeToken(TOKEN_INT_LITERAL);
}

static Token string() {
  const char* start = scanner.start;
  advance();
  while (peek() != '"' && !isAtEnd()) {
    if (peek() == '\n') scanner.line++;
    advance();
  }
  if (isAtEnd()) {
    return errorToken("Unterminated string.");
  }
  advance();
  int length = (int)(scanner.current - start);
  Token token = makeToken(TOKEN_STRING_LITERAL);
  token.value.string_value = (char*)malloc(length);
  strncpy(token.value.string_value, start + 1, length - 2);
  token.value.string_value[length - 2] = '\0';
  return token;
}

Token scanToken() {
  //printf("scanning token\n");
  skipWhitespace();
  scanner.start = scanner.current;
  if (isAtEnd()) return makeToken(TOKEN_EOF);
  char c = advance();
  if (isAlpha(c)) return identifier();
  if (isDigit(c)) return number(); 
  switch (c) {
    case '(': return makeToken(TOKEN_LEFT_PAREN);
    case ')': return makeToken(TOKEN_RIGHT_PAREN);
    case '{': return makeToken(TOKEN_LEFT_BRACE);
    case '}': return makeToken(TOKEN_RIGHT_BRACE);
    case ':': return makeToken(TOKEN_COLON);
    case ';': {
      printf("Entered ; case\n");
      return makeToken(TOKEN_SEMICOLON);
    }
    case ',': return makeToken(TOKEN_COMMA);
    case '.': return makeToken(TOKEN_DOT);
    case '-': return makeToken(TOKEN_MINUS);
    case '+': return makeToken(TOKEN_PLUS);
    case '/': return makeToken(TOKEN_SLASH);
    case '*': return makeToken(TOKEN_STAR);
//> two-char
    case '!':
      return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
    case '=':
      //printf("reached = case \n");
      return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL); break;
    case '<':
      return makeToken(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
    case '>':
      return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
    case '"': return string();
  }
  return errorToken("Unexpected character.");
}

