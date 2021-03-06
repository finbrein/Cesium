#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "environment.h"
#include "exception.h"
#include "environment.h"
#include "backend.h"
#include "unify.h"
#include "ast.h"
#include "gc.h"

#include <llvm-c/Core.h>  
#include <llvm-c/Analysis.h>  
#include <llvm-c/ExecutionEngine.h>  
#include <llvm-c/Target.h>  
#include <llvm-c/Transforms/Scalar.h> 

#ifndef CS_MALLOC_NAME /* make it easy to turn off GC */
#define CS_MALLOC_NAME "GC_malloc"
#define CS_MALLOC_NAME2 "GC_malloc_atomic"
#endif

/* 
   Tell LLVM about some external library functions so we can call them 
   and about some constants we want to use from jit'd code
*/
void llvm_functions(jit_t * jit)
{
   LLVMTypeRef args[2];
   LLVMTypeRef fntype; 
   LLVMTypeRef ret;
   LLVMValueRef fn;

   /* patch in the printf function */
   args[0] = LLVMPointerType(LLVMInt8Type(), 0);
   ret = LLVMWordType();
   fntype = LLVMFunctionType(ret, args, 1, 1);
   fn = LLVMAddFunction(jit->module, "printf", fntype);

   /* patch in the exit function */
   args[0] = LLVMWordType();
   ret = LLVMVoidType();
   fntype = LLVMFunctionType(ret, args, 1, 0);
   fn = LLVMAddFunction(jit->module, "exit", fntype);

   /* patch in the GC_malloc function */
   args[0] = LLVMWordType();
   ret = LLVMPointerType(LLVMInt8Type(), 0);
   fntype = LLVMFunctionType(ret, args, 1, 0);
   fn = LLVMAddFunction(jit->module, CS_MALLOC_NAME, fntype);
   LLVMAddFunctionAttr(fn, LLVMNoAliasAttribute);

   /* patch in the GC_malloc_atomic function */
   args[0] = LLVMWordType();
   ret = LLVMPointerType(LLVMInt8Type(), 0);
   fntype = LLVMFunctionType(ret, args, 1, 0);
   fn = LLVMAddFunction(jit->module, CS_MALLOC_NAME2, fntype);
   LLVMAddFunctionAttr(fn, LLVMNoAliasAttribute);
}

/* count parameters as represented by %'s in format string */
int count_params(const char * fmt)
{
   int len = strlen(fmt);
   int i, count = 0;

   for (i = 0; i < len - 1; i++)
      if (fmt[i] == '%')
         if (fmt[i + 1] == '%')
               i++;
         else
            count++;

   return count;
}

/* 
   When an expression is evaluated it is a pain to pass it back out of
   jit'd code and print the value returned by the expression. So we 
   print it on the jit side. So this is our runtime printf for 
   printing LLVMValRef's from jit'd code.
*/
void llvm_printf(jit_t * jit, const char * fmt, ...)
{
   int i, count = count_params(fmt);
   va_list ap;
   
   /* get printf function */
   LLVMValueRef fn = LLVMGetNamedFunction(jit->module, "printf");
   LLVMSetFunctionCallConv(fn, LLVMCCallConv);
   
   /* Add a global variable for format string */
   LLVMValueRef str = LLVMConstString(fmt, strlen(fmt), 0);
   jit->fmt_str = LLVMAddGlobal(jit->module, LLVMTypeOf(str), "fmt");
   LLVMSetInitializer(jit->fmt_str, str);
   LLVMSetGlobalConstant(jit->fmt_str, 1);
   LLVMSetLinkage(jit->fmt_str, LLVMInternalLinkage);
   
   /* build variadic parameter list for printf */
   LLVMValueRef indices[2] = { LLVMConstInt(LLVMWordType(), 0, 0), LLVMConstInt(LLVMWordType(), 0, 0) };
   LLVMValueRef GEP = LLVMBuildGEP(jit->builder, jit->fmt_str, indices, 2, "str");
   LLVMValueRef args[count + 1];
   args[0] = GEP;
   
   va_start(ap, fmt);

   for (i = 0; i < count; i++)
      args[i + 1] = va_arg(ap, LLVMValueRef);

   va_end(ap);

   /* build call to printf */
   LLVMValueRef call_printf = LLVMBuildCall(jit->builder, fn, args, count + 1, "printf");
   LLVMSetTailCall(call_printf, 1);
   LLVMAddInstrAttribute(call_printf, 0, LLVMNoUnwindAttribute);
   LLVMAddInstrAttribute(call_printf, 1, LLVMNoAliasAttribute);
}

/*
   Printing booleans requires special attention. We want to print
   the strings "true" and "false". To do this from the jit side we
   need this function.
*/
void llvm_printbool(jit_t * jit, LLVMValueRef obj)
{
    /* jit an if statement which checks for true/false */
    LLVMBasicBlockRef i;
    LLVMBasicBlockRef b1;
    LLVMBasicBlockRef b2;
    LLVMBasicBlockRef e;

    i = LLVMAppendBasicBlock(jit->function, "if");
    b1 = LLVMAppendBasicBlock(jit->function, "ifbody");
    b2 = LLVMAppendBasicBlock(jit->function, "ebody");
    e = LLVMAppendBasicBlock(jit->function, "ifend");
    LLVMBuildBr(jit->builder, i);
    LLVMPositionBuilderAtEnd(jit->builder, i); 

    LLVMBuildCondBr(jit->builder, obj, b1, b2);
    LLVMPositionBuilderAtEnd(jit->builder, b1);

    /* print "true" */
    llvm_printf(jit, "%s", jit->true_str);

    LLVMBuildBr(jit->builder, e);
    LLVMPositionBuilderAtEnd(jit->builder, b2);  

    /* print "false" */
    llvm_printf(jit, "%s", jit->false_str);

    LLVMBuildBr(jit->builder, e);
    LLVMPositionBuilderAtEnd(jit->builder, e);
}

/* 
    Jits a print of a tuple
*/
void print_tuple(jit_t * jit, type_t * type, LLVMValueRef obj)
{
    int count = type->arity;
    int i;

    llvm_printf(jit, "(");
    
    /* print comma separated list of params */
    for (i = 0; i < count - 1; i++)
    {
        LLVMValueRef index[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
        LLVMValueRef p = LLVMBuildInBoundsGEP(jit->builder, obj, index, 2, "tuple");
        p = LLVMBuildLoad(jit->builder, p, "entry");
        print_obj(jit, type->param[i], p);
        llvm_printf(jit, ", ");
    }
    
    /* print final param */
    LLVMValueRef index[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
    LLVMValueRef p = LLVMBuildInBoundsGEP(jit->builder, obj, index, 2, "tuple");
    p = LLVMBuildLoad(jit->builder, p, "entry");
    print_obj(jit, type->param[i], p);
        
    llvm_printf(jit, ")");
}

/*
   This jits a printf for various cesium types. We use it to print
   the result of expressions that are evaluated, before returning from
   a jit'd expression.
*/
void print_obj(jit_t * jit, type_t * type, LLVMValueRef obj)
{
   switch (type->typ)
   {
      case INT:
         llvm_printf(jit, "%ld", obj);
         break;
      case CHAR:
         llvm_printf(jit, "%c", obj);
         break;
      case DOUBLE:
         llvm_printf(jit, "%.5g", obj);
         break;
      case STRING:
         llvm_printf(jit, "\"%s\"", obj);
         break;
      case BOOL:
         llvm_printbool(jit, obj);
         break;
      case FN:
      case LAMBDA:
      case DATATYPE:
      case ARRAY:
      case NIL:
         llvm_printf(jit, "%s", jit->nil_str);
         break;
      case TUPLE:
         print_tuple(jit, type, obj);
         break;
   }
}

/*
   Initialise the LLVM JIT
*/
jit_t * llvm_init(void)
{
    char * error = NULL;
    
    /* create jit struct */
    jit_t * jit = (jit_t *) GC_MALLOC(sizeof(jit_t));

    /* Jit setup */
    LLVMLinkInJIT();
    LLVMInitializeNativeTarget();

    /* Create module */
    jit->module = LLVMModuleCreateWithName("cesium");

    /* Create JIT engine */
    if (LLVMCreateJITCompilerForModule(&(jit->engine), jit->module, 2, &error) != 0) 
    {   
       fprintf(stderr, "%s\n", error);  
       LLVMDisposeMessage(error);  
       abort();  
    } 
   
    /* Create optimisation pass pipeline */
    jit->pass = LLVMCreateFunctionPassManagerForModule(jit->module);  
    LLVMAddTargetData(LLVMGetExecutionEngineTargetData(jit->engine), jit->pass);  
    LLVMAddAggressiveDCEPass(jit->pass); /* */
    LLVMAddDeadStoreEliminationPass(jit->pass); 
    LLVMAddIndVarSimplifyPass(jit->pass); 
    LLVMAddJumpThreadingPass(jit->pass); 
    LLVMAddLICMPass(jit->pass); 
    LLVMAddLoopDeletionPass(jit->pass); 
    LLVMAddLoopRotatePass(jit->pass); 
    LLVMAddLoopUnrollPass(jit->pass); 
    LLVMAddLoopUnswitchPass(jit->pass);
    LLVMAddMemCpyOptPass(jit->pass); 
    LLVMAddReassociatePass(jit->pass); 
    LLVMAddSCCPPass(jit->pass); 
    LLVMAddScalarReplAggregatesPass(jit->pass); 
    LLVMAddSimplifyLibCallsPass(jit->pass);
    LLVMAddTailCallEliminationPass(jit->pass); 
    LLVMAddDemoteMemoryToRegisterPass(jit->pass); /* */ 
    LLVMAddConstantPropagationPass(jit->pass);  
    LLVMAddInstructionCombiningPass(jit->pass);  
    LLVMAddPromoteMemoryToRegisterPass(jit->pass);  
    LLVMAddGVNPass(jit->pass);  
    LLVMAddCFGSimplificationPass(jit->pass);
    
    /* link in external functions callable from jit'd code */
    llvm_functions(jit);

    /* initialise some strings */
    START_EXEC;
    jit->fmt_str = NULL;
    jit->true_str = LLVMBuildGlobalStringPtr(jit->builder, "true", "true");
    jit->false_str = LLVMBuildGlobalStringPtr(jit->builder, "false", "false");
    jit->nil_str = LLVMBuildGlobalStringPtr(jit->builder, "nil", "nil");
    END_EXEC;

    return jit;
}

/*
   If something goes wrong after partially jit'ing something we need
   to clean up.
*/
void llvm_reset(jit_t * jit)
{
    LLVMDeleteFunction(jit->function);
    LLVMDisposeBuilder(jit->builder);
    jit->function = NULL;
    jit->builder = NULL;
    jit->breakto = NULL;
}

/*
   Clean up LLVM on exit from Cesium
*/
void llvm_cleanup(jit_t * jit)
{
    /* Clean up */
    LLVMDisposePassManager(jit->pass);  
    LLVMDisposeExecutionEngine(jit->engine); 
    jit->pass = NULL;
    jit->engine = NULL;
    jit->module = NULL;
    jit->fmt_str = NULL;
    jit->nil_str = NULL;
    jit->true_str = NULL;
    jit->false_str = NULL;
}

int is_atomic(type_t * type)
{
   typ_t typ = type->typ;
   return (typ != ARRAY && typ != TUPLE && typ != DATATYPE && typ != FN && typ != LAMBDA);
}

/* 
   Jit a call to GC_malloc
*/
LLVMValueRef LLVMBuildGCMalloc(jit_t * jit, LLVMTypeRef type, const char * name, int atomic)
{
    LLVMValueRef fn;
    if (atomic)
        fn = LLVMGetNamedFunction(jit->module, CS_MALLOC_NAME2);
    else
        fn = LLVMGetNamedFunction(jit->module, CS_MALLOC_NAME);
    LLVMValueRef arg[1] = { LLVMSizeOf(type) };
    LLVMValueRef gcmalloc = LLVMBuildCall(jit->builder, fn, arg, 1, "malloc");
    return LLVMBuildPointerCast(jit->builder, gcmalloc, LLVMPointerType(type, 0), name);
}

/* 
   Jit a call to GC_malloc to create an array
*/
LLVMValueRef LLVMBuildGCArrayMalloc(jit_t * jit, LLVMTypeRef type, LLVMValueRef num, const char * name, int atomic)
{
    LLVMValueRef fn;
    if (atomic)
        fn = LLVMGetNamedFunction(jit->module, CS_MALLOC_NAME2);
    else
        fn = LLVMGetNamedFunction(jit->module, CS_MALLOC_NAME);
    LLVMValueRef size = LLVMSizeOf(type);
    LLVMValueRef arg[1] = { LLVMBuildMul(jit->builder, num, size, "arr_size") };
    LLVMValueRef gcmalloc = LLVMBuildCall(jit->builder, fn, arg, 1, "malloc");
    return LLVMBuildPointerCast(jit->builder, gcmalloc, LLVMPointerType(type, 0), name);
}

/* Build llvm lambda fn type from ordinary function type  */
LLVMTypeRef lambda_fn_type(jit_t * jit, type_t * type)
{
    int params = type->arity;
    int i;

    /* get parameter types, one extra for the environment struct */
    LLVMTypeRef * args = (LLVMTypeRef *) GC_MALLOC((params + 1)*sizeof(LLVMTypeRef));
    for (i = 0; i < params; i++)
        args[i] = type_to_llvm(jit, type->param[i]); 

    /* we'll cast the environment struct to a pointer */
    args[params] = LLVMPointerType(LLVMInt8Type(), 0);

    /* get return type */
    LLVMTypeRef ret = type_to_llvm(jit, type->ret); 
    
    /* make LLVM function type */
    return LLVMFunctionType(ret, args, params + 1, 0);
}

/* 
    Make an llvm lambda struct type from an ordinary function type
*/
LLVMTypeRef lambda_type(jit_t * jit, type_t * type)
{
    LLVMTypeRef fn_ty = lambda_fn_type(jit, type);
    
    /* get lambda struct element types */
    LLVMTypeRef * str_ty = (LLVMTypeRef *) GC_MALLOC(2*sizeof(LLVMTypeRef));
    str_ty[0] = LLVMPointerType(fn_ty, 0);
    str_ty[1] = LLVMPointerType(LLVMInt8Type(), 0);

    return LLVMStructType(str_ty, 2, 1);
}

/* Build llvm struct type from ordinary tuple type  */
LLVMTypeRef tup_type(jit_t * jit, type_t * type)
{
    int params = type->arity;
    int i;

    /* get parameter types */
    LLVMTypeRef * args = (LLVMTypeRef *) GC_MALLOC(params*sizeof(LLVMTypeRef));
    for (i = 0; i < params; i++)
        args[i] = type_to_llvm(jit, type->param[i]); 

    /* make LLVM struct type */
    return LLVMStructType(args, params, 1);
}

/* Build llvm struct type for array type  */
LLVMTypeRef arr_type(jit_t * jit, type_t * type)
{
    int i;

    /* get parameter types */
    LLVMTypeRef * args = (LLVMTypeRef *) GC_MALLOC(2*sizeof(LLVMTypeRef));
    args[0] = LLVMPointerType(type_to_llvm(jit, type->ret), 0); 
    args[1] = LLVMWordType();

    /* make LLVM struct type */
    return LLVMStructType(args, 2, 1);
}

/* Convert a type to an LLVMTypeRef */
LLVMTypeRef type_to_llvm(jit_t * jit, type_t * type)
{
    int i;
    
    if (type == t_double)
         return LLVMDoubleType();
    else if (type == t_int)
         return LLVMWordType();
    else if (type == t_bool)
         return LLVMInt1Type();
    else if (type == t_string)
         return LLVMPointerType(LLVMInt8Type(), 0);
    else if (type == t_char)
         return LLVMInt8Type();
    else if (type == t_nil)
         return LLVMVoidType();
    else if (type->typ == FN)
    {
        int params = type->arity;
        
        /* get parameter types */
        LLVMTypeRef * args = (LLVMTypeRef *) GC_MALLOC(params*sizeof(LLVMTypeRef));
        for (i = 0; i < params; i++)
            args[i] = type_to_llvm(jit, type->param[i]); 

        /* get return type */
        LLVMTypeRef ret = type_to_llvm(jit, type->ret); 
    
        /* make LLVM function type */
        return LLVMPointerType(LLVMFunctionType(ret, args, params, 0), 0);
    } else if (type->typ == LAMBDA)
        return LLVMPointerType(lambda_type(jit, type), 0);
    else if (type->typ == TUPLE || type->typ == DATATYPE)
        return LLVMPointerType(tup_type(jit, type), 0);
    else if (type->typ == ARRAY)
        return LLVMPointerType(arr_type(jit, type), 0);
    else if (type->typ == TYPEVAR)
        jit_exception(jit, "Unable to infer types\n");
    else
        jit_exception(jit, "Internal error: unknown type in type_to_llvm\n");
}

/*
   Jit an int literal
*/
int exec_int(jit_t * jit, ast_t * ast)
{
    long num = atol(ast->sym->name);
    
    ast->val = LLVMConstInt(LLVMWordType(), num, 0);

    return 0;
}

/*
   Jit a double literal
*/
int exec_double(jit_t * jit, ast_t * ast)
{
    double num = atof(ast->sym->name);
    
    ast->val = LLVMConstReal(LLVMDoubleType(), num);

    return 0;
}

/*
   Jit a bool
*/
int exec_bool(jit_t * jit, ast_t * ast)
{
    if (strcmp(ast->sym->name, "true") == 0)
        ast->val = LLVMConstInt(LLVMInt1Type(), 1, 0);
    else
        ast->val = LLVMConstInt(LLVMInt1Type(), 0, 0);

    return 0;
}

/*
   Jit a string literali, being careful to replace special 
   characters with their ascii equivalent
*/
int exec_string(jit_t * jit, ast_t * ast)
{
    char * name = ast->sym->name;
         
    if (ast->sym->val == NULL)
    {
        int length = strlen(name) - 2;
        int i, j, bs = 0;
            
        for (i = 0; i < length; i++)
        {
            if (name[i + 1] == '\\')
            { 
                bs++;
                i++;
            }
        }

        char * str = (char *) GC_MALLOC(length + 1 - bs);
            
        for (i = 0, j = 0; i < length; i++, j++)
        {
            if (name[i + 1] == '\\')
            {
                switch (name[i + 2])
                {
                case '0':
                    str[j] = '\0';
                    break;
                case '\\':
                    str[j] = '\\';
                    break;
                case 'n':
                    str[j] = '\n';
                    break;
                case 'r':
                    str[j] = '\r';
                    break;
                case 't':
                    str[j] = '\t';
                    break;
                default:
                    str[j] = name[i + 2];
                }
                i++;
            } else
                str[j] = name[i + 1];
        }

        length = length - bs;
        str[length] = '\0';
            
        ast->sym->val = LLVMBuildGlobalStringPtr(jit->builder, str, "string");
    }

    ast->val = ast->sym->val;

    return 0;
}

/*
   We have a number of unary ops we want to jit and they
   all look the same, so define macros for them.
*/
#define exec_unary(__name, __fop, __iop, __str)         \
__name(jit_t * jit, ast_t * ast)                        \
{                                                       \
    ast_t * expr1 = ast->child;                         \
                                                        \
    exec_ast(jit, expr1);                               \
                                                        \
    LLVMValueRef v1 = expr1->val;                       \
                                                        \
    if (expr1->type == t_double)                        \
       ast->val = __fop(jit->builder, v1, __str);       \
    else                                                \
       ast->val = __iop(jit->builder, v1, __str);       \
                                                        \
    ast->type = expr1->type;                            \
                                                        \
    return 0;                                           \
}

#define exec_unary1(__name, __iop, __str)               \
__name(jit_t * jit, ast_t * ast)                        \
{                                                       \
    ast_t * expr1 = ast->child;                         \
                                                        \
    exec_ast(jit, expr1);                               \
                                                        \
    LLVMValueRef v1 = expr1->val;                       \
                                                        \
    ast->val = __iop(jit->builder, v1, __str);          \
                                                        \
    ast->type = expr1->type;                            \
                                                        \
    return 0;                                           \
}

#define exec_unary_pre(__name, __fop, __iop, __c1, __c2, __str) \
__name(jit_t * jit, ast_t * ast)                                \
{                                                               \
    ast_t * expr1 = ast->child;                                 \
                                                                \
    exec_place(jit, expr1);                                     \
                                                                \
    LLVMValueRef v1 = LLVMBuildLoad(jit->builder,               \
                      expr1->val, expr1->sym->name);            \
                                                                \
    if (expr1->type == t_double)                                \
       ast->val = __fop(jit->builder, v1,                       \
          LLVMConstReal(LLVMDoubleType(), __c1), __str);        \
    else                                                        \
       ast->val = __iop(jit->builder, v1,                       \
          LLVMConstInt(LLVMWordType(), __c2, 0), __str);        \
    LLVMBuildStore(jit->builder, ast->val, expr1->val);         \
                                                                \
    ast->type = expr1->type;                                    \
                                                                \
    return 0;                                                   \
}

#define exec_unary_post(__name, __fop, __iop, __c1, __c2, __str) \
__name(jit_t * jit, ast_t * ast)                                 \
{                                                                \
    ast_t * expr1 = ast->child;                                  \
                                                                 \
    exec_place(jit, expr1);                                      \
                                                                 \
    LLVMValueRef v1 = LLVMBuildLoad(jit->builder,                \
                      expr1->val, expr1->sym->name);             \
                                                                 \
    if (expr1->type == t_double)                                 \
       ast->val = __fop(jit->builder, v1,                        \
          LLVMConstReal(LLVMDoubleType(), __c1), __str);         \
    else                                                         \
       ast->val = __iop(jit->builder, v1,                        \
          LLVMConstInt(LLVMWordType(), __c2, 0), __str);         \
    LLVMBuildStore(jit->builder, ast->val, expr1->val);          \
                                                                 \
    ast->type = expr1->type;                                     \
    ast->val = v1;                                               \
                                                                 \
    return 0;                                                    \
}

/* Jit !, ~, -, ... unary ops */
int exec_unary(exec_unminus, LLVMBuildFNeg, LLVMBuildNeg, "unary-minus")

int exec_unary1(exec_lognot, LLVMBuildNot, "log-not")

int exec_unary1(exec_bitnot, LLVMBuildNot, "bit-not")

int exec_unary_pre(exec_preinc, LLVMBuildFAdd, LLVMBuildAdd, 1.0, 1, "pre-inc")

int exec_unary_pre(exec_predec, LLVMBuildFSub, LLVMBuildSub, 1.0, 1, "pre-dec")

int exec_unary_post(exec_postinc, LLVMBuildFAdd, LLVMBuildAdd, 1.0, 1, "post-inc")

int exec_unary_post(exec_postdec, LLVMBuildFSub, LLVMBuildSub, 1.0, 1, "post-dec")

/*
   We have a number of binary ops we want to jit and they
   all look the same, so define macros for them.
*/
#define exec_binary(__name, __fop, __iop, __str)        \
__name(jit_t * jit, ast_t * ast)                        \
{                                                       \
    ast_t * expr1 = ast->child;                         \
    ast_t * expr2 = expr1->next;                        \
                                                        \
    exec_ast(jit, expr1);                               \
    exec_ast(jit, expr2);                               \
                                                        \
    LLVMValueRef v1 = expr1->val, v2 = expr2->val;      \
                                                        \
    if (expr1->type == t_double)                        \
       ast->val = __fop(jit->builder, v1, v2, __str);   \
    else                                                \
       ast->val = __iop(jit->builder, v1, v2, __str);   \
                                                        \
    ast->type = expr1->type;                            \
                                                        \
    return 0;                                           \
}

#define exec_binary_rel(__name, __fop, __frel, __iop, __irel, __str)  \
__name(jit_t * jit, ast_t * ast)                                      \
{                                                                     \
    ast_t * expr1 = ast->child;                                       \
    ast_t * expr2 = expr1->next;                                      \
                                                                      \
    exec_ast(jit, expr1);                                             \
    exec_ast(jit, expr2);                                             \
                                                                      \
    LLVMValueRef v1 = expr1->val, v2 = expr2->val;                    \
                                                                      \
    if (expr1->type == t_double)                                      \
       ast->val = __fop(jit->builder, __frel, v1, v2, __str);         \
    else                                                              \
       ast->val = __iop(jit->builder, __irel, v1, v2, __str);         \
                                                                      \
    return 0;                                                         \
}

#define exec_binary1(__name, __iop, __str)              \
__name(jit_t * jit, ast_t * ast)                        \
{                                                       \
    ast_t * expr1 = ast->child;                         \
    ast_t * expr2 = expr1->next;                        \
                                                        \
    exec_ast(jit, expr1);                               \
    exec_ast(jit, expr2);                               \
                                                        \
    LLVMValueRef v1 = expr1->val, v2 = expr2->val;      \
                                                        \
    ast->val = __iop(jit->builder, v1, v2, __str);      \
                                                        \
    ast->type = expr1->type;                            \
                                                        \
    return 0;                                           \
}

#define exec_binary_pre(__name, __fop, __iop, __str)          \
__name(jit_t * jit, ast_t * ast)                              \
{                                                             \
    ast_t * expr1 = ast->child;                               \
    ast_t * expr2 = ast->child->next;                         \
                                                              \
    exec_place(jit, expr1);                                   \
    exec_ast(jit, expr2);                                     \
                                                              \
    LLVMValueRef v1 = LLVMBuildLoad(jit->builder,             \
                      expr1->val, expr1->sym->name);          \
                                                              \
    if (expr1->type == t_double)                              \
       ast->val = __fop(jit->builder, v1, expr2->val, __str); \
    else                                                      \
       ast->val = __iop(jit->builder, v1, expr2->val, __str); \
    LLVMBuildStore(jit->builder, ast->val, expr1->val);       \
                                                              \
    ast->type = expr1->type;                                  \
                                                              \
    return 0;                                                 \
}

#define exec_binary_pre1(__name, __iop, __str)                \
__name(jit_t * jit, ast_t * ast)                              \
{                                                             \
    ast_t * expr1 = ast->child;                               \
    ast_t * expr2 = ast->child->next;                         \
                                                              \
    exec_place(jit, expr1);                                   \
    exec_ast(jit, expr2);                                     \
                                                              \
    LLVMValueRef v1 = LLVMBuildLoad(jit->builder,             \
                      expr1->val, expr1->sym->name);          \
                                                              \
    ast->val = __iop(jit->builder, v1, expr2->val, __str);    \
    LLVMBuildStore(jit->builder, ast->val, expr1->val);       \
                                                              \
    ast->type = expr1->type;                                  \
                                                              \
    return 0;                                                 \
}

/* Jit add, sub, .... ops */
int exec_binary(exec_plus, LLVMBuildFAdd, LLVMBuildAdd, "add")

int exec_binary(exec_minus, LLVMBuildFSub, LLVMBuildSub, "sub")

int exec_binary(exec_times, LLVMBuildFMul, LLVMBuildMul, "times")

int exec_binary(exec_div, LLVMBuildFDiv, LLVMBuildSDiv, "div")

int exec_binary(exec_mod, LLVMBuildFRem, LLVMBuildSRem, "mod")

int exec_binary1(exec_lsh, LLVMBuildShl, "lsh")

int exec_binary1(exec_rsh, LLVMBuildAShr, "rsh")

int exec_binary1(exec_bitor, LLVMBuildOr, "bitor")

int exec_binary1(exec_bitand, LLVMBuildAnd, "bitand")

int exec_binary1(exec_bitxor, LLVMBuildXor, "bitxor")

int exec_binary1(exec_logand, LLVMBuildAnd, "logand")

int exec_binary1(exec_logor, LLVMBuildOr, "logor")

int exec_binary_rel(exec_le, LLVMBuildFCmp, LLVMRealOLE, LLVMBuildICmp, LLVMIntSLE, "le")

int exec_binary_rel(exec_ge, LLVMBuildFCmp, LLVMRealOGE, LLVMBuildICmp, LLVMIntSGE, "ge")

int exec_binary_rel(exec_lt, LLVMBuildFCmp, LLVMRealOLT, LLVMBuildICmp, LLVMIntSLT, "lt")

int exec_binary_rel(exec_gt, LLVMBuildFCmp, LLVMRealOGT, LLVMBuildICmp, LLVMIntSGT, "gt")

int exec_binary_rel(exec_eq, LLVMBuildFCmp, LLVMRealOEQ, LLVMBuildICmp, LLVMIntEQ, "eq")

int exec_binary_rel(exec_ne, LLVMBuildFCmp, LLVMRealONE, LLVMBuildICmp, LLVMIntNE, "ne")

int exec_binary_pre(exec_pluseq, LLVMBuildFAdd, LLVMBuildAdd, "pluseq")

int exec_binary_pre(exec_minuseq, LLVMBuildFSub, LLVMBuildSub, "minuseq")

int exec_binary_pre(exec_timeseq, LLVMBuildFMul, LLVMBuildMul, "timeseq")

int exec_binary_pre(exec_diveq, LLVMBuildFDiv, LLVMBuildSDiv, "diveq")

int exec_binary_pre(exec_modeq, LLVMBuildFRem, LLVMBuildSRem, "modeq")

int exec_binary_pre1(exec_andeq, LLVMBuildAnd, "andeq")

int exec_binary_pre1(exec_oreq, LLVMBuildOr, "oreq")

int exec_binary_pre1(exec_xoreq, LLVMBuildXor, "xoreq")

int exec_binary_pre1(exec_lsheq, LLVMBuildShl, "lsheq")

int exec_binary_pre1(exec_rsheq, LLVMBuildAShr, "rsheq")

/*
   Given a function make a lambda function (i.e. function with same
   params and an exta one for an env) which calls it
*/
LLVMValueRef make_fn_lambda(jit_t * jit,  
                             LLVMValueRef fn, LLVMTypeRef fn_type)
{
    /* make llvm function object */
    LLVMValueRef fn_res = LLVMAddFunction(jit->module, "lambda", fn_type);
    
    /* jit setup */
    LLVMBuilderRef build_res = LLVMCreateBuilder();

    /* first basic block */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn_res, "entry");
    LLVMPositionBuilderAtEnd(build_res, entry);
       
    /* make space for arguments */
    int count = LLVMCountParams(fn);
    LLVMValueRef * args = (LLVMValueRef *) GC_MALLOC(count*sizeof(LLVMValueRef));
    
    /* load arguments */
    int i = 0;
    for (i = 0; i < count; i++)
        args[i] = LLVMGetParam(fn_res, i);

    /* call function and return value */
    LLVMValueRef ret = LLVMBuildCall(build_res, fn, args, count, "");
    LLVMBuildRet(build_res, ret);
    
    /* run the pass manager on the jit'd function */
    LLVMRunFunctionPassManager(jit->pass, fn_res); 
    
    /* clean up */
    LLVMDisposeBuilder(build_res);  
    
    return fn_res;
}

/*
   Load value of identifier
*/
int exec_ident(jit_t * jit, ast_t * ast)
{
    if (ast->tag == AST_SLOT) /* deal specially with slots */
        return exec_slot(jit, ast);

    bind_t * bind = ast->bind;

    if (bind->val != NULL) /* we've already got a value and thus a type */
         ast->type = bind->type;
    else /* substitute the inferred type and update the binding type */
    {
        subst_type(&ast->type);
        bind->type = ast->type;
    }
    
    if (ast->type->typ == ARRAY && ast->type->arity != 0)
    {
        LLVMTypeRef str_ty = arr_type(jit, ast->type);
        LLVMValueRef val = LLVMBuildGCMalloc(jit, str_ty, "tuple_s", 0);

        /* insert length into array struct */
        LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 1, 0) };
        LLVMValueRef entry = LLVMBuildInBoundsGEP(jit->builder, val, indices, 2, "length");
        LLVMBuildStore(jit->builder, LLVMConstInt(LLVMWordType(), ast->type->arity, 0), entry);
    
        /* create array */
        int atomic = is_atomic(ast->type->ret);
        LLVMValueRef arr = LLVMBuildGCArrayMalloc(jit, type_to_llvm(jit, ast->type->ret), LLVMConstInt(LLVMWordType(), (long) ast->type->arity, 0), "array", atomic);

        LLVMValueRef indices2[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 0, 0) };
        entry = LLVMBuildInBoundsGEP(jit->builder, val, indices2, 2, "arr");
        LLVMBuildStore(jit->builder, arr, entry);
        
        ast->type->arity = 0;
        ast->bind->initialised = 0;

        exec_place(jit, ast);
        LLVMBuildStore(jit->builder, val, ast->val);
        ast->bind->initialised = 1;
    }

    /* if the id is a datatype constructor we do nothing */
    if (ast->type->typ == DATATYPE && bind->val == NULL)
        return 0;
    /* if it's not an LLVM function just load the value */
    else if ((ast->type->typ != FN && ast->type->typ != LAMBDA) || bind->ast == NULL)
        ast->val = LLVMBuildLoad(jit->builder, bind->val, bind->sym->name);
    else if (bind->val != NULL) /* if the function has been jit'd, load that */
        ast->val = bind->val;
    else if (ast->type->typ == FN || ast->type->typ == LAMBDA)/* jit the fn, update the binding and load it */
    {
        exec_fndef(jit, bind->ast);
        ast->val = bind->val;
    }
    
    return 0;
}

/*
   Jit a variable declaration
*/
int exec_decl(jit_t * jit, ast_t * ast)
{
    bind_t * bind = ast->bind;
    
    subst_type(&bind->type); /* fill in the type */
    
    if (bind->type->typ != TYPEVAR) /* if we now know what it is */
    {
        LLVMTypeRef type = type_to_llvm(jit, bind->type); /* convert to llvm type */
        
        if (scope_is_global(bind)) /* variable is global */
        {
            bind->val = LLVMAddGlobal(jit->module, type, bind->sym->name);
            LLVMSetInitializer(bind->val, LLVMGetUndef(type));
        } else /* variable is local */
            bind->val = LLVMBuildAlloca(jit->builder, type, bind->sym->name);
    }

    ast->type = bind->type;
    ast->val = bind->val;
   
    return 0;
}

/*
   Jit a slot access
*/
int exec_lslot(jit_t * jit, ast_t * ast)
{
    ast_t * id = ast->child;
    ast_t * p = id->next;
    int i, params = id->type->arity;
    
    if (ast->sym == NULL) /* need some kind of name */
        ast->sym = sym_lookup("none");
    
    exec_ast(jit, id);
    
    for (i = 0; i < params; i++)
    {
        if (id->type->slot[i] == p->sym)
            break;
    }
    
    /* get slot from datatype */
    LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
    ast->val = LLVMBuildInBoundsGEP(jit->builder, id->val, indices, 2, p->sym->name);
    
    ast->type = id->type->param[i];
    
    return 0;
}

/*
   Jit an array location access
*/
int exec_llocation(jit_t * jit, ast_t * ast)
{
    ast_t * id = ast->child;
    ast_t * p = id->next;
    int i;
    
    if (ast->sym == NULL) /* need some kind of name */
        ast->sym = sym_lookup("none");
    
    exec_ast(jit, id);
    exec_ast(jit, p);
    
    /* get array from datatype */
    LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 0, 0) };
    ast->val = LLVMBuildInBoundsGEP(jit->builder, id->val, indices, 2, "arr");
    ast->val = LLVMBuildLoad(jit->builder, ast->val, "array");
    
    /* get location within array */
    LLVMValueRef indices2[1] = { p->val };
    ast->val = LLVMBuildInBoundsGEP(jit->builder, ast->val, indices2, 1, "arr_entry");
    
    ast->type = id->type->ret;
    
    return 0;
}

/*
   Find identifier binding and load the place corresponding to it (lvalue)
*/
int exec_place(jit_t * jit, ast_t * ast)
{
    if (ast->tag == AST_SLOT) /* deal specially with slots */
        return exec_lslot(jit, ast);
    
    if (ast->tag == AST_LOCATION) /* deal specially with array locations */
        return exec_llocation(jit, ast);
    
    if (ast->val == NULL) /* we haven't already loaded it */
    {
        bind_t * bind = ast->bind;

        subst_type(&bind->type); /* fill in the type */
        
        if (bind->type->typ == FN) /* prepare for lambda */
            bind->type = fn_to_lambda_type(bind->type);

        if (bind->val == NULL) /* we still haven't jit'd it */  
            exec_decl(jit, ast);
        
        ast->type = bind->type; /* load particulars from binding */
        ast->val = bind->val;
    }

    return 0;
}

int exec_assign_id(jit_t * jit, ast_t * id, type_t * type, LLVMValueRef val)
{
    int allocated = 0;

    if (id->val != NULL) 
        allocated = 1; /* lambda struct has already been allocated */
    
    /* get place */
    exec_place(jit, id);
    
    /* convert function to lambda */
    if (id->type->typ == LAMBDA && type->typ == FN)
    {
        id->val = make_fn_lambda(jit, val, lambda_fn_type(jit, id->type));
        
        /* malloc space for lambda struct */
        if (!allocated) /* if we didn't already allocate the struct for dest */
        {
            fn_to_lambda(jit, &id->type, &id->val, NULL, NULL);
            
            /* place allocated struct into struct pointer */
            bind_t * bind = find_symbol(id->sym);
            LLVMBuildStore(jit->builder, id->val, bind->val);
        } else
        {
            /* get lambda struct */
            LLVMValueRef str = LLVMBuildLoad(jit->builder, id->val, "lambda_s");
            fn_to_lambda(jit, &id->type, &id->val, NULL, str);
        }
    } else /* just do the store */
        LLVMBuildStore(jit->builder, val, id->val);
    
    if (id->bind != NULL) /* slots don't have a bind */
        id->bind->initialised = 1; /* mark it as initialised */
}

int exec_assign_tuple(jit_t * jit, ast_t * t1, type_t * type, LLVMValueRef val)
{
    ast_t * p1 = t1->child;
    int count = type->arity;
    int i;

    for (i = 0; i < count; i++)
    {
        /* get parameter in struct */
        LLVMValueRef index[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
        LLVMValueRef v = LLVMBuildInBoundsGEP(jit->builder, val, index, 2, "strptr");
        v = LLVMBuildLoad(jit->builder, v, "entry");

        if (p1->tag != AST_LTUPLE)
            exec_assign_id(jit, p1, type->param[i], v);
        else
            exec_assign_tuple(jit, p1, type->param[i], v);

        p1 = p1->next;
    }

    return 0;
}

/*
   Jit a variable assignment
*/
int exec_assignment(jit_t * jit, ast_t * ast)
{
    ast_t * id = ast->child;
    ast_t * expr = ast->child->next;
    
    exec_ast(jit, expr);
    
    if (expr->type->typ == ARRAY && expr->type->arity != 0)
    {
        bind_t * bind = id->bind;
        subst_type(&bind->type); /* fill in the type if known */
        
        id->type = bind->type; /* load particulars from binding */
        
        id->type->arity = expr->type->arity;
        ast->type = expr->type;
        
        return 0;
    }

    if (id->tag != AST_LTUPLE)
        exec_assign_id(jit, id, expr->type, expr->val);
    else
        exec_assign_tuple(jit, id, expr->type, expr->val);
    
    ast->val = expr->val;
    ast->type = expr->type;
    
    return 0;
}

int exec_varassign_id(jit_t * jit, ast_t * id)
{
    bind_t * bind = find_symbol(id->sym); /* deal with environment vars */
    if (bind->val != NULL) /* the variable is in an env */
    {
        LLVMValueRef index[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), bind->val };
        bind->val = LLVMBuildInBoundsGEP(jit->builder, jit->env, index, 2, "env");
        id->val = bind->val;
        id->type = bind->type;
    } 

    return 0;
}

int exec_varassign_tuple(jit_t * jit, ast_t * p)
{
    p = p->child; /* first element of tuple */

    while (p != NULL)
    {
        if (p->tag == AST_LVALUE) /* we have an identifier */
            exec_varassign_id(jit, p);
        else /* we have a tuple */
            exec_varassign_tuple(jit, p);

        p = p->next;
    }

    return 0;
}

/*
   Declare local/global variables
*/
int exec_varassign(jit_t * jit, ast_t * ast)
{
    ast_t * p = ast->child;

    while (p != NULL)
    {
        bind_t * bind; 

        if (p->tag == AST_IDENT) /* no initialisation */
        {
            bind = find_symbol(p->sym); /* deal with environment vars */
            if (bind->val != NULL) /* the variable is in an env */
            {
                LLVMValueRef index[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), bind->val };
                bind->val = LLVMBuildInBoundsGEP(jit->builder, jit->env, index, 2, "env");
                p->val = bind->val;
                p->type = bind->type;
            } else
               exec_decl(jit, p);
        } else /* AST_ASSIGNMENT */
        {
            ast_t * id = p->child;
            
            if (id->tag == AST_LTUPLE) /* we have a tuple */
                exec_varassign_tuple(jit, id);
            else /* we have an identifier */
                exec_varassign_id(jit, id);
            
            exec_assignment(jit, p); /* combined declaration and assignment */
        }

        p = p->next;
    }

    ast->type = t_nil;

    return 0;
}

/*
   Jit an if statement
*/
int exec_if(jit_t * jit, ast_t * ast)
{
    ast_t * exp = ast->child;
    ast_t * con = exp->next;
    int exit1;

    LLVMBasicBlockRef i = LLVMAppendBasicBlock(jit->function, "if");
    LLVMBasicBlockRef b = LLVMAppendBasicBlock(jit->function, "ifbody");
    LLVMBasicBlockRef e = LLVMAppendBasicBlock(jit->function, "ifend");

    LLVMBuildBr(jit->builder, i);
    LLVMPositionBuilderAtEnd(jit->builder, i);  
    
    exec_ast(jit, exp); /* expression */
    
    LLVMBuildCondBr(jit->builder, exp->val, b, e);
    LLVMPositionBuilderAtEnd(jit->builder, b); 
   
    exit1 = exec_ast(jit, con); /* stmt1 */
    
    if (!exit1)
        LLVMBuildBr(jit->builder, e);

    LLVMPositionBuilderAtEnd(jit->builder, e);  

    return 0;
}

/*
   Jit an if..else statement
*/
int exec_ifelse(jit_t * jit, ast_t * ast)
{
    ast_t * exp = ast->child;
    ast_t * con = exp->next;
    ast_t * alt = con->next;
    int exit1, exit2;

    LLVMBasicBlockRef i = LLVMAppendBasicBlock(jit->function, "if");
    LLVMBasicBlockRef b1 = LLVMAppendBasicBlock(jit->function, "ifbody");
    LLVMBasicBlockRef b2 = LLVMAppendBasicBlock(jit->function, "elsebody");
    LLVMBasicBlockRef e = LLVMAppendBasicBlock(jit->function, "ifend");

    LLVMBuildBr(jit->builder, i);
    LLVMPositionBuilderAtEnd(jit->builder, i);  
    
    exec_ast(jit, exp); /* expression */
    
    LLVMBuildCondBr(jit->builder, exp->val, b1, b2);
    LLVMPositionBuilderAtEnd(jit->builder, b1); 
   
    exit1 = exec_ast(jit, con); /* stmt1 */
    
    if (!exit1)
        LLVMBuildBr(jit->builder, e);

    LLVMPositionBuilderAtEnd(jit->builder, b2);  

    exit2 = exec_ast(jit, alt); /* stmt2 */

    if (!exit2)
        LLVMBuildBr(jit->builder, e);

    if (exit1 && exit2)
    {
        LLVMDeleteBasicBlock(e);
        return 1;
    } else 
    {
        LLVMPositionBuilderAtEnd(jit->builder, e);  
        return 0;
    }
}

/*
   Jit an if expression
*/
int exec_ifexpr(jit_t * jit, ast_t * ast)
{
    ast_t * exp = ast->child;
    ast_t * con = exp->next;
    ast_t * alt = con->next;
    
    LLVMBasicBlockRef i = LLVMAppendBasicBlock(jit->function, "if");
    LLVMBasicBlockRef b1 = LLVMAppendBasicBlock(jit->function, "ifbody");
    LLVMBasicBlockRef b2 = LLVMAppendBasicBlock(jit->function, "elsebody");
    LLVMBasicBlockRef e = LLVMAppendBasicBlock(jit->function, "ifend");

    LLVMBuildBr(jit->builder, i);
    LLVMPositionBuilderAtEnd(jit->builder, i);  
    
    exec_ast(jit, exp); /* expression */

    subst_type(&ast->type);
    LLVMValueRef val = LLVMBuildAlloca(jit->builder, type_to_llvm(jit, ast->type), "ifexpr");
    
    LLVMBuildCondBr(jit->builder, exp->val, b1, b2);
    LLVMPositionBuilderAtEnd(jit->builder, b1); 
   
    exec_ast(jit, con); /* stmt1 */
    LLVMBuildStore(jit->builder, con->val, val);

    LLVMBuildBr(jit->builder, e);

    LLVMPositionBuilderAtEnd(jit->builder, b2);  

    exec_ast(jit, alt); /* stmt2 */
    LLVMBuildStore(jit->builder, alt->val, val);

    LLVMBuildBr(jit->builder, e);

    LLVMPositionBuilderAtEnd(jit->builder, e); 
    
    ast->val = LLVMBuildLoad(jit->builder, val, "val");
      
    return 0;
}

/*
   Jit a block of statements
*/
int exec_block(jit_t * jit, ast_t * ast)
{
    ast_t * c = ast->child;
    current_scope = ast->env;
    int exit1;

    while (c != NULL)
    {
        exit1 = exec_ast(jit, c);
                    
        c = c->next;
    }

    scope_down();

    return exit1;
}

/*
   Jit a while statement
*/
int exec_while(jit_t * jit, ast_t * ast)
{
    ast_t * exp = ast->child;
    ast_t * con = exp->next;
    int exit1;
    LLVMBasicBlockRef breaksave = jit->breakto;

    LLVMBasicBlockRef w = LLVMAppendBasicBlock(jit->function, "while");
    LLVMBasicBlockRef b = LLVMAppendBasicBlock(jit->function, "whilebody");
    LLVMBasicBlockRef e = LLVMAppendBasicBlock(jit->function, "whileend");

    LLVMBuildBr(jit->builder, w);
    LLVMPositionBuilderAtEnd(jit->builder, w);  
    
    exec_ast(jit, exp); /* expression */
    
    LLVMBuildCondBr(jit->builder, exp->val, b, e);
    LLVMPositionBuilderAtEnd(jit->builder, b); 
   
    jit->breakto = e;

    exit1 = exec_ast(jit, con); /* stmt1 */
    
    jit->breakto = breaksave;

    if (!exit1)
        LLVMBuildBr(jit->builder, w);

    LLVMPositionBuilderAtEnd(jit->builder, e);  

    return 0;
}

/*
   Jit a break statement
*/
int exec_break(jit_t * jit, ast_t * ast)
{
    if (jit->breakto == NULL)
        jit_exception(jit, "Attempt to break outside loop\n");

    LLVMBuildBr(jit->builder, jit->breakto);
         
    return 1;
}

/*
   Jit a return statement
*/
int exec_return(jit_t * jit, ast_t * ast)
{
    ast_t * p = ast->child;
    
    if (p)
    {
        exec_ast(jit, p);
        if (p->type->typ == FN) /* convert to lambda */
        {
            p->val = make_fn_lambda(jit, p->val, lambda_fn_type(jit, p->type));
            fn_to_lambda(jit, &p->type, &p->val, NULL, NULL);
        }
        
        LLVMBuildRet(jit->builder, p->val);
        
    } else
        LLVMBuildRetVoid(jit->builder);
         
    return 1;
}

/* recursively fill in an ast with inferred types */
void fill_in_types(ast_t * ast)
{
    if (ast->env != NULL)
        current_scope = ast->env;
    
    if (ast->type != NULL) /* slots for example don't have types */
        subst_type(&ast->type); /* fill in the type at this level*/
    
    if (ast->tag == AST_IDENT) /* update identifier binding */
    {
        bind_t * bind = find_symbol(ast->sym);
        
        if (bind != NULL) /* slots for example don't have types */
        {
            if (bind->type->typ == TYPEVAR) /* we don't know what type it is */
                subst_type(&bind->type); /* fill in the type */
        }
    }
    if (ast->child != NULL) /* depth first */
        fill_in_types(ast->child);

    if (ast->env != NULL)
        scope_down();

    if (ast->next != NULL) /* then breadth */
        fill_in_types(ast->next);
}

/*
   Jit a list of function parameters making allocas for them
*/
int exec_fnparams(jit_t * jit, ast_t * ast)
{
    ast_t * p = ast->child;
    int i = 0;

    if (p->tag != AST_NIL)
    {
        while (p != NULL)
        {
            bind_t * bind = find_symbol(p->sym);
            subst_type(&bind->type);
            p->type = bind->type;

            LLVMValueRef param = LLVMGetParam(jit->function, i);
              
            if (bind->val == NULL) /* ordinary param */
            {
               LLVMValueRef palloca = LLVMBuildAlloca(jit->builder, type_to_llvm(jit, p->type), p->sym->name);
               LLVMBuildStore(jit->builder, param, palloca);
        
               bind->val = palloca;
               p->val = palloca;
            } else /* param is in an env */
            {
               LLVMValueRef index[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), bind->val };
               bind->val = LLVMBuildInBoundsGEP(jit->builder, jit->env, index, 2, "env");
               LLVMBuildStore(jit->builder, param, bind->val);
               p->val = bind->val;
            }

            i++;
            p = p->next;
        }
    }

    return 0;
}

/*
   Jit a list of function parameters making allocas for them
*/
int exec_lambdaparams(jit_t * jit, ast_t * ast)
{
    ast_t * p = ast->child;
    int i = 0;

    if (jit->bind_num != 0) /* we have an environment */
    {
        while (p != NULL) /* get env parameter */
        {
            i++;
            p = p->next;
        }
        LLVMValueRef param = LLVMGetParam(jit->function, i);
        jit->env = LLVMBuildPointerCast(jit->builder, param, LLVMPointerType(jit->env_s, 0), "env");
    }
    
    p = ast->child;
    i = 0;
    while (p != NULL)
    {
        bind_t * bind = find_symbol(p->sym);
        subst_type(&bind->type);
        p->type = bind->type;

        LLVMValueRef param = LLVMGetParam(jit->function, i);
            
        if (bind->val == NULL) /* we have an ordinary param */
        {
            LLVMValueRef palloca = LLVMBuildAlloca(jit->builder, type_to_llvm(jit, p->type), p->sym->name);
            LLVMValueRef val = LLVMBuildStore(jit->builder, param, palloca);
            
            bind->val = palloca;
            p->val = palloca;
        } else /* param is in an env */
        {
            LLVMValueRef index[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), bind->val };
            bind->val = LLVMBuildInBoundsGEP(jit->builder, jit->env, index, 2, "env");
            LLVMBuildStore(jit->builder, param, bind->val);
            p->val = bind->val;
        }
        
        i++;
        p = p->next;
    }
        
    return 0;
}

/*
   Jit a function declaration
*/
int exec_fndec(jit_t * jit, ast_t * ast)
{
    fill_in_types(ast); /* just fill in all the types we've inferred */
      
    return 0;
}

/*
   Jit a function declaration
*/
int exec_datatype(jit_t * jit, ast_t * ast)
{
    /* can't do anything until we know the types */
      
    return 0;
}

/*
   Jit a function
*/
int exec_fndef(jit_t * jit, ast_t * ast)
{
    ast_t * fn = ast->child;
    int params = ast->type->arity;
    int i;
    
    bind_t ** bind_arr_save = jit->bind_arr; /* load correct bind array */
    int bind_num_save = jit->bind_num;
    LLVMTypeRef env_s_save = jit->env_s;
    LLVMValueRef env_save = jit->env;
    jit->bind_arr = ast->bind_arr;
    jit->bind_num = ast->bind_num;

    make_env_s(jit); /* make environment struct */
          
    /* get argument types */
    LLVMTypeRef * args = (LLVMTypeRef *) GC_MALLOC(params*sizeof(LLVMTypeRef));
    for (i = 0; i < params; i++)
    {
        subst_type(&ast->type->param[i]);
        args[i] = type_to_llvm(jit, ast->type->param[i]); 
    }

    /* get return type */
    subst_type(&ast->type->ret);
    LLVMTypeRef ret = type_to_llvm(jit, ast->type->ret); 
    
    /* make LLVM function type */
    LLVMTypeRef fn_type = LLVMFunctionType(ret, args, params, 0);
    
    /* make llvm function object */
    char * fn_name = fn->sym->name;
    LLVMValueRef fn_save = jit->function;
    jit->function = LLVMAddFunction(jit->module, fn_name, fn_type);
    ast->val = jit->function;

    type_t * t;
    /* set nocapture on all structured params */
    for (i = 0; i < params; i++)
    {
        t = ast->type->param[i];
        if (t->typ == ARRAY || t->typ == TUPLE || t->typ == DATATYPE)
            LLVMAddAttribute(LLVMGetParam(ast->val, i), LLVMNoCaptureAttribute);
    }
    
    /* set nocapture on all structured return values */
    t = ast->type->ret;
    if (t->typ == ARRAY || t->typ == TUPLE || t->typ == DATATYPE)
        LLVMAddFunctionAttr(ast->val, LLVMNoAliasAttribute);
 
    /* add the prototype to the symbol binding in case the function calls itself */
    bind_t * bind = find_symbol(fn->sym);
    bind->val = jit->function;
    bind->type = ast->type;

    env_t * scope_save = current_scope;
    current_scope = ast->env;

    /* jit setup */
    LLVMBuilderRef build_save = jit->builder;
    jit->builder = LLVMCreateBuilder();

    /* first basic block */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(jit->function, "entry");
    LLVMPositionBuilderAtEnd(jit->builder, entry);
       
    /* make environment malloc */
    if (jit->bind_num != 0)
        jit->env = LLVMBuildGCMalloc(jit, jit->env_s, "env", 0);

    /* make allocas for the function parameters */
    exec_fnparams(jit, fn->next);
    
    /* jit the statements in the lambda body */
    ast_t * p = fn->next->next->child;
    while (p != NULL)
    {
        exec_ast(jit, p);
        p = p->next;
    }
            
    /* run the pass manager on the jit'd function */
    LLVMRunFunctionPassManager(jit->pass, jit->function); 
    
    /* clean up */
    LLVMDisposeBuilder(jit->builder);  
    jit->builder = build_save;
    jit->function = fn_save;    
    current_scope = scope_save;

    jit->bind_arr = bind_arr_save;
    jit->bind_num = bind_num_save;
    jit->env_s = env_s_save;
    jit->env = env_save;

    return 0;
}

/*
   Jit a lambda
*/
int exec_lambda(jit_t * jit, ast_t * ast)
{
    ast_t * fn = ast->child;
    int params = ast->type->arity;
    int i;
    
    LLVMValueRef env_save = jit->env;
      
    /* get argument types */
    LLVMTypeRef * args = (LLVMTypeRef *) GC_MALLOC((params + 1)*sizeof(LLVMTypeRef));
    for (i = 0; i < params; i++)
    {
        subst_type(&ast->type->param[i]);
        args[i] = type_to_llvm(jit, ast->type->param[i]); 
    }
    args[params] = LLVMPointerType(LLVMInt8Type(), 0);

    /* get return type */
    subst_type(&ast->type->ret);
    LLVMTypeRef ret = type_to_llvm(jit, ast->type->ret); 
    
    /* make LLVM function type */
    LLVMTypeRef fn_type = LLVMFunctionType(ret, args, params + 1, 0);
    
    /* make llvm function object */
    LLVMValueRef fn_save = jit->function;
    jit->function = LLVMAddFunction(jit->module, "lambda", fn_type);
    ast->val = jit->function;

    /* add the prototype to the symbol binding */
    bind_t * bind = find_symbol(sym_lookup("lambda"));
    bind->val = jit->function;
    bind->type = ast->type;

    env_t * scope_save = current_scope;
    current_scope = ast->env;

    /* jit setup */
    LLVMBuilderRef build_save = jit->builder;
    jit->builder = LLVMCreateBuilder();

    /* first basic block */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(jit->function, "entry");
    LLVMPositionBuilderAtEnd(jit->builder, entry);
       
    /* make allocas for the function parameters */
    exec_lambdaparams(jit, ast->child);
    
    /* back up bindings in bind array and set them to environment entries */
    LLVMValueRef * bind_save = (LLVMValueRef *) GC_MALLOC(jit->bind_num*sizeof(LLVMValueRef));
    for (i = 0; i < jit->bind_num; i++)
    {
        bind_save[i] = jit->bind_arr[i]->val;
        LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
        jit->bind_arr[i]->val = LLVMBuildInBoundsGEP(jit->builder, jit->env, indices, 2, "env");
    }
    
    /* jit the statements in the function body */
    ast_t * p = ast->child->next;
    if (p->tag == AST_EXPRBLOCK)
    {
        p = p->child;
        while (p->next != NULL)
        {
            exec_ast(jit, p);
            p = p->next;
        }
    }
    exec_ast(jit, p);
    if (p->type->typ == FN) /* convert to lambda */
    {
        p->val = make_fn_lambda(jit, p->val, lambda_fn_type(jit, p->type));
        fn_to_lambda(jit, &p->type, &p->val, NULL, NULL);
    }

    /* jit return */
    LLVMBuildRet(jit->builder, p->val);

    /* run the pass manager on the jit'd function */
    LLVMRunFunctionPassManager(jit->pass, jit->function); 
    
    /* restore original values in bind array */
    for (i = 0; i < jit->bind_num; i++)
        jit->bind_arr[i]->val = bind_save[i];
    jit->env = env_save;
    
    /* clean up */
    LLVMDisposeBuilder(jit->builder);  
    jit->builder = build_save;
    jit->function = fn_save;    
    current_scope = scope_save;
    
    if (jit->bind_num == 0) /* no environment required */
        fn_to_lambda(jit, &bind->type, &bind->val, NULL, NULL);
    else
        fn_to_lambda(jit, &bind->type, &bind->val, 
           LLVMBuildPointerCast(jit->builder, jit->env, 
              LLVMPointerType(LLVMInt8Type(), 0), "env"), NULL);
 
    ast->val = bind->val;
    
    return 0;
}

/* 
   Given an ast node whose val is a proper function
   i) build a lambda type and malloc space for it
   ii) set its function entry to the given function
   iii) set its environment to the supplied LLVM pointer 
        (if it is not NULL), otherwise set it to NULL
   iv) change type to LAMBDA instead of FN
*/
void fn_to_lambda(jit_t * jit, type_t ** type, 
                  LLVMValueRef * val, LLVMValueRef env_ptr, LLVMValueRef str)
{
    if (str == NULL)
    {
        /* malloc space for lambda struct */
        LLVMTypeRef str_ty = lambda_type(jit, *type);
        str = LLVMBuildGCMalloc(jit, str_ty, "lambda_s", 0);
    }

    /* set function entry */
    LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 0, 0) };
    LLVMValueRef fn_entry = LLVMBuildInBoundsGEP(jit->builder, str, indices, 2, "fn");
    LLVMBuildStore(jit->builder, *val, fn_entry);
            
    /* set environment entry */
    LLVMValueRef indices2[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 1, 0) };
    LLVMValueRef env = LLVMBuildInBoundsGEP(jit->builder, str, indices2, 2, "env");
    if (env_ptr == NULL)
        LLVMBuildStore(jit->builder, LLVMConstPointerNull(LLVMPointerType(LLVMInt8Type(), 0)), env);
    else
        LLVMBuildStore(jit->builder, env_ptr, env);
    *val = str;

    /* set lambda type */
    *type = fn_to_lambda_type(*type);
}

/*
   Jit a type constructor
*/
int exec_typeconstr(jit_t * jit, ast_t * ast, LLVMValueRef * args)
{
    ast_t * id = ast->child;
    int i, params = id->type->arity;
    int atomic = 1;

    LLVMTypeRef str_ty = tup_type(jit, id->type);

    /* determine whether fields are atomic */
    for (i = 0; i < params; i++)
        atomic &= is_atomic(id->type->param[i]);

    ast->val = LLVMBuildGCMalloc(jit, str_ty, id->sym->name, atomic);
    ast->type = id->type;
    
    for (i = 0; i < params; i++)
    {
        /* insert value into datatype */
        LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
        LLVMValueRef entry = LLVMBuildInBoundsGEP(jit->builder, ast->val, indices, 2, id->sym->name);
        LLVMBuildStore(jit->builder, args[i], entry);
    }
   
    return 0;
}

/*
   Jit a slot access
*/
int exec_slot(jit_t * jit, ast_t * ast)
{
    ast_t * id = ast->child;
    ast_t * p = id->next;
    int i, params = id->type->arity;
    
    if (ast->sym == NULL) /* need some kind of name */
        ast->sym = sym_lookup("none");
    
    exec_ast(jit, id);

    for (i = 0; i < params; i++)
    {
        if (id->type->slot[i] == p->sym)
            break;
    }
    
    /* get slot from datatype */
    LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
    LLVMValueRef entry = LLVMBuildInBoundsGEP(jit->builder, id->val, indices, 2, p->sym->name);
    ast->val = LLVMBuildLoad(jit->builder, entry, p->sym->name);
    
    ast->type = id->type->param[i];
   
    return 0;
}

/*
   Jit an array access
*/
int exec_location(jit_t * jit, ast_t * ast)
{
    ast_t * id = ast->child;
    ast_t * p = id->next;
    int i;
    
    if (ast->sym == NULL) /* need some kind of name */
        ast->sym = sym_lookup("none");
    
    exec_ast(jit, id);
    exec_ast(jit, p);
    
    /* get array from datatype */
    LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 0, 0) };
    ast->val = LLVMBuildInBoundsGEP(jit->builder, id->val, indices, 2, "arr");
    ast->val = LLVMBuildLoad(jit->builder, ast->val, "array");
    
    /* get location within array */
    LLVMValueRef indices2[1] = { p->val };
    ast->val = LLVMBuildInBoundsGEP(jit->builder, ast->val, indices2, 1, "arr_entry");
    
    /* load value */
    ast->val = LLVMBuildLoad(jit->builder, ast->val, "entry");
    
    ast->type = id->type->ret;
   
    return 0;
}

/*
   Jit a function application
*/
int exec_appl(jit_t * jit, ast_t * ast)
{
    ast_t * fn = ast->child;
    ast_t * p;
    int i, params;

    /* load function or type constructor */
    exec_ast(jit, fn);
    
    params = fn->type->arity;
    LLVMValueRef * args = (LLVMValueRef *) /* one extra for env if lambda */
        GC_MALLOC((params + (fn->type->typ == LAMBDA))*sizeof(LLVMValueRef));
    
    /* jit function arguments */
    p = fn->next;
    for (i = 0; i < params; i++)
    {
        exec_ast(jit, p);
        
        if (p->type->typ == FN) /* convert function to lambda */
        {
            p->val = make_fn_lambda(jit, p->val, lambda_fn_type(jit, p->type));
            fn_to_lambda(jit, &p->type, &p->val, NULL, NULL);
        }

        args[i] = p->val;
        p = p->next;
    }
    
    /* call function */
    if (fn->type->typ == FN)
        ast->val = LLVMBuildCall(jit->builder, fn->val, args, params, "");
    else if (fn->type->typ == DATATYPE)
        return exec_typeconstr(jit, ast, args);
    else /* lambda */
    {
        /* load struct */
        LLVMValueRef str = fn->val;

        /* load function entry */
        LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 0, 0) };
        LLVMValueRef fn_entry = LLVMBuildInBoundsGEP(jit->builder, str, indices, 2, "fn");
        LLVMValueRef function = LLVMBuildLoad(jit->builder, fn_entry, "lambda");
        
        /* load environment entry */
        LLVMValueRef indices2[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 1, 0) };
        LLVMValueRef env = LLVMBuildInBoundsGEP(jit->builder, str, indices2, 2, "env");
        env = LLVMBuildLoad(jit->builder, env, "env");
        args[i] = env;

        /* call function */
        ast->val = LLVMBuildCall(jit->builder, function, args, params + 1, "");
    }

    /* update return type */
    ast->type = fn->type->ret; 
        
    return 0;
}

/*
   Jit an array initialisation
*/
int exec_array(jit_t * jit, ast_t * ast)
{
    int i;

    subst_type(&ast->type);

    ast_t * p = ast->child;
    
    /* for global arrays whose type is not known just jit the length */
    if (ast->type->ret->typ == TYPEVAR && current_scope->next == NULL)
    {
        subst_type(&p->type);
        long r;
        START_EXEC;
        exec_ast(jit, p);
        INT_EXEC(r, p->val);
        ast->type->arity = (int) r;
        return 0;
    } else
        exec_ast(jit, p);

    LLVMTypeRef str_ty = arr_type(jit, ast->type);
    ast->val = LLVMBuildGCMalloc(jit, str_ty, "tuple_s", 0);

    /* insert length into array struct */
    LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 1, 0) };
    LLVMValueRef entry = LLVMBuildInBoundsGEP(jit->builder, ast->val, indices, 2, "length");
    LLVMBuildStore(jit->builder, p->val, entry);
    
    /* create array */
    int atomic = is_atomic(ast->type->ret);
    LLVMValueRef arr = LLVMBuildGCArrayMalloc(jit, type_to_llvm(jit, ast->type->ret), p->val, "array", atomic);

    LLVMValueRef indices2[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 0, 0) };
    entry = LLVMBuildInBoundsGEP(jit->builder, ast->val, indices2, 2, "arr");
    LLVMBuildStore(jit->builder, arr, entry);
    
    return 0;
}

/*
   Jit a tuple expression
*/
int exec_tuple(jit_t * jit, ast_t * ast)
{
    int params = ast->type->arity;
    int i;
    int atomic = 1;

    subst_type(&ast->type);

    LLVMTypeRef str_ty = tup_type(jit, ast->type);

    /* determine whether fields are atomic */
    for (i = 0; i < params; i++)
        atomic &= is_atomic(ast->type->param[i]);

    ast->val = LLVMBuildGCMalloc(jit, str_ty, "tuple_s", atomic);

    ast_t * p = ast->child;
    for (i = 0; i < params; i++)
    {
        exec_ast(jit, p);
        
        if (p->type->typ == FN) /* convert function to lambda */
        {
            p->val = make_fn_lambda(jit, p->val, lambda_fn_type(jit, p->type));
            fn_to_lambda(jit, &p->type, &p->val, NULL, NULL);
        }
 
        /* insert value into tuple */
        LLVMValueRef indices[2] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), i, 0) };
        LLVMValueRef entry = LLVMBuildInBoundsGEP(jit->builder, ast->val, indices, 2, "tuple");
        LLVMBuildStore(jit->builder, p->val, entry);
    
        p = p->next;
    }

    return 0;
}

/*
    Build an llvm struct type from bindings from the bind array 
*/
void make_env_s(jit_t * jit)
{
    int i;
    int num = jit->bind_num;
    
    if (num != 0)
    {
        LLVMTypeRef * types = (LLVMTypeRef *) GC_MALLOC(num*sizeof(LLVMTypeRef));
        for (i = 0; i < num; i++)
        {
            bind_t * bind = jit->bind_arr[i];
            subst_type(&bind->type);
            types[i] = type_to_llvm(jit, bind->type);
        }
        jit->env_s = LLVMStructType(types, num, 1);
    }
}

/* 
   Add binding to bind array. which signifies we need it in an 
   environment struct
   Realloc the bind array if at a power of 2 length
*/
void add_bind(jit_t * jit, bind_t * bind)
{
    if (jit->bind_num == 0) /* do first allocation */
        jit->bind_arr = (bind_t **) GC_MALLOC(sizeof(bind_t *));
    else if ((jit->bind_num & (jit->bind_num - 1)) == 0) /* realloc if power of 2 */
        jit->bind_arr = (bind_t **) GC_REALLOC(jit->bind_arr, (jit->bind_num << 1)*sizeof(bind_t *));
    
    bind->val = LLVMConstInt(LLVMInt32Type(), jit->bind_num, 0);
    jit->bind_arr[jit->bind_num++] = bind;

    if (TRACE) printf("Added %s to environment\n", bind->sym->name);
}

/*
   Add bindings to bind arrat for all local identifiers refered to 
   out of their own scope
*/
void collect_idents(jit_t * jit, ast_t * ast)
{
    bind_t * bind;
    int i;
    int child_only = 0;

    /* check if we are entering a new scope */
    if (ast->tag == AST_FNDEC || ast->tag == AST_LAMBDA || ast->tag == AST_BLOCK)
        current_scope = ast->env;

child_process:
    if (ast->tag == AST_IDENT)
    {
        bind = find_symbol(ast->sym); /* only locals get put into lambda envs */
        if (!scope_is_current(bind)) /* also ensure the identifier is from another scope */
        {
            if (!scope_is_global(bind)) /* ensure the variable is local */
            {
                for (i = 0; i < jit->bind_num; i++) /* see if we already have it */
                    if (jit->bind_arr[i] == bind)
                        break;

                if (i == jit->bind_num) /* if not, add it */
                    add_bind(jit, bind);
            }
        }
    }

    if (child_only)
        return;

    if (ast->tag == AST_SLOT) /* only process the id for a slot access */
    {
        ast = ast->child;
        child_only = 1;
        goto child_process;
    }

    if (ast->child != NULL && ast->tag != AST_PARAMS) /* don't process params */
        collect_idents(jit, ast->child); /* depth first */

    if (ast->tag == AST_FNDEC || ast->tag == AST_LAMBDA || ast->tag == AST_BLOCK)
        scope_down();

    if (ast->next != NULL)
        collect_idents(jit, ast->next); /* then breadth */
}

/*
   For each lambda in the ast, collect bindings in the bind array
   for each local that needs to go into an environment struct
*/
void process_lambdas(jit_t * jit, ast_t * ast)
{
    if (ast->tag == AST_LAMBDA) /* found a lambda */
        collect_idents(jit, ast);
    else
    {
        if (ast->child != NULL)
            process_lambdas(jit, ast->child); /* depth first */

        if (ast->next != NULL)
            process_lambdas(jit, ast->next); /* then breadth */
    }
}

/*
   As we traverse the ast we dispatch on ast tag to various jit 
   functions defined above
*/
int exec_ast(jit_t * jit, ast_t * ast)
{
    switch (ast->tag)
    {
    case AST_INT:
        return exec_int(jit, ast);
    case AST_DOUBLE:
        return exec_double(jit, ast);
    case AST_STRING:
        return exec_string(jit, ast);
    case AST_BOOL:
        return exec_bool(jit, ast);
    case AST_PLUS:
        return exec_plus(jit, ast);
    case AST_MINUS:
        return exec_minus(jit, ast);
    case AST_TIMES:
        return exec_times(jit, ast);
    case AST_DIV:
        return exec_div(jit, ast);
    case AST_MOD:
        return exec_mod(jit, ast);
    case AST_LSH:
        return exec_lsh(jit, ast);
    case AST_RSH:
        return exec_rsh(jit, ast);
    case AST_BITOR:
        return exec_bitor(jit, ast);
    case AST_BITAND:
        return exec_bitand(jit, ast);
    case AST_BITXOR:
        return exec_bitxor(jit, ast);
    case AST_VARASSIGN:
        return exec_varassign(jit, ast);
    case AST_ASSIGNMENT:
        return exec_assignment(jit, ast);
    case AST_IDENT:
        return exec_ident(jit, ast);
    case AST_LE:
        return exec_le(jit, ast);
    case AST_GE:
        return exec_ge(jit, ast);
    case AST_LT:
        return exec_lt(jit, ast);
    case AST_GT:
        return exec_gt(jit, ast);
    case AST_EQ:
        return exec_eq(jit, ast);
    case AST_NE:
        return exec_ne(jit, ast);
    case AST_LOGAND:
        return exec_logand(jit, ast);
    case AST_LOGOR:
        return exec_logor(jit, ast);
    case AST_LOGNOT:
        return exec_lognot(jit, ast);
    case AST_BITNOT:
        return exec_bitnot(jit, ast);
    case AST_UNMINUS:
        return exec_unminus(jit, ast);
    case AST_PRE_INC:
        return exec_preinc(jit, ast);
    case AST_PRE_DEC:
        return exec_predec(jit, ast);
    case AST_POST_INC:
        return exec_postinc(jit, ast);
    case AST_POST_DEC:
        return exec_postdec(jit, ast);
    case AST_PLUSEQ:
        return exec_pluseq(jit, ast);
    case AST_MINUSEQ:
        return exec_minuseq(jit, ast);
    case AST_TIMESEQ:
        return exec_timeseq(jit, ast);
    case AST_DIVEQ:
        return exec_diveq(jit, ast);
    case AST_MODEQ:
        return exec_modeq(jit, ast);
    case AST_ANDEQ:
        return exec_andeq(jit, ast);
    case AST_OREQ:
        return exec_oreq(jit, ast);
    case AST_XOREQ:
        return exec_xoreq(jit, ast);
    case AST_LSHEQ:
        return exec_lsheq(jit, ast);
    case AST_RSHEQ:
        return exec_rsheq(jit, ast);
    case AST_IF:
        return exec_if(jit, ast);
    case AST_IFELSE:
        return exec_ifelse(jit, ast);
    case AST_IFEXPR:
        return exec_ifexpr(jit, ast);
    case AST_BLOCK:
        return exec_block(jit, ast);
    case AST_WHILE:
        return exec_while(jit, ast);
    case AST_BREAK:
        return exec_break(jit, ast);
    case AST_FNDEC:
        return exec_fndec(jit, ast);
    case AST_LAMBDA:
        return exec_lambda(jit, ast);
    case AST_RETURN:
        return exec_return(jit, ast);
    case AST_APPL:
        return exec_appl(jit, ast);
    case AST_TUPLE:
        return exec_tuple(jit, ast);
    case AST_DATATYPE:
        return exec_datatype(jit, ast);
    case AST_LOCATION:
        return exec_location(jit, ast);
    case AST_SLOT:
        return exec_slot(jit, ast);
    case AST_ARRAY:
        return exec_array(jit, ast);
    default:
        ast->type = t_nil;
        return 0;
    }
}

/* We start traversing the ast to do jit'ing */
void exec_root(jit_t * jit, ast_t * ast)
{
    /* Traverse the ast jit'ing everything, then run the jit'd code */
    START_EXEC;
         
    process_lambdas(jit, ast); /* get any locals that need to go in an env */
    
    if (ast->tag == AST_FNDEC) /* save bind array for when function is jit'd */
    {
        ast->bind_arr = jit->bind_arr;
        ast->bind_num = jit->bind_num;
        jit->bind_arr = NULL;
        jit->bind_num = 0;
    } else /* otherwise make the struct and allocate it now */
    {
        make_env_s(jit);
        if (jit->bind_num != 0)
            jit->env = LLVMBuildGCMalloc(jit, jit->env_s, "env", 0);
    }
    
    /* jit the ast */
    exec_ast(jit, ast);
    
    /* print the resulting value */
    print_obj(jit, ast->type, ast->val);
    
    END_EXEC;
         
    /* 
       If our jit side print_obj had to create a format string
       clean it up now that we've executed the jit'd code.
    */
    if (jit->fmt_str)
    {
        LLVMDeleteGlobal(jit->fmt_str);
        jit->fmt_str = NULL;
    }

    jit->bind_num = 0; /* clean up bind array */
}

