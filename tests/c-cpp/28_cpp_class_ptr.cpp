// 28_cpp_class_ptr.cpp
// Class containing a raw pointer member initialised through the constructor.
// Expected: b.data -> {val}
int val = 99;

class Box {
public:
    int *data;
    explicit Box(int *d) : data(d) {}
    int get() const { return *data; }
};

Box *bp;

int main()
{
    Box b(&val);
    bp = &b;
    return bp->get();
}
