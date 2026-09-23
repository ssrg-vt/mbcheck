/* 04_flow_overwrite.c
 * Flow-sensitivity: p is first assigned &x, then strongly updated to &y.
 * With flow-sensitive analysis the second store dominates, so at program
 * exit: p -> {y}.
 * Expected: y has p as its pointer; x should have no pointers at exit.
 */
int x = 1;
int y = 2;
int *p;

int main(void)
{
    p = &x;   /* first assignment */
    p = &y;   /* strong update: overwrites previous */
    return *p;
}
