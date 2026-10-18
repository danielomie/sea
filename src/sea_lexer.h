/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_common.h"

/*
 * Sea lexer / tokenizer.
 * Converts source text into a stream of tokens.
 */

typedef enum {
    // Literals
    TK_NIL, TK_TRUE, TK_FALSE, TK_NUMBER, TK_STRING, TK_IDENT,
    TK_TEMPLATE,   /* `text ${expr} text`: start/len cover the whole literal, backticks included */

    // Keywords
    TK_AND, TK_BREAK, TK_CHAN, TK_CONST, TK_CONTINUE, TK_DEFAULT,
    TK_DEL, TK_DEFER, TK_DO, TK_ELSE, TK_ELSEIF, TK_END,
    TK_FOR, TK_FUNCTION, TK_GO, TK_IF, TK_IN,
    TK_LOCAL, TK_NOT, TK_OR, TK_PANIC, TK_RECOVER,
    TK_RETURN, TK_SELECT, TK_CASE, TK_THEN, TK_WHILE,

    // Operators
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_DSLASH, TK_PERCENT,
    TK_CARET, TK_AMP, TK_PIPE, TK_TILDE, TK_LSHIFT, TK_RSHIFT,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ASSIGN, TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET, TK_SEMICOLON, TK_COLON, TK_COMMA,
    TK_DOT, TK_DOTDOT, TK_DOTDOTDOT,
    TK_ARROW_LEFT,   // <-
    TK_ARROW_RIGHT,  // ->
    TK_QUESTION,     // ?
    TK_COMMENT,      // -- ... (only with SeaLexer.keep_comments)

    TK_EOF,
    TK_ERROR,
} SeaTK;

typedef struct {
    SeaTK    type;
    int      line;       /* line and column (1-based, in bytes) of the first char */
    int      col;
    const char *start;
    int      len;
    bool     is_float;   /* true when literal has '.', 'e', or 'E' (e.g. 0.0, 1e5) */
    union {
        double   number;
        struct { const char *ptr; size_t len; } string;
    } val;
} SeaToken;

typedef struct {
    const char *src;
    const char *cur;
    const char *start;
    int         line;
    const char *line_start;  /* first char of the current line (for columns) */
    int         tok_line;    /* position of the token being scanned */
    int         tok_col;
    bool        keep_comments; /* return comments as TK_COMMENT (tokenizers) */
    char        msg_buf[64];
    SeaToken    current;
    SeaToken    lookahead;
    bool        has_lookahead;
    char       *err_msg;
    const char *bad_comment;     /* an unfinished long comment starts here */
    int         bad_comment_line, bad_comment_col;
} SeaLexer;

// Initialize lexer with source code
void      sea_lexer_init(SeaLexer *l, const char *source);
// Same, optionally returning comments as TK_COMMENT tokens
void      sea_lexer_init_ex(SeaLexer *l, const char *source, bool keep_comments);

// Get next token (advances the lexer)
SeaToken  sea_lexer_next(SeaLexer *l);

// Peek at next token without consuming
SeaToken  sea_lexer_peek(SeaLexer *l);

// Free any resources
void      sea_lexer_free(SeaLexer *l);

// Get token name for error messages
const char* sea_tk_name(SeaTK tk);

/* Decode the escapes of a string body (tmpl: also \` and \$, as in
 * `templates`). On success *out is a new NUL-terminated buffer of *out_len
 * bytes (free with sea_free(*out, len + 1)); on failure *err says why. */
bool sea_lex_unescape(const char *s, size_t len, bool tmpl, char **out, size_t *out_len, const char **err);
