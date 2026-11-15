#define _POSIX_C_SOURCE 200809L
#include "tokenizer.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

static void add_token(TokenStream* stream, TokenType type, const char* start, size_t len) {
    if (stream->count >= MAX_TOKENS) {
        return;
    }
    Token* tok = &stream->tokens[stream->count++];
    tok->type = type;
    if (len >= MAX_TOKEN_LEN) {
        len = MAX_TOKEN_LEN - 1;
    }
    memcpy(tok->text, start, len);
    tok->text[len] = '\0';
}

bool tokenize_input(const char* input, TokenStream* stream) {
    memset(stream, 0, sizeof(TokenStream));
    const char* p = input;

    while (*p != '\0') {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (stream->count >= MAX_TOKENS - 1) {
            snprintf(stream->error_msg, sizeof(stream->error_msg), "Too many tokens in statement.");
            return false;
        }

        /* Quoted String Literal ('...' or "...") */
        if (*p == '\'' || *p == '"') {
            char quote = *p++;
            Token* tok = &stream->tokens[stream->count];
            tok->type = TOKEN_STRING;
            size_t len = 0;

            while (*p != '\0' && *p != quote) {
                if (*p == '\\' && *(p + 1) != '\0') {
                    p++;
                }
                if (len < MAX_TOKEN_LEN - 1) {
                    tok->text[len++] = *p;
                }
                p++;
            }
            if (*p != quote) {
                snprintf(stream->error_msg, sizeof(stream->error_msg), "Unterminated string literal.");
                return false;
            }
            p++; /* consume closing quote */
            tok->text[len] = '\0';
            stream->count++;
            continue;
        }

        /* Single-character punctuation & operators */
        if (*p == '(') { add_token(stream, TOKEN_LPAREN, p++, 1); continue; }
        if (*p == ')') { add_token(stream, TOKEN_RPAREN, p++, 1); continue; }
        if (*p == ',') { add_token(stream, TOKEN_COMMA, p++, 1); continue; }
        if (*p == '=') { add_token(stream, TOKEN_EQUALS, p++, 1); continue; }
        if (*p == '*') { add_token(stream, TOKEN_STAR, p++, 1); continue; }
        if (*p == ';') { add_token(stream, TOKEN_SEMICOLON, p++, 1); continue; }

        /* Signed/Unsigned Integer Number */
        if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)*(p + 1)))) {
            const char* start = p;
            if (*p == '-') p++;
            while (isdigit((unsigned char)*p)) {
                p++;
            }
            if (*p != '\0' && !isspace((unsigned char)*p) &&
                *p != ',' && *p != ')' && *p != ';') {
                snprintf(stream->error_msg, sizeof(stream->error_msg), "Invalid numeric literal.");
                return false;
            }
            add_token(stream, TOKEN_NUMBER, start, (size_t)(p - start));
            continue;
        }

        /* Identifier / Keyword / Unquoted shorthand word (e.g. email in shorthand mode) */
        const char* start = p;
        while (*p != '\0' && !isspace((unsigned char)*p) &&
               *p != '(' && *p != ')' && *p != ',' && *p != '=' &&
               *p != '*' && *p != ';' && *p != '\'' && *p != '"') {
            p++;
        }
        add_token(stream, TOKEN_IDENTIFIER, start, (size_t)(p - start));
    }

    add_token(stream, TOKEN_EOF, "", 0);
    return true;
}

const Token* peek_token(const TokenStream* stream) {
    if (stream->pos >= stream->count) {
        return &stream->tokens[stream->count - 1];
    }
    return &stream->tokens[stream->pos];
}

const Token* next_token(TokenStream* stream) {
    const Token* tok = peek_token(stream);
    if (stream->pos < stream->count - 1) {
        stream->pos++;
    }
    return tok;
}

bool match_keyword(const TokenStream* stream, const char* kw) {
    const Token* tok = peek_token(stream);
    return tok->type == TOKEN_IDENTIFIER && strcasecmp(tok->text, kw) == 0;
}

bool expect_keyword(TokenStream* stream, const char* kw) {
    if (match_keyword(stream, kw)) {
        next_token(stream);
        return true;
    }
    return false;
}

bool expect_token(TokenStream* stream, TokenType type) {
    if (peek_token(stream)->type == type) {
        next_token(stream);
        return true;
    }
    return false;
}
