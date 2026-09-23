/* 05_conditional_branch.c
 * Pointer assigned on two branches of an if/else.
 * At the join point p may point to either x or y.
 * Expected: p -> {x, y} (both x and y list p as pointer).
 */
int x = 1;
int y = 2;
int *p;
extern int cond;

int main(void)
{
    if (cond)
        p = &x;
    else
        p = &y;
    return *p;
}
