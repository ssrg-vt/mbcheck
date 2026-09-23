/* 25_union_ptr.c
 * Union with an int field and a pointer field.
 * When the pointer field is used, it should point to x.
 * Expected: u.pval -> {x}
 */
int x = 5;

union U {
    int  ival;
    int *pval;
};

union U u;

int main(void)
{
    u.pval = &x;
    return u.ival;   /* type-punning read, but pointer analysis tracks u.pval */
}
