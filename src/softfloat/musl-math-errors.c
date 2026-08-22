/* zedBSD software-fenv versions of musl's libm error helpers. */
#include <fenv.h>
#include <math.h>
#include <stdint.h>

double __math_invalid(double value)
{ (void)value; (void)feraiseexcept(FE_INVALID); return NAN; }
float __math_invalidf(float value)
{ (void)value; (void)feraiseexcept(FE_INVALID); return NAN; }
long double __math_invalidl(long double value)
{ (void)value; (void)feraiseexcept(FE_INVALID); return NAN; }

double __math_divzero(uint32_t sign)
{ (void)feraiseexcept(FE_DIVBYZERO); return sign ? -INFINITY : INFINITY; }
float __math_divzerof(uint32_t sign)
{ (void)feraiseexcept(FE_DIVBYZERO); return sign ? -INFINITY : INFINITY; }

double __math_oflow(uint32_t sign)
{ (void)feraiseexcept(FE_OVERFLOW | FE_INEXACT); return sign ? -INFINITY : INFINITY; }
float __math_oflowf(uint32_t sign)
{ (void)feraiseexcept(FE_OVERFLOW | FE_INEXACT); return sign ? -INFINITY : INFINITY; }
double __math_uflow(uint32_t sign)
{ (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT); return sign ? -0.0 : 0.0; }
float __math_uflowf(uint32_t sign)
{ (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT); return sign ? -0.0f : 0.0f; }

double __math_xflow(uint32_t sign, double value)
{ return sign ? -(value * value) : value * value; }
float __math_xflowf(uint32_t sign, float value)
{ return sign ? -(value * value) : value * value; }
