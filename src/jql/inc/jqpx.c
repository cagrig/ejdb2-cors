#include "jqp.h"
#include <stdlib.h>
#include <string.h>

#define JQRC(yy_, rc_) do {           \
    iwrc __rc = (rc_);                  \
    if (__rc) _jqp_fatal(yy_, __rc); \
  } while(0)

static void _jqp_fatal(yycontext *yy, iwrc rc) {
  JQPAUX *aux = yy->aux;
  aux->rc = rc;
  longjmp(aux->fatal_jmp, 1);
}

static void *_jqp_malloc(struct _yycontext *yy, size_t size) {
  void *ret = malloc(size);
  if (!ret) {
    JQPAUX *aux = yy->aux;
    aux->rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    longjmp(aux->fatal_jmp, 1);
  }
  return ret;
}

static void *_jqp_realloc(struct _yycontext *yy, void *ptr, size_t size) {
  void *ret = realloc(ptr, size);
  if (!ret) {
    JQPAUX *aux = yy->aux;
    aux->rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    longjmp(aux->fatal_jmp, 1);
  }
  return ret;
}

static iwrc _jqp_aux_set_input(JQPAUX *aux, const char *input) {
  size_t len = strlen(input) + 1;
  char *buf = iwpool_alloc(len, aux->pool);
  if (!buf) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  memcpy(buf, input, len);
  aux->buf = buf;
  return 0;
}

//-----------------

IW_INLINE char *_jqp_strdup(struct _yycontext *yy, const char *text) {
  iwrc rc = 0;
  char *ret = iwpool_strdup(yy->aux->pool, text, &rc);
  JQRC(yy, rc);
  return ret;
}

static JQPUNIT *_jqp_unit(yycontext *yy) {
  JQPUNIT *ret = iwpool_calloc(sizeof(JQPUNIT), yy->aux->pool);
  if (!ret) JQRC(yy, iwrc_set_errno(IW_ERROR_ALLOC, errno));
  return ret;
}

static JQPSTACK *_jqp_push(yycontext *yy) {
  JQPAUX *aux = yy->aux;
  JQPSTACK *stack;
  if (aux->stackn < (sizeof(aux->stackpool) / sizeof(aux->stackpool[0]))) {
    stack = &aux->stackpool[aux->stackn++];
  } else {
    stack = malloc(sizeof(*aux->stack));
    if (!stack) JQRC(yy, iwrc_set_errno(IW_ERROR_ALLOC, errno));
    aux->stackn++;
  }
  memset(stack, 0, sizeof(*stack));
  stack->next = 0;
  if (!aux->stack) {
    stack->prev = 0;
  } else {
    aux->stack->next = stack;
    stack->prev = aux->stack;
  }
  aux->stack = stack;
  return aux->stack;
}

static JQPSTACK _jqp_pop(yycontext *yy) {
  JQPAUX *aux = yy->aux;
  JQPSTACK *stack = aux->stack, ret;
  if (!stack || aux->stackn < 1) {
    iwlog_error2("Unbalanced stack");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  aux->stack = stack->prev;
  if (aux->stack) {
    aux->stack->next = 0;
  }
  stack->prev = 0;
  stack->next = 0;
  ret = *stack;
  if (aux->stackn-- > (sizeof(aux->stackpool) / sizeof(aux->stackpool[0]))) {
    free(stack);
  }
  return ret;
}

static void _jqp_unit_push(yycontext *yy, JQPUNIT *unit) {
  JQPSTACK *stack = _jqp_push(yy);
  stack->type = STACK_UNIT;
  stack->unit = unit;
}

static JQPUNIT *_jqp_unit_pop(yycontext *yy) {
  JQPSTACK stack = _jqp_pop(yy);
  if (stack.type != STACK_UNIT) {
    iwlog_error("Unexpected type: %d", stack.type);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  return stack.unit;
}

static void _jqp_string_push(yycontext *yy, char *str, bool dup) {
  JQPSTACK *stack = _jqp_push(yy);
  stack->type = STACK_STRING;
  stack->str = str;
  if (dup) {
    iwrc rc = 0;
    JQPAUX *aux = yy->aux;
    stack->str = iwpool_strdup(aux->pool, stack->str, &rc);
    if (rc) {
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
  }
}

static char *_jqp_string_pop(yycontext *yy) {
  JQPSTACK stack = _jqp_pop(yy);
  if (stack.type != STACK_STRING) {
    iwlog_error("Unexpected type: %d", stack.type);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  return stack.str;
}

static JQPUNIT *_jqp_string(yycontext *yy, jqp_string_flavours_t flavour, const char *text) {
  JQPUNIT *unit = _jqp_unit(yy);
  unit->type = JQP_STRING_TYPE;
  unit->string.flavour = flavour;
  unit->string.value = _jqp_strdup(yy, text);
  return unit;
}

static JQPUNIT *_jqp_number(yycontext *yy, const char *text) {
  JQPUNIT *unit = _jqp_unit(yy);
  char *eptr;
  int64_t ival = strtoll(text, &eptr, 0);
  if (eptr == text || errno == ERANGE) {
    iwlog_error("Invalid number: %s", text);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  if (*eptr == '.' || *eptr == 'e' || *eptr == 'E') {
    unit->type = JQP_DOUBLE_TYPE;
    unit->dblval.value = strtod(text, &eptr);
    if (eptr == text || errno == ERANGE) {
      iwlog_error("Invalid double number: %s", text);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
  } else {
    unit->type = JQP_INTEGER_TYPE;
    unit->intval.value = ival;
  }
  return unit;
}

static JQPUNIT *_jqp_json_number(yycontext *yy, const char *text) {
  JQPUNIT *unit = _jqp_unit(yy);
  char *eptr;
  unit->type = JQP_JSON_TYPE;
  int64_t ival = strtoll(text, &eptr, 0);
  if (eptr == text || errno == ERANGE) {
    iwlog_error("Invalid number: %s", text);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  if (*eptr == '.' || *eptr == 'e' || *eptr == 'E') {
    unit->json.jn.type = JBV_F64;
    unit->json.jn.vf64 = strtod(text, &eptr);
    if (eptr == text || errno == ERANGE) {
      iwlog_error("Invalid double number: %s", text);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
  } else {
    unit->json.jn.type = JBV_I64;
    unit->json.jn.vi64 = ival;
  }
  return unit;
}

static JQPUNIT *_jqp_json_string(struct _yycontext *yy, const char *text) {
  JQPUNIT *unit = _jqp_unit(yy);
  unit->type = JQP_JSON_TYPE;
  unit->json.jn.type = JBV_STR;
  unit->json.jn.vptr = _jqp_strdup(yy, text);
  unit->json.jn.vsize = strlen(unit->json.jn.vptr);
  return unit;
}

static JQPUNIT *_jqp_json_pair(yycontext *yy, JQPUNIT *key, JQPUNIT *val) {
  if (key->type != JQP_JSON_TYPE || val->type != JQP_JSON_TYPE || key->json.jn.type != JBV_STR) {
    iwlog_error2("Invalid arguments");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  val->json.jn.key = key->json.jn.vptr;
  return val;
}

static JQPUNIT *_jqp_json_collect(yycontext *yy, jbl_type_t type, JQPUNIT *until) {
  JQPAUX *aux = yy->aux;
  JQPUNIT *ret = _jqp_unit(yy);
  ret->type = JQP_JSON_TYPE;
  JBLNODE jn = &ret->json.jn;
  jn->type = type;
  while (aux->stack && aux->stack->type == STACK_UNIT) {
    JQPUNIT *unit = aux->stack->unit;
    if (unit == until) {
      _jqp_pop(yy);
      break;
    }
    if (unit->type != JQP_JSON_TYPE) {
      iwlog_error("Unexpected type: %d", unit->type);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
    JBLNODE ju = &unit->json.jn;
    if (!jn->child) {
      jn->child = ju;
    } else {
      ju->next = jn->child;
      ju->prev = jn->child->prev;
      jn->child->prev = ju;
      jn->child = ju;
    }
    _jqp_pop(yy);
  }
  return ret;
}

static JQPUNIT *_jqp_json_true_false_null(yycontext *yy, const char *text) {
  JQPAUX *aux = yy->aux;
  JQPUNIT *unit = _jqp_unit(yy);
  unit->type = JQP_JSON_TYPE;
  int len = strlen(text);
  if (!strncmp("null", text, len)) {
    unit->json.jn.type = JBV_NULL;
  } else if (!strncmp("true", text, len)) {
    unit->json.jn.type = JBV_BOOL;
    unit->json.jn.vbool = true;
  } else if (!strncmp("false", text, len)) {
    unit->json.jn.type = JBV_BOOL;
    unit->json.jn.vbool = false;
  } else {
    iwlog_error("Invalid json value: %s", text);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  return unit;
}

static void _jqp_op_negate(yycontext *yy) {
  yy->aux->negate = true;
}

static JQPUNIT *_jqp_unit_op(yycontext *yy, const char *text) {
  JQPAUX *aux = yy->aux;
  JQPUNIT *unit = _jqp_unit(yy);
  unit->type = JQP_OP_TYPE;
  unit->op.negate = aux->negate;
  aux->negate = false;
  if (!strcmp(text, "=") || !strcmp(text, "eq")) {
    unit->op.op = JQP_OP_EQ;
  } else if (!strcmp(text, ">") || !strcmp(text, "gt")) {
    unit->op.op = JQP_OP_GT;
  } else if (!strcmp(text, ">=") || !strcmp(text, "gte")) {
    unit->op.op = JQP_OP_GTE;
  } else if (!strcmp(text, "<") || !strcmp(text, "lt")) {
    unit->op.op = JQP_OP_LT;
  } else if (!strcmp(text, "<=") || !strcmp(text, "lte")) {
    unit->op.op = JQP_OP_LTE;
  } else if (!strcmp(text, "in")) {
    unit->op.op = JQP_OP_IN;
  } else if (!strcmp(text, "re")) {
    unit->op.op = JQP_OP_RE;
  } else if (!strcmp(text, "like")) {
    unit->op.op = JQP_OP_RE;
  } else {
    iwlog_error("Invalid operation: %s", text);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  return unit;
}

static JQPUNIT *_jqp_unit_join(yycontext *yy, const char *text) {
  JQPAUX *aux = yy->aux;
  JQPUNIT *unit = _jqp_unit(yy);
  unit->type = JQP_JOIN_TYPE;
  unit->join.negate = aux->negate;
  aux->negate = false;
  if (!strcmp(text, "and")) {
    unit->join.join = JQP_JOIN_AND;
  } else if (!strcmp(text, "or")) {
    unit->join.join = JQP_JOIN_OR;
  }
  return unit;
}

static JQPUNIT *_jqp_expr(yycontext *yy, JQPUNIT *left, JQPUNIT *op, JQPUNIT *right) {
  if (!left || !op || !right) {
    iwlog_error2("Invalid arguments");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  if (op->type != JQP_OP_TYPE && op->type != JQP_JOIN_TYPE) {
    iwlog_error("Unexpected type: %d", op->type);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  JQPAUX *aux = yy->aux;
  JQPUNIT *unit = _jqp_unit(yy);
  unit->type = JQP_EXPR_TYPE;
  unit->expr.left = left;
  unit->expr.op = &op->op;
  unit->expr.right = right;
  return unit;
}

static JQPUNIT *_jqp_pop_expr_chain(yycontext *yy, JQPUNIT *until) {
  JQPUNIT *expr = 0;
  JQPAUX *aux = yy->aux;
  while (aux->stack && aux->stack->type == STACK_UNIT) {
    JQPUNIT *unit = aux->stack->unit;
    if (unit->type == JQP_EXPR_TYPE) {
      if (expr) {
        unit->expr.next = &expr->expr;
      }
      expr = unit;
    } else if (unit->type == JQP_JOIN_TYPE && expr) {
      expr->expr.join = &unit->join;
    } else {
      iwlog_error("Unexpected type: %d", unit->type);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
    _jqp_pop(yy);
    if (unit == until) {
      break;
    }
  }
  return expr;
}

static JQPUNIT *_jqp_projection(struct _yycontext *yy, JQPUNIT *value) {
  if (value->type != JQP_STRING_TYPE) {
    iwlog_error("Unexpected type: %d", value->type);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  JQPUNIT *projection = _jqp_unit(yy);
  projection->type = JQP_PROJECTION_TYPE;
  projection->projection.value = &value->string;
  return projection;
}

static JQPUNIT *_jqp_push_joined_projection(struct _yycontext *yy, JQPUNIT *p) {
  JQPAUX *aux = yy->aux;
  if (!aux->stack || aux->stack->type != STACK_STRING) {
    iwlog_error2("Invalid stack state");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  if (aux->stack->str[0] == '-') {
    p->projection.exclude = true;
  }
  _jqp_pop(yy);
  _jqp_unit_push(yy, p);
  return p;
}

static JQPUNIT *_jqp_pop_joined_projections(yycontext *yy, JQPUNIT *until) {
  JQPUNIT *first = 0;
  JQPAUX *aux = yy->aux;
  while (aux->stack && aux->stack->type == STACK_UNIT) {
    JQPUNIT *unit = aux->stack->unit;
    if (unit->type != JQP_PROJECTION_TYPE) {
      iwlog_error("Unexpected type: %d", unit->type);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
    if (first) {
      unit->projection.next = &first->projection;
    }
    first = unit;
    _jqp_pop(yy);
    if (unit == until) {
      break;
    }
  }
  return first;
}

static JQPUNIT *_jqp_pop_projections(yycontext *yy, JQPUNIT *until) {
  JQPUNIT *first = 0;
  JQPAUX *aux = yy->aux;
  while (aux->stack && aux->stack->type == STACK_UNIT) {
    JQPUNIT *unit = aux->stack->unit;
    if (unit->type != JQP_STRING_TYPE) {
      iwlog_error("Unexpected type: %d", unit->type);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
    if (first) {
      unit->string.next = &first->string;
    }
    first = unit;
    _jqp_pop(yy);
    if (unit == until) {
      break;
    }
  }
  return _jqp_projection(yy, first);
}

static JQPUNIT *_jqp_pop_projfields_chain(yycontext *yy, JQPUNIT *until) {
  JQPUNIT *field = 0;
  JQPAUX *aux = yy->aux;
  while (aux->stack && aux->stack->type == STACK_UNIT) {
    JQPUNIT *unit = aux->stack->unit;
    if (unit->type != JQP_STRING_TYPE) {
      iwlog_error("Unexpected type: %d", unit->type);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
    unit->string.flavour = JQP_STR_PROJFIELD;
    if (field) {
      unit->string.next = &field->string;
    }
    field = unit;
    _jqp_pop(yy);
    if (unit == until) {
      break;
    }
  }
  return field;
}

static JQPUNIT *_jqp_node(yycontext *yy, JQPUNIT *value) {
  JQPAUX *aux = yy->aux;
  JQPUNIT *unit = _jqp_unit(yy);
  unit->type = JQP_NODE_TYPE;
  unit->node.value = value;
  if (value->type == JQP_EXPR_TYPE) {
    unit->node.ntype = JQP_NODE_EXPR;
  } else if (value->type == JQP_STRING_TYPE) {
    const char *str = value->string.value;
    size_t len = strlen(str);
    if (!strncmp("*", str, len)) {
      unit->node.ntype = JQP_NODE_ANY;
    } else if (!strncmp("**", str, len)) {
      unit->node.ntype = JQP_NODE_ANYS;
    } else {
      unit->node.ntype = JQP_NODE_FIELD;
    }
  } else {
    iwlog_error("Invalid node value type: %d", value->type);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  return unit;
}

static JQPUNIT *_jqp_pop_node_chain(yycontext *yy, JQPUNIT *until) {
  JQPUNIT *filter, *first = 0;
  JQPAUX *aux = yy->aux;
  while (aux->stack && aux->stack->type == STACK_UNIT) {
    JQPUNIT *unit = aux->stack->unit;
    if (unit->type != JQP_NODE_TYPE) {
      iwlog_error("Unexpected type: %d", unit->type);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
    if (first) {
      unit->node.next = &first->node;
    }
    first = unit;
    _jqp_pop(yy);
    if (unit == until) {
      break;
    }
  }
  if (!first) {
    iwlog_error2("Invalid state");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  filter = _jqp_unit(yy);
  filter->type = JQP_FILTER_TYPE;
  filter->filter.node = &first->node;
  if (aux->stack
      && aux->stack->type == STACK_UNIT
      && aux->stack->unit->type == JQP_STRING_TYPE
      && aux->stack->unit->string.flavour == JQP_STR_ANCHOR) {
    filter->filter.anchor = _jqp_unit_pop(yy)->string.value;
  }
  return filter;
}

static JQPUNIT *_jqp_pop_filters_and_set_query(yycontext *yy, JQPUNIT *until) {
  JQPUNIT *query, *filter = 0;
  JQPAUX *aux = yy->aux;
  while (aux->stack && aux->stack->type == STACK_UNIT) {
    JQPUNIT *unit = aux->stack->unit;
    if (unit->type == JQP_JOIN_TYPE) {
      if (!filter) {
        iwlog_error2("Invalid state");
        JQRC(yy, JQL_ERROR_QUERY_PARSE);
      }
      filter->filter.join = &unit->join;
    } else if (unit->type == JQP_FILTER_TYPE) {
      if (filter) {
        filter->filter.next = &filter->filter;
      }
      filter = unit;
    } else {
      iwlog_error("Unexpected type: %d", unit->type);
      JQRC(yy, JQL_ERROR_QUERY_PARSE);
    }
    _jqp_pop(yy);
    if (unit == until) {
      break;
    }
  }
  if (!filter) {
    iwlog_error2("Invalid state");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  query = _jqp_unit(yy);
  query->type = JQP_QUERY_TYPE;
  query->query.filter = &filter->filter;
  aux->query = &query->query;
  return query;
}

static void _jqp_set_apply(yycontext *yy, JQPUNIT *unit) {
  JQPAUX *aux = yy->aux;
  if (!unit || !aux->query) {
    iwlog_error2("Invalid arguments");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  if (unit->type == JQP_JSON_TYPE) {
    aux->query->apply = &unit->json.jn;
    aux->query->apply_placeholder = 0;
  } else if (unit->type == JQP_STRING_TYPE && unit->string.flavour == JQP_STR_PLACEHOLDER) {
    aux->query->apply_placeholder = unit->string.value;
    aux->query->apply = 0;
  } else {
    iwlog_error("Unexpected type: %d", unit->type);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
}

static void _jqp_set_projection(yycontext *yy, JQPUNIT *unit) {
  JQPAUX *aux = yy->aux;
  if (!unit || !aux->query) {
    iwlog_error2("Invalid arguments");
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
  if (unit->type == JQP_PROJECTION_TYPE) {
    aux->query->projection = &unit->projection;
  } else {
    iwlog_error("Unexpected type: %d", unit->type);
    JQRC(yy, JQL_ERROR_QUERY_PARSE);
  }
}

//--------------- Public API

iwrc jqp_aux_create(JQPAUX **auxp, const char *input) {
  iwrc rc = 0;
  *auxp = calloc(1, sizeof(**auxp));
  if (!*auxp) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  JQPAUX *aux = *auxp;
  aux->line = 1;
  aux->col = 1;
  aux->xerr = iwxstr_new();
  if (!aux->xerr) {
    rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    goto finish;
  }
  aux->pool = iwpool_create(4 * 1024);
  if (!aux->pool) {
    rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    goto finish;
  }
  rc = _jqp_aux_set_input(aux, input);
  
finish:
  if (rc) {
    jqp_aux_destroy(auxp);
  }
  return rc;
}

void jqp_aux_destroy(JQPAUX **auxp) {
  JQPAUX *aux = *auxp;
  if (aux) {
    if (aux->pool) {
      iwpool_destroy(aux->pool);
    }
    if (aux->xerr) {
      iwxstr_destroy(aux->xerr);
    }
    free(aux);
    *auxp = 0;
  }
}

IW_INLINE iwrc _iwxstr_cat2(IWXSTR *xstr, const char *buf) {
  return iwxstr_cat(xstr, buf, strlen(buf));
}

static void yyerror(yycontext *yy) {
  JQPAUX *aux = yy->aux;
  IWXSTR *xerr = aux->xerr;
  if (yy->__text[0]) {
    _iwxstr_cat2(xerr, "near token: '");
    _iwxstr_cat2(xerr, yy->__text);
    _iwxstr_cat2(xerr, "'\n");
  }
  if (yy->__pos < yy->__limit) {
    char buf[2] = {0};
    yy->__buf[yy->__limit] = '\0';
    _iwxstr_cat2(xerr, "\n");
    while (yy->__pos < yy->__limit) {
      buf[0] = yy->__buf[yy->__pos++];
      iwxstr_cat(xerr, buf, 1);
    }
  }
  _iwxstr_cat2(xerr, " <--- \n");
}

iwrc jqp_parse(JQPAUX *aux) {
  yycontext yy = {0};
  yy.aux = aux;
  if (setjmp(aux->fatal_jmp)) {
    if (aux->rc) {
      iwlog_ecode_error3(aux->rc);
    }
    goto finish;
  }
  if (!yyparse(&yy)) {
    if (!aux->rc) {
      aux->rc = JQL_ERROR_QUERY_PARSE;
    }
    yyerror(&yy);
    if (iwxstr_size(aux->xerr)) {
      iwlog_error("Syntax error: %s\n", iwxstr_ptr(aux->xerr));
    }
  }
  
finish:
  yyrelease(&yy);
  return aux->rc;
}