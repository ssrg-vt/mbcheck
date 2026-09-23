/* 21_loop_addr.c
 * Pointer table filled in a loop; each slot points to a different global.
 * With field-sensitive analysis each ptrs[i] slot should be kept separate.
 * Expected: ptrs[i] -> {arr[i]}  for i in 0..4
 */
#define N 5
int arr[N];
int *ptrs[N];

int main(void)
{
    int i;
    for (i = 0; i < N; i++)
        ptrs[i] = &arr[i];
    return *ptrs[0];
}
