#include "sea_parser.h"
#include "sea_memory.h"
#include <stdarg.h>

// ---- Parser helpers ----

static SeaToken cur(SeaParser *p) {
    return p->lexer.current;
}

static SeaToken peek(SeaParser *p) {
    return sea_lexer_peek(&p->lexer);
}

static SeaToken advance(SeaParser *p) {
    SeaToken t = sea_lexer_next(&p->lexer);
    p->last_line = t.line;
    return t;
}

static bool check(SeaParser *p, SeaTK tk) {
    return cur(p).type == tk;
}

static bool match(SeaParser *p, SeaTK tk) {
    if (cur(p).type == tk) {
        advance(p);
        return true;
    }
    return false;
}

static void parser_error(SeaParser *p, const char *fmt, ...) {
    if (p->had_error) return;
    p->had_error = true;
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (p->err_msg) sea_free(p->err_msg, strlen(p->err_msg)+1);
    p->err_msg = (char*)sea_malloc(strlen(msg)+1);
    strcpy(p->err_msg, msg);
}

static bool expect(SeaParser *p, SeaTK tk) {
    if (cur(p).type == tk) {
        advance(p);
        return true;
    }
    parser_error(p, "line %d: expected '%s' but got '%s'",
                 cur(p).line, sea_tk_name(tk), sea_tk_name(cur(p).type));
    return false;
}

// Synchronize on error (skip to next statement boundary)
static void synchronize(SeaParser *p) {
    p->had_error = false;
    while (!check(p, TK_EOF)) {
        SeaTK t = cur(p).type;
        if (t == TK_END || t == TK_ELSE || t == TK_ELSEIF ||
            t == TK_RETURN || t == TK_FUNCTION || t == TK_LOCAL ||
            t == TK_IF || t == TK_FOR || t == TK_WHILE || t == TK_DO) {
            return;
        }
        advance(p);
    }
}

static char* dup_str(const char *s, size_t len) {
    char *out = (char*)sea_malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static char* dup_cstr(const char *s) {
    return dup_str(s, strlen(s));
}

// ---- Forward declarations ----
static SeaNode* parse_expr(SeaParser *p);
static SeaNode* parse_stmt(SeaParser *p);
static SeaNode* parse_block(SeaParser *p);
static SeaNode* parse_simple_expr(SeaParser *p);

// ---- Expression parsing (Pratt parser) ----

typedef enum {
    PREC_NONE = 0,
    PREC_OR,       // or
    PREC_AND,      // and
    PREC_COMPARE,  // == ~= < <= > >=
    PREC_BOR,      // |
    PREC_BXOR,     // ~
    PREC_BAND,     // &
    PREC_SHIFT,    // << >>
    PREC_CONCAT,   // ..
    PREC_ADD,      // + -
    PREC_MUL,      // * / // %
    PREC_UNARY,    // not # ~ - <-
    PREC_POW,      // ^ (right assoc)
    PREC_POSTFIX,  // . [] () :
} Prec;

static Prec token_infix_prec(SeaTK tk) {
    switch (tk) {
        case TK_OR:        return PREC_OR;
        case TK_AND:       return PREC_AND;
        case TK_EQ:
        case TK_NEQ:
        case TK_LT:
        case TK_LE:
        case TK_GT:
        case TK_GE:        return PREC_COMPARE;
        case TK_PIPE:      return PREC_BOR;
        case TK_TILDE:     return PREC_BXOR;
        case TK_AMP:       return PREC_BAND;
        case TK_LSHIFT:
        case TK_RSHIFT:    return PREC_SHIFT;
        case TK_DOTDOT:    return PREC_CONCAT;
        case TK_PLUS:
        case TK_MINUS:     return PREC_ADD;
        case TK_STAR:
        case TK_SLASH:
        case TK_DSLASH:
        case TK_PERCENT:   return PREC_MUL;
        case TK_CARET:     return PREC_POW;
        case TK_DOT:
        case TK_LBRACKET:
        case TK_LPAREN:
        case TK_COLON:     return PREC_POSTFIX;
        case TK_ARROW_LEFT: return PREC_POSTFIX; // chan send in expression
        case TK_QUESTION:  return PREC_POSTFIX;
        default:           return PREC_NONE;
    }
}

static SeaBinop tk_to_binop(SeaTK tk) {
    switch (tk) {
        case TK_PLUS:    return BINOP_ADD;
        case TK_MINUS:   return BINOP_SUB;
        case TK_STAR:    return BINOP_MUL;
        case TK_SLASH:   return BINOP_DIV;
        case TK_DSLASH:  return BINOP_IDIV;
        case TK_PERCENT: return BINOP_MOD;
        case TK_CARET:   return BINOP_POW;
        case TK_EQ:      return BINOP_EQ;
        case TK_NEQ:     return BINOP_NEQ;
        case TK_LT:      return BINOP_LT;
        case TK_LE:      return BINOP_LE;
        case TK_GT:      return BINOP_GT;
        case TK_GE:      return BINOP_GE;
        case TK_AND:     return BINOP_AND;
        case TK_OR:      return BINOP_OR;
        case TK_DOTDOT:  return BINOP_CONCAT;
        case TK_AMP:     return BINOP_BAND;
        case TK_PIPE:    return BINOP_BOR;
        case TK_TILDE:   return BINOP_BXOR;
        case TK_LSHIFT:  return BINOP_SHL;
        case TK_RSHIFT:  return BINOP_SHR;
        default:         return BINOP_ADD; // unreachable
    }
}

// Parse argument list: (a, b, c) - returns array of expressions
static SeaNode** parse_arg_list(SeaParser *p, int *argc) {
    SeaNode **args = NULL;
    int count = 0, cap = 0;

    if (!check(p, TK_RPAREN)) {
        do {
            SeaNode *arg = parse_expr(p);
            if (!arg || p->had_error) break;
            SEA_DYNARRAY_PUSH(args, count, cap, SeaNode*, arg);
        } while (match(p, TK_COMMA) && !p->had_error);
    }
    *argc = count;
    return args;
}

// Parse a primary expression (literal, ident, grouped, etc.)
static SeaNode* parse_primary(SeaParser *p) {
    SeaToken t = cur(p);
    int line = t.line;

    switch (t.type) {
        case TK_NIL: {
            advance(p);
            return sea_node_alloc(NODE_NIL, line);
        }
        case TK_TRUE: {
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_BOOL, line);
            n->boolean = true;
            return n;
        }
        case TK_FALSE: {
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_BOOL, line);
            n->boolean = false;
            return n;
        }
        case TK_NUMBER: {
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_NUMBER, line);
            n->number = t.val.number;
            n->num_is_float = t.is_float;
            return n;
        }
        case TK_STRING: {
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_STRING, line);
            n->string.ptr = dup_str(t.val.string.ptr, t.val.string.len);
            n->string.len = t.val.string.len;
            // Free the lexer-allocated string
            sea_free((void*)t.val.string.ptr, t.val.string.len + 1);
            return n;
        }
        case TK_IDENT: {
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_IDENT, line);
            n->ident = dup_str(t.start, (size_t)t.len);
            return n;
        }
        case TK_DOTDOTDOT: {
            advance(p);
            return sea_node_alloc(NODE_VARARG, line);
        }
        case TK_LPAREN: {
            advance(p);
            SeaNode *inner = parse_expr(p);
            expect(p, TK_RPAREN);
            return inner;
        }
        case TK_FUNCTION: {
            advance(p);
            return parse_simple_expr(p);  // will handle function body
        }
        case TK_LBRACE: {
            // Table literal
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_TABLE, line);
            SeaNode **keys = NULL;
            SeaNode **vals = NULL;
            int count = 0, keys_cap = 0, vals_cap = 0;

            while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                SeaNode *key = NULL;
                SeaNode *val = NULL;

                if (check(p, TK_IDENT) && peek(p).type == TK_ASSIGN) {
                    // name = expr
                    SeaToken kt = cur(p);
                    advance(p); advance(p); // consume ident and =
                    key = sea_node_alloc(NODE_STRING, kt.line);
                    key->string.ptr = dup_str(kt.start, (size_t)kt.len);
                    key->string.len = (size_t)kt.len;
                    val = parse_expr(p);
                } else if (check(p, TK_LBRACKET)) {
                    // [expr] = expr
                    advance(p);
                    key = parse_expr(p);
                    expect(p, TK_RBRACKET);
                    expect(p, TK_ASSIGN);
                    val = parse_expr(p);
                } else {
                    // positional
                    val = parse_expr(p);
                }

                if (!val || p->had_error) break;
                /* Ensure both arrays have capacity for index `count` */
                if (count >= keys_cap) {
                    int old = keys_cap;
                    keys_cap = old < 8 ? 8 : old * 2;
                    keys = SEA_REALLOC_N(keys, SeaNode*, old, keys_cap);
                }
                if (count >= vals_cap) {
                    int old = vals_cap;
                    vals_cap = old < 8 ? 8 : old * 2;
                    vals = SEA_REALLOC_N(vals, SeaNode*, old, vals_cap);
                }
                keys[count] = key;
                vals[count] = val;
                count++;

                if (!match(p, TK_COMMA)) match(p, TK_SEMICOLON);
            }
            expect(p, TK_RBRACE);

            n->table.keys = keys;
            n->table.vals = vals;
            n->table.count = count;
            return n;
        }
        case TK_ARROW_LEFT: {
            // Channel receive: <-expr
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_CHAN_RECV, line);
            n->unop.op = UNOP_CHAN_RECV;
            n->unop.expr = parse_expr(p);
            return n;
        }
        case TK_LBRACKET: {
            advance(p);
            if (check(p, TK_ASSIGN)) {
                // [=] → map literal
                advance(p);
                expect(p, TK_RBRACKET);
                return sea_node_alloc(NODE_MAP_LIT, line);
            }
            // [] or [cap_expr] → array literal
            SeaNode *n = sea_node_alloc(NODE_ARRAY_LIT, line);
            if (check(p, TK_RBRACKET)) {
                n->array_lit.cap = NULL;
            } else {
                n->array_lit.cap = parse_expr(p);
            }
            expect(p, TK_RBRACKET);
            return n;
        }
        default:
            parser_error(p, "line %d: unexpected token '%s' in expression",
                         t.line, sea_tk_name(t.type));
            return NULL;
    }
}

// Parse a function body (after 'function')
// Called either from function statement or function expression
static SeaNode* parse_func_body(SeaParser *p, int line, const char *name) {
    SeaNode *n = sea_node_alloc(NODE_FUNC, line);
    n->func.name = name ? dup_cstr(name) : NULL;

    expect(p, TK_LPAREN);

    SeaParam *params = NULL;
    int paramc = 0, paramc_cap = 0;

    if (!check(p, TK_RPAREN)) {
        do {
            SeaParam param;
            if (check(p, TK_DOTDOTDOT)) {
                advance(p);
                param.name = dup_cstr("...");
                param.is_vararg = true;
                SEA_DYNARRAY_PUSH(params, paramc, paramc_cap, SeaParam, param);
                break;
            }
            SeaToken pt = cur(p);
            if (pt.type != TK_IDENT) {
                parser_error(p, "line %d: expected parameter name", pt.line);
                break;
            }
            advance(p);
            param.name = dup_str(pt.start, (size_t)pt.len);
            param.is_vararg = false;
            SEA_DYNARRAY_PUSH(params, paramc, paramc_cap, SeaParam, param);
        } while (match(p, TK_COMMA) && !p->had_error);
    }
    expect(p, TK_RPAREN);

    n->func.params = params;
    n->func.paramc = paramc;

    p->func_depth++;
    n->func.body = parse_block(p);
    p->func_depth--;

    expect(p, TK_END);
    return n;
}

// This handles the continuation of "function" keyword parsing
static SeaNode* parse_simple_expr(SeaParser *p) {
    // Called after consuming TK_FUNCTION
    // Just parse the function body
    int line = cur(p).line;
    SeaNode *n = parse_func_body(p, line, NULL);
    return n;
}

// Parse postfix operations (calls, indexing, field access)
static SeaNode* parse_postfix(SeaParser *p, SeaNode *left) {
    for (;;) {
        SeaToken t = cur(p);
        int line = t.line;

        if (t.type == TK_DOT) {
            advance(p);
            SeaToken field_tok = cur(p);
            if (field_tok.type != TK_IDENT) {
                parser_error(p, "line %d: expected field name after '.'", line);
                return left;
            }
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_FIELD, line);
            n->field.obj = left;
            n->field.field = dup_str(field_tok.start, (size_t)field_tok.len);
            left = n;
        } else if (t.type == TK_LBRACKET) {
            advance(p);
            SeaNode *key = NULL;
            if (!check(p, TK_RBRACKET)) {
                key = parse_expr(p);  // expr[expr] — normal indexing
            }
            // key=NULL means arr[] — append lvalue (only valid on assignment LHS)
            expect(p, TK_RBRACKET);
            SeaNode *n = sea_node_alloc(NODE_INDEX, line);
            n->index.obj = left;
            n->index.key = key;
            left = n;
        } else if (t.type == TK_LPAREN) {
            advance(p);
            int argc = 0;
            SeaNode **args = parse_arg_list(p, &argc);
            expect(p, TK_RPAREN);
            SeaNode *n = sea_node_alloc(NODE_CALL, line);
            n->call.fn = left;
            n->call.args = args;
            n->call.argc = argc;
            left = n;
        } else if (t.type == TK_COLON) {
            // Method call: expr:name(args)
            advance(p);
            SeaToken method_tok = cur(p);
            if (method_tok.type != TK_IDENT) {
                parser_error(p, "line %d: expected method name after ':'", line);
                return left;
            }
            advance(p);
            expect(p, TK_LPAREN);
            int argc = 0;
            SeaNode **args = parse_arg_list(p, &argc);
            expect(p, TK_RPAREN);
            SeaNode *n = sea_node_alloc(NODE_METHOD_CALL, line);
            n->method.obj = left;
            n->method.method = dup_str(method_tok.start, (size_t)method_tok.len);
            n->method.args = args;
            n->method.argc = argc;
            left = n;
        } else if (t.type == TK_ARROW_LEFT) {
            // Chan send expression: left <- right
            advance(p);
            SeaNode *right = parse_expr(p);
            SeaNode *n = sea_node_alloc(NODE_CHAN_SEND, line);
            n->binop.op = BINOP_CHAN_SEND;
            n->binop.left = left;
            n->binop.right = right;
            left = n;
            break; // send is not further chained
        } else if (t.type == TK_QUESTION) {
            // Error propagation: left?
            advance(p);
            SeaNode *n = sea_node_alloc(NODE_PROPAGATE, line);
            n->propagate = left;
            left = n;
        } else if (t.type == TK_STRING) {
            // Implicit call with string arg: f"string"
            advance(p);
            SeaNode *arg = sea_node_alloc(NODE_STRING, line);
            arg->string.ptr = dup_str(t.val.string.ptr, t.val.string.len);
            arg->string.len = t.val.string.len;
            sea_free((void*)t.val.string.ptr, t.val.string.len + 1);

            SeaNode **args = SEA_ALLOC_N(SeaNode*, 1);
            args[0] = arg;
            SeaNode *n = sea_node_alloc(NODE_CALL, line);
            n->call.fn = left;
            n->call.args = args;
            n->call.argc = 1;
            left = n;
        } else {
            break;
        }
    }
    return left;
}

// Pratt expression parser
static SeaNode* parse_expr_prec(SeaParser *p, Prec min_prec) {
    SeaToken t = cur(p);
    int line = t.line;
    SeaNode *left = NULL;

    // Prefix / unary
    switch (t.type) {
        case TK_MINUS: {
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = sea_node_alloc(NODE_UNOP, line);
            n->unop.op = UNOP_NEG;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_NOT: {
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = sea_node_alloc(NODE_UNOP, line);
            n->unop.op = UNOP_NOT;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_TILDE: {
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = sea_node_alloc(NODE_UNOP, line);
            n->unop.op = UNOP_BNOT;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_ARROW_LEFT: {
            // Channel receive as expression: <-ch
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = sea_node_alloc(NODE_CHAN_RECV, line);
            n->unop.op = UNOP_CHAN_RECV;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_FUNCTION: {
            advance(p);
            left = parse_func_body(p, line, NULL);
            break;
        }
        default:
            left = parse_primary(p);
            if (!left) return NULL;
            break;
    }

    if (!left || p->had_error) return left;
    left = parse_postfix(p, left);

    // Infix / binary
    while (!p->had_error) {
        SeaTK op_tk = cur(p).type;
        Prec prec = token_infix_prec(op_tk);
        if (prec <= min_prec) break;

        // Skip postfix ops (handled above)
        if (op_tk == TK_DOT || op_tk == TK_LBRACKET ||
            op_tk == TK_LPAREN || op_tk == TK_COLON ||
            op_tk == TK_QUESTION || op_tk == TK_ARROW_LEFT) break;

        int op_line = cur(p).line;
        advance(p);

        SeaBinop binop = tk_to_binop(op_tk);

        // Right associative: ^ and ..
        Prec next_prec = (op_tk == TK_CARET || op_tk == TK_DOTDOT)
                         ? prec - 1 : prec;

        SeaNode *right = parse_expr_prec(p, next_prec);
        if (!right || p->had_error) return left;

        right = parse_postfix(p, right);

        SeaNode *n = sea_node_alloc(NODE_BINOP, op_line);
        n->binop.op = binop;
        n->binop.left = left;
        n->binop.right = right;
        left = n;
    }

    return left;
}

static SeaNode* parse_expr(SeaParser *p) {
    return parse_expr_prec(p, PREC_NONE);
}

// ---- Statement parsing ----

static SeaNode* parse_local(SeaParser *p) {
    int line = cur(p).line;
    // 'local' already consumed

    // local function name ... end
    if (check(p, TK_FUNCTION)) {
        advance(p);
        SeaToken name_tok = cur(p);
        if (name_tok.type != TK_IDENT) {
            parser_error(p, "line %d: expected function name", name_tok.line);
            return NULL;
        }
        advance(p);
        char *fname = dup_str(name_tok.start, (size_t)name_tok.len);
        SeaNode *fn = parse_func_body(p, line, fname);
        sea_free(fname, strlen(fname)+1);

        // Wrap as: local name = function...
        SeaNode *n = sea_node_alloc(NODE_LOCAL, line);
        n->local.names = SEA_ALLOC_N(char*, 1);
        n->local.names[0] = dup_str(name_tok.start, (size_t)name_tok.len);
        n->local.count = 1;
        n->local.exprs = SEA_ALLOC_N(SeaNode*, 1);
        n->local.exprs[0] = fn;
        n->local.expr_count = 1;
        return n;
    }

    // local (
    //     var1 = expr
    //     var2 = expr
    // )
    if (match(p, TK_LPAREN)) {
        SeaNode *n = sea_node_alloc(NODE_LOCAL, line);
        char **names = NULL;
        int name_count = 0, name_cap = 0;
        SeaNode **exprs = NULL;
        int expr_count = 0, expr_cap = 0;

        while (!check(p, TK_RPAREN) && !check(p, TK_EOF) && !p->had_error) {
            SeaToken nt = cur(p);
            if (nt.type != TK_IDENT) {
                parser_error(p, "line %d: expected variable name", nt.line);
                break;
            }
            advance(p);
            char *nm = dup_str(nt.start, (size_t)nt.len);
            SEA_DYNARRAY_PUSH(names, name_count, name_cap, char*, nm);

            if (!expect(p, TK_ASSIGN)) break;

            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, expr_count, expr_cap, SeaNode*, e);
        }
        expect(p, TK_RPAREN);

        n->local.names = names;
        n->local.count = name_count;
        n->local.exprs = exprs;
        n->local.expr_count = expr_count;
        return n;
    }

    // local a, b, c = e1, e2, e3
    SeaNode *n = sea_node_alloc(NODE_LOCAL, line);
    char **names = NULL;
    int name_count = 0, name_cap = 0;

    do {
        SeaToken nt = cur(p);
        if (nt.type != TK_IDENT) {
            parser_error(p, "line %d: expected variable name", nt.line);
            break;
        }
        advance(p);
        char *nm = dup_str(nt.start, (size_t)nt.len);
        SEA_DYNARRAY_PUSH(names, name_count, name_cap, char*, nm);
    } while (match(p, TK_COMMA) && !p->had_error);

    n->local.names = names;
    n->local.count = name_count;

    SeaNode **exprs = NULL;
    int expr_count = 0, expr_cap = 0;

    if (match(p, TK_ASSIGN)) {
        do {
            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, expr_count, expr_cap, SeaNode*, e);
        } while (match(p, TK_COMMA) && !p->had_error);
    }

    n->local.exprs = exprs;
    n->local.expr_count = expr_count;
    return n;
}

// Check if a node is a valid assignment target
static bool is_lvalue(SeaNode *n) {
    return n && (n->type == NODE_IDENT || n->type == NODE_INDEX || n->type == NODE_FIELD);
}

static SeaNode* parse_assign_or_call(SeaParser *p) {
    int line = cur(p).line;
    SeaNode *first = parse_expr(p);
    if (!first || p->had_error) return first;

    // Check for assignment: a, b = ...
    if (check(p, TK_COMMA) || check(p, TK_ASSIGN)) {
        // It's an assignment
        SeaNode **targets = NULL;
        int tcount = 0, tcap = 0;

        if (!is_lvalue(first)) {
            parser_error(p, "line %d: invalid assignment target", line);
            sea_node_free(first);
            return NULL;
        }
        SEA_DYNARRAY_PUSH(targets, tcount, tcap, SeaNode*, first);

        while (match(p, TK_COMMA)) {
            SeaNode *t2 = parse_expr(p);
            if (!t2 || !is_lvalue(t2)) {
                parser_error(p, "line %d: invalid assignment target", line);
                break;
            }
            SEA_DYNARRAY_PUSH(targets, tcount, tcap, SeaNode*, t2);
        }

        expect(p, TK_ASSIGN);

        SeaNode **exprs = NULL;
        int ecount = 0, ecap = 0;
        do {
            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, ecount, ecap, SeaNode*, e);
        } while (match(p, TK_COMMA) && !p->had_error);

        SeaNode *n = sea_node_alloc(NODE_ASSIGN, line);
        n->assign.targets = targets;
        n->assign.count = tcount;
        n->assign.exprs = exprs;
        n->assign.expr_count = ecount;
        return n;
    }

    // It's a call expression used as statement
    if (first->type == NODE_CALL || first->type == NODE_METHOD_CALL ||
        first->type == NODE_CHAN_SEND) {
        SeaNode *n = sea_node_alloc(NODE_EXPR_STMT, line);
        n->expr_stmt = first;
        return n;
    }

    // Single expression - might be an assignment with <- send
    if (first->type == NODE_BINOP && first->binop.op == BINOP_CHAN_SEND) {
        SeaNode *n = sea_node_alloc(NODE_EXPR_STMT, line);
        n->expr_stmt = first;
        return n;
    }

    parser_error(p, "line %d: expression is not a statement", line);
    sea_node_free(first);
    return NULL;
}

static SeaNode* parse_if(SeaParser *p) {
    int line = cur(p).line;
    // 'if' already consumed

    SeaNode *cond = parse_expr(p);
    expect(p, TK_THEN);
    SeaNode *then_body = parse_block(p);
    SeaNode *else_body = NULL;

    if (check(p, TK_ELSEIF)) {
        advance(p);
        else_body = parse_if(p);  // recursive for elseif chains
        // The "end" will be consumed by the inner if
    } else if (match(p, TK_ELSE)) {
        else_body = parse_block(p);
        expect(p, TK_END);
    } else {
        expect(p, TK_END);
    }

    SeaNode *n = sea_node_alloc(NODE_IF, line);
    n->if_stmt.cond = cond;
    n->if_stmt.then_body = then_body;
    n->if_stmt.else_body = else_body;
    return n;
}

static SeaNode* parse_while(SeaParser *p) {
    int line = cur(p).line;
    SeaNode *cond = parse_expr(p);
    expect(p, TK_DO);
    p->loop_depth++;
    SeaNode *body = parse_block(p);
    p->loop_depth--;
    expect(p, TK_END);

    SeaNode *n = sea_node_alloc(NODE_WHILE, line);
    n->while_loop.cond = cond;
    n->while_loop.body = body;
    return n;
}

static SeaNode* parse_for(SeaParser *p) {
    int line = cur(p).line;
    // 'for' already consumed

    SeaToken var_tok = cur(p);
    if (var_tok.type != TK_IDENT) {
        parser_error(p, "line %d: expected variable name in for", line);
        return NULL;
    }
    advance(p);

    if (check(p, TK_ASSIGN)) {
        // Numeric for: for i = start, limit[, step] do
        advance(p);
        SeaNode *start = parse_expr(p);
        expect(p, TK_COMMA);
        SeaNode *limit = parse_expr(p);
        SeaNode *step = NULL;
        if (match(p, TK_COMMA)) {
            step = parse_expr(p);
        }
        expect(p, TK_DO);
        p->loop_depth++;
        SeaNode *body = parse_block(p);
        p->loop_depth--;
        expect(p, TK_END);

        SeaNode *n = sea_node_alloc(NODE_FOR_NUM, line);
        n->for_num.var = dup_str(var_tok.start, (size_t)var_tok.len);
        n->for_num.start = start;
        n->for_num.limit = limit;
        n->for_num.step = step;
        n->for_num.body = body;
        return n;
    }

    // For-in: for k[, v, ...] in expr do
    char **vars = NULL;
    int var_count = 0, var_cap = 0;
    char *first_var = dup_str(var_tok.start, (size_t)var_tok.len);
    SEA_DYNARRAY_PUSH(vars, var_count, var_cap, char*, first_var);

    while (match(p, TK_COMMA)) {
        SeaToken vt = cur(p);
        if (vt.type != TK_IDENT) {
            parser_error(p, "line %d: expected variable name in for-in", vt.line);
            break;
        }
        advance(p);
        char *vn = dup_str(vt.start, (size_t)vt.len);
        SEA_DYNARRAY_PUSH(vars, var_count, var_cap, char*, vn);
    }

    expect(p, TK_IN);
    SeaNode *iter = parse_expr(p);
    expect(p, TK_DO);
    p->loop_depth++;
    SeaNode *body = parse_block(p);
    p->loop_depth--;
    expect(p, TK_END);

    SeaNode *n = sea_node_alloc(NODE_FOR_IN, line);
    n->for_in.vars = vars;
    n->for_in.var_count = var_count;
    n->for_in.iter_expr = iter;
    n->for_in.body = body;
    return n;
}

static SeaNode* parse_return(SeaParser *p) {
    int line = cur(p).line;

    SeaNode **exprs = NULL;
    int count = 0, cap = 0;

    // Return values: everything until end/else/elseif/EOF/;
    if (!check(p, TK_END) && !check(p, TK_ELSE) && !check(p, TK_ELSEIF) &&
        !check(p, TK_CASE) && !check(p, TK_EOF) && !check(p, TK_SEMICOLON)) {
        do {
            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, count, cap, SeaNode*, e);
        } while (match(p, TK_COMMA) && !p->had_error);
    }
    match(p, TK_SEMICOLON);

    SeaNode *n = sea_node_alloc(NODE_RETURN, line);
    n->ret.exprs = exprs;
    n->ret.count = count;
    return n;
}

static SeaNode* parse_select(SeaParser *p) {
    int line = cur(p).line;
    // 'select' already consumed

    SeaSelectCase *cases = NULL;
    int case_count = 0, case_cap = 0;

    while (!check(p, TK_END) && !check(p, TK_EOF)) {
        if (!expect(p, TK_CASE)) break;

        SeaSelectCase sc;
        sc.is_default = false;
        sc.stmt = NULL;
        sc.body = NULL;

        if (check(p, TK_DEFAULT)) {
            advance(p);
            sc.is_default = true;
        } else {
            // Case stmt can be:
            // - receive assignment: x = <-ch  or  x, ok = <-ch
            // - send: ch <- val
            sc.stmt = parse_assign_or_call(p);
        }

        expect(p, TK_THEN);
        sc.body = parse_block(p);

        SEA_DYNARRAY_PUSH(cases, case_count, case_cap, SeaSelectCase, sc);
    }

    expect(p, TK_END);

    SeaNode *n = sea_node_alloc(NODE_SELECT, line);
    n->select.cases = cases;
    n->select.count = case_count;
    return n;
}

static SeaNode* parse_function_stmt(SeaParser *p) {
    int line = cur(p).line;
    // 'function' already consumed

    // function name[.name...][method] (params) body end
    SeaToken name_tok = cur(p);
    if (name_tok.type != TK_IDENT) {
        parser_error(p, "line %d: expected function name", name_tok.line);
        return NULL;
    }
    advance(p);

    char *base_name = dup_str(name_tok.start, (size_t)name_tok.len);
    SeaNode *target = sea_node_alloc(NODE_IDENT, line);
    target->ident = dup_cstr(base_name);

    bool is_method = false;
    char *method_name = NULL;

    while (check(p, TK_DOT)) {
        advance(p);
        SeaToken ft = cur(p);
        if (ft.type != TK_IDENT) {
            parser_error(p, "line %d: expected field name", ft.line);
            break;
        }
        advance(p);
        SeaNode *field = sea_node_alloc(NODE_FIELD, ft.line);
        field->field.obj = target;
        field->field.field = dup_str(ft.start, (size_t)ft.len);
        target = field;
    }

    // Method definition: function obj:method(...)
    if (check(p, TK_COLON)) {
        advance(p);
        SeaToken mt = cur(p);
        if (mt.type != TK_IDENT) {
            parser_error(p, "line %d: expected method name", mt.line);
        } else {
            advance(p);
            method_name = dup_str(mt.start, (size_t)mt.len);
            is_method = true;
        }
    }

    SeaNode *fn = parse_func_body(p, line, base_name);

    // For method syntax, add 'self' as first parameter
    if (is_method && fn && fn->type == NODE_FUNC) {
        int old_count = fn->func.paramc;
        SeaParam *new_params = SEA_ALLOC_N(SeaParam, old_count + 1);
        new_params[0].name = dup_cstr("self");
        new_params[0].is_vararg = false;
        if (old_count > 0) {
            memcpy(new_params + 1, fn->func.params, sizeof(SeaParam) * (size_t)old_count);
            sea_free(fn->func.params, sizeof(SeaParam) * (size_t)old_count);
        }
        fn->func.params = new_params;
        fn->func.paramc = old_count + 1;
    }

    sea_free(base_name, strlen(base_name)+1);

    // Build assignment: target = fn  (or target.method = fn)
    if (is_method) {
        SeaNode *field = sea_node_alloc(NODE_FIELD, line);
        field->field.obj = target;
        field->field.field = method_name;
        target = field;
    }

    SeaNode **targets = SEA_ALLOC_N(SeaNode*, 1);
    targets[0] = target;
    SeaNode **exprs = SEA_ALLOC_N(SeaNode*, 1);
    exprs[0] = fn;

    SeaNode *assign = sea_node_alloc(NODE_ASSIGN, line);
    assign->assign.targets = targets;
    assign->assign.count = 1;
    assign->assign.exprs = exprs;
    assign->assign.expr_count = 1;
    return assign;
}

static SeaNode* parse_stmt(SeaParser *p) {
    SeaToken t = cur(p);
    int line = t.line;

    // Skip semicolons
    while (check(p, TK_SEMICOLON)) advance(p);

    switch (t.type) {
        case TK_LOCAL:
            advance(p);
            return parse_local(p);

        case TK_CONST: {
            advance(p);
            if (check(p, TK_FUNCTION)) {
                parser_error(p, "line %d: 'const function' is not supported; use 'local function'", cur(p).line);
                return NULL;
            }
            SeaNode *n = parse_local(p);
            if (n) n->local.is_const = true;
            return n;
        }

        case TK_IF:
            advance(p);
            return parse_if(p);

        case TK_WHILE:
            advance(p);
            return parse_while(p);

        case TK_FOR:
            advance(p);
            return parse_for(p);

        case TK_RETURN:
            advance(p);
            return parse_return(p);

        case TK_BREAK:
            advance(p);
            if (p->loop_depth == 0) {
                parser_error(p, "line %d: 'break' outside of loop", line);
            }
            return sea_node_alloc(NODE_BREAK, line);

        case TK_CONTINUE:
            advance(p);
            if (p->loop_depth == 0) {
                parser_error(p, "line %d: 'continue' outside of loop", line);
            }
            return sea_node_alloc(NODE_CONTINUE, line);

        case TK_DO: {
            advance(p);
            SeaNode *body = parse_block(p);
            expect(p, TK_END);
            SeaNode *n = sea_node_alloc(NODE_DO, line);
            n->block = body->block;
            // Steal the block contents
            body->type = NODE_NIL; // prevent double free
            sea_node_free(body);
            return n;
        }

        case TK_FUNCTION:
            advance(p);
            return parse_function_stmt(p);

        case TK_GO: {
            advance(p);
            SeaNode *call = parse_expr(p);
            if (!call) return NULL;
            SeaNode *n = sea_node_alloc(NODE_GO, line);
            n->go_call = call;
            return n;
        }

        case TK_SELECT:
            advance(p);
            return parse_select(p);

        case TK_DEFER: {
            advance(p);
            SeaNode *stmt = parse_expr(p);
            SeaNode *n = sea_node_alloc(NODE_DEFER, line);
            n->defer_stmt = stmt;
            return n;
        }

        case TK_PANIC: {
            advance(p);
            expect(p, TK_LPAREN);
            SeaNode *val = parse_expr(p);
            expect(p, TK_RPAREN);
            SeaNode *n = sea_node_alloc(NODE_PANIC, line);
            n->panic_expr = val;
            return n;
        }

        case TK_DEL: {
            advance(p);
            SeaNode *target = parse_expr(p);
            if (!target || target->type != NODE_INDEX || target->index.key == NULL) {
                parser_error(p, "line %d: 'del' expects indexed expression (arr[i] or map[k])", line);
                return NULL;
            }
            SeaNode *n = sea_node_alloc(NODE_DEL, line);
            n->del_stmt = target;
            return n;
        }

        default:
            return parse_assign_or_call(p);
    }
}

static bool is_block_end(SeaParser *p) {
    SeaTK t = cur(p).type;
    return t == TK_END || t == TK_ELSE || t == TK_ELSEIF || t == TK_CASE || t == TK_EOF;
}

static SeaNode* parse_block(SeaParser *p) {
    int line = cur(p).line;
    SeaNode **stmts = NULL;
    int count = 0, cap = 0;

    while (!is_block_end(p) && !p->had_error) {
        while (check(p, TK_SEMICOLON)) advance(p);
        if (is_block_end(p)) break;

        SeaNode *stmt = parse_stmt(p);
        if (p->had_error) {
            sea_node_free(stmt);
            break;
        }
        if (stmt) {
            SEA_DYNARRAY_PUSH(stmts, count, cap, SeaNode*, stmt);
        }

        // Return is always the last statement in a block
        if (stmt && stmt->type == NODE_RETURN) break;

        // Enforce one statement per line: the next token must either be on a
        // new line, be a semicolon (explicit separator), or end the block.
        if (!is_block_end(p) && !check(p, TK_SEMICOLON) && !p->had_error) {
            if (cur(p).line == p->last_line) {
                parser_error(p, "line %d: multiple statements on the same line; "
                             "use a newline to separate statements",
                             cur(p).line);
            }
        }
    }

    SeaNode *block = sea_node_alloc(NODE_BLOCK, line);
    block->block.stmts = stmts;
    block->block.count = count;
    return block;
}

// ---- Public API ----

void sea_parser_init(SeaParser *p, const char *source) {
    sea_lexer_init(&p->lexer, source);
    p->err_msg = NULL;
    p->had_error = false;
    p->loop_depth = 0;
    p->func_depth = 0;
    p->last_line  = 0;
}

void sea_parser_free(SeaParser *p) {
    sea_lexer_free(&p->lexer);
    if (p->err_msg) {
        sea_free(p->err_msg, strlen(p->err_msg)+1);
        p->err_msg = NULL;
    }
}

SeaNode* sea_parser_parse(SeaParser *p) {
    SeaNode *prog = parse_block(p);
    if (p->had_error) {
        // Copy error from lexer if we don't have one
        if (!p->err_msg && p->lexer.err_msg) {
            p->err_msg = (char*)sea_malloc(strlen(p->lexer.err_msg)+1);
            strcpy(p->err_msg, p->lexer.err_msg);
        }
        sea_node_free(prog);
        return NULL;
    }
    if (!check(p, TK_EOF)) {
        parser_error(p, "line %d: unexpected token '%s'",
                     cur(p).line, sea_tk_name(cur(p).type));
        sea_node_free(prog);
        return NULL;
    }
    return prog;
}
