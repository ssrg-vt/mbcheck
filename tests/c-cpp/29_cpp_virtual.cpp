// 29_cpp_virtual.cpp
// Virtual dispatch: base pointer holds a derived object.
// SVF uses CHA/type-based analysis to resolve virtual calls.
// Expected: bp -> {Derived_heap_object}
class Base {
public:
    virtual int compute() { return 0; }
    virtual ~Base() {}
};

class Derived : public Base {
public:
    int compute() override { return 42; }
};

Base *bp;

int main()
{
    bp = new Derived();
    int r = bp->compute();
    delete bp;
    return r;
}
