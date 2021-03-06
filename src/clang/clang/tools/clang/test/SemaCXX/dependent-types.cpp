// RUN: clang-cc -fsyntax-only -pedantic -verify %s

template<typename T, int Size> void f() {
  T x1;
  T* x2;
  T& x3; // expected-error{{declaration of reference variable 'x3' requires an initializer}}
  T x4[]; // expected-error{{variable has incomplete type 'T []'}}
  T x5[Size];
  int x6[Size];
}
