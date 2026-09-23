/* 11_ptr_to_struct.c
 * Pointer to a struct; accesses go through the pointer.
 * Expected: sp -> {obj}
 */
struct Point { int x; int y; };

struct Point obj;
struct Point *sp;

int main(void)
{
    sp    = &obj;
    sp->x = 3;
    sp->y = 4;
    return sp->x + sp->y;
}
