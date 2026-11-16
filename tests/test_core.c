/* SPDX-License-Identifier: MIT */
/* Include the implementation to inject allocator failures without production hooks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static size_t calls, fail_at, live;
static void *checked_malloc(size_t size) {
    if (++calls == fail_at) return NULL;
    void *p = malloc(size);
    if (p) live++;
    return p;
}
static void *checked_calloc(size_t count, size_t size) {
    if (++calls == fail_at) return NULL;
    void *p = calloc(count, size);
    if (p) live++;
    return p;
}
static void *checked_realloc(void *old, size_t size) {
    if (++calls == fail_at) return NULL;
    int fresh = old == NULL;
    void *p = realloc(old, size);
    if (p && fresh) live++;
    return p;
}
static void checked_free(void *p) {
    if (p) live--;
    free(p);
}
#define malloc checked_malloc
#define calloc checked_calloc
#define realloc checked_realloc
#define free checked_free
#include "../src/chcl.c"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static void json_is(const char *source, const char *expected) {
    chcl_document_t *doc = chcl_parse_string(source);
    CHECK(doc && !chcl_document_error(doc));
    char *json = chcl_to_json(doc);
    CHECK(json && !strcmp(json, expected));
    free(json);
    chcl_free_document(doc);
    CHECK(live == 0);
}
static void invalid(const char *source) {
    chcl_document_t *doc = chcl_parse_string(source);
    CHECK(doc && chcl_document_error(doc));
    CHECK(!chcl_to_json(doc));
    chcl_free_document(doc);
    CHECK(live == 0);
}
static void stable(const char *source) {
    char *once = chcl_format(source);
    CHECK(once);
    char *twice = chcl_format(once);
    CHECK(twice && !strcmp(once, twice));
    free(once); free(twice);
    CHECK(live == 0);
}
/* Formatting supported syntax must preserve the serialized AST, not just text. */
static void roundtrip(const char *source) {
    chcl_document_t *before = chcl_parse_string(source);
    CHECK(before && !chcl_document_error(before));
    char *formatted = chcl_format(source);
    CHECK(formatted);
    chcl_document_t *after = chcl_parse_string(formatted);
    if (!after || chcl_document_error(after)) fprintf(stderr, "roundtrip failed: %s\nformatted: %s\n", source, formatted);
    CHECK(after && !chcl_document_error(after));
    char *left = chcl_to_json(before), *right = chcl_to_json(after);
    if (!left || !right || strcmp(left, right)) fprintf(stderr, "AST changed: %s\nformatted: %s\n", source, formatted);
    CHECK(left && right && !strcmp(left, right));
    free(left); free(right); free(formatted);
    chcl_free_document(before); chcl_free_document(after);
    CHECK(live == 0);
    stable(source);
}

static void generated_roundtrips(void) {
    const char *values[] = {"1", "-3", "!true", "var.x", "\"a\\n\"", "[1, 2]", "{a = 1}", "f(1, 2)"};
    const char *operators[] = {"+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||"};
    char source[1024];
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        for (size_t j = 0; j < sizeof values / sizeof *values; j++) {
            for (size_t k = 0; k < sizeof operators / sizeof *operators; k++) {
                snprintf(source, sizeof source, "x=(%s) %s (%s)\n", values[i], operators[k], values[j]);
                roundtrip(source);
            }
            snprintf(source, sizeof source, "x = true ? %s : %s\n", values[i], values[j]);
            roundtrip(source);
            snprintf(source, sizeof source, "x = f(%s, %s)\n", values[i], values[j]);
            roundtrip(source);
        }
    }
}

static void malformed_smoke(void) {
    uint32_t state = 0x6c686363u;
    char source[129];
    for (unsigned trial = 0; trial < 5000; trial++) {
        state = state * 1664525u + 1013904223u;
        size_t length = state % (sizeof source - 1);
        for (size_t i = 0; i < length; i++) {
            state = state * 1664525u + 1013904223u;
            source[i] = (char)(1u + ((state >> 16) % 255u));
        }
        source[length] = '\0';
        chcl_document_t *doc = chcl_parse_string(source);
        CHECK(doc);
        if (!chcl_document_error(doc)) {
            char *json = chcl_to_json(doc);
            CHECK(json);
            free(json);
        }
        chcl_free_document(doc);
        char *formatted = chcl_format(source);
        free(formatted);
        CHECK(live == 0);
    }
}

static void allocation_failures(const char *source) {
    fail_at = 0; calls = 0;
    chcl_document_t *doc = chcl_parse_string(source);
    CHECK(doc && !chcl_document_error(doc));
    char *json = chcl_to_json(doc);
    CHECK(json);
    free(json); chcl_free_document(doc);
    size_t total = calls;
    CHECK(live == 0);
    for (size_t failure = 1; failure <= total; failure++) {
        calls = 0; fail_at = failure;
        doc = chcl_parse_string(source);
        if (doc) {
            CHECK(!chcl_document_error(doc));
            json = chcl_to_json(doc);
            free(json);
            chcl_free_document(doc);
        }
        CHECK(live == 0);
    }
    fail_at = 0; calls = 0;
    char *formatted = chcl_format(source);
    CHECK(formatted);
    free(formatted);
    total = calls;
    for (size_t failure = 1; failure <= total; failure++) {
        calls = 0; fail_at = failure;
        formatted = chcl_format(source);
        CHECK(!formatted);
        CHECK(live == 0);
    }
    fail_at = 0;
}
int main(void) {
    generated_roundtrips();
    malformed_smoke();
    const char *invalid_encoding[] = {
        "x = \"\xc0\xaf\"", "x = \"\xed\xa0\x80\"", "x = \"\xf4\x90\x80\x80\"",
        "x = \"\x80\"", "x = \"\xc3\"", "x = \"line\nbreak\"", "x = \"\\q\"", "x = \"\\u12\""
    };
    for (size_t i = 0; i < sizeof invalid_encoding / sizeof *invalid_encoding; i++) {
        invalid(invalid_encoding[i]);
        CHECK(!chcl_format(invalid_encoding[i]));
    }
    roundtrip("x = \"caf\xc3\xa9\"\n");
    json_is("", "{}");
    json_is("x = 123456789012345678901234567890\ny = 1.234567890123456789e-20\n",
            "{\"x\": 123456789012345678901234567890, \"y\": 1.234567890123456789e-20}");
    json_is("x = \"a\\n\\t\\r\\\"\\\\\\u00e8\\U0001f600\"\n",
            "{\"x\": \"a\\n\\t\\r\\\"\\\\\xc3\xa8\xf0\x9f\x98\x80\"}");
    json_is("x = <<EOF\na = 1\nlonger = 2\nEOF\ny = true\n",
            "{\"x\": \"a = 1\\nlonger = 2\\n\", \"y\": true}");
    json_is("x = <<-EOF\n    a\n      b\n  EOF\n",
            "{\"x\": \"a\\n  b\\n\"}");
    json_is("r \"one\" {}\nr \"two\" {}\n",
            "{\"r\": [{\"labels\": [\"one\"], \"body\": {}}, {\"labels\": [\"two\"], \"body\": {}}]}");
    json_is("x = 1 # comment\ny = 2 // comment\n", "{\"x\": 1, \"y\": 2}");
    json_is("x = var . foo . bar\n", "{\"x\": {\"traversal\": \"var.foo.bar\"}}");
    const char *valid = "r \"x\" {\n a = [1, 2, {x = true}]\n b = fn(1, 2)\n c = 5 - 3\n d = !false ? var.x : 0\n e = 1 == 2 || 3 != 4 && 5 <= 6 && 7 >= 8\n}\n";
    /* Cross vector (8 elements), line metadata (128 lines), and sbuf growth. */
    char many[8192] = "";
    size_t used = 0;
    for (unsigned i = 0; i < 150; i++) {
        int count = snprintf(many + used, sizeof many - used, "a%u = [1,2,3,4,5,6,7,8,9,10]\n", i);
        CHECK(count > 0 && (size_t)count < sizeof many - used);
        used += (size_t)count;
    }
    allocation_failures(many);
    stable(many);
    allocation_failures(valid);
    allocation_failures("");
    stable(valid);
    const char *object_expression = "x = f({a = 1})\ny = true ? {a = 1} : {b = 2}\nz = {a = 1} == {a = 1}\n";
    stable(object_expression);
    char *expression_fmt = chcl_format(object_expression);
    chcl_document_t *expression_doc = chcl_parse_string(expression_fmt);
    CHECK(expression_doc && !chcl_document_error(expression_doc));
    chcl_free_document(expression_doc); free(expression_fmt);
    stable("");
    stable("x = [for a in xs :\na == 1]\n");
    char *comparison = chcl_format("x = [for a in xs :\na == 1]\n");
    CHECK(comparison && strstr(comparison, "a == 1"));
    free(comparison);
    const char *literal = "x = <<EOF\na = 1\nlonger = 2\nEOF\ny = 2\n";
    char *formatted = chcl_format(literal);
    CHECK(formatted && strstr(formatted, "\na = 1\nlonger = 2\nEOF\n"));
    free(formatted);
    stable(literal);
    stable("x = <<EOF\r\na = 1\r\nlonger = 2\r\nEOF\r\n");
    stable("/* multi\n a = 1\n longer = 2\n*/\nx = 1\n");
    json_is("x = [1, 2,]\ny = f(\n1,\n2,\n)\n", "{\"x\": [1, 2], \"y\": {\"call\": \"f\", \"args\": [1, 2]}}");
    const char *bad[] = {
        "x = 1 @ trailing", "}", "x = [1 2]", "x = f(1 2)",
        "x = \"unterminated", "x = \"trailing\\", "/* unterminated",
        "x = <<EOF\nmissing\n", "x = [for x in xs : x", "r {",
        "x = {a = 1 b = 2}", "x = 1\nx = 2", "x {}\nx = 1",
        "x = {a = 1, a = 2}", "x = 01", "x = 1e", "x = 1.",
        "x = \"\\uD800\"", "x = \"\\u0000\"", "x = \"\\q\""
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) invalid(bad[i]);
    CHECK(!chcl_format("x = 1 @ trailing"));
    CHECK(!chcl_format("x = \"trailing\\"));
    CHECK(!chcl_format("/* unterminated"));
    char deep[4096];
    memset(deep, '!', sizeof deep - 8);
    memcpy(deep, "x = ", 4);
    memcpy(deep + sizeof deep - 8, "true\n", 6);
    invalid(deep);
    /* Error cleanup also owns arrays from already completed/partial nodes. */
    fail_at = 0; calls = 0;
    chcl_document_t *doc = chcl_parse_string("r \"x\" {a = [1,2,3]}\nb = [1 2]");
    CHECK(doc && chcl_document_error(doc));
    size_t total = calls;
    chcl_free_document(doc);
    for (size_t i = 1; i <= total; i++) {
        calls = 0; fail_at = i;
        doc = chcl_parse_string("r \"x\" {a = [1,2,3]}\nb = [1 2]");
        CHECK(!doc || chcl_document_error(doc));
        chcl_free_document(doc);
        CHECK(live == 0);
    }
    fail_at = 0;
    CHECK(!chcl_parse_string(NULL));
    CHECK(!chcl_format(NULL));
    CHECK(live == 0);
    puts("core regressions and allocation-failure checks: OK");
    return 0;
}
