/* 10_struct_field_ptr.c
 * A struct contains a pointer field.
 * Expected: s.ptr -> {x}
 */
int x = 100;

struct S {
    int *ptr;
    int  val;
};

struct S s;

int main(void)
{
    s.ptr = &x;
    s.val = 1;
    return *s.ptr;
}
