/* 08_pointer_to_pointer.c
 * Pointer-to-pointer: pp -> {p_var}, p -> {x}
 * Dereferencing pp twice should reach x.
 * Expected: pp -> {p}, p -> {x}
 */
int x = 5;
int  *p;
int **pp;

int main(void)
{
    p  = &x;
    pp = &p;
    return **pp;
}
