/* SPDX-License-Identifier: MIT */

/*
 * chcl.h - Public API for the chcl HCL toolkit.
 *
 * This header exposes a small, stable C API to parse, format,
 * and convert HCL (HashiCorp Configuration Language) documents.
 *
 * Distinct documents can be used concurrently. Do not access the same
 * document concurrently while another thread releases it.
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

/* Parse a NUL-terminated string. NULL on OOM, NULL input or size limit.
 * Syntax errors return a document: inspect chcl_document_error().
 * Input limit: INT_MAX / 4 bytes; parser recursion limit: 128 frames.
 * Escaped U+0000 is rejected because API strings are NUL-terminated.
 */
chcl_document_t *chcl_parse_string(const char *src);

/* Parse a seekable file. NULL on I/O/OOM/size failure or embedded NUL.
 * Syntax errors return a document with chcl_document_error() set.
 */
chcl_document_t *chcl_parse_file(const char *path);

/* Release a document handle and all memory owned by it. Safe on NULL. */
void chcl_free_document(chcl_document_t *doc);

/* Format a source buffer; free() the result, including an empty string.
 * NULL on invalid arguments, lexical error, OOM or size limit.
 * Formatting does not validate the complete parser grammar.
 */
char *chcl_format(const char *src);

/* Read a file, format it, and return the result (free() it). */
char *chcl_format_file(const char *path);

/* Serialize a parsed document; free() the result. NULL on invalid/error
 * document, OOM, output size limit or serialization depth over 512 nodes.
 * Numbers retain their source spelling. Each block type maps to an array.
 */
char *chcl_to_json(chcl_document_t *doc);

/* Human-readable error message for the last failure, or NULL if OK. */
const char *chcl_document_error(chcl_document_t *doc);

/* 1-based line of the last error (0 if no error). */
int chcl_document_error_line(chcl_document_t *doc);

/* 1-based column of the last error (0 if no error). */
int chcl_document_error_column(chcl_document_t *doc);

/*
 * Return 1 if the file at `path` is already canonically formatted,
 * 0 if formatting would change it, -1 on I/O/formatting/OOM error.
 * Useful for CI checks (`chcl fmt --check <file>`).
 */
int chcl_format_is_canonical(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CHCL_H */