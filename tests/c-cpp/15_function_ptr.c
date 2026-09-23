/* 15_function_ptr.c
 * Function pointer: fp can point to foo or bar.
 * Expected in SVF: fp -> {foo_obj, bar_obj}
 */
int foo(void) { return 1; }
int bar(void) { return 2; }

typedef int (*FP)(void);

FP fp;

int main(void)
{
    fp = foo;
    fp = bar;
    return fp();   /* indirect call */
}
