// 35_cpp_placement_new.cpp
// Placement new: object constructed inside a pre-allocated stack buffer.
// The pointer pp should point to the buffer, not to a heap allocation.
// Expected: pp -> {buf_object}
#include <new>
struct Point { int x; int y; };

alignas(Point) char buf[sizeof(Point)];
Point *pp;

int main()
{
    pp = new (buf) Point;   /* placement new: no heap allocation */
    pp->x = 1;
    pp->y = 2;
    pp->~Point();
    return pp->x + pp->y;
}
