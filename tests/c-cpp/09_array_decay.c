/* 09_array_decay.c
 * Array name decays to a pointer to its first element.
 * SVF should create a single abstract object for the whole array.
 * Expected: p -> {arr_object}
 */
int arr[8];
int *p;

int main(void)
{
    p = arr;      /* decay: pointer to arr[0] */
    *p = 7;
    p[3] = 42;
    return p[0];
}
