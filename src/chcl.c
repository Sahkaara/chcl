/* SPDX-License-Identifier: MIT */

/*
 * chcl.c - Implementation of the chcl HCL toolkit.
 *
 * Architecture overview:
 *   1. Lexer  : turns a source buffer into a stream of tokens.
 *   2. Parser : turns the token stream into an AST (node_t tree).
 *   3. Arena  : bump allocator that owns all AST memory, so freeing
 *               a document is a single call instead of a full tree walk.
 *   4. Formatter : re-emits tokens with canonical whitespace, working
 *                  directly on the token stream (no AST needed).
 *   5. JSON serializer : walks the AST to produce a JSON view.
 *
 * Only a subset of HCL is supported (attributes, blocks, strings,
 * numbers, booleans, null, lists, and object literals). Expressions
 * and interpolations are deliberately NOT supported in this MVP.
 */
#include "chcl.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>

/* ============================================================
 * Arena allocator
 * ------------------------------------------------------------
 * AST nodes are allocated from an arena to simplify ownership:
 * the caller never has to free individual nodes. When the
 * document is released, the arena frees every block at once.
 * ============================================================ */

typedef struct arena_block arena_block_t;
typedef struct { arena_block_t *head; } arena_t;

struct arena_block {
    arena_block_t *next;
    size_t used;    /* byte already handed out */
    size_t cap;     /* capacity of 'data' */
    char data[];    /* flexible array holding the payload */
};

/* Default block size; sized so most small configs fit in one block. */
#define ARENA_BLOCK (64 * 1024)

/* Allocate 'n' bytes (8-bytes aligned). Returns NULL on OOM. */
static void *arena_alloc(arena_t *a, size_t n) {
    n = (n + 7) & ~(size_t)7; /* round up to 8 bytes */
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
        arena_block_t *b = malloc(sizeof(*b) + cap);
        if (!b) return NULL;
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

/* Free every block. The arena becomes reusable (head = NULL). */
static void arena_free(arena_t *a) {
    for (arena_block_t *b = a->head; b; ) {
        arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}

/* Copy 'len' bytes from 's' into the arena, adding a NUL terminator. */
static char *arena_strdup(arena_t *a, const char *s, size_t len) {
    char *p = arena_alloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ============================================================
 * Lexer
 * ------------------------------------------------------------
 * The lexer is a simple hand-written scanner. Every token points
 * into the original source buffer (`start` + `len`), so tokens
 * are cheap to produce and never allocate memory.
 * ============================================================ */

typedef enum {
    TOKEN_EOF,
    TOKEN_NEWLINE,
    TOKEN_IDENT,
    TOKEN_STRING,
    TOKEN_NUMBER,
    /* Structural tokens */
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_EQUALS,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_QUESTION,
    TOKEN_COLON,
    /* NEW: Math and logic operators for HCL expressions */
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_EQEQ,
    TOKEN_NEQ,
    TOKEN_LT,
    TOKEN_GT,
    TOKEN_LTE,
    TOKEN_GTE,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    /* Keywords and comments */
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NULL,
    TOKEN_HEREDOC,
    TOKEN_COMMENT
} token_type_t;

typedef struct {
    token_type_t type;
    const char *start;  /* pointer into the source buffer */
    size_t      len;    /* lenght in bytes */
    int         line;   /* 1-based, for diagnostics */
    int         column; /* 1-based, for diagnostic */
} token_t;

typedef struct {
    const char *src;
    size_t      pos;
    int         line;
    int         column;
} lexer_t;

/* Return the next token from the source. Handles coments and skips */
/* whitespace (but NOT newlines, since HCL is line-sensitive) */
static token_t lexer_next(lexer_t *l) {
    token_t t = {0};

    /* Skip inline whitespace, keeping line/column accurate. */
    while (l->src[l->pos] == ' ' || l->src[l->pos] == '\t' || l->src[l->pos] == '\r') {
        l->pos++;
        l->column++;
    }

    t.start  = l->src + l->pos;
    t.line   = l->line;
    t.column = l->column;
    char c = l->src[l->pos];

    if (c == '\0') { t.type = TOKEN_EOF; return t; }

    /* Newlines are significant: they end attribute assignments. */
    if (c == '\n') {
        l->pos++; l->line++; l->column = 1;
        t.type = TOKEN_NEWLINE; t.len = 1; return t;
    }

    /* Line comments: '#' and '//' to end of line. */
    if (c == '#' || (c == '/' && l->src[l->pos + 1] == '/')) {
        while (l->src[l->pos] && l->src[l->pos] != '\n') {
            l->pos++; l->column++;
        }
        t.type = TOKEN_COMMENT;
        t.len = l->src + l->pos - t.start;
        return t;
    }

    if (c == '/' && l->src[l->pos + 1] == '*') {
        l->pos += 2; l->column += 2;
        while (l->src[l->pos] && !(l->src[l->pos] == '*' && l->src[l->pos + 1] == '/')) {
                if (l->src[l->pos] == '\n') { l->line++; l->column = 1; }
                else l->column++;
                l->pos++;
        }
        if (l->src[l->pos]) { l->pos += 2; l->column += 2; }
        t.type = TOKEN_COMMENT;
        t.len  = l->src + l->pos - t.start;
        return t;
    }

    /* Handle Heredocs: <<EOF ... EOF or <<-EOF ... EOF (trim indentation) */
    if (c == '<' && l->src[l->pos+1] == '<') {
        int start = l->pos;
        l->pos += 2; l->column += 2;
        if (l->src[l->pos] == '-') { l->pos++; l->column++; }
        int id_start = l->pos;
        while (isalnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_') { l->pos++; l->column++; }
        int id_len = l->pos - id_start;
        while (l->src[l->pos] && l->src[l->pos] != '\n') { l->pos++; l->column++; }
        if (l->src[l->pos] == '\n') { l->pos++; l->line++; l->column = 1; }

        /* Scan until we find the closing indentifier on its own line */
        while (l->src[l->pos]) {
            int line_start = l->pos;
            while (l->src[l->pos] && l->src[l->pos] != '\n') { l->pos++; }
            int k = line_start;
            while (l->src[k] == ' ' || l->src[k] == '\t') k++;
            if ((size_t)(l->pos - k) == (size_t)id_len && !strncmp(l->src + k, l->src + id_start, id_len)) {
                if (l->src[l->pos] == '\n') { l->pos++; l->line++; l->column = 1; }
                break;
            }
            if (l->src[l->pos] == '\n') { l->pos++; l->line++; l->column = 1; }
        }
        /* Treat the entire heredoc as a single STRING token */
        t.type = TOKEN_HEREDOC; t.len = l->pos - start; return t;
    }

    /* Single-character structural tokens */
    if (c == '{') { l->pos++; l->column++; t.type = TOKEN_LBRACE;   t.len = 1; return t; }
    if (c == '}') { l->pos++; l->column++; t.type = TOKEN_RBRACE;   t.len = 1; return t; }
    if (c == '[') { l->pos++; l->column++; t.type = TOKEN_LBRACKET; t.len = 1; return t; }
    if (c == ']') { l->pos++; l->column++; t.type = TOKEN_RBRACKET; t.len = 1; return t; }
    if (c == '(') { l->pos++; l->column++; t.type = TOKEN_LPAREN;   t.len = 1; return t; }
    if (c == ')') { l->pos++; l->column++; t.type = TOKEN_RPAREN;   t.len = 1; return t; }
    if (c == ',') { l->pos++; l->column++; t.type = TOKEN_COMMA;    t.len = 1; return t; }
    if (c == '.') { l->pos++; l->column++; t.type = TOKEN_DOT;      t.len = 1; return t; }
    if (c == '?') { l->pos++; l->column++; t.type = TOKEN_QUESTION; t.len = 1; return t; }
    if (c == ':') { l->pos++; l->column++; t.type = TOKEN_COLON;    t.len = 1; return t; }
    if (c == '+') { l->pos++; l->column++; t.type = TOKEN_PLUS;     t.len = 1; return t; }
    if (c == '*') { l->pos++; l->column++; t.type = TOKEN_STAR;     t.len = 1; return t; }
    if (c == '/') { l->pos++; l->column++; t.type = TOKEN_SLASH;    t.len = 1; return t; }
    if (c == '%') { l->pos++; l->column++; t.type = TOKEN_PERCENT;  t.len = 1; return t; }
    if (c == '=') { l->pos++; l->column++; t.type = TOKEN_EQUALS;   t.len = 1; return t; }
    if (c == '!') { l->pos++; l->column++; t.type = TOKEN_NOT;      t.len = 1; return t; }
    if (c == '<') { l->pos++; l->column++; t.type = TOKEN_LT;       t.len = 1; return t; }
    if (c == '>') { l->pos++; l->column++; t.type = TOKEN_GT;       t.len = 1; return t; }

    /* Multi-character operators (must be checked before single-char ones) */
    if (c == '=' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_EQEQ; t.len = 2; return t; }
    if (c == '!' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_NEQ;  t.len = 2; return t; }
    if (c == '<' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_LTE;  t.len = 2; return t; }
    if (c == '>' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_GTE;  t.len = 2; return t; }
    if (c == '&' && l->src[l->pos+1] == '&') { l->pos += 2; l->column += 2; t.type = TOKEN_AND;  t.len = 2; return t; }
    if (c == '|' && l->src[l->pos+1] == '|') { l->pos += 2; l->column += 2; t.type = TOKEN_OR;   t.len = 2; return t; }

    /* String literal (no escape processing on the value itself yet). */
    if (c == '"') {
        l->pos++; l->column++;
        while (l->src[l->pos] && l->src[l->pos] != '"') {
            if (l->src[l->pos] == '\\') { l->pos++; l->column++; }
            if (l->src[l->pos] == '\n') { l->line++; l->column = 1; }
            else l->column++;
            l->pos++;
        }
        if (l->src[l->pos] == '"') { l->pos++; l->column++; }
        t.type = TOKEN_STRING;
        t.len = l->src + l->pos - t.start;
        return t;
    }

    /* Numbers: optional '-', integer part, optional fractional part. */
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)l->src[l->pos + 1]))) {
        if (c == '-') { l->pos++; l->column++; } /* Consume the minus sign */
        while (isdigit((unsigned char)l->src[l->pos])) { l->pos++; l->column++; }
        if (l->src[l->pos] == '.') {
            l->pos++; l->column++;
            while (isdigit((unsigned char)l->src[l->pos])) { l->pos++; l->column++; }
        }
        t.type = TOKEN_NUMBER;
        t.len  = l->src + l->pos - t.start;
        return t;
    }
    
    /* Identifiers and reserved words (true/false/null). */
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_' || l->src[l->pos] == '-') {
            l->pos++; l->column++;
        }
        size_t n = l->src + l->pos - t.start;
        if (n == 4 && !strncmp(t.start, "true", 4)) t.type = TOKEN_TRUE;
        else if (n == 5 && !strncmp(t.start, "false", 5)) t.type = TOKEN_FALSE;
        else if (n == 4 && !strncmp(t.start, "null", 4)) t.type = TOKEN_NULL;
        else t.type = TOKEN_IDENT;
        t.len = n;
        return t;
    }

    /* Unknown character: consume one byte so the parser can report it. */
    l->pos++; l->column++;
    t.type = TOKEN_EOF; t.len = 1;
    return t;

}

/* ============================================================
 * AST (Abstract Syntax Tree)
 * ------------------------------------------------------------
 * A `node_t` represents either a scalar value (string, number,
 * bool, null), a container (list, object), or an HCL block.
 * A `body_t` groups all pairs (key = value) and blocks found in
 * the same scope.
 * ============================================================ */

typedef struct node node_t;
typedef struct { char *key; node_t *value; } pair_t;

typedef struct {
    pair_t *pairs;      size_t count, cap;
    node_t **blocks;    size_t block_count, block_cap;
} body_t;

typedef enum { 
    NODE_STRING,
    NODE_NUMBER,
    NODE_BOOL,
    NODE_NULL,
    NODE_LIST,
    NODE_OBJECT,
    NODE_BLOCK,
    /* NEW: Expression node types for AST parsing */
    NODE_TRAVERSAL,   /* e.g., var.ami_id or local.name */
    NODE_CALL,        /* e.g., max(a, b) or length(var.list) */
    NODE_BINARY,      /* e.g., a + b, a == b, a > b */
    NODE_UNARY,       /* e.g., !var.enabled */
    NODE_CONDITIONAL  /* e.g., var.enabled ? 1 : 0 */
} node_type_t;

struct node {
    node_type_t type;

    /* Basic data types */
    char *s;    /* String value or variable name */
    double n;   /* Number value */
    int b;      /* Boolean value */

    /* Lists and Object */
    node_t **items; size_t item_count, item_cap;
    pair_t *pairs;  size_t pair_count, pair_cap;

    /* Blocks */
    char *block_type;
    char **labels; size_t label_count, label_cap;
    body_t body;

    /* NEW: Expression metadata */
    char *op;                                       /* Operator text (e.g., "+", "==", "?") */
    node_t *left, *right;                           /* Binary and Unary operands */
    node_t *cond, *true_val, *false_val;            /* Conditional branches (cond ? true : false) */
    node_t *callee;                                 /* Function identifier for calls (e.g., "max") */
    node_t **args; size_t arg_count, arg_cap;       /* Arguments list for function calls */
};

/* Top-level document handle. */
struct chcl_document {
    char     *error;   /* error message, or NULL */
    int       line;
    int       column;
    arena_t   arena;   /* owns every allocation tied to this document */
    body_t    root;    /* top-level body */
};

/* ============================================================
 * Parser
 * ------------------------------------------------------------
 * Recursive-descent, one-token lookahead. The parser reports
 * errors by setting doc->error once (first error wins) and then
 * unwinding cleanly. The caller checks chcl_document_error().
 * ============================================================ */

typedef struct {
    lexer_t lex;
    token_t cur;
    token_t peeked;
    int has_peek;
    chcl_document_t *doc;
}parser_t;

/* Consume the current token and advance. */
static void advance(parser_t *p) {
    if (p->has_peek) { p->cur = p->peeked; p->has_peek = 0; }
    else p->cur = lexer_next(&p->lex);
}

/* Record the first error encountered; subsequent calls are no-ops. */
static void set_error(parser_t *p, const char *fmt, ...) {
    if (p->doc->error) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    p->doc->error = arena_strdup(&p->doc->arena, buf, strlen(buf));
    p->doc->line = p->cur.line;
    p->doc->column = p->cur.column;
}

/* Allocate and zero-initialize an AST node in the arena. */
static node_t *alloc_node(parser_t *p, node_type_t t) {
    node_t *n = arena_alloc(&p->doc->arena, sizeof(*n));
    if (!n) { set_error(p, "out of memory"); return NULL; }
    memset(n, 0, sizeof(*n));
    n->type = t;
    return n;
}

/* Extract the txtual content of a token,unquoting string tokens. */
static char *token_text(arena_t *a, token_t tok) {
    if (tok.type == TOKEN_STRING && tok.len >= 2)
        return arena_strdup(a, tok.start + 1, tok.len -2);
            return arena_strdup(a, tok.start, tok.len);
}

/* Grow '*arr' so it can hold at least 'count + 1' elements. */
static int grow(void **arr, size_t *cap, size_t count, size_t elem) {
    if (count + 1 <=*cap) return 1;
    size_t nc = *cap ? *cap * 2 : 8;
    void *p = realloc(*arr, nc * elem);
    if (!p) return 0;
    *arr = p; *cap = nc;
    return 1;
}

static void body_add_pair(parser_t *p, body_t *b, char *key, node_t *v) {
    if (!grow((void **)&b->pairs, &b->cap, b->count, sizeof(pair_t))) {
        set_error(p, "out of memory"); return;
    }
    b->pairs[b->count++] = (pair_t){ key, v };
}

static void body_add_block(parser_t *p, body_t *b, node_t *blk) {
    if (!grow((void **)&b->blocks, &b->block_cap, b->block_count, sizeof(node_t *))) {
        set_error(p, "out of memory"); return;
    }
    b->blocks[b->block_count++] = blk;
}

/* Forward declaration; expression can contain nested bodies. */
static node_t *parse_expr(parser_t *p);

/* Parse a sequence of attributes and blocks until '}' or EOF. */
static body_t parse_body(parser_t *p) {
    body_t b = {0};
    for (;;) {
        /* Skip blank lines and comments between statements. */
        while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
        if (p->cur.type == TOKEN_EOF || p->cur.type == TOKEN_RBRACE) break;
        if (p->cur.type != TOKEN_IDENT) {
            set_error(p, "expected identifier");
            return b;
        }
        
        token_t ident = p->cur;
        advance(p);

        if (p->cur.type == TOKEN_EQUALS) {
            /* 'ident = expr' - an attribute. */
            advance(p);
            node_t *v = parse_expr(p);
            if (!v) return b;
            body_add_pair(p, &b, token_text(&p->doc->arena, ident), v);
            if (p->cur.type == TOKEN_NEWLINE) advance(p);
            else if (p->cur.type != TOKEN_RBRACE && p->cur.type != TOKEN_EOF) {
                set_error(p, "expected newline after attribute");
                return b;
            }
        }else if (p->cur.type == TOKEN_LBRACE || p->cur.type == TOKEN_STRING  || p->cur.type == TOKEN_IDENT) {
            /* `ident "label" "label" { ... }` — a block. */
            node_t *blk = alloc_node(p, NODE_BLOCK);
            if (!blk) return b;
            blk->block_type = token_text(&p->doc->arena, ident);
            while (p->cur.type == TOKEN_STRING || p->cur.type == TOKEN_IDENT) {
                if (!grow((void **)&blk->labels, &blk->label_cap,
                          blk->label_count, sizeof(char *))) {
                    set_error(p, "out of memory"); return b;
                }
                blk->labels[blk->label_count++] = token_text(&p->doc->arena, p->cur);
                advance(p);
            }
            if (p->cur.type != TOKEN_LBRACE) { set_error(p, "expected '{'"); return b; }
            advance(p);
            blk->body = parse_body(p);
            if (p->doc->error) return b;
            if (p->cur.type != TOKEN_RBRACE) { set_error(p, "expected '}'"); return b; }
            advance(p);
            body_add_block(p, &b, blk);
        } else {
            set_error(p, "unexpected token");
            return b;
        }
    }
    return b;
}

/* 
 * Returns the precedence level of an operator token. 
 * Higher numbers bind tighter (e.g., * is 6, + is 5).
 */
static int get_prec(token_type_t t) {
    switch (t) {
        case TOKEN_OR: return 1;
        case TOKEN_AND: return 2;
        case TOKEN_EQEQ: case TOKEN_NEQ: return 3;
        case TOKEN_LT: case TOKEN_GT: case TOKEN_LTE: case TOKEN_GTE: return 4;
        case TOKEN_PLUS: case TOKEN_MINUS: return 5;
        case TOKEN_STAR: case TOKEN_SLASH: case TOKEN_PERCENT: return 6;
        default: return -1;
    }
}

/* 
 * Parses basic values: literals, variable traversals, function calls, lists, and objects.
 */
static node_t *parse_primary(parser_t *p) { 
    /* Handle unary operators: !true or -5 */
    if (p->cur.type == TOKEN_NOT || p->cur.type == TOKEN_MINUS) {
        token_t op = p->cur;
        advance(p);
        node_t *n = alloc_node(p, NODE_UNARY);
        n->op = token_text(&p->doc->arena, op);
        n->left = parse_primary(p); /* Legge il valore subito dopo (es. il 5) */
        return n;
    }
        /* Accept both strings and heredocs as literal string nodes */
    if (p->cur.type == TOKEN_STRING || p->cur.type == TOKEN_HEREDOC) {
        node_t *n = alloc_node(p, NODE_STRING); 
        n->s = token_text(&p->doc->arena, p->cur); 
        advance(p); 
        return n;
    }
    if (p->cur.type == TOKEN_NUMBER) {
        node_t *n = alloc_node(p, NODE_NUMBER); n->n = strtod(p->cur.start, NULL); advance(p); return n;
    }
    if (p->cur.type == TOKEN_TRUE || p->cur.type == TOKEN_FALSE) {
        node_t *n = alloc_node(p, NODE_BOOL); n->b = (p->cur.type == TOKEN_TRUE); advance(p); return n;
    }
    if (p->cur.type == TOKEN_NULL) { 
        node_t *n = alloc_node(p, NODE_NULL); advance(p); return n; 
    }
    
    /* Handle identifiers, traversals (var.ami), and calls (max(a,b)) */
    if (p->cur.type == TOKEN_IDENT) {
        token_t ident = p->cur;
        advance(p);
        node_t *n = alloc_node(p, NODE_TRAVERSAL);
        n->s = token_text(&p->doc->arena, ident);
        
        /* Handle dotted traversals: var.foo.bar */
        while (p->cur.type == TOKEN_DOT) {
            advance(p);
            if (p->cur.type != TOKEN_IDENT) { set_error(p, "expected identifier after '.'"); return NULL; }
            char *part = token_text(&p->doc->arena, p->cur);
            size_t new_len = strlen(n->s) + 1 + strlen(part);
            char *new_s = arena_alloc(&p->doc->arena, new_len + 1);
            sprintf(new_s, "%s.%s", n->s, part);
            n->s = new_s;
            advance(p);
        }

        /* Handle function calls: max(a, b) */
        if (p->cur.type == TOKEN_LPAREN) {
            advance(p);
            node_t *call = alloc_node(p, NODE_CALL);
            call->callee = n;
            while (p->cur.type != TOKEN_RPAREN && p->cur.type != TOKEN_EOF) {
                node_t *arg = parse_expr(p); /* Forward-declared above */
                if (!arg) return NULL;
                if (!grow((void**)&call->args, &call->arg_cap, call->arg_count, sizeof(node_t*))) return NULL;
                call->args[call->arg_count++] = arg;
                if (p->cur.type == TOKEN_COMMA) advance(p);
            }
            if (p->cur.type != TOKEN_RPAREN) { set_error(p, "expected ')'"); return NULL; }
            advance(p);
            return call;
        }
        return n;
    }

    /* Handle parenthesized expressions: (a + b) */
    if (p->cur.type == TOKEN_LPAREN) {
        advance(p);
        node_t *n = parse_expr(p);
        if (p->cur.type != TOKEN_RPAREN) { set_error(p, "expected ')'"); return NULL; }
        advance(p); return n;
    }

    /* Handle list literals: [a, b, c] */
    if (p->cur.type == TOKEN_LBRACKET) {
        advance(p);
        /* Special case: skip `for` expressions to prevent parser crashes. */
        if (p->cur.type == TOKEN_IDENT && p->cur.len == 3 && !strncmp(p->cur.start, "for", 3)) {
            node_t *n = alloc_node(p, NODE_STRING);
            int depth = 1;
            const char *start = p->cur.start;
            while (p->cur.type != TOKEN_EOF && depth > 0) {
                if (p->cur.type == TOKEN_LBRACKET) depth++;
                if (p->cur.type == TOKEN_RBRACKET) depth--;
                advance(p);
            }
            n->s = arena_strdup(&p->doc->arena, start, p->cur.start - start);
            return n;
        }
        
        node_t *n = alloc_node(p, NODE_LIST);
        for (;;) {
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
            if (p->cur.type == TOKEN_RBRACKET || p->cur.type == TOKEN_EOF) break;
            node_t *item = parse_expr(p);
            if (!item) return NULL;
            if (!grow((void**)&n->items, &n->item_cap, n->item_count, sizeof(node_t*))) return NULL;
            n->items[n->item_count++] = item;
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
            if (p->cur.type == TOKEN_COMMA) advance(p);
        }
        if (p->cur.type != TOKEN_RBRACKET) { set_error(p, "expected ']'"); return NULL; }
        advance(p); return n;
    }
    
    /* Handle object literals: { key = value } */
    if (p->cur.type == TOKEN_LBRACE) {
        node_t *n = alloc_node(p, NODE_OBJECT);
        advance(p);
        for (;;) {
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
            if (p->cur.type == TOKEN_RBRACE || p->cur.type == TOKEN_EOF) break;
            if (p->cur.type != TOKEN_IDENT && p->cur.type != TOKEN_STRING) { set_error(p, "expected key"); return NULL; }
            char *key = token_text(&p->doc->arena, p->cur);
            advance(p);
            if (p->cur.type != TOKEN_EQUALS && p->cur.type != TOKEN_COLON) { set_error(p, "expected '=' or ':'"); return NULL; }
            advance(p);
            node_t *val = parse_expr(p);
            if (!val) return NULL;
            if (!grow((void**)&n->pairs, &n->pair_cap, n->pair_count, sizeof(pair_t))) return NULL;
            n->pairs[n->pair_count++] = (pair_t){key, val};
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
            if (p->cur.type == TOKEN_COMMA) advance(p);
        }
        if (p->cur.type != TOKEN_RBRACE) { set_error(p, "expected '}'"); return NULL; }
        advance(p); return n;
    }
    
    set_error(p, "expected expression"); return NULL;
}

/* 
 * Precedence climbing algorithm for binary operators.
 * Ensures that e.g. `a + b * c` parses as `a + (b * c)`.
 */
static node_t *parse_binary(parser_t *p, int min_prec) {
    node_t *left = parse_primary(p);
    if (!left) return NULL;
    for (;;) {
        int prec = get_prec(p->cur.type);
        if (prec < min_prec) break;
        token_t op = p->cur;
        advance(p);
        node_t *right = parse_binary(p, prec + 1);
        if (!right) return NULL;
        node_t *n = alloc_node(p, NODE_BINARY);
        n->op = token_text(&p->doc->arena, op);
        n->left = left; n->right = right;
        left = n;
    }
    return left;
}

/* 
 * Top-level expression parser: handles binary operators and conditionals.
 */
    static node_t *parse_expr(parser_t *p) {
        node_t *cond = parse_binary(p, 0);
        if (!cond) return NULL;
    
    /* Handle ternary conditional: cond ? true_val : false_val */
    if (p->cur.type == TOKEN_QUESTION) {
        advance(p);
        node_t *t = parse_expr(p);
        if (!t) return NULL;
        if (p->cur.type != TOKEN_COLON) { set_error(p, "expected ':'"); return NULL; }
        advance(p);
        node_t *f = parse_expr(p);
        if (!f) return NULL;
        node_t *n = alloc_node(p, NODE_CONDITIONAL);
        n->cond = cond; n->true_val = t; n->false_val = f;
        return n;
    }
    return cond;
}

/* Public entry point: parse a NUL-terminated source string. */
chcl_document_t *chcl_parse_string(const char *src) {
    chcl_document_t *doc = calloc(1, sizeof(*doc));
    if (!doc) return NULL;

    parser_t p = {0};
    p.lex.src    = src;
    p.lex.line   = 1;
    p.lex.column = 1;
    p.doc        = doc;

    advance(&p);
    doc->root = parse_body(&p);
    return doc;
}

/* Release a document handle (memory is owned by the arena). */
void chcl_free_document(chcl_document_t *doc) {
    if (!doc) return;
    arena_free(&doc->arena);
    free(doc);
}

/* Accessors for the last error. */
const char *chcl_document_error(chcl_document_t *d)       { return d ? d->error  : NULL; }
int          chcl_document_error_line(chcl_document_t *d) { return d ? d->line   : 0;    }
int          chcl_document_error_column(chcl_document_t *d){ return d ? d->column : 0;    }

/* ============================================================
 * Growable string buffer
 * ------------------------------------------------------------
 * Used by both the formatter and the JSON serializer.
 * ============================================================ */

typedef struct { char *data; size_t len, cap; } sbuf_t;

static void sb_putc(sbuf_t *sb, char c) {
    if (sb->len + 2 > sb->cap) {
        sb->cap  = sb->cap ? sb->cap * 2 : 256;
        sb->data = realloc(sb->data, sb->cap);
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len]   = '\0';
}
static void sb_puts (sbuf_t *sb, const char *s)         { while (*s) sb_putc(sb, *s++); }
static void sb_write(sbuf_t *sb, const char *s, size_t n){ while (n--) sb_putc(sb, *s++); }

/* ============================================================
 * Formatter
 * ------------------------------------------------------------
 * The formatter does NOT build an AST; it re-emits the token
 * stream with canonical whitespace. This keeps it resilient
 * against malformed input (it will still produce output).
 * ============================================================ */

/* Tokens that behave like "words" and thus need spacing between them. */
static int is_word_tok(token_type_t t) {
    return t == TOKEN_IDENT  || t == TOKEN_STRING || t == TOKEN_NUMBER ||
        t == TOKEN_TRUE   || t == TOKEN_FALSE  || t == TOKEN_NULL;
}

/* Decide whether a space is required between two consecutive tokens. */
static int is_op_tok(token_type_t t) {
    return t == TOKEN_PLUS || t == TOKEN_MINUS || t == TOKEN_STAR || t == TOKEN_SLASH || t == TOKEN_PERCENT ||
           t == TOKEN_EQEQ || t == TOKEN_NEQ || t == TOKEN_LT || t == TOKEN_GT || t == TOKEN_LTE || t == TOKEN_GTE ||
           t == TOKEN_AND || t == TOKEN_OR || t == TOKEN_QUESTION || t == TOKEN_COLON;
}

static int needs_space(token_t prev, token_t cur) {
    if (prev.type == TOKEN_EQUALS) return 1;
    if (cur.type  == TOKEN_EQUALS) return 1;
    if (prev.type == TOKEN_COMMA)  return 1;
    
    if (cur.type == TOKEN_COMMA || cur.type == TOKEN_RPAREN || cur.type == TOKEN_RBRACKET || cur.type == TOKEN_DOT) return 0;
    if (prev.type == TOKEN_LPAREN || prev.type == TOKEN_LBRACKET || prev.type == TOKEN_DOT || prev.type == TOKEN_NOT) return 0;
    
    if (cur.type == TOKEN_LPAREN && prev.type == TOKEN_IDENT) return 0;
    
    if (is_op_tok(prev.type) || is_op_tok(cur.type)) return 1;
    
    if (cur.type == TOKEN_LBRACE) return 1;
    if (is_word_tok(prev.type) && is_word_tok(cur.type)) return 1;
    return 0;
}

/* Emit `depth` levels of 2-space indentation. */
static void emit_indent(sbuf_t *sb, int depth) {
    for (int i = 0; i < depth; i++) sb_puts(sb, "  ");
}

/*
 * Post-process the formatter's output so that within each block body,
 * consecutive `key = value` lines at the same indentation are aligned
 * on the `=` column, mimicking `terraform fmt`.
 *
 * Works on the already-formatted output (line-based, not token-based),
 * which keeps the change isolated and easy to reason about.
 */
static char *align_equals(const char *input) {
    size_t len = strlen(input);

    /* Per-line metadata collected in a first pass. */
    typedef struct {
        size_t start, end;  /* line content [start, end), indentation excluded */
        int    indent;      /* number of leading spaces */
        size_t key_end;     /* offset in input where the key ends */
        int    is_attr;     /* 1 if the line matches `key = value` */
    } line_t;

    size_t cap = 128, n = 0;
    line_t *lines = malloc(cap * sizeof(*lines));
    if (!lines) return NULL;

    /* First pass: split into lines and flag attribute lines. */
    for (size_t i = 0; i < len; ) {
        size_t s = i;
        while (i < len && input[i] != '\n') i++;
        size_t e = i;
        if (i < len) i++;                       /* skip '\n' */
        if (e > s && input[e - 1] == '\r') e--; /* strip CRLF */

        int indent = 0;
        while (s < e && input[s] == ' ') { indent++; s++; }

        int is_attr = 0;
        size_t key_end = s;
        if (s < e && (isalpha((unsigned char)input[s]) || input[s] == '_')) {
            size_t k = s;
            while (k < e && (isalnum((unsigned char)input[k]) ||
                             input[k] == '_' || input[k] == '-')) k++;
            size_t k2 = k;
            while (k2 < e && input[k2] == ' ') k2++;
            if (k2 < e && input[k2] == '=') {
                size_t k3 = k2 + 1;
                while (k3 < e && input[k3] == ' ') k3++;
                if (k3 < e && input[k3] != '#') {
                    is_attr = 1;
                    key_end = k;
                }
            }
        }

        if (n == cap) {
            cap *= 2;
            line_t *tmp = realloc(lines, cap * sizeof(*lines));
            if (!tmp) { free(lines); return NULL; }
            lines = tmp;
        }
        lines[n++] = (line_t){ .start = s, .end = e, .indent = indent,
                               .key_end = key_end, .is_attr = is_attr };
    }

    /* Second pass: emit, aligning within runs of attribute lines. */
    sbuf_t out = {0};
    for (size_t i = 0; i < n; ) {
        if (!lines[i].is_attr) {
            for (int x = 0; x < lines[i].indent; x++) sb_putc(&out, ' ');
            sb_write(&out, input + lines[i].start, lines[i].end - lines[i].start);
            sb_putc(&out, '\n');
            i++;
            continue;
        }
        /* Find the contiguous run at the same indentation level. */
        size_t j = i, max_key = 0;
        while (j < n && lines[j].is_attr && lines[j].indent == lines[i].indent) {
            size_t kl = lines[j].key_end - lines[j].start;
            if (kl > max_key) max_key = kl;
            j++;
        }
        /* Emit the run with keys padded to max_key. */
        for (size_t k = i; k < j; k++) {
            for (int x = 0; x < lines[k].indent; x++) sb_putc(&out, ' ');
            size_t kl = lines[k].key_end - lines[k].start;
            sb_write(&out, input + lines[k].start, kl);
            for (size_t x = kl; x < max_key; x++) sb_putc(&out, ' ');
            sb_putc(&out, ' '); sb_putc(&out, '='); sb_putc(&out, ' ');
            /* Skip the original ` = ` to copy only the value. */
            size_t vs = lines[k].key_end;
            size_t ve = lines[k].end;
            while (vs < ve && input[vs] == ' ') vs++;
            if (vs < ve && input[vs] == '=') vs++;
            while (vs < ve && input[vs] == ' ') vs++;
            sb_write(&out, input + vs, ve - vs);
            sb_putc(&out, '\n');
        }
        i = j;
    }

    free(lines);
    return out.data;
}

/* Format a source buffer. Caller must free() the returned string. */
char *chcl_format(const char *src) {
    sbuf_t  sb      = {0};
    lexer_t lex     = {.src = src, .line = 1, .column = 1};
    int     indent  = 0;
    int     at_bol  = 1;              /* "at beginning of line"? */
    token_t prev    = {.type = TOKEN_EOF};
    token_t tok     = lexer_next(&lex);
    token_t next    = lexer_next(&lex);

    while (tok.type != TOKEN_EOF) {
        switch (tok.type) {
            case TOKEN_NEWLINE:
                sb_putc(&sb, '\n');
                at_bol = 1;
                break;

            case TOKEN_COMMENT:
                if (at_bol) emit_indent(&sb, indent);
                else        sb_putc(&sb, ' ');
                sb_write(&sb, tok.start, tok.len);
                at_bol = 0;
                break;

            case TOKEN_LBRACE:
                if (!at_bol) sb_putc(&sb, ' ');
                sb_putc(&sb, '{');
                at_bol = 0;
                indent++;
                /* Force a newline after `{` unless the body is empty. */
                if (next.type != TOKEN_NEWLINE && next.type != TOKEN_RBRACE
                    && next.type != TOKEN_EOF) {
                    sb_putc(&sb, '\n');
                    at_bol = 1;
                }
                break;

            case TOKEN_RBRACE:
                if (indent > 0) indent--;
                if (!at_bol && prev.type != TOKEN_LBRACE) {
                    sb_putc(&sb, '\n');
                    at_bol = 1;
                }
                if (at_bol) emit_indent(&sb, indent);
                sb_putc(&sb, '}');
                at_bol = 0;
                if (next.type != TOKEN_NEWLINE && next.type != TOKEN_RBRACE
                    && next.type != TOKEN_EOF && next.type != TOKEN_COMMA
                    && next.type != TOKEN_RBRACKET) {
                    sb_putc(&sb, '\n');
                    at_bol = 1;
                }
                break;

            default:
                if (at_bol) { emit_indent(&sb, indent); at_bol = 0; }
                else if (needs_space(prev, tok)) sb_putc(&sb, ' ');
                sb_write(&sb, tok.start, tok.len);
                break;
        }
        prev = tok;
        tok  = next;
        next = lexer_next(&lex);
    }
    if (!at_bol && sb.len > 0) sb_putc(&sb, '\n');
    if (sb.len == 0) return sb.data;   /* empty input: nothing to align */
    char *aligned = align_equals(sb.data);
    free(sb.data);
    return aligned;
}

/* ============================================================
 * JSON serializer
 * ------------------------------------------------------------
 * Emits a JSON view of the AST. Useful for debugging and for
 * tooling that wants to consume HCL with a JSON stack.
 * ============================================================ */

static void json_emit_node(sbuf_t *sb, node_t *n);
static void json_emit_body(sbuf_t *sb, body_t *b);

/* Emit a JSON string literal with the required escapes. */
static void json_emit_str(sbuf_t *sb, const char *s) {
    sb_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n");  break;
            case '\r': sb_puts(sb, "\\r");  break;
            case '\t': sb_puts(sb, "\\t");  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    sb_puts(sb, buf);
                } else {
                    sb_putc(sb, (char)c);
                }
        }
    }
    sb_putc(sb, '"');
}

static void json_emit_node(sbuf_t *sb, node_t *n) {
    switch (n->type) {
        case NODE_STRING: json_emit_str(sb, n->s ? n->s : ""); break;
        case NODE_NUMBER: {
            char buf[64];
            snprintf(buf, sizeof buf, "%g", n->n);
            sb_puts(sb, buf);
            break;
        }
        case NODE_BOOL: sb_puts(sb, n->b ? "true" : "false"); break;
        case NODE_NULL: sb_puts(sb, "null"); break;

                case NODE_TRAVERSAL: 
            sb_puts(sb, "{\"traversal\": ");
            json_emit_str(sb, n->s);
            sb_putc(sb, '}');
            break;
        case NODE_CALL:
            sb_puts(sb, "{\"call\": ");
            json_emit_str(sb, n->callee->s);
            sb_puts(sb, ", \"args\": [");
            for (size_t i = 0; i < n->arg_count; i++) {
                if (i) sb_puts(sb, ", ");
                json_emit_node(sb, n->args[i]);
            }
            sb_puts(sb, "]}");
            break;
        case NODE_BINARY:
            sb_puts(sb, "{\"op\": ");
            json_emit_str(sb, n->op);
            sb_puts(sb, ", \"left\": ");
            json_emit_node(sb, n->left);
            sb_puts(sb, ", \"right\": ");
            json_emit_node(sb, n->right);
            sb_putc(sb, '}');
            break;
        case NODE_CONDITIONAL:
            sb_puts(sb, "{\"cond\": ");
            json_emit_node(sb, n->cond);
            sb_puts(sb, ", \"true\": ");
            json_emit_node(sb, n->true_val);
            sb_puts(sb, ", \"false\": ");
            json_emit_node(sb, n->false_val);
            sb_putc(sb, '}');
            break;

        case NODE_UNARY:
            sb_puts(sb, "{\"unary_op\": ");
            json_emit_str(sb, n->op);
            sb_puts(sb, ", \"operand\": ");
            json_emit_node(sb, n->left);
            sb_putc(sb, '}');
            break;

        case NODE_LIST:
            sb_putc(sb, '[');
            for (size_t i = 0; i < n->item_count; i++) {
                if (i) sb_puts(sb, ", ");
                json_emit_node(sb, n->items[i]);
            }
            sb_putc(sb, ']');
            break;

        case NODE_OBJECT:
            sb_putc(sb, '{');
            for (size_t i = 0; i < n->pair_count; i++) {
                if (i) sb_puts(sb, ", ");
                json_emit_str(sb, n->pairs[i].key);
                sb_puts(sb, ": ");
                json_emit_node(sb, n->pairs[i].value);
            }
            sb_putc(sb, '}');
            break;

        case NODE_BLOCK:
            /* Blocks are serialized as {"labels": [...], "body": {...}}. */
            sb_puts(sb, "{\"labels\": [");
            for (size_t i = 0; i < n->label_count; i++) {
                if (i) sb_puts(sb, ", ");
                json_emit_str(sb, n->labels[i]);
            }
            sb_puts(sb, "], \"body\": ");
            json_emit_body(sb, &n->body);
            sb_putc(sb, '}');
            break;
    }
}

static void json_emit_body(sbuf_t *sb, body_t *b) {
    sb_putc(sb, '{');
    int first = 1;

    /* Attributes first. */
    for (size_t i = 0; i < b->count; i++) {
        if (!first) sb_puts(sb, ", ");
        first = 0;
        json_emit_str(sb, b->pairs[i].key);
        sb_puts(sb, ": ");
        json_emit_node(sb, b->pairs[i].value);
    }

    /* Then blocks, keyed by their block type. */
    for (size_t i = 0; i < b->block_count; i++) {
        if (!first) sb_puts(sb, ", ");
        first = 0;
        node_t *blk = b->blocks[i];
        json_emit_str(sb, blk->block_type);
        sb_puts(sb, ": ");
        json_emit_node(sb, blk);
    }
    sb_putc(sb, '}');
}

/* Public entry point: serialize a parsed document to JSON. */
char *chcl_to_json(chcl_document_t *doc) {
    if (!doc || doc->error) return NULL;
    sbuf_t sb = {0};
    json_emit_body(&sb, &doc->root);
    return sb.data;
}

/* ============================================================
 * File helpers
 * ============================================================ */

/* Read an entire file into a heap buffer. Returns NULL on failure. */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

chcl_document_t *chcl_parse_file(const char *path) {
    char *buf = read_whole_file(path);
    if (!buf) return NULL;
    chcl_document_t *d = chcl_parse_string(buf);
    free(buf);
    return d;
}

char *chcl_format_file(const char *path) {
    char *buf = read_whole_file(path);
    if (!buf) return NULL;
    char *r = chcl_format(buf);
    free(buf);
    return r;
}

/*
 * Check whether a file on disk is already canonically formatted.
 * Returns 1 (already formatted), 0 (would change), or -1 (I/O error).
 * This lets `chcl fmt --check` behave like `gofmt -l` in CI pipelines.
 */
int chcl_format_is_canonical(const char *path) {
    char *orig = read_whole_file(path);
    if (!orig) return -1;

    char *fmt = chcl_format(orig);
    if (!fmt) { free(orig); return -1; }

    int same = (strcmp(orig, fmt) == 0);
    free(orig);
    free(fmt);
    return same;
}
