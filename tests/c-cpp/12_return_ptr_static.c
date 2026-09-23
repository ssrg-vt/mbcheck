/* 12_return_ptr_static.c
 * Function returns a pointer to a function-static variable.
 * Expected: p -> {static_count}
 */
int *getCounter(void)
{
    static int count = 0;
    count++;
    return &count;
}

int *p;

int main(void)
{
    p = getCounter();
    return *p;
}
