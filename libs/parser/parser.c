/*
 * parser.so — expose the Sea parser as a Sea library.
 *
 * API:
 *   local parser = require("./libs/parser/parser")
 *   local r = parser.parse(source_string)
 *   local r = parser.parse_file(path)
 *
 * Returns {ok: bool, ast: node|nil, error: string|nil, line: int|nil}
 *
 * Each AST node is a table with at least "type" (string) and "line" (int).
 * See docs/libs.md for the full node schema.
 */

#include "sea.h"
#include "sea_parser.h"
#include "sea_ast.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- low-level helpers ---- */

/* Create a SeaValue string from a C string literal. RC=1; caller must
 * release after storing (tset/apush do this automatically). */
static SeaValue make_str(const char *cstr) {
    SeaString *s = sea_string_from_cstr(cstr);
    return sea_obj_val(&s->obj);
}

/* Create a SeaValue string from ptr+len (for Sea string literals). */
static SeaValue make_str_n(const char *ptr, size_t len) {
    SeaString *s = sea_string_new(ptr, len);
    return sea_obj_val(&s->obj);
}

/* Set a field in a table, releasing the value's RC if it is an object.
 * After this call the table owns the value (RC=1 in table). */
static void tset(SeaTable *t, const char *key, SeaValue val) {
    sea_table_set_str(t, key, val);
    if (sea_is_obj(val)) sea_obj_release(sea_as_obj(val));
}

/* Push a value into an array, releasing the value's RC if it is an object. */
static void apush(SeaArray *a, SeaValue val) {
    sea_array_push(a, val);
    if (sea_is_obj(val)) sea_obj_release(sea_as_obj(val));
}

/* ---- operator names ---- */

static const char *binop_name(SeaBinop op) {
    switch (op) {
        case BINOP_ADD:      return "+";
        case BINOP_SUB:      return "-";
        case BINOP_MUL:      return "*";
        case BINOP_DIV:      return "/";
        case BINOP_IDIV:     return "//";
        case BINOP_MOD:      return "%";
        case BINOP_POW:      return "^";
        case BINOP_EQ:       return "==";
        case BINOP_NEQ:      return "!=";
        case BINOP_LT:       return "<";
        case BINOP_LE:       return "<=";
        case BINOP_GT:       return ">";
        case BINOP_GE:       return ">=";
        case BINOP_AND:      return "and";
        case BINOP_OR:       return "or";
        case BINOP_CONCAT:   return "..";
        case BINOP_BAND:     return "&";
        case BINOP_BOR:      return "|";
        case BINOP_BXOR:     return "~";
        case BINOP_SHL:      return "<<";
        case BINOP_SHR:      return ">>";
        case BINOP_CHAN_SEND: return "<-";
        default:             return "?";
    }
}

static const char *unop_name(SeaUnop op) {
    switch (op) {
        case UNOP_NEG:      return "-";
        case UNOP_NOT:      return "not";
        case UNOP_BNOT:     return "~";
        case UNOP_CHAN_RECV: return "<-";
        default:            return "?";
    }
}

static const char *node_type_str(SeaNodeType t) {
    switch (t) {
        case NODE_NIL:         return "nil";
        case NODE_BOOL:        return "bool";
        case NODE_NUMBER:      return "number";
        case NODE_STRING:      return "string";
        case NODE_IDENT:       return "ident";
        case NODE_VARARG:      return "vararg";
        case NODE_UNOP:        return "unop";
        case NODE_BINOP:       return "binop";
        case NODE_INDEX:       return "index";
        case NODE_FIELD:       return "field";
        case NODE_CALL:        return "call";
        case NODE_METHOD_CALL: return "method_call";
        case NODE_FUNC:        return "func";
        case NODE_TABLE:       return "table";
        case NODE_ARRAY_LIT:   return "array_lit";
        case NODE_MAP_LIT:     return "map_lit";
        case NODE_CHAN_RECV:   return "chan_recv";
        case NODE_CHAN_SEND:   return "chan_send";
        case NODE_PROPAGATE:   return "propagate";
        case NODE_BLOCK:       return "block";
        case NODE_LOCAL:       return "local";
        case NODE_ASSIGN:      return "assign";
        case NODE_IF:          return "if";
        case NODE_WHILE:       return "while";
        case NODE_FOR_NUM:     return "for_num";
        case NODE_FOR_IN:      return "for_in";
        case NODE_RETURN:      return "return";
        case NODE_BREAK:       return "break";
        case NODE_CONTINUE:    return "continue";
        case NODE_DO:          return "do";
        case NODE_GO:          return "go";
        case NODE_SELECT:      return "select";
        case NODE_DEFER:       return "defer";
        case NODE_PANIC:       return "panic";
        case NODE_DEL:         return "del";
        case NODE_EXPR_STMT:   return "expr_stmt";
        default:               return "unknown";
    }
}

/* ---- AST → Sea table conversion ---- */

static SeaValue node_to_val(SeaNode *n);  /* forward declaration */

/* Convert an array of SeaNode* into a Sea array. Returns SeaValue (RC=1). */
static SeaValue nodes_to_arr(SeaNode **nodes, int count) {
    SeaArray *a = sea_array_new((uint32_t)(count < 0 ? 0 : count));
    for (int i = 0; i < count; i++)
        apush(a, node_to_val(nodes[i]));
    return sea_obj_val(&a->obj);
}

/* Convert an array of char* into a Sea array of strings. Returns SeaValue (RC=1). */
static SeaValue strs_to_arr(char **strs, int count) {
    SeaArray *a = sea_array_new((uint32_t)(count < 0 ? 0 : count));
    for (int i = 0; i < count; i++)
        apush(a, make_str(strs[i]));
    return sea_obj_val(&a->obj);
}

static SeaValue node_to_val(SeaNode *n) {
    if (!n) return SEA_NIL_VAL;

    SeaTable *t = sea_table_new(TABLE_MAP);
    tset(t, "type", make_str(node_type_str(n->type)));
    tset(t, "line", sea_int_val(n->line));

    switch (n->type) {

        /* --- literals with no extra fields --- */
        case NODE_NIL:
        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_VARARG:
        case NODE_MAP_LIT:
            break;

        case NODE_BOOL:
            tset(t, "value", sea_bool_val(n->boolean));
            break;

        case NODE_NUMBER:
            if (n->num_is_float)
                tset(t, "value", sea_number_val(n->number));
            else
                tset(t, "value", sea_int_val((int64_t)n->number));
            tset(t, "is_float", sea_bool_val(n->num_is_float));
            break;

        case NODE_STRING:
            tset(t, "value", make_str_n(n->string.ptr, n->string.len));
            break;

        case NODE_IDENT:
            tset(t, "name", make_str(n->ident));
            break;

        /* --- operations --- */
        case NODE_UNOP:
            tset(t, "op",      make_str(unop_name(n->unop.op)));
            tset(t, "operand", node_to_val(n->unop.expr));
            break;

        case NODE_BINOP:
            tset(t, "op",    make_str(binop_name(n->binop.op)));
            tset(t, "left",  node_to_val(n->binop.left));
            tset(t, "right", node_to_val(n->binop.right));
            break;

        /* --- access --- */
        case NODE_INDEX:
            tset(t, "object", node_to_val(n->index.obj));
            tset(t, "index",  node_to_val(n->index.key));
            break;

        case NODE_FIELD:
            tset(t, "object", node_to_val(n->field.obj));
            tset(t, "field",  make_str(n->field.field));
            break;

        /* --- calls --- */
        case NODE_CALL:
            tset(t, "callee", node_to_val(n->call.fn));
            tset(t, "args",   nodes_to_arr(n->call.args, n->call.argc));
            break;

        case NODE_METHOD_CALL:
            tset(t, "object", node_to_val(n->method.obj));
            tset(t, "method", make_str(n->method.method));
            tset(t, "args",   nodes_to_arr(n->method.args, n->method.argc));
            break;

        /* --- function definition --- */
        case NODE_FUNC: {
            SeaArray *params = sea_array_new((uint32_t)n->func.paramc);
            for (int i = 0; i < n->func.paramc; i++) {
                SeaTable *p = sea_table_new(TABLE_MAP);
                tset(p, "name",      make_str(n->func.params[i].name));
                tset(p, "is_vararg", sea_bool_val(n->func.params[i].is_vararg));
                apush(params, sea_obj_val(&p->obj));
            }
            tset(t, "params", sea_obj_val(&params->obj));
            tset(t, "body", node_to_val(n->func.body));
            tset(t, "name", n->func.name ? make_str(n->func.name) : SEA_NIL_VAL);
            break;
        }

        /* --- collection literals --- */
        case NODE_TABLE: {
            SeaArray *fields = sea_array_new((uint32_t)n->table.count);
            for (int i = 0; i < n->table.count; i++) {
                SeaTable *f = sea_table_new(TABLE_MAP);
                tset(f, "key",   n->table.keys[i]
                                    ? node_to_val(n->table.keys[i])
                                    : SEA_NIL_VAL);
                tset(f, "value", node_to_val(n->table.vals[i]));
                apush(fields, sea_obj_val(&f->obj));
            }
            tset(t, "fields", sea_obj_val(&fields->obj));
            break;
        }

        case NODE_ARRAY_LIT:
            tset(t, "cap", n->array_lit.cap
                               ? node_to_val(n->array_lit.cap)
                               : SEA_NIL_VAL);
            break;

        /* --- channel operations (reuse unop/binop union fields) --- */
        case NODE_CHAN_RECV:
            tset(t, "channel", node_to_val(n->unop.expr));
            break;

        case NODE_CHAN_SEND:
            tset(t, "channel", node_to_val(n->binop.left));
            tset(t, "value",   node_to_val(n->binop.right));
            break;

        /* --- error propagation --- */
        case NODE_PROPAGATE:
            tset(t, "expr", node_to_val(n->propagate));
            break;

        /* --- statements --- */
        case NODE_BLOCK: {
            tset(t, "stmts", nodes_to_arr(n->block.stmts, n->block.count));
            break;
        }

        case NODE_LOCAL: {
            tset(t, "names",  strs_to_arr(n->local.names, n->local.count));
            tset(t, "values", nodes_to_arr(n->local.exprs, n->local.expr_count));
            break;
        }

        case NODE_ASSIGN:
            tset(t, "targets", nodes_to_arr(n->assign.targets, n->assign.count));
            tset(t, "values",  nodes_to_arr(n->assign.exprs,   n->assign.expr_count));
            break;

        case NODE_IF:
            tset(t, "cond",      node_to_val(n->if_stmt.cond));
            tset(t, "then_body", node_to_val(n->if_stmt.then_body));
            tset(t, "else_body", node_to_val(n->if_stmt.else_body));
            break;

        case NODE_WHILE:
            tset(t, "cond", node_to_val(n->while_loop.cond));
            tset(t, "body", node_to_val(n->while_loop.body));
            break;

        case NODE_FOR_NUM:
            tset(t, "var",   make_str(n->for_num.var));
            tset(t, "start", node_to_val(n->for_num.start));
            tset(t, "limit", node_to_val(n->for_num.limit));
            tset(t, "step",  n->for_num.step
                                 ? node_to_val(n->for_num.step)
                                 : SEA_NIL_VAL);
            tset(t, "body",  node_to_val(n->for_num.body));
            break;

        case NODE_FOR_IN:
            tset(t, "vars", strs_to_arr(n->for_in.vars, n->for_in.var_count));
            tset(t, "iter", node_to_val(n->for_in.iter_expr));
            tset(t, "body", node_to_val(n->for_in.body));
            break;

        case NODE_RETURN:
            tset(t, "values", nodes_to_arr(n->ret.exprs, n->ret.count));
            break;

        case NODE_DO:
            tset(t, "stmts", nodes_to_arr(n->block.stmts, n->block.count));
            break;

        case NODE_GO:
            tset(t, "call", node_to_val(n->go_call));
            break;

        case NODE_SELECT: {
            SeaArray *cases = sea_array_new((uint32_t)n->select.count);
            for (int i = 0; i < n->select.count; i++) {
                SeaSelectCase *sc = &n->select.cases[i];
                SeaTable *c = sea_table_new(TABLE_MAP);
                tset(c, "stmt",       node_to_val(sc->stmt));
                tset(c, "body",       node_to_val(sc->body));
                tset(c, "is_default", sea_bool_val(sc->is_default));
                apush(cases, sea_obj_val(&c->obj));
            }
            tset(t, "cases", sea_obj_val(&cases->obj));
            break;
        }

        case NODE_DEFER:
            tset(t, "call", node_to_val(n->defer_stmt));
            break;

        case NODE_PANIC:
            tset(t, "value", node_to_val(n->panic_expr));
            break;

        case NODE_DEL:
            tset(t, "target", node_to_val(n->del_stmt));
            break;

        case NODE_EXPR_STMT:
            tset(t, "expr", node_to_val(n->expr_stmt));
            break;

        default:
            break;
    }

    return sea_obj_val(&t->obj);
}

/* ---- build result table from a completed parse ---- */

static SeaValue build_result(SeaParser *p, SeaNode *root) {
    SeaTable *res = sea_table_new(TABLE_MAP);
    if (!root) {
        const char *err = (p->had_error && p->err_msg) ? p->err_msg
                                                        : "unknown parse error";
        tset(res, "ok",    SEA_FALSE_VAL);
        tset(res, "error", make_str(err));   /* copies before parser_free */
        tset(res, "ast",   SEA_NIL_VAL);
        tset(res, "line",  sea_int_val(p->last_line));
    } else {
        SeaValue ast = node_to_val(root);
        sea_node_free(root);
        tset(res, "ok",    SEA_TRUE_VAL);
        tset(res, "ast",   ast);
        tset(res, "error", SEA_NIL_VAL);
        tset(res, "line",  SEA_NIL_VAL);
    }
    return sea_obj_val(&res->obj);
}

/* ---- exported functions ---- */

/* parse(source: string) -> {ok, ast, error, line} */
static SeaValue fn_parse(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_obj_is_type(args[0], OBJ_STRING)) {
        SeaTable *res = sea_table_new(TABLE_MAP);
        tset(res, "ok",    SEA_FALSE_VAL);
        tset(res, "error", make_str("parse: expected a string argument"));
        tset(res, "ast",   SEA_NIL_VAL);
        tset(res, "line",  SEA_NIL_VAL);
        return sea_obj_val(&res->obj);
    }

    SeaString *src = SEA_AS_STRING(args[0]);
    SeaParser p;
    sea_parser_init(&p, src->data);
    SeaNode *root = sea_parser_parse(&p);
    SeaValue result = build_result(&p, root);
    sea_parser_free(&p);
    return result;
}

/* parse_file(path: string) -> {ok, ast, error, line} */
static SeaValue fn_parse_file(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_obj_is_type(args[0], OBJ_STRING)) {
        SeaTable *res = sea_table_new(TABLE_MAP);
        tset(res, "ok",    SEA_FALSE_VAL);
        tset(res, "error", make_str("parse_file: expected a string argument"));
        tset(res, "ast",   SEA_NIL_VAL);
        tset(res, "line",  SEA_NIL_VAL);
        return sea_obj_val(&res->obj);
    }

    const char *path = SEA_AS_STRING(args[0])->data;

    FILE *f = fopen(path, "rb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "cannot open '%s': %s", path, strerror(errno));
        SeaTable *res = sea_table_new(TABLE_MAP);
        tset(res, "ok",    SEA_FALSE_VAL);
        tset(res, "error", make_str(buf));
        tset(res, "ast",   SEA_NIL_VAL);
        tset(res, "line",  SEA_NIL_VAL);
        return sea_obj_val(&res->obj);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = malloc((size_t)size + 1);
    if (!source) {
        fclose(f);
        SeaTable *res = sea_table_new(TABLE_MAP);
        tset(res, "ok",    SEA_FALSE_VAL);
        tset(res, "error", make_str("parse_file: out of memory"));
        tset(res, "ast",   SEA_NIL_VAL);
        tset(res, "line",  SEA_NIL_VAL);
        return sea_obj_val(&res->obj);
    }

    size_t nread = fread(source, 1, (size_t)size, f);
    source[nread] = '\0';
    fclose(f);

    SeaParser p;
    sea_parser_init(&p, source);
    SeaNode *root = sea_parser_parse(&p);
    SeaValue result = build_result(&p, root);
    sea_parser_free(&p);
    free(source);
    return result;
}

/* ---- module entry point ---- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "parse",      fn_parse);
    sea_mod_set(mod, "parse_file", fn_parse_file);
    return sea_obj_val(&mod->obj);
}
