// RUN: %clang_cc1 -fsyntax-only -verify %s 

// Verify that we can't initialize non-aggregates with an initializer
// list.
struct NonAggr1 {
  NonAggr1(int) { }

  int m;
};

struct Base { };
struct NonAggr2 : public Base {
  int m;
};

class NonAggr3 {
  int m;
};

struct NonAggr4 {
  int m;
  virtual void f();
};

NonAggr1 na1 = { 17 }; // expected-error{{non-aggregate type 'NonAggr1' cannot be initialized with an initializer list}}
NonAggr2 na2 = { 17 }; // expected-error{{non-aggregate type 'NonAggr2' cannot be initialized with an initializer list}}
NonAggr3 na3 = { 17 }; // expected-error{{non-aggregate type 'NonAggr3' cannot be initialized with an initializer list}}
NonAggr4 na4 = { 17 }; // expected-error{{non-aggregate type 'NonAggr4' cannot be initialized with an initializer list}}

// PR5817
typedef int type[][2];
const type foo = {0};

// Vector initialization.
typedef short __v4hi __attribute__ ((__vector_size__ (8)));
__v4hi v1 = { (void *)1, 2, 3 }; // expected-error {{cannot initialize a vector element of type 'short' with an rvalue of type 'void *'}}

// Array initialization.
int a[] = { (void *)1 }; // expected-error {{cannot initialize an array element of type 'int' with an rvalue of type 'void *'}}

// Struct initialization.
struct S { int a; } s = { (void *)1 }; // expected-error {{cannot initialize a member subobject of type 'int' with an rvalue of type 'void *'}}

// Check that we're copy-initializing the structs.
struct A {
  A();
  A(int);
  ~A();

private:
  A(const A&) {} // expected-note 4 {{declared private here}}
};

struct B {
  A a;
};

struct C {
  const A& a;
};

void f() {
  A as1[1] = { };
  A as2[1] = { 1 }; // expected-error {{calling a private constructor of class 'A'}} expected-warning {{requires an accessible copy constructor}}

  B b1 = { };
  B b2 = { 1 }; // expected-error {{field of type 'A' has private copy constructor}} expected-warning {{requires an accessible copy constructor}}
  
  C c1 = { 1 };
}

class Agg {
public:
  int i, j;
};

class AggAgg {
public:
  Agg agg1;
  Agg agg2;
};

AggAgg aggagg = { 1, 2, 3, 4 };
