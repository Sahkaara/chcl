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
 * numbers, booleans, null, lists, objects and basic expressions).
 * Interpolations are preserved as text, not evaluated.
 */
#include "chcl.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <setjmp.h>

/* ============================================================
 * Arena allocator
 * ------------------------------------------------------------
 * AST nodes are allocated from an arena to simplify ownership:
 * the caller never has to free individual nodes. When the
 * document is released, the arena frees every block at once.
 * ============================================================ */

typedef struct arena_block arena_block_t;
typedef struct { arena_block_t *head; jmp_buf *failure; } arena_t;

struct arena_block {
    arena_block_t *next;
    size_t used;    /* byte already handed out */
    size_t cap;     /* capacity of 'data' */
    _Alignas(max_align_t) char data[];    /* flexible array holding the payload */
};

/* Default block size; sized so most small configs fit in one block. */
#define ARENA_BLOCK (64 * 1024)

/* Parser allocations unwind to the public entry point on OOM. */
static void *arena_alloc(arena_t *a, size_t n) {
    size_t alignment = _Alignof(max_align_t);
    if (n > SIZE_MAX - (alignment - 1)) longjmp(*a->failure, 1);
    n = ((n + alignment - 1) / alignment) * alignment;
    if (!a->head || n > a->head->cap - a->head->used) {
        size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
        if (cap > SIZE_MAX - sizeof(arena_block_t)) longjmp(*a->failure, 1);
        arena_block_t *b = malloc(sizeof(*b) + cap);
        if (!b) longjmp(*a->failure, 1);
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
    if (len == SIZE_MAX) longjmp(*a->failure, 1);
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
    TOKEN_ERROR,
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

/* Reject overlong encodings, surrogate code points and truncated UTF-8. */
static int valid_utf8(const char *text, size_t length) {
    for (size_t i = 0; i < length; ) {
        unsigned char first = (unsigned char)text[i++];
        if (first < 0x80) continue;
        unsigned remaining;
        uint32_t scalar, minimum;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; scalar = first & 0x1fu; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; scalar = first & 0x0fu; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; scalar = first & 0x07u; minimum = 0x10000; }
        else return 0;
        if (remaining > length - i) return 0;
        while (remaining--) {
            unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0u) != 0x80u) return 0;
            scalar = (scalar << 6) | (next & 0x3fu);
        }
        if (scalar < minimum || scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu)) return 0;
    }
    return 1;
}

/* Return the next token from the source. Handles coments and skips */
/* whitespace (but NOT newlines, since HCL is line-sensitive) */
static token_t lexer_next(lexer_t *l) {
    token_t t = {0};
    if (l->line >= INT_MAX - 2 || l->column >= INT_MAX - 2) {
        t.type = TOKEN_ERROR; return t;
    }

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
        t.len = (size_t)(l->src + l->pos - t.start);
        return t;
    }

    if (c == '/' && l->src[l->pos + 1] == '*') {
        l->pos += 2; l->column += 2;
        while (l->src[l->pos] && !(l->src[l->pos] == '*' && l->src[l->pos + 1] == '/')) {
                if (l->src[l->pos] == '\n') { l->line++; l->column = 1; }
                else l->column++;
                l->pos++;
        }
        if (!l->src[l->pos]) { t.type = TOKEN_ERROR; return t; }
        l->pos += 2; l->column += 2;
        t.type = TOKEN_COMMENT;
        t.len  = (size_t)(l->src + l->pos - t.start);
        return t;
    }

    /* Handle Heredocs: <<EOF ... EOF or <<-EOF ... EOF (trim indentation) */
    if (c == '<' && l->src[l->pos+1] == '<') {
        size_t start = l->pos;
        int trim = l->src[l->pos + 2] == '-';
        l->pos += trim ? 3u : 2u;
        size_t id_start = l->pos;
        while (isalnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_') l->pos++;
        size_t id_len = l->pos - id_start;
        while (l->src[l->pos] == ' ' || l->src[l->pos] == '\t' || l->src[l->pos] == '\r') l->pos++;
        if (!id_len || l->src[l->pos] != '\n') { t.type = TOKEN_ERROR; return t; }
        l->pos++; l->line++; l->column = 1;
        while (l->src[l->pos]) {
            size_t line_start = l->pos;
            while (l->src[l->pos] && l->src[l->pos] != '\n') l->pos++;
            size_t end = l->pos, k = line_start;
            if (end > k && l->src[end - 1] == '\r') end--;
            if (trim) while (k < end && (l->src[k] == ' ' || l->src[k] == '\t')) k++;
            if (end - k == id_len && !memcmp(l->src + k, l->src + id_start, id_len)) {
                /* Leave the final newline to the parser/formatter. */
                l->column = (int)(l->pos - line_start) + 1;
                t.len = l->pos - start;
                t.type = valid_utf8(t.start, t.len) ? TOKEN_HEREDOC : TOKEN_ERROR;
                return t;
            }
            if (l->src[l->pos] == '\n') { l->pos++; l->line++; l->column = 1; }
        }
        t.type = TOKEN_ERROR; return t;
    }

    /* Multi-character operators (must be checked before single-char ones) */
    if (c == '=' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_EQEQ; t.len = 2; return t; }
    if (c == '!' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_NEQ;  t.len = 2; return t; }
    if (c == '<' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_LTE;  t.len = 2; return t; }
    if (c == '>' && l->src[l->pos+1] == '=') { l->pos += 2; l->column += 2; t.type = TOKEN_GTE;  t.len = 2; return t; }
    if (c == '&' && l->src[l->pos+1] == '&') { l->pos += 2; l->column += 2; t.type = TOKEN_AND;  t.len = 2; return t; }
    if (c == '|' && l->src[l->pos+1] == '|') { l->pos += 2; l->column += 2; t.type = TOKEN_OR;   t.len = 2; return t; }

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
    if (c == '-') { l->pos++; l->column++; t.type = TOKEN_MINUS; t.len = 1; return t; }
    if (c == '+') { l->pos++; l->column++; t.type = TOKEN_PLUS;     t.len = 1; return t; }
    if (c == '*') { l->pos++; l->column++; t.type = TOKEN_STAR;     t.len = 1; return t; }
    if (c == '/') { l->pos++; l->column++; t.type = TOKEN_SLASH;    t.len = 1; return t; }
    if (c == '%') { l->pos++; l->column++; t.type = TOKEN_PERCENT;  t.len = 1; return t; }
    if (c == '=') { l->pos++; l->column++; t.type = TOKEN_EQUALS;   t.len = 1; return t; }
    if (c == '!') { l->pos++; l->column++; t.type = TOKEN_NOT;      t.len = 1; return t; }
    if (c == '<') { l->pos++; l->column++; t.type = TOKEN_LT;       t.len = 1; return t; }
    if (c == '>') { l->pos++; l->column++; t.type = TOKEN_GT;       t.len = 1; return t; }

    /* String literal (no escape processing on the value itself yet). */
    if (c == '"') {
        l->pos++; l->column++;
        while (l->src[l->pos] && l->src[l->pos] != '"') {
            if ((unsigned char)l->src[l->pos] < 0x20) { t.type = TOKEN_ERROR; return t; }
            if (l->src[l->pos] == '\\') {
                l->pos++; l->column++;
                char escape = l->src[l->pos];
                if (!escape) { t.type = TOKEN_ERROR; return t; }
                if (escape == 'u' || escape == 'U') {
                    unsigned digits = escape == 'u' ? 4u : 8u;
                    while (digits--) {
                        l->pos++; l->column++;
                        if (!isxdigit((unsigned char)l->src[l->pos])) { t.type = TOKEN_ERROR; return t; }
                    }
                } else if (!strchr("nrt\\\"", escape)) { t.type = TOKEN_ERROR; return t; }
            }
            if (l->src[l->pos] == '\n') { l->line++; l->column = 1; }
            else l->column++;
            l->pos++;
        }
        if (!l->src[l->pos]) { t.type = TOKEN_ERROR; return t; }
        l->pos++; l->column++;
        t.len = (size_t)(l->src + l->pos - t.start);
        t.type = valid_utf8(t.start, t.len) ? TOKEN_STRING : TOKEN_ERROR;
        return t;
    }

    /* Decimal numbers; unary minus is handled by the parser. */
    if (isdigit((unsigned char)c)) {
        while (isdigit((unsigned char)l->src[l->pos])) { l->pos++; l->column++; }
        if (c == '0' && l->pos > (size_t)(t.start - l->src) + 1) { t.type = TOKEN_ERROR; return t; }
        if (l->src[l->pos] == '.') {
            l->pos++; l->column++;
            if (!isdigit((unsigned char)l->src[l->pos])) { t.type = TOKEN_ERROR; return t; }
            while (isdigit((unsigned char)l->src[l->pos])) { l->pos++; l->column++; }
        }
        if (l->src[l->pos] == 'e' || l->src[l->pos] == 'E') {
            l->pos++; l->column++;
            if (l->src[l->pos] == '+' || l->src[l->pos] == '-') { l->pos++; l->column++; }
            if (!isdigit((unsigned char)l->src[l->pos])) { t.type = TOKEN_ERROR; return t; }
            while (isdigit((unsigned char)l->src[l->pos])) { l->pos++; l->column++; }
        }
        t.type = TOKEN_NUMBER;
        t.len  = (size_t)(l->src + l->pos - t.start);
        return t;
    }

    /* Identifiers and reserved words (true/false/null). */
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_' || l->src[l->pos] == '-') {
            l->pos++; l->column++;
        }
        size_t n = (size_t)(l->src + l->pos - t.start);
        if (n == 4 && !strncmp(t.start, "true", 4)) t.type = TOKEN_TRUE;
        else if (n == 5 && !strncmp(t.start, "false", 5)) t.type = TOKEN_FALSE;
        else if (n == 4 && !strncmp(t.start, "null", 4)) t.type = TOKEN_NULL;
        else t.type = TOKEN_IDENT;
        t.len = n;
        return t;
    }

    /* Unknown character: consume one byte so the parser can report it. */
    l->pos++; l->column++;
    t.type = TOKEN_ERROR; t.len = 1;
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
    char error_storage[256];
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
    jmp_buf failure;
    unsigned depth;
} parser_t;

#define MAX_DEPTH 128u
#define MAX_SOURCE ((size_t)INT_MAX / 4u)

/* Consume the current token and advance. */
static void advance(parser_t *p) {
    if (p->has_peek) { p->cur = p->peeked; p->has_peek = 0; }
    else p->cur = lexer_next(&p->lex);
}

/* Record the first error encountered; subsequent calls are no-ops. */
static void set_error(parser_t *p, const char *fmt, ...) {
    if (p->doc->error) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(p->doc->error_storage, sizeof p->doc->error_storage, fmt, ap);
    va_end(ap);
    p->doc->error = p->doc->error_storage;
    p->doc->line = p->cur.line;
    p->doc->column = p->cur.column;
    longjmp(p->failure, 2);
}

/* Allocate and zero-initialize an AST node in the arena. */
static node_t *alloc_node(parser_t *p, node_type_t t) {
    node_t *n = arena_alloc(&p->doc->arena, sizeof(*n));
    if (!n) { set_error(p, "out of memory"); return NULL; }
    memset(n, 0, sizeof(*n));
    n->type = t;
    return n;
}

/* Decode quoted escapes and reject invalid Unicode scalar values. */
static unsigned hex_digit(parser_t *p, char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a') + 10u;
    if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A') + 10u;
    set_error(p, "invalid Unicode escape");
    return 0;
}

static char *token_text(parser_t *p, token_t tok) {
    arena_t *a = &p->doc->arena;
    if (tok.type == TOKEN_HEREDOC) {
        const char *start = memchr(tok.start, '\n', tok.len);
        if (!start) set_error(p, "invalid heredoc");
        start++;
        const char *end = tok.start + tok.len;
        while (end > start && end[-1] != '\n') end--;
        char *text = arena_strdup(a, start, (size_t)(end - start));
        if (tok.start[2] == '-') {
            size_t minimum = SIZE_MAX;
            for (char *line = text; *line; ) {
                size_t spaces = 0;
                while (line[spaces] == ' ' || line[spaces] == '\t') spaces++;
                if (line[spaces] && line[spaces] != '\n' && line[spaces] != '\r' && spaces < minimum) minimum = spaces;
                char *next = strchr(line, '\n');
                if (!next) break;
                line = next + 1;
            }
            char *out = text;
            for (char *line = text; *line; ) {
                size_t skipped = 0;
                while (skipped < minimum && (*line == ' ' || *line == '\t')) { line++; skipped++; }
                while (*line) { char c = *line++; *out++ = c; if (c == '\n') break; }
            }
            *out = '\0';
        }
        return text;
    }
    if (tok.type != TOKEN_STRING) return arena_strdup(a, tok.start, tok.len);
    char *text = arena_alloc(a, tok.len + 1);
    size_t out = 0;
    for (size_t i = 1; i + 1 < tok.len; i++) {
        unsigned char c = (unsigned char)tok.start[i];
        if (c < 0x20) set_error(p, "control character in quoted string");
        if (c != '\\') { text[out++] = (char)c; continue; }
        if (++i + 1 >= tok.len) set_error(p, "incomplete escape");
        switch (tok.start[i]) {
            case 'n': text[out++] = '\n'; break;
            case 'r': text[out++] = '\r'; break;
            case 't': text[out++] = '\t'; break;
            case '"': text[out++] = '"'; break;
            case '\\': text[out++] = '\\'; break;
            case 'u': case 'U': {
                size_t digits = tok.start[i] == 'u' ? 4u : 8u;
                if (digits > tok.len - i - 2) set_error(p, "incomplete Unicode escape");
                uint32_t code = 0;
                for (size_t j = 0; j < digits; j++) code = (code << 4) | hex_digit(p, tok.start[++i]);
                if (!code || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu))
                    set_error(p, "unsupported or invalid Unicode scalar");
                if (code < 0x80u) text[out++] = (char)code;
                else if (code < 0x800u) {
                    text[out++] = (char)(0xc0u | (code >> 6));
                    text[out++] = (char)(0x80u | (code & 0x3fu));
                } else if (code < 0x10000u) {
                    text[out++] = (char)(0xe0u | (code >> 12));
                    text[out++] = (char)(0x80u | ((code >> 6) & 0x3fu));
                    text[out++] = (char)(0x80u | (code & 0x3fu));
                } else {
                    text[out++] = (char)(0xf0u | (code >> 18));
                    text[out++] = (char)(0x80u | ((code >> 12) & 0x3fu));
                    text[out++] = (char)(0x80u | ((code >> 6) & 0x3fu));
                    text[out++] = (char)(0x80u | (code & 0x3fu));
                }
                break;
            }
            default: set_error(p, "invalid string escape");
        }
    }
    text[out] = '\0';
    return text;
}

/* Grow an arena-owned vector; prior storage remains owned by the arena. */
static void *grow(parser_t *parser, void *old_array, size_t *cap, size_t count, size_t elem) {
    if (count < *cap) return old_array;
    if (*cap > SIZE_MAX / 2) longjmp(parser->failure, 1);
    size_t nc = *cap ? *cap * 2 : 8;
    if (nc > SIZE_MAX / elem) longjmp(parser->failure, 1);
    void *new_array = arena_alloc(&parser->doc->arena, nc * elem);
    if (count) memcpy(new_array, old_array, count * elem);
    *cap = nc;
    return new_array;
}

static void body_add_pair(parser_t *p, body_t *b, char *key, node_t *v) {
    for (size_t i = 0; i < b->count; i++)
        if (!strcmp(b->pairs[i].key, key)) set_error(p, "duplicate attribute");
    for (size_t i = 0; i < b->block_count; i++)
        if (!strcmp(b->blocks[i]->block_type, key)) set_error(p, "attribute conflicts with block type");
    b->pairs = grow(p, b->pairs, &b->cap, b->count, sizeof(pair_t));
    b->pairs[b->count++] = (pair_t){ key, v };
}

static void body_add_block(parser_t *p, body_t *b, node_t *blk) {
    for (size_t i = 0; i < b->count; i++)
        if (!strcmp(b->pairs[i].key, blk->block_type)) set_error(p, "block type conflicts with attribute");
    b->blocks = grow(p, b->blocks, &b->block_cap, b->block_count, sizeof(node_t *));
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
            body_add_pair(p, &b, token_text(p, ident), v);
            while (p->cur.type == TOKEN_COMMENT) advance(p);
            if (p->cur.type == TOKEN_NEWLINE) advance(p);
            else if (p->cur.type != TOKEN_RBRACE && p->cur.type != TOKEN_EOF) {
                set_error(p, "expected newline after attribute");
                return b;
            }
        }else if (p->cur.type == TOKEN_LBRACE || p->cur.type == TOKEN_STRING  || p->cur.type == TOKEN_IDENT) {
            /* `ident "label" "label" { ... }` — a block. */
            node_t *blk = alloc_node(p, NODE_BLOCK);
            if (!blk) return b;
            blk->block_type = token_text(p, ident);
            while (p->cur.type == TOKEN_STRING || p->cur.type == TOKEN_IDENT) {
                blk->labels = grow(p, blk->labels, &blk->label_cap, blk->label_count, sizeof(char *));
                blk->labels[blk->label_count++] = token_text(p, p->cur);
                advance(p);
            }
            if (p->cur.type != TOKEN_LBRACE) { set_error(p, "expected '{'"); return b; }
            advance(p);
            if (++p->depth > MAX_DEPTH) set_error(p, "maximum nesting depth exceeded");
            blk->body = parse_body(p);
            p->depth--;
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
static node_t *parse_primary_impl(parser_t *p);
static node_t *parse_primary(parser_t *p) {
    if (++p->depth > MAX_DEPTH) set_error(p, "maximum nesting depth exceeded");
    node_t *n = parse_primary_impl(p);
    p->depth--;
    return n;
}

static node_t *parse_primary_impl(parser_t *p) {
    /* Handle unary operators: !true or -5 */
    if (p->cur.type == TOKEN_NOT || p->cur.type == TOKEN_MINUS) {
        token_t op = p->cur;
        advance(p);
        node_t *n = alloc_node(p, NODE_UNARY);
        n->op = token_text(p, op);
        n->left = parse_primary(p);
        return n;
    }
    /* Accept both strings and heredocs as literal string nodes */
    if (p->cur.type == TOKEN_STRING || p->cur.type == TOKEN_HEREDOC) {
        node_t *n = alloc_node(p, NODE_STRING);
        n->s = token_text(p, p->cur);
        advance(p);
        return n;
    }
    if (p->cur.type == TOKEN_NUMBER) {
        node_t *n = alloc_node(p, NODE_NUMBER); n->s = token_text(p, p->cur); advance(p); return n;
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
        /* Collect token spans once, omitting whitespace around dots. */
        lexer_t scan = p->lex;
        token_t cur = p->cur;
        size_t length = ident.len;
        while (cur.type == TOKEN_DOT) {
            token_t part = lexer_next(&scan);
            if (part.type != TOKEN_IDENT) set_error(p, "expected identifier after '.'");
            if (part.len >= SIZE_MAX - length) longjmp(p->failure, 1);
            length += 1 + part.len;
            cur = lexer_next(&scan);
        }
        n->s = arena_alloc(&p->doc->arena, length + 1);
        memcpy(n->s, ident.start, ident.len);
        size_t written = ident.len;
        while (p->cur.type == TOKEN_DOT) {
            advance(p);
            n->s[written++] = '.';
            memcpy(n->s + written, p->cur.start, p->cur.len);
            written += p->cur.len;
            advance(p);
        }
        n->s[written] = '\0';

        /* Handle function calls: max(a, b) */
        if (p->cur.type == TOKEN_LPAREN) {
            advance(p);
            node_t *call = alloc_node(p, NODE_CALL);
            call->callee = n;
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
            while (p->cur.type != TOKEN_RPAREN && p->cur.type != TOKEN_EOF) {
                node_t *arg = parse_expr(p); /* Forward-declared above */
                if (!arg) return NULL;
                call->args = grow(p, call->args, &call->arg_cap, call->arg_count, sizeof(node_t*));
                call->args[call->arg_count++] = arg;
                while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
                if (p->cur.type == TOKEN_COMMA) {
                    advance(p);
                    while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
                } else if (p->cur.type != TOKEN_RPAREN) set_error(p, "expected ',' or ')'");
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
                if (p->cur.type == TOKEN_ERROR) set_error(p, "invalid token in for expression");
                if (p->cur.type == TOKEN_LBRACKET && ++depth > (int)MAX_DEPTH) set_error(p, "maximum nesting depth exceeded");
                if (p->cur.type == TOKEN_RBRACKET) depth--;
                advance(p);
            }
            if (depth) set_error(p, "unterminated for expression");
            n->s = arena_strdup(&p->doc->arena, start, (size_t)(p->cur.start - start));
            return n;
        }

        node_t *n = alloc_node(p, NODE_LIST);
        for (;;) {
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
            if (p->cur.type == TOKEN_RBRACKET || p->cur.type == TOKEN_EOF) break;
            node_t *item = parse_expr(p);
            if (!item) return NULL;
            n->items = grow(p, n->items, &n->item_cap, n->item_count, sizeof(node_t*));
            n->items[n->item_count++] = item;
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) advance(p);
            if (p->cur.type == TOKEN_COMMA) advance(p);
            else if (p->cur.type != TOKEN_RBRACKET) set_error(p, "expected comma in list");
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
            char *key = token_text(p, p->cur);
            advance(p);
            if (p->cur.type != TOKEN_EQUALS && p->cur.type != TOKEN_COLON) { set_error(p, "expected '=' or ':'"); return NULL; }
            advance(p);
            node_t *val = parse_expr(p);
            if (!val) return NULL;
            n->pairs = grow(p, n->pairs, &n->pair_cap, n->pair_count, sizeof(pair_t));
            for (size_t i = 0; i < n->pair_count; i++)
                if (!strcmp(n->pairs[i].key, key)) set_error(p, "duplicate object key");
            n->pairs[n->pair_count++] = (pair_t){key, val};
            int separated = 0;
            while (p->cur.type == TOKEN_NEWLINE || p->cur.type == TOKEN_COMMENT) {
                if (p->cur.type == TOKEN_NEWLINE) separated = 1;
                advance(p);
            }
            if (p->cur.type == TOKEN_COMMA) advance(p);
            else if (!separated && p->cur.type != TOKEN_RBRACE) set_error(p, "expected object separator");
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
    unsigned operators = 0;
    node_t *left = parse_primary(p);
    if (!left) return NULL;
    for (;;) {
        int prec = get_prec(p->cur.type);
        if (prec < min_prec) break;
        if (++operators > MAX_DEPTH) set_error(p, "expression too deep");
        token_t op = p->cur;
        advance(p);
        node_t *right = parse_binary(p, prec + 1);
        if (!right) return NULL;
        node_t *n = alloc_node(p, NODE_BINARY);
        n->op = token_text(p, op);
        n->left = left; n->right = right;
        left = n;
    }
    return left;
}

/*
 * Top-level expression parser: handles binary operators and conditionals.
 */
static node_t *parse_expr_impl(parser_t *p);
static node_t *parse_expr(parser_t *p) {
    if (++p->depth > MAX_DEPTH) set_error(p, "maximum nesting depth exceeded");
    node_t *n = parse_expr_impl(p);
    p->depth--;
    return n;
}

static node_t *parse_expr_impl(parser_t *p) {
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
    if (!src || strlen(src) > MAX_SOURCE) return NULL;
    chcl_document_t *volatile doc = calloc(1, sizeof(*doc));
    if (!doc) return NULL;

    parser_t p = {0};
    p.lex.src    = src;
    p.lex.line   = 1;
    p.lex.column = 1;
    p.doc        = doc;

    doc->arena.failure = &p.failure;
    switch (setjmp(p.failure)) {
    case 1:
        chcl_free_document(doc);
        return NULL;
    case 0: {
        advance(&p);
        doc->root = parse_body(&p);
        if (p.cur.type != TOKEN_EOF) set_error(&p, "unexpected trailing token");
        break;
    }
    default: break; /* Syntax error already stored in the document. */
    }
    doc->arena.failure = NULL;
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

typedef struct { char *data; size_t len, cap; int failed; unsigned depth; } sbuf_t;

static void sb_putc(sbuf_t *sb, char c) {
    if (sb->failed) return;
    if (sb->len >= MAX_SOURCE) { sb->failed = 1; return; }
    if (sb->len > SIZE_MAX - 2) { sb->failed = 1; return; }
    if (sb->len + 2 > sb->cap) {
        if (sb->cap > SIZE_MAX / 2) { sb->failed = 1; return; }
        size_t cap = sb->cap ? sb->cap * 2 : 256;
        char *data = realloc(sb->data, cap);
        if (!data) { sb->failed = 1; return; }
        sb->data = data;
        sb->cap = cap;
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len]   = '\0';
}
static void sb_puts (sbuf_t *sb, const char *s)         { while (*s && !sb->failed) sb_putc(sb, *s++); }
static void sb_write(sbuf_t *sb, const char *s, size_t n){ while (n-- && !sb->failed) sb_putc(sb, *s++); }

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
    /* Mark multiline literal/comment interiors before line-based alignment. */
    unsigned char *protected = calloc(len + 1, 1);
    if (!protected) return NULL;
    lexer_t scan = {.src = input, .line = 1, .column = 1};
    for (;;) {
        token_t token = lexer_next(&scan);
        if (token.type == TOKEN_EOF) break;
        if (token.type == TOKEN_ERROR) { free(protected); return NULL; }
        if (token.type == TOKEN_HEREDOC || token.type == TOKEN_STRING || token.type == TOKEN_COMMENT) {
            size_t start = (size_t)(token.start - input);
            memset(protected + start, 1, token.len);
        }
    }

    /* Per-line metadata collected in a first pass. */
    typedef struct {
        size_t start, end;  /* line content [start, end), indentation excluded */
        int    indent;      /* number of leading spaces */
        size_t key_end;     /* offset in input where the key ends */
        int    is_attr;     /* 1 if the line matches `key = value` */
    } line_t;

    size_t cap = 128, n = 0;
    line_t *lines = malloc(cap * sizeof(*lines));
    if (!lines) { free(protected); return NULL; }

    /* First pass: split into lines and flag attribute lines. */
    for (size_t i = 0; i < len; ) {
        size_t s = i;
        while (i < len && input[i] != '\n') i++;
        size_t e = i;
        if (i < len) i++;                       /* skip '\n' */
        if (e > s && input[e - 1] == '\r' && !protected[e - 1]) e--; /* strip CRLF */

        int indent = 0;
        while (s < e && input[s] == ' ') { indent++; s++; }

        int is_attr = 0;
        size_t key_end = s;
        if (!protected[s] && s < e && (isalpha((unsigned char)input[s]) || input[s] == '_')) {
            size_t k = s;
            while (k < e && (isalnum((unsigned char)input[k]) ||
                             input[k] == '_' || input[k] == '-')) k++;
            size_t k2 = k;
            while (k2 < e && input[k2] == ' ') k2++;
            if (k2 < e && input[k2] == '=' && (k2 + 1 == e || input[k2 + 1] != '=')) {
                size_t k3 = k2 + 1;
                while (k3 < e && input[k3] == ' ') k3++;
                if (k3 < e && input[k3] != '#') {
                    is_attr = 1;
                    key_end = k;
                }
            }
        }

        if (n == cap) {
            if (cap > SIZE_MAX / 2 / sizeof(*lines)) { free(lines); free(protected); return NULL; }
            cap *= 2;
            line_t *tmp = realloc(lines, cap * sizeof(*lines));
            if (!tmp) { free(lines); free(protected); return NULL; }
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
    free(protected);
    if (out.failed) { free(out.data); return NULL; }
    return out.data;
}

/* Format a source buffer. Caller must free() the returned string. */
char *chcl_format(const char *src) {
    if (!src || strlen(src) > MAX_SOURCE) return NULL;
    sbuf_t  sb      = {0};
    lexer_t lex     = {.src = src, .line = 1, .column = 1};
    int     indent  = 0;
    int     at_bol  = 1;              /* "at beginning of line"? */
    token_t prev    = {.type = TOKEN_EOF};
    token_t tok     = lexer_next(&lex);
    token_t next    = lexer_next(&lex);

    while (tok.type != TOKEN_EOF) {
        if (tok.type == TOKEN_ERROR || sb.failed) { free(sb.data); return NULL; }
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
                    && next.type != TOKEN_RBRACKET && next.type != TOKEN_RPAREN
                    && !is_op_tok(next.type)) {
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
    if (sb.failed) { free(sb.data); return NULL; }
    if (sb.len == 0) return calloc(1, 1);   /* empty input: nothing to align */
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
    if (++sb->depth > MAX_DEPTH * 4u || sb->failed) { sb->failed = 1; sb->depth--; return; }
    switch (n->type) {
        case NODE_STRING: json_emit_str(sb, n->s ? n->s : ""); break;
        case NODE_NUMBER: sb_puts(sb, n->s); break;
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
    sb->depth--;
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

    /* Every block type maps to an array, including a single block. */
    for (size_t i = 0; i < b->block_count; i++) {
        node_t *blk = b->blocks[i];
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(b->blocks[j]->block_type, blk->block_type)) seen = 1;
        if (seen) continue;
        if (!first) sb_puts(sb, ", ");
        first = 0;
        json_emit_str(sb, blk->block_type);
        sb_puts(sb, ": [");
        int first_block = 1;
        for (size_t j = i; j < b->block_count; j++) {
            if (strcmp(b->blocks[j]->block_type, blk->block_type)) continue;
            if (!first_block) sb_puts(sb, ", ");
            first_block = 0;
            json_emit_node(sb, b->blocks[j]);
        }
        sb_putc(sb, ']');
    }
    sb_putc(sb, '}');
}

/* Public entry point: serialize a parsed document to JSON. */
char *chcl_to_json(chcl_document_t *doc) {
    if (!doc || doc->error) return NULL;
    sbuf_t sb = {0};
    json_emit_body(&sb, &doc->root);
    if (sb.failed) { free(sb.data); return NULL; }
    return sb.data;
}

/* ============================================================
 * File helpers
 * ============================================================ */

/* Read an entire file into a heap buffer. Returns NULL on failure. */
static char *read_whole_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || (uintmax_t)n > MAX_SOURCE || fseek(f, 0, SEEK_SET)) {
        fclose(f); return NULL;
    }
    size_t length = (size_t)n;
    char *buf = malloc(length + 1);
    if (!buf) { fclose(f); return NULL; }
    int bad = fread(buf, 1, length, f) != length || ferror(f);
    if (fclose(f)) bad = 1;
    if (bad || memchr(buf, '\0', length)) { free(buf); return NULL; }
    buf[length] = '\0';
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
