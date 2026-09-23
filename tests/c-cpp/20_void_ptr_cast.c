/* 20_void_ptr_cast.c
 * void* carries the same abstract object through a bitcast.
 * Expected: vp -> {x}, ip -> {x}  (both point to the same object)
 */
int x = 10;
void *vp;
int  *ip;

int main(void)
{
    vp = &x;
    ip = (int *)vp;
    return *ip;
}
