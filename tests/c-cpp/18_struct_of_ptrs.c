/* 18_struct_of_ptrs.c
 * Struct whose two fields are pointers to different globals.
 * Expected: pair.first -> {x}, pair.second -> {y}
 */
int x = 10, y = 20;

struct PairPtrs {
    int *first;
    int *second;
};

struct PairPtrs pair;

int main(void)
{
    pair.first  = &x;
    pair.second = &y;
    return *pair.first + *pair.second;
}
