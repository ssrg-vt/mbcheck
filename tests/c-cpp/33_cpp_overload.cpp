// 33_cpp_overload.cpp
// Overloaded functions that return pointers to different typed globals.
// Expected: p -> {x},  q -> {y}
int   x = 1;
float y = 2.0f;

int   *getPtr(int   &v) { return &v; }
float *getPtr(float &v) { return &v; }

int   *p;
float *q;

int main()
{
    p = getPtr(x);
    q = getPtr(y);
    return *p;
}
