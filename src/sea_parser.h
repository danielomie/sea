#pragma once
#include "sea_lexer.h"
#include "sea_ast.h"

/*
 * Recursive descent parser for Sea.
 * Produces a SeaNode* AST from source code.
 */

typedef struct {
    SeaLexer  lexer;
    char     *err_msg;
    bool      had_error;
    int       loop_depth;    // for break/continue validation
    int       func_depth;    // for return validation
    int       last_line;     // line of the last consumed token (for same-line check)
} SeaParser;

// Initialize parser with source code
void     sea_parser_init(SeaParser *p, const char *source);

// Free parser resources
void     sea_parser_free(SeaParser *p);

// Parse full program (returns block node or NULL on error)
SeaNode* sea_parser_parse(SeaParser *p);
