/* 01_simple_addr.c
 * Simplest case: one pointer, one object.
 * Expected: p -> {x}
 */
int x = 10;
int *p;

int main(void)
{
    p = &x;
    return *p;
}
