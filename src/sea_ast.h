/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_common.h"

/*
 * AST node definitions for the Sea parser.
 * All nodes are allocated on the heap and freed recursively.
 */

typedef enum {
    // Literals
    NODE_NIL,
    NODE_BOOL,
    NODE_NUMBER,
    NODE_STRING,
    NODE_IDENT,
    NODE_VARARG,

    // Operations
    NODE_UNOP,
    NODE_BINOP,

    // Access
    NODE_INDEX,        // expr[expr]
    NODE_FIELD,        // expr.name

    // Calls
    NODE_CALL,         // expr(args...)
    NODE_METHOD_CALL,  // expr:name(args...)

    // Function definition
    NODE_FUNC,         // function(params) body end

    // Table/struct literals
    NODE_TABLE,        // {key=val, ...}
    NODE_ARRAY_LIT,
    NODE_MAP_LIT,

    // Channel operations
    NODE_CHAN_RECV,    // <-expr (expression form)
    NODE_CHAN_SEND,    // expr <- expr (expression form)

    // Error propagation
    NODE_PROPAGATE,   // expr?

    // Statements
    NODE_BLOCK,
    NODE_LOCAL,        // local x, y = expr, expr
    NODE_ASSIGN,       // target = expr (or a,b = e1,e2)
    NODE_IF,
    NODE_WHILE,
    NODE_FOR_NUM,      // for i = start, limit[, step] do body end
    NODE_FOR_IN,       // for k, v in expr do body end
    NODE_RETURN,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_DO,           // do ... end
    NODE_GO,           // go expr()
    NODE_SELECT,       // select ... end
    NODE_DEFER,        // defer ...
    NODE_PANIC,        // panic(expr)
    NODE_DEL,          // del expr[expr]
    NODE_EXPR_STMT,    // expression used as statement
} SeaNodeType;

typedef struct SeaNode SeaNode;

// Binary operator codes
typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_IDIV, BINOP_MOD, BINOP_POW,
    BINOP_EQ, BINOP_NEQ, BINOP_LT, BINOP_LE, BINOP_GT, BINOP_GE,
    BINOP_AND, BINOP_OR,
    BINOP_CONCAT,
    BINOP_BAND, BINOP_BOR, BINOP_BXOR, BINOP_SHL, BINOP_SHR,
    BINOP_CHAN_SEND,
} SeaBinop;

typedef enum {
    UNOP_NEG,
    UNOP_NOT,
    UNOP_BNOT,
    UNOP_CHAN_RECV,
} SeaUnop;

// Parameter in a function definition
typedef struct {
    char *name;
    bool  is_vararg;
} SeaParam;

// A select case
typedef struct {
    SeaNode *stmt;     // assignment or send statement
    SeaNode *body;
    bool     is_default;
} SeaSelectCase;

struct SeaNode {
    SeaNodeType type;
    int         line;
    int         col;           /* column of the node's first token (operators: of the operator) */
    bool        num_is_float;  /* for NODE_NUMBER: true when source had '.', 'e', or 'E' */
    bool        paren;         /* written in parentheses (kept by sea/parser's format) */
    union {
        // Literals
        double  number;
        bool    boolean;
        struct { char *ptr; size_t len; } string;
        char   *ident;

        // Unary op
        struct { SeaUnop op; SeaNode *expr; } unop;

        // Binary op
        struct { SeaBinop op; SeaNode *left, *right; } binop;

        // Index: expr[expr]
        struct { SeaNode *obj; SeaNode *key; } index;

        // Field: expr.name
        struct { SeaNode *obj; char *field; } field;

        // Function call: expr(args...)   (end_line: line of the ')')
        struct { SeaNode *fn; SeaNode **args; int argc; int end_line; } call;

        // Method call: expr:name(args...)
        struct { SeaNode *obj; char *method; SeaNode **args; int argc; int end_line; } method;

        // Function definition
        struct {
            SeaParam *params;
            int       paramc;
            SeaNode  *body;
            char     *name;   // optional name
            bool      is_method;  // function a:b() — `self` was added as params[0]
        } func;

        // Table literal: {key=val, ...}
        struct {
            SeaNode **keys;    // NULL entry = array-style (sequential int key)
            SeaNode **vals;
            int       count;
            int       end_line;  // line of the '}'
        } table;

        // Array literal: [] or [cap_expr]
        struct { SeaNode *cap; } array_lit;   // cap=NULL for [], cap=expr for [n]

        // Block of statements (end_line: line of the token that ends it:
        // end, else, elseif, case or the end of the input)
        struct { SeaNode **stmts; int count; int end_line; } block;

        // Local: local x, y = expr, expr  (or const x = expr)
        struct {
            char    **names;
            int       count;
            SeaNode **exprs;
            int       expr_count;
            bool      is_const;
        } local;

        // Assignment: a, b = e1, e2
        struct {
            SeaNode **targets;
            int       count;
            SeaNode **exprs;
            int       expr_count;
        } assign;

        // If statement
        struct {
            SeaNode *cond;
            SeaNode *then_body;
            SeaNode *else_body;  // may be another if (elseif) or block
        } if_stmt;

        // While loop
        struct {
            SeaNode *cond;
            SeaNode *body;
        } while_loop;

        // Numeric for: for i = start, limit, step do body end
        struct {
            char    *var;
            SeaNode *start, *limit, *step;
            SeaNode *body;
        } for_num;

        // For-in: for k, v in expr do body end
        struct {
            char    **vars;
            int       var_count;
            SeaNode  *iter_expr;
            SeaNode  *body;
        } for_in;

        // Return
        struct { SeaNode **exprs; int count; } ret;

        // go expr()
        SeaNode *go_call;

        // select
        struct { SeaSelectCase *cases; int count; } select;

        // defer/panic/del/propagate/expr_stmt
        SeaNode *defer_stmt;
        SeaNode *panic_expr;
        SeaNode *del_stmt;    // del target: NODE_INDEX
        SeaNode *propagate;
        SeaNode *expr_stmt;
    };
};

// Allocate a node
SeaNode* sea_node_alloc(SeaNodeType type, int line);

// Free a node and all its children
void sea_node_free(SeaNode *node);
