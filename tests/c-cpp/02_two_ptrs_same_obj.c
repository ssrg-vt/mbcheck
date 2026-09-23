/* 02_two_ptrs_same_obj.c
 * Two distinct pointers both targeting the same object.
 * Expected: p -> {x}, q -> {x}
 * Checks that the pointee appears once with two pointer entries.
 */
int x = 42;
int *p;
int *q;

int main(void)
{
    p = &x;
    q = &x;
    return *p + *q;
}
