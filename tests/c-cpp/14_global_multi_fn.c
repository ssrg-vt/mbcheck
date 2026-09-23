/* 14_global_multi_fn.c
 * Global pointer updated by two different functions.
 * After calling both, g may point to either x or y.
 * Expected: g -> {x, y}
 */
int x = 1;
int y = 2;
int *g;

void setX(void) { g = &x; }
void setY(void) { g = &y; }

int main(void)
{
    setX();
    setY();
    return *g;
}
