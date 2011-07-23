#include <stdio.h>
#include "types.h"
#include "gc.h"

type_t * t_nil;
type_t * t_int;
type_t * t_bool;
type_t * t_double;
type_t * t_string;
type_t * t_char;

type_t * new_type(typ_t typ)
{
   type_t * t = GC_MALLOC(sizeof(type_t));
   t->typ = typ;
   t->arity = 0;
   return t;
}

void type_init(void)
{
   t_nil = new_type(NIL);
   t_int = new_type(INT);
   t_bool = new_type(BOOL);
   t_double = new_type(DOUBLE);
   t_string = new_type(STRING);
   t_char = new_type(CHAR);
}

int type_equal(type_t * t1, type_t * t2)
{
   if (t1 == t2)
      return 1;
   return 0;
}

type_t * fn_type(type_t * ret, int num, type_t ** param)
{
   type_t * t = GC_MALLOC(sizeof(type_t));
   t->typ = FN;
   t->param = GC_MALLOC(sizeof(type_t *)*num);
   t->ret = ret;
   t->arity = num;

   int i;
   for (i = 0; i < num; i++)
      t->param[i] = param[i];

   return t;
}

/* convert to a lambda type */
type_t * fn_to_lambda_type(type_t * type)
{
    type = fn_type(type->ret, type->arity, type->param);
    type->typ = LAMBDA; 
    return type;
}


type_t * new_typevar(void)
{
    static long typevarnum = 0;
    type_t * t = new_type(TYPEVAR);
    t->arity = typevarnum++;
    return t;
}

void print_type(type_t * t)
{
    int i;
    
    if (t == NULL)
    {
        printf("NULL");
        return;
    }
    
    switch (t->typ)
    {
    case NIL:
        printf("nil");
        break;
    case BOOL:
        printf("bool");
        break;
    case INT:
        printf("int");
        break;
    case DOUBLE:
        printf("double");
        break;
    case STRING:
        printf("string");
        break;
    case CHAR:
        printf("char");
        break;
    case FN:
        printf("(");
        if (t->arity == 0)
            printf("nil");
        else
        {
            for (i = 0; i < t->arity - 1; i++)
            {
                print_type(t->param[i]);
                printf(", ");
            }
            print_type(t->param[i]);
        }
        printf(" -> ");
        print_type(t->ret);
        printf(")");
        break;
    case TYPEVAR:
        printf("T%ld", t->arity);
        break;
    } 
}
