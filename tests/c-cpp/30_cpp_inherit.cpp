// 30_cpp_inherit.cpp
// Pointer upcast: a Derived object is stored via a Base pointer.
// Expected: gp -> {dog_obj}  (dog_obj is a Derived/Dog instance)
class Animal {
public:
    int id;
    explicit Animal(int i) : id(i) {}
};

class Dog : public Animal {
public:
    explicit Dog(int i) : Animal(i) {}
};

Animal *gp;

int main()
{
    Dog d(5);
    gp = &d;            /* upcast: Dog* -> Animal* */
    return gp->id;
}
