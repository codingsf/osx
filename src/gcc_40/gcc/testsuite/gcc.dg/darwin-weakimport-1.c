/* { dg-do compile { target *-*-darwin* } } */
/* { dg-require-weak "" } */
/* { dg-options "-fno-common" } */

/* { dg-final { scan-assembler "weak_reference _a" } } */
/* { dg-final { scan-assembler-not "weak_\[a-z \t\]*_b" } } */

extern void a (void) __attribute__((weak_import));
extern void b (void) __attribute__((weak_import));

void b(void)
{
  a();
}

/* APPLE LOCAL put in 4.1 */
