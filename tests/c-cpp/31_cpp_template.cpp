// 31_cpp_template.cpp
// Instantiated template: getAddr<T> returns &v.
// Expected: p -> {x},  q -> {y}
template<typename T>
T *getAddr(T &v) { return &v; }

int    x = 10;
double y = 3.14;

int    *p;
double *q;

int main()
{
    p = getAddr(x);
    q = getAddr(y);
    return *p;
}
