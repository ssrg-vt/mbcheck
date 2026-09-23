// 34_cpp_lambda.cpp
// Lambda capturing a pointer by value.
// The capture copy is a separate variable but holds the same points-to set.
// Expected: p -> {x}, and the lambda's captured copy also -> {x}
int x = 10;
int *gp;

int main()
{
    int *p = &x;
    // Lambda captures p by value; SVF models the capture as a struct field.
    auto f = [p]() -> int { return *p; };
    gp = p;
    return f();
}
