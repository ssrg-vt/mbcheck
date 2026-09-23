/* 03_copy_propagation.c
 * Copy propagation: q inherits p's points-to set.
 * Expected: p -> {x}, q -> {x}
 * Both p and q should appear as pointers to x.
 */
int x = 10;
int *p;
int *q;

int main(void)
{
    p = &x;
    q = p;        /* copy: q now points wherever p points */
    return *q;
}
