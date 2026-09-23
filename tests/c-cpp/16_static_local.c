/* 16_static_local.c
 * Two calls to a function both return the address of the same static local.
 * Expected: p and q should both point to the same single static object.
 */
int *getStatic(void)
{
    static int s = 0;
    s++;
    return &s;
}

int main(void)
{
    int *p = getStatic();
    int *q = getStatic();
    /* p == q at run time; pointer analysis should reflect pts(p) = pts(q) */
    return *p + *q;
}
