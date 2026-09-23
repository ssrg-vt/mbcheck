/* 24_chain_deref.c
 * Three-level chain: l1 -> x, l2 -> l1, l3 -> l2.
 * Dereferencing l3 three times reaches x.
 * Expected:
 *   l1 -> {x}
 *   l2 -> {l1}
 *   l3 -> {l2}
 */
int    x  = 42;
int   *l1;
int  **l2;
int ***l3;

int main(void)
{
    l1 = &x;
    l2 = &l1;
    l3 = &l2;
    return ***l3;
}
