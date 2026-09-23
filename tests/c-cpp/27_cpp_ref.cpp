// 27_cpp_ref.cpp
// C++ reference is modelled as a pointer internally by SVF.
// Expected: the reference variable points to x (same as &x).
int x = 10;

int main()
{
    int &r = x;   /* SVF treats r as a pointer to x */
    r = 20;
    return r;
}
