#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"

#define USE_OPLINE const zend_op *opline = EX(opline);
#define GET_OP1_ZVAL_PTR_UNDEF(fetch) \
  get_zval_ptr_undef(opline->op1_type, opline->op1, free_op1, execute_data, opline)
#define GET_OP2_ZVAL_PTR_UNDEF(fetch) \
  get_zval_ptr_undef(opline->op2_type, opline->op2, free_op2, execute_data, opline)

#define FREE_OP1 if (free_op1) { zval_ptr_dtor_nogc(free_op1); }
#define FREE_OP2 if (free_op2) { zval_ptr_dtor_nogc(free_op2); }

static
zval *get_zval_ptr_undef(zend_uchar op_type, znode_op node, zval *free_op,
                         zend_execute_data *execute_data, const zend_op* opline) {
  switch (op_type) {
    case IS_TMP_VAR:
    case IS_VAR:      return (free_op = EX_VAR(node.var));
    case IS_CONST:    return RT_CONSTANT(opline, node);
    case IS_CV:       return EX_VAR(node.var);
    default:          return NULL;
  }
}

/* ----------------------------------------------------------------------- */

#define UNARY_OPS(X) \
  X(BW_NOT,              __bw_not)

#define BINARY_OPS(X) \
  X(ADD,                 __add) \
  X(SUB,                 __sub) \
  X(MUL,                 __mul) \
  X(DIV,                 __div) \
  X(MOD,                 __mod) \
  X(POW,                 __pow) \
  X(SL,                  __sl) \
  X(SR,                  __sr) \
  X(CONCAT,              __concat) \
  X(BW_OR,               __bw_or) \
  X(BW_AND,              __bw_and) \
  X(BW_XOR,              __bw_xor) \
  X(IS_IDENTICAL,        __is_identical) \
  X(IS_NOT_IDENTICAL,    __is_not_identical) \
  X(IS_EQUAL,            __is_equal) \
  X(IS_NOT_EQUAL,        __is_not_equal) \
  X(IS_SMALLER,          __is_smaller) \
  X(IS_SMALLER_OR_EQUAL, __is_smaller_or_equal) \
  X(SPACESHIP,           __cmp)

#define UNARY_ASSIGN_OPS(X) \
  X(PRE_INC,             __pre_inc) \
  X(POST_INC,            __post_inc) \
  X(PRE_DEC,             __pre_dec) \
  X(POST_DEC,            __post_dec)

// NOTE: Assign operators are now a pair of one of the following assignment ops with the corresponding "normal" op, which is stored in opline->extended_value
// https://github.com/php/php-src/commit/48ca5a1e176c5301fedd1bc4f661969d6f9a49eb
// ZEND_ASSIGN_OP: For $var += $value
// ZEND_ASSIGN_DIM_OP: For $array[$key] += $value
// ZEND_ASSIGN_OBJ_OP: For $obj->prop += $value
// ZEND_ASSIGN_STATIC_PROP_OP: For self::$prop += $value
// See https://github.com/php/php-src/blob/7be3649016d2c2a67d592d0e738433898d7ce81e/Zend/zend_compile.c#L3645-L3646

// It seems like these opcodes don't get passed to op_handler,
// so these methods don't actually work.
// I'm keeping them here to keep the structure of the macros
#define BINARY_ASSIGN_OPS(X) \
  X(ASSIGN_OP,              __assign) \
  X(ASSIGN_DIM_OP,          __assign_dim) \
  X(ASSIGN_OBJ_OP,          __assign_obj) \
  X(ASSIGN_STATIC_PROP_OP,  __assign)

#define COMBINED_BINARY_ASSIGN_OPS(X) \
  X(ADD,          __assign_add) \
  X(SUB,          __assign_sub) \
  X(MUL,          __assign_mul) \
  X(DIV,          __assign_div) \
  X(MOD,          __assign_mod) \
  X(POW,          __assign_pow) \
  X(SL,           __assign_sl) \
  X(SR,           __assign_sr) \
  X(CONCAT,       __assign_concat) \
  X(BW_OR,        __assign_bw_or) \
  X(BW_AND,       __assign_bw_and) \
  X(BW_XOR,       __assign_bw_xor)

#define ALL_OPS(X) \
  UNARY_OPS(X) \
  BINARY_OPS(X) \
  UNARY_ASSIGN_OPS(X) \
  BINARY_ASSIGN_OPS(X)

/* greater/greater-equal are encoded as smaller/smaller-equal
 * so they require special handling
 */
#define GREATER_OPS(X) \
  X(IS_SMALLER,          __is_greater) \
  X(IS_SMALLER_OR_EQUAL, __is_greater_or_equal)

#define X(op, meth) static zend_string *s_##meth;
ALL_OPS(X)
GREATER_OPS(X)
COMBINED_BINARY_ASSIGN_OPS(X)
#undef X

/* {{{ operator_method_name */
static inline zend_string* operator_method_name(const zend_op *opline) {
  switch (opline->opcode) {
#define X(op, meth) case ZEND_##op: return s_##meth;
UNARY_OPS(X)
BINARY_OPS(X)
UNARY_ASSIGN_OPS(X)
#undef X

#define Y(op, meth) case ZEND_##op: return s_##meth;
#define X(op, meth) case ZEND_##op: switch (opline->extended_value) {\
    COMBINED_BINARY_ASSIGN_OPS(Y) \
    default: \
      ZEND_ASSERT(0); \
      return NULL; \
  }

BINARY_ASSIGN_OPS(X)
#undef X
#undef Y
    default:
		ZEND_ASSERT(0);
		return NULL;
  }
}
/* }}} */

/* {{{ operator_get_method */
static zend_bool operator_get_method(zend_string *method, zval *obj,
                                     zend_fcall_info *fci,
                                     zend_fcall_info_cache *fcc) {
  memset(fci, 0, sizeof(zend_fcall_info));
  fci->size = sizeof(zend_fcall_info);
  fci->object = Z_OBJ_P(obj);
  ZVAL_STR(&(fci->function_name), method);

  if (!zend_is_callable_ex(&(fci->function_name), fci->object,
                           IS_CALLABLE_SUPPRESS_DEPRECATIONS,
                           NULL, fcc, NULL)) {
    return 0;
  }
  /* Disallow dispatch via __call */
  if (fcc->function_handler == Z_OBJCE_P(obj)->__call) { return 0; }
  if (fcc->function_handler->type == ZEND_USER_FUNCTION) {
    zend_op_array *oparray = (zend_op_array*)(fcc->function_handler);
    if (oparray->fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) {
      return 0;
    }
  }

  return 1;
}
/* }}} */

/* {{{ operator_is_greater_op */
static inline zend_bool operator_is_greater_op(const zend_op *opline, zend_string **pmethod) {
  if (opline->extended_value == 1) {
    switch (opline->opcode) {
#define X(op, meth) \
      case ZEND_##op: *pmethod = s_##meth; return 1;
GREATER_OPS(X)
#undef X
    }
  }
  return 0;
}
/* }}} */

/* {{{ op_handler */
static int op_handler(zend_execute_data *execute_data) {
  USE_OPLINE
  zval *free_op1, *free_op2 = NULL;
  zval *op1, *op2 = NULL;
  zend_fcall_info fci;
  zend_fcall_info_cache fcc;
  zend_string *method = operator_method_name(opline);

  if (opline->op1_type == IS_UNUSED) {
    /* Assign op */
    op1 = EX_VAR(opline->result.var);
  } else {
    op1 = GET_OP1_ZVAL_PTR_UNDEF(BP_VAR_R);
  }
  ZVAL_DEREF(op1);

  switch (opline->opcode) {
#define X(op, meth) \
    case ZEND_##op:
BINARY_OPS(X)
BINARY_ASSIGN_OPS(X)
#undef X
      op2 = GET_OP2_ZVAL_PTR_UNDEF(BP_VAR_R);
  }

  if (operator_is_greater_op(opline, &method)) {
    zval *tmp = op1;
    zval *free_tmp = free_op1;
    op1 = op2; op2 = tmp;
    free_op1 = free_op2; free_op2 = free_tmp;
  }

  if ((Z_TYPE_P(op1) != IS_OBJECT) ||
      !operator_get_method(method, op1, &fci, &fcc)) {
    /* Not an overloaded call */
    return ZEND_USER_OPCODE_DISPATCH;
  }

  fci.retval = EX_VAR(opline->result.var);
  fci.params = op2;
  fci.param_count = op2 ? 1 : 0;
  if (FAILURE == zend_call_function(&fci, &fcc)) {
    php_error(E_WARNING, "Failed calling %s::%s()", ZSTR_VAL(Z_OBJCE_P(op1)->name), Z_STRVAL(fci.function_name));
    ZVAL_NULL(fci.retval);
  }

  FREE_OP2
  FREE_OP1
  EX(opline) = opline + 1;
  return ZEND_USER_OPCODE_CONTINUE;
}
/* }}} */

/* {{{ MINIT */
static PHP_MINIT_FUNCTION(operator) {
#define X(op, meth) \
  s_##meth = zend_string_init(#meth, strlen(#meth), 1);
GREATER_OPS(X)
COMBINED_BINARY_ASSIGN_OPS(X)
ALL_OPS(X)
#undef X

#define X(op, meth) \
  zend_set_user_opcode_handler(ZEND_##op, op_handler);
ALL_OPS(X)
#undef X

  return SUCCESS;
}
/* }}} */

/* {{{ operator_module_entry
 */
static zend_module_entry operator_module_entry = {
  STANDARD_MODULE_HEADER,
  "operator",
  NULL, /* functions */
  PHP_MINIT(operator),
  NULL, /* MSHUTDOWN */
  NULL, /* RINIT */
  NULL, /* RSHUTDOWN */
  NULL, /* MINFO */
  "7.2.0",
  STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_OPERATOR
ZEND_GET_MODULE(operator)
#endif
