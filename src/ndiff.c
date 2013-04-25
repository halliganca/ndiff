/*
 o---------------------------------------------------------------------o
 |
 | Numdiff
 |
 | Copyright (c) 2012+ laurent.deniau@cern.ch
 | Gnu General Public License
 |
 o---------------------------------------------------------------------o
  
   Purpose:
     numerical diff of files
     provides the main numdiff loop
 
 o---------------------------------------------------------------------o
*/

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>

#include "args.h"
#include "error.h"
#include "utils.h"
#include "ndiff.h"
#include "context.h"
#include "register.h"
#include "constraint.h"

#define T struct ndiff
#define C struct constraint

// ----- types

struct ndiff {
  // files
  FILE *lhs_f, *rhs_f;
  int   row_i,  col_i; // line, num-column

  // context
  struct context* cxt;

  // registers
  double *reg;
  int     reg_n;

  // options
  int blank, check;

  // diff counter
  int   cnt_i, max_i;

  // numbers counter
  long  num_i;

  // buffers
  int   lhs_i,  rhs_i; // char-columns
  int   buf_n;         // capacity
  char *lhs_b, *rhs_b;
};

// ----- private (parser helpers)

static inline int
is_separator (int c)
{
  return !c || isblank(c) || (ispunct(c) && !strchr(option.chr, c));
}

static inline int
is_number (char *buf)
{
  int i = 0;

  // sign
  if (buf[i] == '-' || buf[i] == '+' || buf[i] == ' ') i++;

  // dot
  if (buf[i] == '.') ++i;

  // digits
  return isdigit(buf[i]);
}

static inline char*
// assume that buf has been validated by is_number
backtrack_number (char *buf, const char *beg)
{
  if (*buf == ' ') ++buf;

  else
  if (*buf == '.') {
    if (buf > beg && (buf[-1] == '-' || buf[-1] == '+')) --buf;
  }

  else
  if (isdigit(*buf)) {
    if (buf > beg &&  buf[-1] == '.') --buf;
    if (buf > beg && (buf[-1] == '-' || buf[-1] == '+')) --buf;
  }

  return buf;
}

static inline int
// assume that buf has been validated by is_number and backtracked
is_number_start(char *buf, const char *beg)
{
  // number is at the beginning or is preceded by a separator
  return *buf == '-' || *buf == '+' || buf == beg || (buf > beg && is_separator(buf[-1]));
}

static inline int
parse_number (char *buf, int *d_, int *n_, int *e_, int *f_)
{
  int i = 0, d = 0, e = 0, n = 0;
  char c;

  // sign
  if (buf[i] == '-' || buf[i] == '+') i++;

  // drop leading zeros
  while(buf[i] == '0') i++;

  // digits
  while(isdigit(buf[i])) n++, i++;

  // dot
  if (buf[i] == '.') d = ++i;

  // decimals
  if (d) {
    // drop leading zeros
    if (!n) while(buf[i] == '0') i++;

    // digits
    while(isdigit(buf[i])) n++, i++;
  }

  // ensure at least ±# or ±#. or ±.#
  if(!(i > 0 && (isdigit(buf[i-1]) || (i > 1 &&  isdigit(buf[i-2])))))
    return 0;

  // exponent
  if (buf[i] == 'e' || buf[i] == 'E' || buf[i] == 'd' || buf[i] == 'D')
    c = buf[i], buf[i] = 'e', e = ++i;

  if (e) {
    // sign
    if (buf[i] == '-' || buf[i] == '+') i++;

    // digits
    while(isdigit(buf[i])) i++;

    // ensure e# or e±# otherwise backtrack
    if (!isdigit(buf[i-1]))
      i = e-1, buf[i] = c, e = 0;
  }

  if (n_) *n_ = n;
  if (d_) *d_ = d-1;
  if (e_) *e_ = e-1;
  if (f_) *f_ = d > 0 || e > 0;

  return i;
}

static inline void
skip_identifier(char *restrict *lhs, char *restrict *rhs, int strict)
{
  if (strict) {
    assert(lhs && rhs);
    while (**lhs == **rhs && !is_separator(**lhs)) ++*lhs, ++*rhs;
  }
  else {
    assert(lhs || rhs);
    if (lhs) while (!is_separator(**lhs)) ++*lhs;
    if (rhs) while (!is_separator(**rhs)) ++*rhs;
  }
}

static inline int
is_valid_omit(const char *lhs_p, const char *rhs_p, const T *dif, const char *tag)
{
  const char *p = tag+strlen(tag);

  while (--p >= tag && --lhs_p >= dif->lhs_b && --rhs_p >= dif->rhs_b)
    if (*p != *lhs_p || *p != *rhs_p) return false;

  return true;
}

// ----- private (ctor & dtor helpers)

static inline void
ndiff_reset_buf (T *dif)
{
  dif->lhs_i = dif->rhs_i = 0;
  dif->lhs_b[0] = dif->rhs_b[0] = 0;
}

static inline void
ndiff_setup (T *dif, int n, int r)
{
  enum { min_alloc = 65536, min_regs = 99 };

  if (n < min_alloc) n = min_alloc;
  if (r < min_regs ) r = min_regs;
  if (r > REG_MAX  ) r = REG_MAX;

  dif->lhs_b = malloc(n * sizeof *dif->lhs_b);
  dif->rhs_b = malloc(n * sizeof *dif->rhs_b);
  dif->reg   = malloc(r * sizeof *dif->reg  );
  ensure(dif->lhs_b && dif->rhs_b && dif->reg, "out of memory");

  dif->lhs_b[0] = 0;
  dif->rhs_b[0] = 0;
  memset(dif->reg, 0, r * sizeof *dif->reg);

  *dif = (T) {
    .lhs_f = dif->lhs_f, .rhs_f = dif->rhs_f,
    .lhs_b = dif->lhs_b, .rhs_b = dif->rhs_b,
    .blank = dif->blank, .check = dif->check,
    .max_i = dif->max_i,
    .reg = dif->reg, .reg_n = r,
    .cxt = dif->cxt,      
    .buf_n = n
  };
}

static inline void
ndiff_teardown (T *dif)
{
  free(dif->lhs_b);
  free(dif->rhs_b);
  free(dif->reg  );

  *dif = (T) {
    .lhs_f = dif->lhs_f, .rhs_f = dif->rhs_f,
    .blank = dif->blank, .check = dif->check,
    .cxt = dif->cxt
  };
}

static inline void
ndiff_grow (T *dif, int n)
{
  if (n > dif->buf_n) { // enlarge on need
    dif->lhs_b = realloc(dif->lhs_b, n * sizeof *dif->lhs_b);
    dif->rhs_b = realloc(dif->rhs_b, n * sizeof *dif->rhs_b);
    ensure(dif->lhs_b && dif->rhs_b, "out of memory");
    dif->buf_n = n;
  }
}

// ----- private (error & trace helpers)

static void
ndiff_error(const struct context *cxt,
            const C *c, const C *c2,
            int row, int col)
{
  warning("dual constraints differ at %d:%d", row, col);
  warning("getIncr select [#%d]", context_findIdx(cxt, c ));
  warning("getAt   select [#%d]", context_findIdx(cxt, c2));
  warning("rules list:");
  context_print(cxt, stderr);
  error("please report bug to mad@cern.ch");
}

static void
ndiff_header(void)
{
  if (option.test)
    warning("(*) files '%s'|'%s' from '%s' differ",
            option.lhs_file, option.rhs_file, option.test);
  else
    warning("(*) files '%s'|'%s' differ",
            option.lhs_file, option.rhs_file);
}

static void
ndiff_traceR(const T *dif, double abs, double _abs, double rel, double _rel, double dig, double _dig)
{
  trace("  abs=%.17g, _abs=%.17g, rel=%.17g, _rel=%.17g, dig=%.17g, _dig=%.17g",
           abs, _abs, rel, _rel, dig, _dig);
  trace("  R1=%.17g, R2=%.17g, R3=%.17g, R4=%.17g, R5=%.17g, "
           "R6=%.17g, R7=%.17g, R8=%.17g, R9=%.17g",
           dif->reg[1], dif->reg[2], dif->reg[3], dif->reg[4], dif->reg[5],
           dif->reg[6], dif->reg[7], dif->reg[8], dif->reg[9]);
}

// -----------------------------------------------------------------------------
// ----- interface
// -----------------------------------------------------------------------------

T*
ndiff_alloc (FILE *lhs_f, FILE *rhs_f, struct context *cxt, int n_, int r_)
{
  assert(lhs_f && rhs_f);

  T *dif = malloc(sizeof *dif);
  ensure(dif, "out of memory");

  *dif = (T) { .lhs_f = lhs_f, .rhs_f = rhs_f, .cxt = cxt };

  ndiff_setup(dif, n_, r_);
  return dif;
}

void
ndiff_free (T *dif)
{
  assert(dif);
  ndiff_teardown(dif);
  free(dif);
}

void
ndiff_clear (T *dif)
{
  assert(dif);
  int rn = dif->reg_n;
  ndiff_teardown(dif);
  ndiff_setup(dif, 0, rn);
}

int
ndiff_skipLine (T *dif)
{
  assert(dif);
  int s1 = 0, s2 = 0;
  int c1, c2;

  ndiff_reset_buf(dif);

  c1 = skipLine(dif->lhs_f, &s1);
  c2 = skipLine(dif->rhs_f, &s2);

  dif->col_i  = 0;
  dif->row_i += 1;

  return c1 == EOF || c2 == EOF ? EOF : !EOF;
}

int
ndiff_fillLine (T *dif, const char *lhs_b, const char *rhs_b)
{
  assert(dif);
  assert(lhs_b && rhs_b);

  ndiff_reset_buf(dif);

  int s1 = strlen(lhs_b)+1; 
  int s2 = strlen(rhs_b)+1; 
  ndiff_grow(dif, imax(s1,s2));
  memcpy(dif->lhs_b, lhs_b, s1);
  memcpy(dif->rhs_b, rhs_b, s2);

  dif->col_i  = 0;
  dif->row_i += 1;

  return 0; // never fails
}

int
ndiff_readLine (T *dif)
{
  assert(dif);
  int s1 = 0, s2 = 0;
  int c1, c2, n = 0;

  trace("->readLine line %d", dif->row_i);

  ndiff_reset_buf(dif);

  while (1) {
    c1 = readLine(dif->lhs_f, dif->lhs_b+s1, dif->buf_n-s1, &n); s1 += n;
    c2 = readLine(dif->rhs_f, dif->rhs_b+s2, dif->buf_n-s2, &n); s2 += n;
    if (c1 == '\n' || c2 == '\n' || c1 == EOF || c2 == EOF) break;
    ndiff_grow(dif, 2*dif->buf_n);
  }

  dif->col_i  = 0;
  dif->row_i += 1;

  trace("  buffers: '%.25s'|'%.25s'", dif->lhs_b, dif->rhs_b);
  trace("<-readLine line %d", dif->row_i);

  return c1 == EOF || c2 == EOF ? EOF : !EOF;
}

int
ndiff_outLine(T *dif, FILE *lhs_fp, FILE *rhs_fp)
{
  int c1=0, c2=0;

  if (lhs_fp) c1 = fprintf(lhs_fp, "%s\n", dif->lhs_b);
  if (rhs_fp) c2 = fprintf(rhs_fp, "%s\n", dif->rhs_b);

  return c1 == EOF || c2 == EOF ? EOF : !EOF;
}

int
ndiff_gotoLine (T *dif, const C *c)
{
  assert(dif && c);

  int c1=0, c2=0, i1=0, i2=0;

  trace("->gotoLine line %d", dif->row_i);

  // --- lhs ---
  while (1) {
    int s = 0, n = 0;

    dif->lhs_i    = 0;
    dif->lhs_b[0] = 0;

    if (c1 == EOF) break;

    while (1) {
      c1 = readLine(dif->lhs_f, dif->lhs_b+s, dif->buf_n-s, &n); s += n;
      if (c1 == '\n' || c1 == EOF) break;
      ndiff_grow(dif, 2*dif->buf_n);
    }

    i1 += 1;
    trace("  lhs[%d]: '%s'", dif->row_i+i1, dif->lhs_b);

    // search for tag
    if (strstr(dif->lhs_b, c->eps.tag)) break;
  }

  // --- rhs ---
  while (1) {
    int s = 0, n = 0;

    dif->rhs_i    = 0;
    dif->rhs_b[0] = 0;

    if (c2 == EOF) break;

    while (1) {
      c2 = readLine(dif->rhs_f, dif->rhs_b+s, dif->buf_n-s, &n); s += n;
      if (c2 == '\n' || c2 == EOF) break;
      ndiff_grow(dif, 2*dif->buf_n);
    }

    i2 += 1;
    trace("  rhs[%d]: '%s'", dif->row_i+i2, dif->rhs_b);

    // search for tag
    if (strstr(dif->rhs_b, c->eps.tag)) break;
  }

  dif->col_i  = 0;
  dif->row_i += i1 < i2 ? i1 : i2;

  // return with last lhs and rhs lines loaded if tag was found

  trace("  buffers: '%.25s'|'%.25s'", dif->lhs_b, dif->rhs_b);
  trace("<-gotoLine line %d (%+d|%+d)", dif->row_i, i1, i2);

  return c1 == EOF || c2 == EOF ? EOF : !EOF;
}

int
ndiff_gotoNum (T *dif, const C *c)
{
  assert(dif && c);

  trace("->gotoNum line %d", dif->row_i);

  int c1=0, c2=0, i1=0, i2=0;
  C _c = *c;

  if (c->eps.gto_reg)
    sprintf(_c.eps.tag, "%.17g", reg_getval(dif->reg, dif->reg_n, c->eps.gto_reg, 0));

  if ((c->eps.cmd & eps_equ) && slice_isFull(&c->col))
    return ndiff_gotoLine(dif, &_c);

  // --- lhs ---
  memcpy(dif->rhs_b, _c.eps.tag, sizeof _c.eps.tag);

  while (1) {
    int s = 0, n = 0;

    dif->lhs_i    = 0;
    dif->lhs_b[0] = 0;

    if (c1 == EOF) break;

    while (1) {
      c1 = readLine(dif->lhs_f, dif->lhs_b+s, dif->buf_n-s, &n); s += n;
      if (c1 == '\n' || c1 == EOF) break;
      ndiff_grow(dif, 2*dif->buf_n);
    }

    i1 += 1;
    trace("  lhs[%d]: '%s'", dif->row_i+i1, dif->lhs_b);

    // search for number
    int col = 0;
    for (dif->rhs_i=0; (col = ndiff_nextNum(dif, &_c)); dif->rhs_i=0) {
      if (slice_isElem(&_c.col, col)) {
        if (ndiff_testNum(dif, &_c) == 0) goto lhs_done;
      }
      else
        dif->lhs_i += parse_number(dif->lhs_b+dif->lhs_i, 0,0,0,0);
    }
  }
lhs_done: ;

  // --- rhs ---
  char tag[sizeof _c.eps.tag];
  memcpy(tag, dif->lhs_b, sizeof tag);
  memcpy(dif->lhs_b, _c.eps.tag, sizeof _c.eps.tag);
  int cmd = _c.eps.cmd | eps_swap; 
  _c.eps.cmd = (enum eps_cmd)cmd;

  while (1) {
    int s = 0, n = 0;

    dif->rhs_i    = 0;
    dif->rhs_b[0] = 0;

    if (c2 == EOF) break;

    while (1) {
      c2 = readLine(dif->rhs_f, dif->rhs_b+s, dif->buf_n-s, &n); s += n;
      if (c2 == '\n' || c2 == EOF) break;
      ndiff_grow(dif, 2*dif->buf_n);
    }

    i2 += 1;
    trace("  rhs[%d]: '%s'", dif->row_i+i2, dif->rhs_b);

    // search for number
    int col = 0;
    for (dif->lhs_i=0; (col = ndiff_nextNum(dif, &_c)); dif->lhs_i=0) {
      if (slice_isElem(&_c.col, col)) {
        if (ndiff_testNum(dif, &_c) == 0) goto rhs_done;
      }
      else
        dif->rhs_i += parse_number(dif->rhs_b+dif->rhs_i, 0,0,0,0);
    }
  }
rhs_done: ;
  memcpy(dif->lhs_b, tag, sizeof tag);

  dif->lhs_i  = 0;
  dif->rhs_i  = 0;
  dif->col_i  = 0;
  dif->row_i += i1 < i2 ? i1 : i2;

  // return with last lhs and rhs lines loaded

  trace("  buffers: '%.25s'|'%.25s'", dif->lhs_b, dif->rhs_b);
  trace("<-gotoNum line %d (%+d|%+d)", dif->row_i, i1, i2);

  return c1 == EOF || c2 == EOF ? EOF : !EOF;
}

int
ndiff_nextNum (T *dif, const C *c)
{
  assert(dif);

  char *restrict lhs_p = dif->lhs_b+dif->lhs_i;
  char *restrict rhs_p = dif->rhs_b+dif->rhs_i;

  trace("->nextNum  line %d, column %d, char-column %d|%d", dif->row_i, dif->col_i, dif->lhs_i, dif->rhs_i);
  trace("  strings: '%.25s'|'%.25s'", lhs_p, rhs_p);

  if (ndiff_isempty(dif)) goto quit_str;

retry:

  // search for digits
  if (c->eps.cmd & eps_istr) {
    while (*lhs_p && !isdigit(*lhs_p)) ++lhs_p;
    while (*rhs_p && !isdigit(*rhs_p)) ++rhs_p;
  }
  // search for difference or digits
  else {
    while (*lhs_p && *lhs_p == *rhs_p && !isdigit(*lhs_p))
      ++lhs_p, ++rhs_p;

    // skip whitespaces differences
    if (dif->blank && (isblank(*lhs_p) || isblank(*rhs_p))) {
      while (isblank(*lhs_p)) ++lhs_p;
      while (isblank(*rhs_p)) ++rhs_p;
      goto retry;
    }
  }

  // end-of-line
  if (!*lhs_p && !*rhs_p)
    goto quit_str;

  // difference in not-a-number
  if (*lhs_p != *rhs_p && (!is_number(lhs_p) || !is_number(rhs_p)))
    goto quit_diff;

  // backtrack numbers
  lhs_p = backtrack_number(lhs_p, dif->lhs_b);
  rhs_p = backtrack_number(rhs_p, dif->rhs_b);

  trace("  backtracking numbers '%.25s'|'%.25s'", lhs_p, rhs_p);

  // at the start of a number?
  if (!is_number_start(lhs_p, dif->lhs_b) || !is_number_start(rhs_p, dif->rhs_b)) {
    if (c->eps.cmd & eps_istr) {
      if (!is_number_start(lhs_p, dif->lhs_b)) skip_identifier(&lhs_p, 0, false);
      if (!is_number_start(rhs_p, dif->rhs_b)) skip_identifier(&rhs_p, 0, false);
    }
    else {
      int strict = true;
      if (c->eps.cmd & eps_omit)
        strict = !is_valid_omit(lhs_p, rhs_p, dif, c->eps.tag);
      int j = strict ? 0 : strlen(c->eps.tag);
      trace("  %s strings '%.25s'|'%.25s'", strict ? "skipping" : "omitting", lhs_p-j, rhs_p-j);
      skip_identifier(&lhs_p, &rhs_p, strict);
    }
    goto retry;
  }

  // numbers found
  dif->lhs_i = lhs_p-dif->lhs_b;
  dif->rhs_i = rhs_p-dif->rhs_b;
  trace("  strnums: '%.25s'|'%.25s'", lhs_p, rhs_p);
  trace("<-nextNum  line %d, column %d, char-column %d|%d", dif->row_i, dif->col_i, dif->lhs_i, dif->rhs_i);
  return ++dif->num_i, ++dif->col_i;

quit_diff:
  dif->lhs_i = lhs_p-dif->lhs_b+1;
  dif->rhs_i = rhs_p-dif->rhs_b+1;
  if (!(c->eps.cmd & eps_nofail) && ++dif->cnt_i <= dif->max_i) {
    if (dif->cnt_i == 1) ndiff_header();
    warning("(%d) files differ at line %d and char-columns %d|%d",
            dif->cnt_i, dif->row_i, dif->lhs_i, dif->rhs_i);
    warning("(%d) strings: '%.25s'|'%.25s'", dif->cnt_i, lhs_p, rhs_p);
  }
  if (c->eps.cmd & eps_onfail) context_onfail(dif->cxt, c);

quit_str:
  dif->lhs_i = lhs_p-dif->lhs_b+1;
  dif->rhs_i = rhs_p-dif->rhs_b+1;
  trace("<-nextNum  line %d, column %d, char-column %d|%d", dif->row_i, dif->col_i, dif->lhs_i, dif->rhs_i);
  return dif->col_i = 0;
}

int
ndiff_testNum (T *dif, const C *c)
{
  assert(dif && c);

  char *restrict lhs_p = dif->lhs_b+dif->lhs_i;
  char *restrict rhs_p = dif->rhs_b+dif->rhs_i;
  char *end=0;

  double lhs_d=0, rhs_d=0, scl_d=0, off_d=0, min_d=0, pow_d=0;
  double dif_d=0, err_d=0, abs_d=0, rel_d=0, dig_d=0;
  double abs=0, _abs=0, rel=0, _rel=0, dig=0, _dig=0;

  trace("->testNum  line %d, column %d, char-column %d|%d", dif->row_i, dif->col_i, dif->lhs_i, dif->rhs_i);
  trace("  strnums: '%.25s'|'%.25s'", lhs_p, rhs_p);

  // parse numbers
  int d1=0, d2=0, n1=0, n2=0, e1=0, e2=0, f1=0, f2=0;
  int l1 = parse_number(lhs_p, &d1, &n1, &e1, &f1);
  int l2 = parse_number(rhs_p, &d2, &n2, &e2, &f2);
  int ret = 0;

  // missing numbers (no eval)
  if (!l1 || !l2) {
    if ((c->eps.cmd & (eps_ign | eps_istr)) == (eps_ign | eps_istr))
      goto quit;

    ret |= eps_ign;
    goto quit_diff;
  }

  // load/interpret numbers
  lhs_d = reg_getval(dif->reg, dif->reg_n, c->eps.lhs_reg, c->eps.cmd & eps_lhs ? (end=lhs_p+l1, c->eps.lhs) : strtod(lhs_p, &end)); assert(lhs_p+l1 == end);
  rhs_d = reg_getval(dif->reg, dif->reg_n, c->eps.rhs_reg, c->eps.cmd & eps_rhs ? (end=rhs_p+l2, c->eps.rhs) : strtod(rhs_p, &end)); assert(rhs_p+l2 == end);
  scl_d = reg_getval(dif->reg, dif->reg_n, c->eps.scl_reg, c->eps.scl);
  off_d = reg_getval(dif->reg, dif->reg_n, c->eps.off_reg, c->eps.off);
  min_d = fmin(fabs(lhs_d),fabs(rhs_d));
  pow_d = pow10(-imax(n1, n2));

  // if one number is zero -> relative becomes absolute
  if (!(min_d > 0.0)) min_d = 1.0;

  // swap lhs and rhs (gtonum)
  if (c->eps.cmd & eps_swap) {
    double tmp = lhs_d; lhd_d = rhs_d; rhs_d = tmp;
  }

  // compute errors
  dif_d = lhs_d - rhs_d;
  err_d = scl_d * dif_d;
  abs_d = err_d + off_d;
  rel_d = abs_d/ min_d;
  dig_d = abs_d/(min_d*pow_d);

  trace("  abs=%.2g, rel=%.2g, ndig=%d", abs_d, rel_d, imax(n1, n2));   

  // ignore difference
  if (c->eps.cmd & eps_ign) {
    trace("  ignoring numbers '%.25s'|'%.25s'", lhs_p, rhs_p);
    goto quit;
  }

  // omit difference
  if (c->eps.cmd & eps_omit) {
    if (is_valid_omit(lhs_p, rhs_p, dif, c->eps.tag)) {
      trace("  omitting numbers '%.25s'|'%.25s'", lhs_p, rhs_p);
      goto quit;
    }
  }

  // strict comparison
  if (c->eps.cmd & eps_equ) {
    if (l1 != l2 || memcmp(lhs_p, rhs_p, l1))
      ret |= eps_equ;

    if (ret) goto quit_diff;
    else     goto quit;
  }

  // absolute comparison
  if (c->eps.cmd & eps_abs) {
     abs = reg_getval(dif->reg, dif->reg_n, c->eps.abs_reg, c->eps.abs);
    _abs = c->eps._abs_reg && c->eps._abs_reg == c->eps.abs_reg ? -abs :
           reg_getval(dif->reg, dif->reg_n, c->eps._abs_reg, c->eps._abs);
    if (abs_d > abs || abs_d < _abs) ret |= eps_abs;
  }

  // relative comparison 
  if (c->eps.cmd & eps_rel) {
     rel = reg_getval(dif->reg, dif->reg_n, c->eps.rel_reg, c->eps.rel);
    _rel = c->eps._rel_reg && c->eps._rel_reg == c->eps.rel_reg ? -rel :
           reg_getval(dif->reg, dif->reg_n, c->eps._rel_reg, c->eps._rel);
    if (rel_d > rel || rel_d < _rel) ret |= eps_rel;
  }

  // input-specific relative comparison (does not apply to integers)
  if ((c->eps.cmd & eps_dig) && (f1 || f2)) {
     dig = reg_getval(dif->reg, dif->reg_n, c->eps.dig_reg, c->eps.dig);
    _dig = c->eps._dig_reg && c->eps._dig_reg == c->eps.dig_reg ? -dig :
           reg_getval(dif->reg, dif->reg_n, c->eps._dig_reg, c->eps._dig);
    if (dig_d > dig || dig_d < _dig) ret |= eps_dig;
  }

  if ((c->eps.cmd & eps_any) && (ret & eps_dra) != (c->eps.cmd & eps_dra)) ret = 0;
  if (!ret) goto quit;

quit_diff:
  if (!(c->eps.cmd & eps_nofail) && ++dif->cnt_i <= dif->max_i) {
    if (dif->cnt_i == 1) ndiff_header();
    warning("(%d) files differ at line %d column %d between char-columns %d|%d and %d|%d",
            dif->cnt_i, dif->row_i, dif->col_i, dif->lhs_i+1, dif->rhs_i+1, dif->lhs_i+1+l1, dif->rhs_i+1+l2);

    char str[128];
    sprintf(str, "(%%d) numbers: '%%.%ds'|'%%.%ds'", l1, l2);
    warning(str, dif->cnt_i, lhs_p, rhs_p);

    if (ret & eps_ign)
      warning("(%d) one number is missing (column count can be wrong)", dif->cnt_i);

    if (ret & eps_equ)
      warning("(%d) numbers strict representation differ", dif->cnt_i);

    if (ret & eps_abs)
      warning("(%d) absolute error (rule #%d, line %d: %.2g<=abs<=%.2g) abs=%.2g, rel=%.2g, ndig=%d",
              dif->cnt_i, context_findIdx(dif->cxt, c), context_findLine(dif->cxt, c),
              _abs, abs, abs_d, rel_d, imax(n1, n2));

    if (ret & eps_rel)
      warning("(%d) relative error (rule #%d, line %d: %.2g<=rel<=%.2g) abs=%.2g, rel=%.2g, ndig=%d",
              dif->cnt_i, context_findIdx(dif->cxt, c), context_findLine(dif->cxt, c),
              _rel, rel, abs_d, rel_d, imax(n1, n2));

    if (ret & eps_dig)
      warning("(%d) numdigit error (rule #%d, line %d: %.2g<=rel<=%.2g) abs=%.2g, rel=%.2g, ndig=%d",
              dif->cnt_i, context_findIdx(dif->cxt, c), context_findLine(dif->cxt, c),
              _dig*pow_d, dig*pow_d, abs_d, rel_d, imax(n1, n2));
  }
  if (c->eps.cmd & eps_onfail) context_onfail(dif->cxt, c);

quit:
  if (!ret || c->eps.cmd & eps_save) {
    // saves
    reg_setval(dif->reg, dif->reg_n, 1, c->eps.lhs_reg || c->eps.cmd & eps_lhs ? strtod(c->eps.cmd & eps_swap ? rhs_p : lhs_p, 0) : lhs_d);
    reg_setval(dif->reg, dif->reg_n, 2, c->eps.rhs_reg || c->eps.cmd & eps_rhs ? strtod(c->eps.cmd & eps_swap ? lhs_p : rhs_p, 0) : rhs_d);
    reg_setval(dif->reg, dif->reg_n, 3, dif_d);
    reg_setval(dif->reg, dif->reg_n, 4, err_d);
    reg_setval(dif->reg, dif->reg_n, 5, abs_d);
    reg_setval(dif->reg, dif->reg_n, 6, rel_d);
    reg_setval(dif->reg, dif->reg_n, 7, dig_d);
    reg_setval(dif->reg, dif->reg_n, 8, min_d);
    reg_setval(dif->reg, dif->reg_n, 9, pow_d);

    // operations with registers trace
    if (c->eps.cmd & eps_traceR) {
      ndiff_traceR(dif, abs, _abs, rel, _rel, dig, _dig);

      char buf[50*sizeof c->eps.op] = "  ";
      int pos = 2;

      for (int i=0; i < c->eps.op_n; i++) {
        reg_eval(dif->reg, dif->reg_n, c->eps.dst[i], c->eps.src[i], c->eps.src2[i], c->eps.op[i]);
        pos += sprintf(buf+pos, "R%d=%.17g, ", c->eps.dst[i], dif->reg[c->eps.dst[i]]);
      }
      if (pos>2) { buf[pos-2] = 0; trace(buf); }
    }

    // operations without trace
    else
      for (int i=0; i < c->eps.op_n; i++)
        reg_eval(dif->reg, dif->reg_n, c->eps.dst[i], c->eps.src[i], c->eps.src2[i], c->eps.op[i]);
  }

  dif->lhs_i += l1;
  dif->rhs_i += l2;
  trace("<-testNum  line %d, column %d, char-column %d|%d", dif->row_i, dif->col_i, dif->lhs_i, dif->rhs_i);

  return ret;
}

void
ndiff_option  (T *dif, const int *keep_, const int *blank_, const int *check_)
{
  assert(dif);
  
  if (keep_ ) dif->max_i = *keep_;
  if (blank_) dif->blank = *blank_; 
  if (check_) dif->check = *check_; 

  ensure(dif->max_i > 0, "number of kept diff must be positive");
}

void
ndiff_getInfo (const T *dif, int *row_, int *col_, int *cnt_, long *num_)
{
  assert(dif);

  if (row_) *row_ = dif->row_i;
  if (col_) *col_ = dif->col_i;
  if (cnt_) *cnt_ = dif->cnt_i;
  if (num_) *num_ = dif->num_i;
}

int
ndiff_feof (const T *dif, int both)
{
  assert(dif);

  return both ? feof(dif->lhs_f) && feof(dif->rhs_f)
              : feof(dif->lhs_f) || feof(dif->rhs_f);
}

int
ndiff_isempty (const T *dif)
{
  assert(dif);

  return !dif->lhs_b[dif->lhs_i] && !dif->rhs_b[dif->rhs_i];
}

// --- main ndiff loop --------------------------------------------------------

void
ndiff_loop(T *dif, FILE *lhs_fp, FILE *rhs_fp)
{
  assert(dif);

  const C *c, *c2;
  int row=0, col, ret;
  int saved_level = logmsg_config.level;

  while(!ndiff_feof(dif, 0)) {
    ++row, col=0, ret=0;

    c = context_getInc(dif->cxt, row, col);
    ensure(c, "invalid context");
    if (dif->check && c != (c2 = context_getAt(dif->cxt, row, col)))
      ndiff_error(dif->cxt, c, c2, row, col);

    // trace rule
    if (c->eps.cmd & eps_trace && c->eps.cmd & eps_sgg) {
      logmsg_config.level = trace_level;
      trace("~>active:  rule #%d, line %d, cmd = %d",
            context_findIdx(dif->cxt,c), context_findLine(dif->cxt,c), c->eps.cmd);
      logmsg_config.level = saved_level;
    }

    // skip this line
    if (c->eps.cmd & eps_skip) {
      ndiff_skipLine(dif);
      continue;
    }

    // goto or read line(s)
    if (c->eps.cmd & eps_goto) {
      ndiff_gotoLine(dif, c);
      ndiff_getInfo(dif, &row, 0, 0, 0);
    } else
    if (c->eps.cmd & eps_gonum) {
      ndiff_gotoNum(dif, c);
      ndiff_getInfo(dif, &row, 0, 0, 0);
    } else {
      ndiff_readLine(dif);
      if (ndiff_isempty(dif)) goto result;
    }

    // for each number column, diff-chars between numbers
    while((col = ndiff_nextNum(dif, c))) {
      c = context_getInc(dif->cxt, row, col);
      ensure(c, "invalid context");
      if (dif->check && c != (c2 = context_getAt(dif->cxt, row, col)))
        ndiff_error(dif->cxt, c, c2, row, col);

      // newly activated action
      if (c->eps.cmd & eps_sgg) break;

      // trace rule
      if (c->eps.cmd & eps_trace) {
        logmsg_config.level = trace_level;
        trace("~>active:  rule #%d, line %d, cmd = %d",
              context_findIdx(dif->cxt,c), context_findLine(dif->cxt,c), c->eps.cmd);
      }

      // check numbers
      ret |= ndiff_testNum(dif, c);

      // restore logmsg
      logmsg_config.level = saved_level;
    }

result:
    if (!ret) ndiff_outLine(dif, lhs_fp, rhs_fp);
  }

  if (dif->blank) {
    skipSpace(dif->lhs_f, 0);
    skipSpace(dif->rhs_f, 0);
  }
}

#undef T
#undef C

// -----------------------------------------------------------------------------
// ----- testsuite
// -----------------------------------------------------------------------------

#ifndef NTEST

#include "utest.h"

#define T struct ndiff

// ----- debug

// ----- teardown

static T*
ut_teardown(T *dif)
{
  ndiff_clear(dif);
  return dif;
}

// ----- test

static void 
ut_testPow10(struct utest *utest, T* dif)
{
  (void)dif;

  for (int k = -100; k < 100; k++)
    UTEST(pow10(k) == pow(10, k));
}

static void
ut_testNul(struct utest *utest, T* dif)
{
  UTEST(dif != 0);
}

// ----- unit tests

static struct spec {
  const char *name;
  T*        (*setup)   (T*);
  void      (*test )   (struct utest*, T*);
  T*        (*teardown)(T*);
} spec[] = {
  { "power of 10",                          0        , ut_testPow10, 0           },
  { "empty input",                          0        , ut_testNul  , ut_teardown },
};
enum { spec_n = sizeof spec/sizeof *spec };

// ----- interface

void
ndiff_utest(struct utest *ut)
{
  assert(ut);
  T *dif = ndiff_alloc(stdout, stdout, 0, 0, 0);

  utest_title(ut, "File diff");

  for (int k = 0; k < spec_n; k++) {
    utest_init(ut, spec[k].name);
    if (spec[k].setup)    dif = spec[k].setup(dif);
    spec[k].test(ut, dif);
    if (spec[k].teardown) dif = spec[k].teardown(dif);
    utest_fini(ut);
  }

  ndiff_free(dif);
}

#endif
