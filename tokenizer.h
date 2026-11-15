#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stddef.h>
#include <stdbool.h>

#define MAX_TOKENS 64
#define MAX_TOKEN_LEN 260

typedef enum {
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_COMMA,
    TOKEN_EQUALS,
    TOKEN_STAR,
    TOKEN_SEMICOLON,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char text[MAX_TOKEN_LEN];
} Token;

typedef struct {
    Token tokens[MAX_TOKENS];
    size_t count;
    size_t pos;
    char error_msg[128];
} TokenStream;

bool tokenize_input(const char* input, TokenStream* stream);
const Token* peek_token(const TokenStream* stream);
const Token* next_token(TokenStream* stream);
bool match_keyword(const TokenStream* stream, const char* kw);
bool expect_keyword(TokenStream* stream, const char* kw);
bool expect_token(TokenStream* stream, TokenType type);

#endif // TOKENIZER_H
