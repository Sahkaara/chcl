/*
 * chcl.h - Public API for the chcl HCL toolkit.
 *
 * This header exposes a small, stable C API to parse, format,
 * and convert HCL (HashiCorp Configuration Language) documents.
 *
 * All functions are thread-unsafe by default; the parser state lives
 * inside the document handle, so distinct documents can be parsed
 * concurrently from distinct threads.
 *
 * Memory ownership rules:
 *   - Every chcl_document_t* returned by a chcl_parse_* function
 *     must be released with chcl_free_document().
 *   - Any char* returned by chcl_format*, chcl_to_json, etc. is
 *     heap-allocated with malloc() and must be released with free().
 */
#ifndef CHCL_H
#define CHCL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Opaque handle to a parsed HCL document */
typedef struct chcl_document chcl_document_t;

/* Parse an in-memory NUL-terminated string. Returns NULL only on OOM. */
chcl_document_t *chcl_parse_string(const char *src);

/* Parse a file from disk. Returns NULL if the file cannot be read or parsed. */
chcl_document_t *chcl_parse_file(const char *path);

/* Release a document handle and all memory owned by it. Safe on NULL. */
void chcl_free_document(chcl_document_t *doc);

/* Format a source buffer and return a newly allocated string (free() it). */
char *chcl_format(const char *src);

/* Read a file, format it, and return the result (free() it). */
char *chcl_format_file(const char *path);

/* Serialize a parsed document to JSON (free() the returned string). */
char *chcl_to_json(chcl_document_t *doc);

/* Human-readable error message for the last failure, or NULL if OK. */
const char *chcl_document_error(chcl_document_t *doc);

/* 1-based line of the last error (0 if no error). */
int chcl_document_error_line(chcl_document_t *doc);

/* 1-based column of the last error (0 if no error). */
int chcl_document_error_column(chcl_document_t *doc);

#ifdef __cplusplus
}
#endif

#endif /* CHCL_H */