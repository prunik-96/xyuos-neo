#include <stdio.h>

class Greeter {
public:
    Greeter(const char *name) : name_(name) {}
    virtual ~Greeter() {}
    virtual void greet() {
        printf("Hello, %s! (virtual call from C++ on xyuOS Neo)\n", name_);
    }

protected:
    const char *name_;
};

class LoudGreeter : public Greeter {
public:
    LoudGreeter(const char *name) : Greeter(name) {}
    void greet() override {
        printf("HELLO, %s!! (overridden virtual call)\n", name_);
    }
};

int main() {
    printf("-- C++ demo: classes, inheritance, virtuals, new/delete --\n");

    Greeter *g1 = new Greeter("xyuOS");
    Greeter *g2 = new LoudGreeter("Neo");

    g1->greet();
    g2->greet();

    delete g1;
    delete g2;

    printf("C++ demo done.\n");
    return 0;
}
