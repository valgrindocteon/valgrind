
/* HOW TO COMPILE:

 * 32bit build:
   gcc -Winline -Wall -g -O -mregnames -maltivec
 * 64bit build:
   gcc -Winline -Wall -g -O -mregnames -maltivec -m64


 * jm_insns_isa_2_07.c:
 * PPC tests for the ISA 2.07.  This file is based on the
 * jm-insns.c file for the new instructions in the ISA 2.07.  The
 * test structure has been kept the same as the original file to
 * the extent possible.
 *
 * Copyright (C) 2013 IBM
 *
 *   Authors: Carl Love <carll@us.ibm.com>
 *            Maynard Johnson <maynardj@us.ibm.com>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation; either version 2 of the
 *   License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*
 * Operation details
 * -----------------
 *
 * The 'loops' (e.g. int_loops) do the actual work:
 *  - loops over as many arguments as the insn needs (regs | imms)
 *     - sets up the environment (reset cr,xer, assign src regs...)
 *     - maybe modifies the asm instn to test different imm args
 *     - calls the test function
 *     - retrieves relevant register data (rD,cr,xer,...)
 *     - prints argument and result data.
 *
 * More specifically...
 *
 * all_tests[i] holds insn tests
 *  - of which each holds: {instn_test_arr[], description, flags}
 *
 * flags hold 3 instn classifiers: {family, type, arg_type}
 *
 * // The main test loop:
 * do_tests( user_ctl_flags ) {
 *    foreach(curr_test = all_test[i]) {
 *
 *       // flags are used to control what tests are run:
 *       if (curr_test->flags && !user_ctl_flags)
 *          continue;
 *
 *       // a 'loop_family_arr' is chosen based on the 'family' flag...
 *       switch(curr_test->flags->family) {
 *       case x: loop_family_arr = int_loops;
 *      ...
 *       }
 *
 *       // ...and the actual test_loop to run is found by indexing into
 *       // the loop_family_arr with the 'arg_type' flag:
 *       test_loop = loop_family[curr_test->flags->arg_type]
 *
 *       // finally, loop over all instn tests for this test:
 *       foreach (instn_test = curr_test->instn_test_arr[i]) {
 *
 *          // and call the test_loop with the current instn_test function,name
 *          test_loop( instn_test->func, instn_test->name )
 *       }
 *    }
 * }
 *
 */


/**********************************************************************/

/* Uncomment to enable output of CR flags for float tests */
//#define TEST_FLOAT_FLAGS

/* Uncomment to enable debug output */
//#define DEBUG_ARGS_BUILD
//#define DEBUG_FILTER

/**********************************************************************/
#include <stdio.h>

#ifdef HAS_ISA_2_07

#include "config.h"
#include <altivec.h>
#include <stdint.h>

#include <assert.h>
#include <ctype.h>     // isspace
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    // getopt

#if !defined (__TEST_PPC_H__)
#define __TEST_PPC_H__

#include "tests/sys_mman.h"
#include "tests/malloc.h"       // memalign16

#define STATIC_ASSERT(e) sizeof(struct { int:-!(e); })

/* Something of the same size as void*, so can be safely be coerced
 * to/from a pointer type. Also same size as the host's gp registers.
 * According to the AltiVec section of the GCC manual, the syntax does
 * not allow the use of a typedef name as a type specifier in conjunction
 * with the vector keyword, so typedefs uint[32|64]_t are #undef'ed here
 * and redefined using #define.
 */
#undef uint32_t
#undef uint64_t
#define uint32_t unsigned int
#define uint64_t unsigned long long int

#ifndef __powerpc64__
typedef uint32_t  HWord_t;
#else
typedef uint64_t  HWord_t;
#endif /* __powerpc64__ */

typedef uint64_t Word_t;

enum {
    compile_time_test1 = STATIC_ASSERT(sizeof(uint32_t) == 4),
    compile_time_test2 = STATIC_ASSERT(sizeof(uint64_t) == 8),
};

#define ALLCR "cr0","cr1","cr2","cr3","cr4","cr5","cr6","cr7"

#define SET_CR(_arg) \
      __asm__ __volatile__ ("mtcr  %0" : : "b"(_arg) : ALLCR );

#define SET_XER(_arg) \
      __asm__ __volatile__ ("mtxer %0" : : "b"(_arg) : "xer" );

#define GET_CR(_lval) \
      __asm__ __volatile__ ("mfcr %0"  : "=b"(_lval) )

#define GET_XER(_lval) \
      __asm__ __volatile__ ("mfxer %0" : "=b"(_lval) )

#define GET_CR_XER(_lval_cr,_lval_xer) \
   do { GET_CR(_lval_cr); GET_XER(_lval_xer); } while (0)

#define SET_CR_ZERO \
      SET_CR(0)

#define SET_XER_ZERO \
      SET_XER(0)

#define SET_CR_XER_ZERO \
   do { SET_CR_ZERO; SET_XER_ZERO; } while (0)

#define SET_FPSCR_ZERO \
   do { double _d = 0.0; \
        __asm__ __volatile__ ("mtfsf 0xFF, %0" : : "f"(_d) ); \
   } while (0)

#define DEFAULT_VSCR 0x0

static vector unsigned long long vec_out, vec_inA, vec_inB;

/* XXXX these must all be callee-save regs! */
register double f14 __asm__ ("fr14");
register double f15 __asm__ ("fr15");
register double f16 __asm__ ("fr16");
register double f17 __asm__ ("fr17");
register HWord_t r14 __asm__ ("r14");
register HWord_t r17 __asm__ ("r17");

typedef void (*test_func_t) (void);
typedef struct _test test_t;
typedef struct _test_table test_table_t;
struct _test {
    test_func_t func;
    const char *name;
};

struct _test_table {
    test_t *tests;
    const char *name;
    uint32_t flags;
};

typedef void (*test_loop_t) (const char *name, test_func_t func,
                             uint32_t flags);

enum test_flags {
    /* Nb arguments */
    PPC_ONE_ARG    = 0x00000001,
    PPC_TWO_ARGS   = 0x00000002,
    PPC_THREE_ARGS = 0x00000003,
    PPC_CMP_ARGS   = 0x00000004,  // family: compare
    PPC_CMPI_ARGS  = 0x00000005,  // family: compare
    PPC_TWO_I16    = 0x00000006,  // family: arith/logical
    PPC_SPECIAL    = 0x00000007,  // family: logical
    PPC_LD_ARGS    = 0x00000008,  // family: ldst
    PPC_LDX_ARGS   = 0x00000009,  // family: ldst
    PPC_ST_ARGS    = 0x0000000A,  // family: ldst
    PPC_STX_ARGS   = 0x0000000B,  // family: ldst
    PPC_STQ_ARGS   = 0x0000000C,  // family: ldst, two args, imm
    PPC_LDQ_ARGS   = 0x0000000D,  // family: ldst, two args, imm
    PPC_STQX_ARGS  = 0x0000000E,  // family: ldst, three args
    PPC_LDQX_ARGS  = 0x0000000F,  // family: ldst, three_args
    PPC_NB_ARGS    = 0x0000000F,
    /* Type */
    PPC_ARITH      = 0x00000100,
    PPC_LOGICAL    = 0x00000200,
    PPC_COMPARE    = 0x00000300,
    PPC_CROP       = 0x00000400,
    PPC_LDST       = 0x00000500,
    PPC_POPCNT     = 0x00000600,
    PPC_MOV        = 0x00000A00,
    PPC_TYPE       = 0x00000F00,
    /* Family */
    PPC_INTEGER    = 0x00010000,
    PPC_FLOAT      = 0x00020000,
    PPC_405        = 0x00030000,  // Leave so we keep numbering consistent
    PPC_ALTIVEC    = 0x00040000,
    PPC_FALTIVEC   = 0x00050000,
    PPC_FAMILY     = 0x000F0000,
    /* Flags: these may be combined, so use separate bitfields. */
    PPC_CR         = 0x01000000,
    PPC_XER_CA     = 0x02000000,
};

#endif /* !defined (__TEST_PPC_H__) */

/* -------------- END #include "test-ppc.h" -------------- */


#if defined (DEBUG_ARGS_BUILD)
#define AB_DPRINTF(fmt, args...) do { fprintf(stderr, fmt , ##args); } while (0)
#else
#define AB_DPRINTF(fmt, args...) do { } while (0)
#endif

#if defined (DEBUG_FILTER)
#define FDPRINTF(fmt, args...) do { fprintf(stderr, fmt , ##args); } while (0)
#else
#define FDPRINTF(fmt, args...) do { } while (0)
#endif

#define unused __attribute__ (( unused ))

typedef struct special {
   const char *name;
   void (*test_cb)(const char* name, test_func_t func,
                   unused uint32_t test_flags);
} special_t;


// VSX move instructions
static void test_mfvsrd (void)
{
   __asm__ __volatile__ ("mfvsrd %0,%x1" : "=r" (r14) : "ws" (vec_inA));
};

static void test_mtvsrd (void)
{
   __asm__ __volatile__ ("mtvsrd %x0,%1" : "=ws" (vec_out) : "r" (r14));
};

static void test_mtfprwa (void)
{
   __asm__ __volatile__ ("mtfprwa %x0,%1" : "=ws" (vec_out) : "r" (r14));
};

static test_t tests_move_ops_spe[] = {
  { &test_mfvsrd          , "mfvsrd" },
  { &test_mtvsrd          , "mtvsrd" },
  { &test_mtfprwa         , "mtfprwa" },
  { NULL,                   NULL }
};

/* Vector Double Word tests.
 * NOTE: Since these are "vector" instructions versus VSX, we must use
 * vector constraints. */
static void test_vaddudm (void)
{
   __asm__ __volatile__ ("vaddudm %0, %1, %2" : "=v" (vec_out): "v" (vec_inA),"v" (vec_inB));
}

static void test_vpkudum (void)
{
   __asm__ __volatile__ ("vpkudum %0, %1, %2" : "=v" (vec_out): "v" (vec_inA),"v" (vec_inB));
}

static test_t tests_aa_dbl_ops_two[] = {
  { &test_vaddudm         , "vaddudm", },
  { &test_vpkudum         , "vpkudum",  },
  { NULL,                   NULL,           },
};

static int verbose = 0;
static int arg_list_size = 0;
static unsigned long long * vdargs = NULL;
#define NB_VDARGS 4

static void build_vdargs_table (void)
{
   // Each VSX register holds two doubleword integer values
   vdargs = memalign16(NB_VDARGS * sizeof(unsigned long long));
   vdargs[0] = 0x0102030405060708ULL;
   vdargs[1] = 0x090A0B0C0E0D0E0FULL;
   vdargs[2] = 0xF1F2F3F4F5F6F7F8ULL;
   vdargs[3] = 0xF9FAFBFCFEFDFEFFULL;
}

static int check_filter (char *filter)
{
   char *c;
   int ret = 1;

   if (filter != NULL) {
      c = strchr(filter, '*');
      if (c != NULL) {
         *c = '\0';
         ret = 0;
      }
   }
   return ret;
}

static int check_name (const char* name, const char *filter,
                       int exact)
{
   int nlen, flen;
   int ret = 0;

   if (filter != NULL) {
      for (; isspace(*name); name++)
         continue;
      FDPRINTF("Check '%s' againt '%s' (%s match)\n",
               name, filter, exact ? "exact" : "starting");
      nlen = strlen(name);
      flen = strlen(filter);
      if (exact) {
         if (nlen == flen && memcmp(name, filter, flen) == 0)
            ret = 1;
      } else {
         if (flen <= nlen && memcmp(name, filter, flen) == 0)
            ret = 1;
      }
   } else {
      ret = 1;
   }
   return ret;
}


typedef struct insn_sel_flags_t_struct {
   int one_arg, two_args, three_args;
   int arith, logical, compare, ldst;
   int integer, floats, altivec, faltivec;
   int cr;
} insn_sel_flags_t;


static void mfvs(const char* name, test_func_t func,
                 unused uint32_t test_flags)
{
   /* This test is for move instructions where the input is a scalar register
    * and the destination is a vector register.
    */
   int i;
   volatile Word_t result;

   for (i=0; i < NB_VDARGS; i++) {
      r14 = 0ULL;
      vec_inA = (vector unsigned long long){ vdargs[i], 0ULL };

      (*func)();
      result = r14;
      printf("%s: %016llx => %016llx\n", name, vdargs[i], result);
   }
}

static void mtvs(const char* name, test_func_t func,
                 unused uint32_t test_flags)
{
   /* This test is for move instructions where the input is a scalar register
    * and the destination is a vector register.
    */
   unsigned long long *dst;
   int i;

   for (i=0; i < NB_VDARGS; i++) {
      r14  = vdargs[i];
      vec_out = (vector unsigned long long){ 0ULL, 0ULL };

      (*func)();
      dst = (unsigned long long *) &vec_out;
      printf("%s: %016llx => %016llx\n", name, vdargs[i], *dst);
   }
}

static void mtvs2s(const char* name, test_func_t func,
                 unused uint32_t test_flags)
{
   /* This test is the mtvsrwa instruction.
    */
   unsigned long long *dst;
   int i;

   for (i=0; i < NB_VDARGS; i++) {
      // Only the lower half of the vdarg doubleword arg will be used as input by mtvsrwa
      unsigned int * src = (unsigned int *)&vdargs[i];
      src++;
      r14  = vdargs[i];
      vec_out = (vector unsigned long long){ 0ULL, 0ULL };

      (*func)();
      // Only doubleword 0 is used in output
      dst = (unsigned long long *) &vec_out;
      printf("%s: %08x => %016llx\n", name, *src, *dst);
   }
}

static void test_special (special_t *table,
                          const char* name, test_func_t func,
                          unused uint32_t test_flags)
{
   const char *tmp;
   int i;

   for (tmp = name; isspace(*tmp); tmp++)
      continue;
   for (i=0; table[i].name != NULL; i++) {
      if (strcmp(table[i].name, tmp) == 0) {
         (*table[i].test_cb)(name, func, test_flags);
         return;
      }
   }
   fprintf(stderr, "ERROR: no test found for op '%s'\n", name);
}

static special_t special_move_ops[] = {
   {
      "mfvsrd",  /* move from vector to scalar reg */
      &mfvs,
   },
   {
      "mtvsrd",  /* move from scalar to vector reg */
      &mtvs,
   },
   {
      "mtfprwa", /* (extended mnemonic for mtvsrwa) move from scalar to vector reg with two’s-complement */
      &mtvs2s,
   },
};

static void test_move_special(const char* name, test_func_t func,
                                uint32_t test_flags)
{
   test_special(special_move_ops, name, func, test_flags);
}

/* Vector Double Word tests */

static void test_av_dint_two_args (const char* name, test_func_t func,
                                   unused uint32_t test_flags)
{

   unsigned long long * dst;
   unsigned int * dst_int;
   int i,j;
   int is_vpkudum;
   if (strcmp(name, "vpkudum") == 0)
      is_vpkudum = 1;
   else
      is_vpkudum = 0;

   for (i = 0; i < NB_VDARGS; i+=2) {
      vec_inA = (vector unsigned long long){ vdargs[i], vdargs[i+1] };
      for (j = 0; j < NB_VDARGS; j+=2) {
         vec_inB = (vector unsigned long long){ vdargs[j], vdargs[j+1] };
         vec_out = (vector unsigned long long){ 0,0 };

         (*func)();
         dst_int = (unsigned int *)&vec_out;
         dst  = (unsigned long long*)&vec_out;

         printf("%s: ", name);

         if (is_vpkudum) {
            printf("Inputs: %08llx %08llx %08llx %08llx\n", vdargs[i] & 0x00000000ffffffffULL,
                   vdargs[i+1] & 0x00000000ffffffffULL, vdargs[j] & 0x00000000ffffffffULL,
                   vdargs[j+1] & 0x00000000ffffffffULL);
            printf("         Output: %08x %08x %08x %08x\n", dst_int[0], dst_int[1],
                   dst_int[2], dst_int[3]);
         } else {
            printf("%016llx @@ %016llx, ", vdargs[i], vdargs[j]);
            printf(" ==> %016llx\n", dst[0]);
            printf("\t%016llx @@ %016llx, ", vdargs[i+1], vdargs[j+1]);
            printf(" ==> %016llx\n", dst[1]);
         }
      }
   }
}

/* Used in do_tests, indexed by flags->nb_args
   Elements correspond to enum test_flags::num args
*/
static test_loop_t altivec_mov_loops[] = {
   &test_move_special,
   NULL
};

static test_loop_t altivec_dint_loops[] = {
   &test_av_dint_two_args,
   NULL
};

static test_table_t all_tests[] = {
   {
       tests_move_ops_spe,
       "PPC VSR special move insns",
       PPC_ALTIVEC | PPC_MOV | PPC_ONE_ARG,
   },
   {
       tests_aa_dbl_ops_two,
       "PC altivec double word integer insns with two args",
       PPC_ALTIVEC | PPC_ARITH | PPC_TWO_ARGS,
   },
   { NULL,                   NULL,               0x00000000, },
};

static void do_tests ( insn_sel_flags_t seln_flags,
                       char *filter)
{
   test_loop_t *loop;
   test_t *tests;
   int nb_args, type, family;
   int i, j, n;
   int exact;

   exact = check_filter(filter);
   n = 0;
   for (i=0; all_tests[i].name != NULL; i++) {
      nb_args = all_tests[i].flags & PPC_NB_ARGS;

      /* Check number of arguments */
      if ((nb_args == 1 && !seln_flags.one_arg) ||
          (nb_args == 2 && !seln_flags.two_args) ||
          (nb_args == 3 && !seln_flags.three_args)){
         continue;
      }
      /* Check instruction type */
      type = all_tests[i].flags & PPC_TYPE;
      if ((type == PPC_ARITH   && !seln_flags.arith)   ||
          (type == PPC_LOGICAL && !seln_flags.logical) ||
          (type == PPC_COMPARE && !seln_flags.compare) ||
          (type == PPC_LDST && !seln_flags.ldst)       ||
          (type == PPC_MOV && !seln_flags.ldst)       ||
          (type == PPC_POPCNT && !seln_flags.arith)) {
         continue;
      }

      /* Check instruction family */
      family = all_tests[i].flags & PPC_FAMILY;
      if ((family == PPC_INTEGER  && !seln_flags.integer) ||
          (family == PPC_FLOAT    && !seln_flags.floats)  ||
          (family == PPC_ALTIVEC && !seln_flags.altivec)  ||
          (family == PPC_FALTIVEC && !seln_flags.faltivec)) {
         continue;
      }
      /* Check flags update */
      if (((all_tests[i].flags & PPC_CR)  && seln_flags.cr == 0) ||
          (!(all_tests[i].flags & PPC_CR) && seln_flags.cr == 1))
         continue;

      /* All passed, do the tests */
      tests = all_tests[i].tests;

      loop = NULL;

      /* Select the test loop */
      switch (family) {
      case PPC_INTEGER:
         printf("Currently there are no integer tests enabled in this testsuite.\n");
         break;

      case PPC_FLOAT:
         printf("Currently there are no float tests enabled in this testsuite.\n");
         break;

      case PPC_ALTIVEC:
         switch (type) {
            case PPC_MOV:
               loop = &altivec_mov_loops[0];
               break;
            case PPC_ARITH:
               loop = &altivec_dint_loops[0];
               break;
            default:
               printf("No altivec test defined for type %x\n", type);
         }
         break;

      case PPC_FALTIVEC:
         printf("Currently there are no floating altivec tests in this testsuite.\n");
         break;

      default:
         printf("ERROR: unknown insn family %08x\n", family);
         continue;
      }
      if (1 || verbose > 0)
      for (j=0; tests[j].name != NULL; j++) {
         if (check_name(tests[j].name, filter, exact)) {
            if (verbose > 1)
               printf("Test instruction %s\n", tests[j].name);
            if (loop != NULL)
               (*loop)(tests[j].name, tests[j].func, all_tests[i].flags);
            printf("\n");
            n++;
         }
        }
      if (verbose) printf("\n");
   }
   printf("All done. Tested %d different instructions\n", n);
}


static void usage (void)
{
   fprintf(stderr,
           "Usage: jm-insns [OPTION]\n"
           "\t-i: test integer instructions (default)\n"
           "\t-f: test floating point instructions\n"
           "\t-a: test altivec instructions\n"
           "\t-A: test all (int, fp, altivec) instructions\n"
           "\t-v: be verbose\n"
           "\t-h: display this help and exit\n"
           );
}

#endif

int main (int argc, char **argv)
{
#ifdef HAS_ISA_2_07
   /* Simple usage:
      ./jm-insns -i   => int insns
      ./jm-insns -f   => fp  insns
      ./jm-insns -a   => av  insns
      ./jm-insns -A   => int, fp and avinsns
   */
   char *filter = NULL;
   insn_sel_flags_t flags;
   int c;

   // Args
   flags.one_arg    = 1;
   flags.two_args   = 1;
   flags.three_args = 1;
   // Type
   flags.arith      = 1;
   flags.logical    = 1;
   flags.compare    = 1;
   flags.ldst       = 1;
   // Family
   flags.integer    = 0;
   flags.floats     = 0;
   flags.altivec    = 0;
   flags.faltivec   = 0;
   // Flags
   flags.cr         = 2;

   while ((c = getopt(argc, argv, "ifahvA")) != -1) {
      switch (c) {
      case 'i':
         flags.integer  = 1;
         break;
      case 'f':
         flags.floats   = 1;
         break;
      case 'a':
         flags.altivec  = 1;
         flags.faltivec = 1;
         break;
      case 'A':
         flags.integer  = 1;
         flags.floats   = 1;
         flags.altivec  = 1;
         flags.faltivec = 1;
         break;
      case 'h':
         usage();
         return 0;
      case 'v':
         verbose++;
         break;
      default:
         usage();
         fprintf(stderr, "Unknown argument: '%c'\n", c);
         return 1;
      }
   }

   arg_list_size = 0;

   build_vdargs_table();
   if (verbose > 1) {
      printf("\nInstruction Selection:\n");
      printf("  n_args: \n");
      printf("    one_arg    = %d\n", flags.one_arg);
      printf("    two_args   = %d\n", flags.two_args);
      printf("    three_args = %d\n", flags.three_args);
      printf("  type: \n");
      printf("    arith      = %d\n", flags.arith);
      printf("    logical    = %d\n", flags.logical);
      printf("    compare    = %d\n", flags.compare);
      printf("    ldst       = %d\n", flags.ldst);
      printf("  family: \n");
      printf("    integer    = %d\n", flags.integer);
      printf("    floats     = %d\n", flags.floats);
      printf("    altivec    = %d\n", flags.altivec);
      printf("    faltivec   = %d\n", flags.faltivec);
      printf("  cr update: \n");
      printf("    cr         = %d\n", flags.cr);
      printf("\n");
   }

   do_tests( flags, filter );
#else
   printf("NO ISA 2.07 SUPPORT\n");
#endif
   return 0;
}
