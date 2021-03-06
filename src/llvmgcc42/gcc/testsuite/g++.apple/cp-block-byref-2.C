/* APPLE LOCAL file radar 6237086  */
/* { dg-do compile } */
/* { dg-options "-g -O0 -dA -mmacosx-version-min=10.6" }*/
/* { dg-final { scan-assembler "DW_OP_fbreg+\[ \t\n]+\[ \t]\\.byte+\[ \t]0x\[0-9a-f]+\[ \t]+\[#;@]\[ \t]sleb128 -\[0-9]+\[ \t\n]+\[ \t]\\.byte+\[ \t]0x6+\[ \t]+\[#;@]\[ \t]DW_OP_deref+\[ \t\n]+\[ \t]\\.byte+\[ \t]\[0x6;0x23]+\[ \t]+\[#;@]\[ \t]\[DW_OP_deref;DW_OP_plus_uconst]" } } */

#include <stdio.h>

int 
main ()
{
  int val_1, val_2;

  int (^my_block) (int);

  if (1)
    {
      __block int use_by_ref = 1;
      int use_by_value = 0xfefefefe;
      
      my_block = ^ (int in_value) {
	use_by_ref++;
	printf ("Block function has: %d %d %d.\n", in_value, use_by_value, use_by_ref);
	return in_value + use_by_ref + use_by_value;
      };
      use_by_ref++;
      val_1 = my_block (10);
    }
      
  val_1 = my_block (20);
  val_2 = my_block (30);
  printf ("Got: %d and %d.\n", val_1, val_2);
  
  return 0;
}
