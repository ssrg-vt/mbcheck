// 32_cpp_this_ptr.cpp
// 'this' pointer inside a method points to the calling object.
// Expected: the implicit 'this' inside inc()/get() -> {c_obj}
class Counter {
    int count;
public:
    Counter() : count(0) {}
    void inc()       { count++; }
    int  get() const { return count; }
};

Counter *gcp;

int main()
{
    Counter c;
    gcp = &c;
    gcp->inc();
    gcp->inc();
    return gcp->get();
}
