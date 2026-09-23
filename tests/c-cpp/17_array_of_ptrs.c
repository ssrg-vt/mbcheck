/* 17_array_of_ptrs.c
 * Array whose elements are pointers to distinct global objects.
 * Expected: arr[0] -> {a}, arr[1] -> {b}, arr[2] -> {c}
 */
int a = 1, b = 2, c = 3;
int *ptrs[3];

int main(void)
{
    ptrs[0] = &a;
    ptrs[1] = &b;
    ptrs[2] = &c;
    return *ptrs[0] + *ptrs[1] + *ptrs[2];
}
