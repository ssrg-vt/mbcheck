// 26_cpp_new.cpp
// Heap allocation via C++ new; after delete the pointer is nulled.
// Expected: p -> {heap_int_object}
int *p;

int main()
{
    p = new int(42);
    delete p;
    p = nullptr;
    return 0;
}
