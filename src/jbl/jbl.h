#pragma once
#ifndef JBLOB_H
#define JBLOB_H

/**************************************************************************************************
 * EJDB2
 *
 * MIT License
 *
 * Copyright (c) 2012-2020 Softmotions Ltd <info@softmotions.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/

/** @file
 *
 * @brief JSON serialization and patching routines.
 *
 * Supported standards:
 *
 *  - [JSON Patch](https://tools.ietf.org/html/rfc6902)
 *  - [JSON Merge patch](https://tools.ietf.org/html/rfc7386)
 *  - [JSON Path specification](https://tools.ietf.org/html/rfc6901)
 *
 * JSON document can be represented in three different formats:
 *
 *  - Plain JSON text.
 *
 *  - @ref JBL Memory compact binary format [Binn](https://github.com/liteserver/binn)
 *    Used for JSON serialization but lacks of data modification flexibility.
 *
 *  - @ref JBL_NODE In memory JSON document presented as tree. Convenient for in-place
 *    document modification and patching.
 *
 * Library function allows conversion of JSON document between above formats.
 */

#include <ejdb2/iowow/iwlog.h>
#include <ejdb2/iowow/iwpool.h>
#include <ejdb2/iowow/iwxstr.h>
#include <stdbool.h>

IW_EXTERN_C_START

/**
 * @brief JSON document in compact binary format [Binn](https://github.com/liteserver/binn)
 */
struct _JBL;
typedef struct _JBL *JBL;

typedef enum {
  _JBL_ERROR_START = (IW_ERROR_START + 15000UL + 1000),
  JBL_ERROR_INVALID_BUFFER,             /**< Invalid JBL buffer (JBL_ERROR_INVALID_BUFFER) */
  JBL_ERROR_CREATION,                   /**< Cannot create JBL object (JBL_ERROR_CREATION) */
  JBL_ERROR_INVALID,                    /**< Invalid JBL object (JBL_ERROR_INVALID) */
  JBL_ERROR_PARSE_JSON,                 /**< Failed to parse JSON string (JBL_ERROR_PARSE_JSON) */
  JBL_ERROR_PARSE_UNQUOTED_STRING,      /**< Unquoted JSON string (JBL_ERROR_PARSE_UNQUOTED_STRING) */
  JBL_ERROR_PARSE_INVALID_CODEPOINT,    /**< Invalid unicode codepoint/escape sequence (JBL_ERROR_PARSE_INVALID_CODEPOINT) */
  JBL_ERROR_PARSE_INVALID_UTF8,         /**< Invalid utf8 string (JBL_ERROR_PARSE_INVALID_UTF8) */
  JBL_ERROR_JSON_POINTER,               /**< Invalid JSON pointer (rfc6901) path (JBL_ERROR_JSON_POINTER) */
  JBL_ERROR_PATH_NOTFOUND,              /**< JSON object not matched the path specified (JBL_ERROR_PATH_NOTFOUND) */
  JBL_ERROR_PATCH_INVALID,              /**< Invalid JSON patch specified (JBL_ERROR_PATCH_INVALID) */
  JBL_ERROR_PATCH_INVALID_OP,           /**< Invalid JSON patch operation specified (JBL_ERROR_PATCH_INVALID_OP) */
  JBL_ERROR_PATCH_NOVALUE,              /**< No value specified in JSON patch (JBL_ERROR_PATCH_NOVALUE) */
  JBL_ERROR_PATCH_TARGET_INVALID,       /**< Could not find target object to set value (JBL_ERROR_PATCH_TARGET_INVALID) */
  JBL_ERROR_PATCH_INVALID_VALUE,        /**< Invalid value specified by patch (JBL_ERROR_PATCH_INVALID_VALUE) */
  JBL_ERROR_PATCH_INVALID_ARRAY_INDEX,  /**< Invalid array index in JSON patch path (JBL_ERROR_PATCH_INVALID_ARRAY_INDEX) */
  JBL_ERROR_NOT_AN_OBJECT,              /**< JBL is not an object (JBL_ERROR_NOT_AN_OBJECT) */
  JBL_ERROR_PATCH_TEST_FAILED,          /**< JSON patch test operation failed (JBL_ERROR_PATCH_TEST_FAILED) */
  _JBL_ERROR_END
} jbl_ecode_t;

typedef struct _JBL_iterator {
  unsigned char *pnext;
  unsigned char *plimit;
  int   type;
  int   count;
  int   current;
} JBL_iterator;

typedef uint8_t jbl_print_flags_t;
#define JBL_PRINT_PRETTY      ((jbl_print_flags_t) 0x01U)
#define JBL_PRINT_CODEPOINTS  ((jbl_print_flags_t) 0x02U)

typedef uint8_t jbn_visitor_cmd_t;
#define JBL_VCMD_OK           ((jbn_visitor_cmd_t) 0x00U)
#define JBL_VCMD_TERMINATE    ((jbn_visitor_cmd_t) 0x01U)
#define JBL_VCMD_SKIP_NESTED  ((jbn_visitor_cmd_t) 0x02U)
#define JBN_VCMD_DELETE       ((jbn_visitor_cmd_t) 0x04U)

typedef enum {
  JBV_NONE = 0, // Do not reorder
  JBV_NULL,

  JBV_BOOL,     // Do not reorder
  JBV_I64,
  JBV_F64,
  JBV_STR,

  JBV_OBJECT,   // Do not reorder
  JBV_ARRAY,
} jbl_type_t;

/**
 * @brief JSON document as in-memory tree (DOM tree).
 */
typedef struct _JBL_NODE {
  struct _JBL_NODE *next;
  struct _JBL_NODE *prev;
  struct _JBL_NODE *parent; /**< Optional parent */
  const char *key;
  int klidx;
  uint32_t flags;           /**< Utility node flags */

  // Do not sort/add members after this point (offsetof usage below)
  struct _JBL_NODE *child;
  int vsize;
  jbl_type_t type;
  union {
    const char *vptr;
    bool vbool;
    int64_t vi64;
    double vf64;
  };
} *JBL_NODE;

/**
 * @brief JSON Patch operation according to rfc6902
 */
typedef enum {
  JBP_ADD = 1,
  JBP_REMOVE,
  JBP_REPLACE,
  JBP_COPY,
  JBP_MOVE,
  JBP_TEST,
  JBP_INCREMENT // Not standard increment operation
} jbp_patch_t;

/**
 * @brief JSON patch specification
 */
typedef struct _JBL_PATCH {
  jbp_patch_t op;
  const char *path;
  const char *from;
  const char *vjson;
  JBL_NODE vnode;
} JBL_PATCH;

/**
 * @brief JSON pointer rfc6901
 * @see jbl_ptr_alloc()
 */
typedef struct _JBL_PTR {
  uint64_t op;      /**< Opaque data associated with pointer */
  int cnt;          /**< Number of nodes */
  int sz;           /**< Size of JBL_PTR allocated area */
  char *n[1];       /**< Path nodes */
} *JBL_PTR;

/** Prints JSON to some oputput specified by `op` */
typedef iwrc(*jbl_json_printer)(const char *data, int size, char ch, int count, void *op);

/**
 * @brief Create empty binary JSON object.
 *
 * @note `jblp` should be disposed by `jbl_destroy()`
 * @see `jbl_fill_from_node()`
 * @param [out] jblp Pointer to be initialized by new object.
 */
IW_EXPORT iwrc jbl_create_empty_object(JBL *jblp);

/**
 * @brief Create empty binary JSON array.
 *
 * @note `jblp` should be disposed by `jbl_destroy()`
 * @see `jbl_fill_from_node()`
 * @param [out] jblp Pointer to be initialized by new object.
 */
IW_EXPORT iwrc jbl_create_empty_array(JBL *jblp);

/**
 * @brief Set integer JBL object property value
 *        or add a new entry to end of array JBL object.
 *
 * In the case when `jbl` object is array value will be added to end array.
 *
 * @warning `jbl` object must writable in other words created with
 *          `jbl_create_empty_object()` or `jbl_create_empty_array()`
 *
 * @param jbl JBL container
 * @param key Object key. Does't makes sense for array objects.
 * @param v   Value to set
 */
IW_EXPORT iwrc jbl_set_int64(JBL jbl, const char *key, int64_t v);

/**
 * @brief Set double JBL object property value
 *        or add a new entry to end of array JBL object.
 *
 * In the case when `jbl` object is array value will be added to end array.
 *
 * @warning `jbl` object must writable in other words created with
 *          `jbl_create_empty_object()` or `jbl_create_empty_array()`
 *
 * @param jbl JBL container
 * @param key Object key. Does't makes sense for array objects.
 * @param v   Value to set
 */
IW_EXPORT iwrc jbl_set_f64(JBL jbl, const char *key, double v);

/**
 * @brief Set string JBL object property value
 *        or add a new entry to end of array JBL object.
 *
 * In the case when `jbl` object is array value will be added to end array.
 *
 * @warning `jbl` object must writable in other words created with
 *          `jbl_create_empty_object()` or `jbl_create_empty_array()`
 *
 * @param jbl JBL container
 * @param key Object key. Does't makes sense for array objects.
 * @param v   Value to set
 */
IW_EXPORT iwrc jbl_set_string(JBL jbl, const char *key, const char *v);

IW_EXPORT iwrc jbl_set_string_printf(JBL jbl, const char *key, const char *format, ...);

/**
 * @brief Set bool JBL object property value
 *        or add a new entry to end of array JBL object.
 *
 * In the case when `jbl` object is array value will be added to end array.
 *
 * @warning `jbl` object must writable in other words created with
 *          `jbl_create_empty_object()` or `jbl_create_empty_array()`
 *
 * @param jbl JBL container
 * @param key Object key. Does't makes sense for array objects.
 * @param v   Value to set
 */
IW_EXPORT iwrc jbl_set_bool(JBL jbl, const char *key, bool v);


/**
 * @brief Set null JBL object property value
 *        or add a new entry to end of array JBL object.
 *
 * In the case when `jbl` object is array value will be added to end array.
 *
 * @warning `jbl` object must writable in other words created with
 *          `jbl_create_empty_object()` or `jbl_create_empty_array()`
 *
 * @param jbl JBL container
 * @param key Object key. Does't makes sense for array objects.
 * @param v   Value to set
 */
IW_EXPORT iwrc jbl_set_null(JBL jbl, const char *key);


IW_EXPORT iwrc jbl_set_empty_array(JBL jbl, const char *key);

IW_EXPORT iwrc jbl_set_empty_object(JBL jbl, const char *key);

/**
 * @brief Set nested JBL object property value
 *        or add a new entry to end of array JBL object.
 *
 * In the case when `jbl` object is array value will be added to end array.
 *
 * @warning `jbl` object must writable in other words created with
 *          `jbl_create_empty_object()` or `jbl_create_empty_array()`
 *
 * @param jbl JBL container
 * @param key Object key. Does't makes sense for array objects.
 * @param v   Value to set
 */
IW_EXPORT iwrc jbl_set_nested(JBL jbl, const char *key, JBL nested);

/**
 * @brief Initialize new `JBL` document by `binn` data from buffer.
 * @note Created document will be allocated by `malloc()`
 * and should be destroyed by `jbl_destroy()`.
 *
 * @param [out] jblp        Pointer initialized by created JBL document. Not zero.
 * @param buf               Memory buffer with `binn` data. Not zero.
 * @param bufsz             Size of `buf`
 * @param keep_on_destroy   If true `buf` not will be freed by `jbl_destroy()`
 */
IW_EXPORT iwrc jbl_from_buf_keep(JBL *jblp, void *buf, size_t bufsz, bool keep_on_destroy);

/**
 * @brief Clones a given `src` JBL object into newly allocated `targetp` object.
 *
 * @param src Source object to clonse
 * @param targetp Pointer on target object
 */
IW_EXPORT iwrc jbl_clone(JBL src, JBL *targetp);

/**
 * @brief Constructs new `JBL` object from JSON string.
 * @note `jblp` should be disposed by `jbl_destroy()`
 * @param [out] jblp  Pointer initialized by created JBL document. Not zero.
 * @param jsonstr     JSON string to be converted
 */
IW_EXPORT iwrc jbl_from_json(JBL *jblp, const char *jsonstr);

/**
 * @brief Get type of `jbl` value.
 */
IW_EXPORT jbl_type_t jbl_type(JBL jbl);

/**
 * @brief Get number of child elements in `jbl` container (object/array) or zero.
 */
IW_EXPORT size_t jbl_count(JBL jbl);

/**
 * @brief Get size of undelying data buffer of `jbl` value passed.
 */
IW_EXPORT size_t jbl_size(JBL jbl);

/**
 * @brief Interpret `jbl` value as `int32_t`.
 * Returns zero if value cannot be converted.
 */
IW_EXPORT int32_t jbl_get_i32(JBL jbl);

/**
 * @brief Interpret `jbl` value as `int64_t`.
 * Returns zero if value cannot be converted.
 */
IW_EXPORT int64_t jbl_get_i64(JBL jbl);

/**
 * @brief Interpret `jbl` value as `double` value.
 * Returns zero if value cannot be converted.
 */
IW_EXPORT double jbl_get_f64(JBL jbl);

/**
 * @brief Interpret `jbl` value as `\0` terminated character array.
 * Returns zero if value cannot be converted.
 */
IW_EXPORT char *jbl_get_str(JBL jbl);

IW_EXPORT iwrc jbl_object_get_i64(JBL jbl, const char *key, int64_t *out);

IW_EXPORT iwrc jbl_object_get_f64(JBL jbl, const char *key, double *out);

IW_EXPORT iwrc jbl_object_get_bool(JBL jbl, const char *key, bool *out);

IW_EXPORT iwrc jbl_object_get_str(JBL jbl, const char *key, const char **out);

IW_EXPORT iwrc jbl_object_get_fill_jbl(JBL jbl, const char *key, JBL out);

IW_EXPORT jbl_type_t jbl_object_get_type(JBL jbl, const char *key);

/**
 * @brief Same as `jbl_get_str()` but copies at most `bufsz` into target `buf`.
 * Target buffer not touched if `jbl` value cannot be converted.
 */
IW_EXPORT size_t jbl_copy_strn(JBL jbl, char *buf, size_t bufsz);

/**
 * @brief Finds value in `jbl` document pointed by rfc6901 `path` and store it into `res`.
 *
 * @note `res` should be disposed by `jbl_destroy()`.
 * @note If value is not fount `res` will be set to zero.
 * @param jbl         JBL document. Not zero.
 * @param path        rfc6901 JSON pointer. Not zero.
 * @param [out] res   Output value holder
 */
IW_EXPORT iwrc jbl_at(JBL jbl, const char *path, JBL *res);

IW_EXPORT iwrc jbn_at(JBL_NODE node, const char *path, JBL_NODE *res);

/**
 * @brief @brief Finds value in `jbl` document pointed by `jp` structure and store it into `res`.
 *
 * @note `res` should be disposed by `jbl_destroy()`.
 * @note If value is not fount `res` will be set to zero.
 * @see `jbl_ptr_alloc()`
 * @param jbl         JBL document. Not zero.
 * @param jp          JSON pointer.
 * @param [out] res   Output value holder
 */
IW_EXPORT iwrc jbl_at2(JBL jbl, JBL_PTR jp, JBL *res);

IW_EXPORT iwrc jbn_at2(JBL_NODE node, JBL_PTR jp, JBL_NODE *res);

/**
 * @brief Represent `jbl` document as raw data buffer.
 *
 * @note Caller do not require release `buf` explicitly.
 * @param jbl         JBL document. Not zero.
 * @param [out] buf   Pointer to data buffer. Not zero.
 * @param [out] size  Pointer to data buffer size. Not zero.
 */
IW_EXPORT iwrc jbl_as_buf(JBL jbl, void **buf, size_t *size);

/**
 * @brief Prints JBL document as JSON string.
 *
 * @see jbl_fstream_json_printer()
 * @see jbl_xstr_json_printer()
 * @see jbl_count_json_printer()
 *
 * @param jbl  JBL document. Not zero.
 * @param pt   JSON printer function pointer. Not zero.
 * @param op   Pointer to user data for JSON printer function.
 * @param pf   JSON printing mode.
 */
IW_EXPORT iwrc jbl_as_json(JBL jbl, jbl_json_printer pt, void *op, jbl_print_flags_t pf);

/**
 * @brief JSON printer to stdlib `FILE*`pointer. Eg: `stderr`, `stdout`
 * @param op `FILE*` pointer
 */
IW_EXPORT iwrc jbl_fstream_json_printer(const char *data, int size, char ch, int count, void *op);

/**
 * @brief JSON printer to extended string buffer `IWXSTR`
 * @param op `IWXSTR*` pointer
 */
IW_EXPORT iwrc jbl_xstr_json_printer(const char *data, int size, char ch, int count, void *op);

/**
 * @brief Just counts bytes in JSON text.
 * @param op `int*` Pointer to counter number.
 */
IW_EXPORT iwrc jbl_count_json_printer(const char *data, int size, char ch, int count, void *op);

/**
 * @brief Destroys JBL document and releases its heap resources.
 * @note Will set `jblp` to zero.
 * @param jblp Pointer holder of JBL document. Not zero.
 */
IW_EXPORT void jbl_destroy(JBL *jblp);

/**
 * @brief Initializes placeholder for jbl iteration.
 *        Must be freed by `jbl_destroy()` after iteration.
 * @param [out] jblp Pointer to be initialized by new object.
 */
IW_EXPORT iwrc jbl_create_iterator_holder(JBL *jblp);

/**
 * @brief Initialize allocated iterator over given `jbl` object.
 *
 * @param jbl JBL object to iterate
 * @param iter Iterator state placeholder allocated by `jbl_create_iter_placeholder()`
 */
IW_EXPORT iwrc jbl_iterator_init(JBL jbl, JBL_iterator *iter);

/**
 * @brief Get next value from JBL_iterator.
 * Returns `false` if iteration is over.
 *
 * @param iter    Iterator object.
 * @param holder  Holder to object pointed by current iteration.
 * @param pkey    Key value holder. Zero in the case of iteration over array.
 * @param klen    Key length or array index in the case of iteration over array.
 */
IW_EXPORT bool jbl_iterator_next(JBL_iterator *iter, JBL holder, char **pkey, int *klen);

//--- JBL_NODE

/**
 * @brief Converts `jbl` value to `JBL_NODE` tree.
 * @note `node` resources will be released when `pool` destroyed.
 *
 * @param jbl         JSON document in compact `binn` format. Not zero.
 * @param [out] node  Holder of new `JBL_NODE` value. Not zero.
 * @param pool        Memory used to allocate new `JBL_NODE` tree. Not zero.
 */
IW_EXPORT iwrc jbl_to_node(JBL jbl, JBL_NODE *node, IWPOOL *pool);

/**
 * @brief Converts `json` text to `JBL_NODE` tree.
 * @note `node` resources will be released when `pool` destroyed.
 *
 * @param json        JSON text
 * @param [out] node  Holder of new `JBL_NODE` value. Not zero.
 * @param pool        Memory used to allocate new `JBL_NODE` tree. Not zero.
 */
IW_EXPORT iwrc jbl_node_from_json(const char *json, JBL_NODE *node, IWPOOL *pool);

/**
 * @brief Prints JBL_NODE document as JSON string.
 *
 * @see jbl_fstream_json_printer()
 * @see jbl_xstr_json_printer()
 * @see jbl_count_json_printer()
 *
 * @param node `JBL_NODE` document. Not zero.
 * @param pt    JSON printer function. Not zero.
 * @param op    Pointer to user data for JSON printer function.
 * @param pf    JSON printing mode.
 */
IW_EXPORT iwrc jbl_node_as_json(JBL_NODE node, jbl_json_printer pt, void *op, jbl_print_flags_t pf);

/**
 * @brief Fill `jbl` document by data from `node`.
 *
 * Common use case:
 *  Create empty document with `jbl_create_empty_object()` `jbl_create_empty_array()`
 *  then fill it with `jbl_fill_from_node()`
 *
 * @param jbl   JBL document to be filled. Not zero.
 * @param node  Source tree node. Not zero.
 */
IW_EXPORT iwrc jbl_fill_from_node(JBL jbl, JBL_NODE node);

/**
 * @brief Compares JSON tree nodes.
 *
 * - Primitive JSON values compared as is.
 * - JSON arrays compared by values held in the same position in array.
 * - JSON objects compared by corresponding values held under lexicographically sorted keys.
 *
 * @param n1
 * @param n2
 * @param [out] rcp
 *
 * @return - Not zero if `n1` and `n2` have different types.
 *         - Zero if `n1` and `n2` are equal.
 *         - Greater than zero  if `n1` greater than `n2`
 *         - Lesser than zero if `n1` lesser than `n2`
 */
IW_EXPORT int jbl_compare_nodes(JBL_NODE n1, JBL_NODE n2, iwrc *rcp);

/**
 * @brief Add item to the `parent` container.
 */
IW_EXPORT void jbl_add_item(JBL_NODE parent, JBL_NODE node);

/**
 * @brief Add item from the `parent` container.
 */
IW_EXPORT void jbl_remove_item(JBL_NODE parent, JBL_NODE child);

/**
 * @brief Remove subtree from `target` node pointed by `path`
 */
IW_EXPORT JBL_NODE jbl_node_detach(JBL_NODE target, JBL_PTR path);

/**
 * @brief Reset tree `node` data.
 */
IW_EXPORT void jbl_node_reset_data(JBL_NODE node);

/**
 * @brief Parses rfc6901 JSON path.
 * @note `jpp` structure should be disposed by `free()`.
 *
 * @param path      JSON path string. Not zero.
 * @param [out] jpp Holder for parsed path structure. Not zero.
 */
IW_EXPORT iwrc jbl_ptr_alloc(const char *path, JBL_PTR *jpp);

/**
 * @brief Parses rfc6901 JSON path.
 *
 * @param path  JSON path string. Not zero.
 * @param [out] jpp JSON path string. Not zero.
 * @param pool  Pool used for memory allocation. Not zero.
 */
IW_EXPORT iwrc jbl_ptr_alloc_pool(const char *path, JBL_PTR *jpp, IWPOOL *pool);

/**
 * @brief Compare JSON pointers.
 */
IW_EXPORT int jbl_ptr_cmp(JBL_PTR p1, JBL_PTR p2);

/**
 * @brief Serialize JSON pointer to as text.
 * @param ptr   JSON pointer. Not zero.
 * @param xstr  Output string buffer. Not zero.
 */
IW_EXPORT iwrc jbl_ptr_serialize(JBL_PTR ptr, IWXSTR *xstr);

/**
 * @brief JBL_NODE visitor context
 */
typedef struct _JBN_VCTX {
  JBL_NODE root;  /**< Root node from which started visitor */
  void *op;       /**< Arbitrary opaque data */
  void *result;
  bool terminate; /**< It `true` document traversal will be terminated immediately. */
  IWPOOL *pool;   /**< Pool placeholder, initialization is responsibility of `JBN_VCTX` creator */
  int pos;        /**< Aux position, not actually used by visitor core */
} JBN_VCTX;

/**
 * Call with lvl: `-1` means end of visiting whole object tree.
 */
typedef jbn_visitor_cmd_t (*JBN_VISITOR)(int lvl, JBL_NODE n, const char *key, int klidx, JBN_VCTX *vctx, iwrc *rc);

IW_EXPORT iwrc jbn_visit(JBL_NODE node, int lvl, JBN_VCTX *vctx, JBN_VISITOR visitor);

//--- PATCHING

IW_EXPORT iwrc jbl_patch_auto(JBL_NODE root, JBL_NODE patch, IWPOOL *pool);

IW_EXPORT iwrc jbl_patch_node(JBL_NODE root, const JBL_PATCH *patch, size_t cnt);

IW_EXPORT iwrc jbl_patch(JBL jbl, const JBL_PATCH *patch, size_t cnt);

IW_EXPORT iwrc jbl_patch_from_json(JBL jbl, const char *patchjson);

IW_EXPORT iwrc jbl_merge_patch_node(JBL_NODE root, const char *patchjson, IWPOOL *pool);

IW_EXPORT iwrc jbl_merge_patch(JBL jbl, const char *patchjson);

IW_EXPORT iwrc jbl_merge_patch_jbl(JBL jbl, JBL patch);


IW_EXPORT iwrc jbl_init(void);

IW_EXTERN_C_END
#endif
