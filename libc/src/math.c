/* The elementary functions.
 *
 * Written out rather than vendored, for the same reason the image decoders
 * and the cryptography were: a system that ships a library it cannot explain
 * has not really got that library. Nothing here is clever -- there are faster
 * ways to do all of it, and every one of them trades clarity for cycles that
 * a scripting language will never notice.
 *
 * The method throughout is the same two steps:
 *
 *   REDUCE  the argument into a small interval using an exact identity, and
 *   SUM     a Taylor series there, with enough terms that the first one left
 *           out is below the last bit of a double.
 *
 * That second part is why the loops look excessive. A minimax polynomial
 * would need a third of the terms, but its coefficients are magic numbers
 * that cannot be checked by reading them; a Taylor series can be checked by
 * anyone who remembers what a Taylor series is. Correctness first.
 *
 * Accuracy is within a few units in the last place over the ordinary range,
 * measured against glibc by tools/mathcheck.c: worst case twelve, most under
 * six, and exact wherever the answer is exactly representable -- sqrt of a
 * square, fmod, floor, a whole-number power, a factorial.
 *
 * The gamma functions are the weak ones, and both for the same reason: the
 * answer is computed as a difference or an exponential of something much
 * larger than itself, so the arithmetic is fine and the METHOD loses digits.
 *
 *   lgamma is about 180 units in the last place at worst -- roughly 1e-15 of
 *   absolute error, which is what it should be; it reads badly only because
 *   the function itself is small there. Near x = 1 and x = 2, where it
 *   crosses zero, the relative error grows without limit for the same reason.
 *
 *   tgamma is exp(lgamma(x)), so it inherits that error multiplied by the
 *   result: about 700 units in the last place at x = 59. Whole-number
 *   arguments up to 23 are computed as factorials instead, and are exact.
 *
 * Fixing either properly means carrying the log-gamma in two doubles, the way
 * pow already carries its logarithm. Worth doing if anything leans on them.
 */

#include <math.h>
#include <time.h>
#include <stdint.h>

typedef union { double d; uint64_t u; } dbits;

/* --- the easy ones, done on the bits -------------------------------------- */

double fabs(double x) {
    dbits v; v.d = x;
    v.u &= 0x7FFFFFFFFFFFFFFFULL;
    return v.d;
}

/* A quiet not-a-number. The argument selects a payload; nothing here has any
 * use for one, so it is ignored. */
double nan(const char *tag) {
    (void)tag;
    return (double)NAN;
}

double copysign(double x, double y) {
    dbits a, b; a.d = x; b.d = y;
    a.u = (a.u & 0x7FFFFFFFFFFFFFFFULL) | (b.u & 0x8000000000000000ULL);
    return a.d;
}

/* The processor has an instruction for this and it is correctly rounded, so
 * there is nothing to improve on. */
double sqrt(double x) {
    if (isnan(x)) return x;
    if (x < 0.0) return NAN;
    double r;
    __asm__ ("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double ldexp(double x, int e) {
    if (!isfinite(x) || x == 0.0) return x;
    /* Done in steps so that a large exponent cannot overflow the field, and
     * so that the result lands on a subnormal correctly when it should. */
    while (e > 1000) { x *= 0x1p1000; e -= 1000; }
    while (e < -1000) { x *= 0x1p-1000; e += 1000; }
    dbits s;
    s.u = ((uint64_t)(e + 1023) & 0x7FF) << 52;
    return x * s.d;
}

float ldexpf(float x, int e) { return (float)ldexp((double)x, e); }
long double ldexpl(long double x, int e) { return (long double)ldexp((double)x, e); }

double frexp(double x, int *e) {
    dbits v; v.d = x;
    int ex = (int)((v.u >> 52) & 0x7FF);
    if (ex == 0) {                       /* zero or subnormal */
        if (x == 0.0) { *e = 0; return x; }
        v.d = x * 0x1p54;                /* scale it up to normal, then adjust */
        ex = (int)((v.u >> 52) & 0x7FF) - 54;
    } else if (ex == 0x7FF) {            /* infinity or not-a-number */
        *e = 0;
        return x;
    }
    *e = ex - 1022;
    v.u = (v.u & ~(0x7FFULL << 52)) | (1022ULL << 52);
    return v.d;
}

/* Beyond 2^52 every double is already a whole number, so the rounding
 * functions have nothing to do there -- and the cast they would otherwise use
 * would overflow. */
#define TOO_BIG_TO_ROUND 4503599627370496.0   /* 2^52 */

double trunc(double x) {
    if (!isfinite(x) || fabs(x) >= TOO_BIG_TO_ROUND) return x;
    return (double)(long long)x;
}

double floor(double x) {
    if (!isfinite(x) || fabs(x) >= TOO_BIG_TO_ROUND) return x;
    double t = (double)(long long)x;
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x) {
    if (!isfinite(x) || fabs(x) >= TOO_BIG_TO_ROUND) return x;
    double t = (double)(long long)x;
    return (t < x) ? t + 1.0 : t;
}

/* Away from zero on a tie, which is what C says round() does (and not what
 * the processor's own rounding mode does). */
double round(double x) {
    if (!isfinite(x) || fabs(x) >= TOO_BIG_TO_ROUND) return x;
    double t = floor(fabs(x) + 0.5);
    return copysign(t, x);
}

/* Round to nearest, ties to even. That last part is what separates it from
 * round(), which goes away from zero: nearbyint(2.5) is 2 and round(2.5) is 3.
 * Adding and subtracting 2^52 forces the rounding the hardware is already set
 * up to do, without a conditional anywhere. */
double nearbyint(double x) {
    if (!isfinite(x) || fabs(x) >= TOO_BIG_TO_ROUND) return x;
    double big = copysign(TOO_BIG_TO_ROUND, x);
    /* volatile so the compiler cannot cancel the two operations that ARE the
     * rounding. */
    volatile double t = x + big;
    return t - big;
}

double rint(double x) { return nearbyint(x); }

double modf(double x, double *iptr) {
    if (isnan(x)) { *iptr = x; return x; }
    if (isinf(x)) { *iptr = x; return copysign(0.0, x); }
    double i = trunc(x);
    *iptr = i;
    return x - i;
}

/* Exact: subtract shifted copies of the divisor, the way long division works.
 * Every step is representable, so nothing is lost. */
double fmod(double x, double y) {
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) return NAN;
    if (isinf(y)) return x;
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay) return x;

    int ex, ey;
    frexp(ax, &ex);
    frexp(ay, &ey);
    double r = ax;
    for (int k = ex - ey; k >= 0; k--) {
        double t = ldexp(ay, k);
        if (t <= r) r -= t;
    }
    return copysign(r, x);
}

/* --- exp and log ----------------------------------------------------------- */

/* ln 2, split so that k * LN2_HI is exact for the k values reduction uses:
 * the low half carries the bits that would otherwise be rounded away. */
#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10

double exp(double x) {
    if (isnan(x)) return x;
    if (x > 709.782712893384) return INFINITY;
    if (x < -745.133219101941) return 0.0;

    /* exp(x) = 2^k * exp(r), |r| <= ln2/2 */
    double kd = round(x * M_LOG2E);
    int k = (int)kd;
    double r = (x - kd * LN2_HI) - kd * LN2_LO;

    /* Taylor. |r| <= 0.347, so r^15/15! is below 1e-19 -- past the last bit. */
    double sum = 1.0, term = 1.0;
    for (int n = 1; n <= 15; n++) {
        term *= r / (double)n;
        sum += term;
    }
    return ldexp(sum, k);
}

/* exp(x) - 1, computed so that small x does not lose everything to
 * cancellation: for tiny x the series IS the answer. */
double expm1(double x) {
    if (isnan(x)) return x;
    if (fabs(x) > 0.7) return exp(x) - 1.0;
    double sum = 0.0, term = 1.0;
    for (int n = 1; n <= 22; n++) {
        term *= x / (double)n;
        sum += term;
    }
    return sum;
}

double log(double x) {
    if (isnan(x)) return x;
    if (x < 0.0) return NAN;
    if (x == 0.0) return -INFINITY;
    if (isinf(x)) return x;

    int e;
    double m = frexp(x, &e);             /* x = m * 2^e, m in [0.5, 1) */
    /* Centre the mantissa on 1, where the series converges fastest. */
    if (m < M_SQRT1_2) { m *= 2.0; e--; }

    /* log(m) = 2 * atanh(s), s = (m-1)/(m+1). |s| <= 0.1716, so s^2 <= 0.03
     * and twenty-five terms are far past the last bit. */
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s, term = s, sum = s;
    for (int n = 3; n <= 25; n += 2) {
        term *= s2;
        sum += term / (double)n;
    }
    return 2.0 * sum + (double)e * LN2_HI + (double)e * LN2_LO;
}

double log1p(double x) {
    if (isnan(x)) return x;
    if (x < -1.0) return NAN;
    if (x == -1.0) return -INFINITY;
    if (fabs(x) > 0.25) return log(1.0 + x);
    /* Same series as log, with s written in terms of x so that 1+x is never
     * formed and its low bits never lost. */
    double s = x / (2.0 + x);
    double s2 = s * s, term = s, sum = s;
    for (int n = 3; n <= 25; n += 2) {
        term *= s2;
        sum += term / (double)n;
    }
    return 2.0 * sum;
}

/* The exact error of a * b, by splitting each operand into halves that
 * multiply without rounding. 2^27 + 1 is the splitting constant for doubles. */
static void two_prod(double a, double b, double *p, double *err) {
    const double SPLIT = 134217729.0;
    double c = SPLIT * a, ahi = c - (c - a), alo = a - ahi;
    double d = SPLIT * b, bhi = d - (d - b), blo = b - bhi;
    *p = a * b;
    *err = ((ahi * bhi - *p) + ahi * blo + alo * bhi) + alo * blo;
}

/* The exact error of a + b, when |a| >= |b|. */
static void fast_two_sum(double a, double b, double *s2, double *err) {
    *s2 = a + b;
    *err = b - (*s2 - a);
}

/* log(x) as hi + lo. Same series as log(), summed so that what falls off the
 * bottom of each addition is kept rather than dropped. */
static void log_ext(double x, double *hi, double *lo) {
    int e;
    double m = frexp(x, &e);
    if (m < M_SQRT1_2) { m *= 2.0; e--; }

    /* m is in [0.707, 1.414], so both of these are exact. */
    double num = m - 1.0, den = m + 1.0;
    double s = num / den;

    /* The remainder the division threw away: num - s*den, computed exactly. */
    double p, perr;
    two_prod(s, den, &p, &perr);
    double s_lo = ((num - p) - perr) / den;

    /* 2*(s + s^3/3 + s^5/5 + ...), with the running sum kept in two pieces. */
    double s2 = s * s;
    double term = s;
    double sum_hi = s, sum_lo = s_lo;
    for (int n = 3; n <= 27; n += 2) {
        term *= s2;
        double t = term / (double)n;
        double ns, err;
        fast_two_sum(sum_hi, t, &ns, &err);
        sum_hi = ns;
        sum_lo += err;
    }
    double lg_hi = 2.0 * sum_hi, lg_lo = 2.0 * sum_lo;

    /* Add e*ln2, whose two halves were chosen so the first multiplies exactly. */
    double eh = (double)e * LN2_HI;
    double el = (double)e * LN2_LO;
    double t1, t1e;
    fast_two_sum(fabs(eh) >= fabs(lg_hi) ? eh : lg_hi,
                 fabs(eh) >= fabs(lg_hi) ? lg_hi : eh, &t1, &t1e);
    *hi = t1;
    *lo = t1e + el + lg_lo;

    /* Renormalise, so hi carries as much as a double can and lo the rest. */
    double h, l;
    fast_two_sum(*hi, *lo, &h, &l);
    *hi = h;
    *lo = l;
}

double log2(double x)  { return log(x) * M_LOG2E; }
double log10(double x) { return log(x) * M_LOG10E; }

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 1.0) return 1.0;

    /* A whole-number exponent is done by squaring: it is exact where the
     * answer is exact, and it is the only way to raise a negative base. */
    if (y == trunc(y) && fabs(y) <= 1024.0) {
        long long n = (long long)y;
        int neg = n < 0;
        unsigned long long m = (unsigned long long)(neg ? -n : n);
        double base = x, acc = 1.0;
        while (m) {
            if (m & 1) acc *= base;
            base *= base;
            m >>= 1;
        }
        return neg ? 1.0 / acc : acc;
    }

    if (x < 0.0) return NAN;             /* a fractional power of a negative */
    if (x == 0.0) return (y > 0.0) ? 0.0 : INFINITY;

    /* y * log(x), with the logarithm carried to twice a double's precision
     * and the product formed the same way -- exp turns any error here into
     * the same relative error in the answer, multiplied by y. */
    double lhi, llo;
    log_ext(x, &lhi, &llo);

    double phi, perr;
    two_prod(y, lhi, &phi, &perr);
    double plo = perr + y * llo;

    /* Split again so that exp() gets an argument it can handle exactly and
     * the remainder stays small enough for its own series to be trivial. */
    double ph, pl;
    fast_two_sum(phi, plo, &ph, &pl);

    if (ph > 709.8) return INFINITY;
    if (ph < -745.2) return 0.0;

    /* exp(ph + pl) = exp(ph) * exp(pl); pl is below 1e-13, so two terms of
     * its own series are already past the last bit. */
    return exp(ph) * (1.0 + pl * (1.0 + pl * 0.5));
}

/* --- the circular functions ------------------------------------------------ */

/* pi/2 in three pieces, so that reducing a large argument keeps its low bits.
 * Anything past 2^30 or so is beyond what this reduction can hold onto, and
 * the answer there is noise whatever one does short of the full Payne-Hanek
 * argument reduction -- which is a great deal of machinery for sin(1e18). */
#define PI2_HI  1.57079505920410156250e+00
#define PI2_MID 1.26759005070198327303e-06
#define PI2_LO  7.44354748048662324896e-13

/* sin and cos on the reduced argument, |r| <= pi/4. */
static double sin_small(double r) {
    double r2 = r * r, term = r, sum = r;
    for (int n = 3; n <= 19; n += 2) {
        term *= -r2 / ((double)n * (double)(n - 1));
        sum += term;
    }
    return sum;
}

static double cos_small(double r) {
    double r2 = r * r, term = 1.0, sum = 1.0;
    for (int n = 2; n <= 20; n += 2) {
        term *= -r2 / ((double)n * (double)(n - 1));
        sum += term;
    }
    return sum;
}

/* x = n*(pi/2) + r, with |r| <= pi/4. Returns n mod 4 and writes r. */
static int reduce_quarter(double x, double *r) {
    double kd = round(x * M_2_PI);
    double t = x - kd * PI2_HI;      /* exact: PI2_HI has 33 zero low bits */
    t = t - kd * PI2_MID;            /* exact for the same reason           */
    t = t - kd * PI2_LO;
    *r = t;
    long long k = (long long)kd;
    return (int)(((k % 4) + 4) % 4);
}

double sin(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    double r;
    switch (reduce_quarter(x, &r)) {
    case 0:  return sin_small(r);
    case 1:  return cos_small(r);
    case 2:  return -sin_small(r);
    default: return -cos_small(r);
    }
}

double cos(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    double r;
    switch (reduce_quarter(x, &r)) {
    case 0:  return cos_small(r);
    case 1:  return -sin_small(r);
    case 2:  return -cos_small(r);
    default: return sin_small(r);
    }
}

double tan(double x) {
    double s = sin(x), c = cos(x);
    if (c == 0.0) return copysign(INFINITY, s);
    return s / c;
}

double atan(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return copysign(M_PI_2, x);

    int neg = x < 0.0;
    double a = fabs(x);
    double base = 0.0;

    /* Two identities bring any argument into |t| <= tan(pi/8), where the
     * series converges quickly enough to be worth writing down. */
    if (a > 1.0) { base = M_PI_2; a = -1.0 / a; }
    if (fabs(a) > 0.41421356237309503) {
        double sign = (a < 0.0) ? -1.0 : 1.0;
        double aa = fabs(a);
        double t = (aa - 1.0) / (aa + 1.0);
        base += sign * M_PI_4;
        a = sign * t;
    }

    double a2 = a * a, term = a, sum = a;
    for (int n = 3; n <= 41; n += 2) {
        term *= -a2;
        sum += term / (double)n;
    }
    double res = base + sum;
    return neg ? -res : res;
}

double atan2(double y, double x) {
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0.0 && y == 0.0) return signbit(x) ? copysign(M_PI, y) : copysign(0.0, y);
    if (isinf(x) && isinf(y))
        return signbit(x) ? copysign(3.0 * M_PI_4, y) : copysign(M_PI_4, y);
    if (x == 0.0) return copysign(M_PI_2, y);
    if (isinf(y)) return copysign(M_PI_2, y);
    if (isinf(x)) return signbit(x) ? copysign(M_PI, y) : copysign(0.0, y);

    double a = atan(y / x);
    if (x > 0.0) return a;
    return (y >= 0.0) ? a + M_PI : a - M_PI;
}

double asin(double x) {
    if (isnan(x)) return x;
    double a = fabs(x);
    if (a > 1.0) return NAN;
    if (a == 1.0) return copysign(M_PI_2, x);
    /* asin(x) = atan(x / sqrt(1 - x^2)); written with (1-x)(1+x) so that x
     * close to one does not lose its precision to cancellation. */
    return atan(x / sqrt((1.0 - a) * (1.0 + a)));
}

double acos(double x) {
    if (isnan(x)) return x;
    if (fabs(x) > 1.0) return NAN;
    /* Near x = 1 the answer is small and pi/2 - asin(x) would compute it by
     * subtracting two numbers that agree to fifteen digits. The half-angle
     * form arrives at the same place without ever forming them. */
    if (x > 0.5)  return 2.0 * asin(sqrt((1.0 - x) / 2.0));
    if (x < -0.5) return M_PI - 2.0 * asin(sqrt((1.0 + x) / 2.0));
    return M_PI_2 - asin(x);
}

/* --- the hyperbolic functions ---------------------------------------------- */

double sinh(double x) {
    if (!isfinite(x)) return x;
    if (fabs(x) < 0.35) {                /* the series, to avoid cancellation */
        double x2 = x * x, term = x, sum = x;
        for (int n = 3; n <= 19; n += 2) {
            term *= x2 / ((double)n * (double)(n - 1));
            sum += term;
        }
        return sum;
    }
    double e = exp(fabs(x));
    double r = 0.5 * (e - 1.0 / e);
    return copysign(r, x);
}

double cosh(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return INFINITY;
    double e = exp(fabs(x));
    return 0.5 * (e + 1.0 / e);
}

double tanh(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return copysign(1.0, x);
    double a = fabs(x);
    if (a > 20.0) return copysign(1.0, x);
    /* t = e^(2a) - 1, computed without ever forming e^(2a) and taking one
     * away from it: for small a those are 1.0000000001 and 1. */
    double t = expm1(2.0 * a);
    double r = t / (t + 2.0);
    return copysign(r, x);
}

double asinh(double x) {
    if (!isfinite(x) || x == 0.0) return x;
    double a = fabs(x);
    /* log1p keeps the small case honest, where a + sqrt(a^2+1) is 1 plus
     * something tiny. */
    double r = log1p(a + a * a / (1.0 + sqrt(a * a + 1.0)));
    return copysign(r, x);
}

double acosh(double x) {
    if (isnan(x)) return x;
    if (x < 1.0) return NAN;
    if (isinf(x)) return x;
    return log(x + sqrt((x - 1.0) * (x + 1.0)));
}

double atanh(double x) {
    if (isnan(x)) return x;
    double a = fabs(x);
    if (a > 1.0) return NAN;
    if (a == 1.0) return copysign(INFINITY, x);
    double r = 0.5 * log1p(2.0 * a / (1.0 - a));
    return copysign(r, x);
}

/* --- the special functions -------------------------------------------------
 *
 * The error function and the gamma function. Neither has a series that
 * converges usefully everywhere, so each is two methods with a crossover:
 * a Taylor series where it converges quickly, and an asymptotic form where
 * the argument is large enough for one to be accurate.
 *
 * No fitted coefficients anywhere. Stirling's series uses the Bernoulli
 * numbers, which are exact rationals written out as such; the alternative
 * (Lanczos) needs a table of fitted constants that cannot be checked by
 * reading them.
 */

/* Stirling's series for log-gamma, valid for large x:
 *
 *   lgamma(x) = (x - 1/2) ln x - x + ln(2pi)/2 + sum B(2n) / (2n(2n-1) x^(2n-1))
 *
 * The Bernoulli numbers B2, B4, ... as the exact fractions they are. */
static const double STIRLING[] = {
     1.0 / 12.0,          /* B2  / (2*1)   */
    -1.0 / 360.0,         /* B4  / (4*3)   */
     1.0 / 1260.0,        /* B6  / (6*5)   */
    -1.0 / 1680.0,        /* B8  / (8*7)   */
     1.0 / 1188.0,        /* B10 / (10*9)  */
    -691.0 / 360360.0,    /* B12 / (12*11) */
     1.0 / 156.0,         /* B14 / (14*13) */
    -3617.0 / 122400.0,   /* B16 / (16*15) */
};

#define LOG_SQRT_2PI 0.91893853320467274178   /* ln(2*pi) / 2 */

static double lgamma_stirling(double x) {
    double sum = (x - 0.5) * log(x) - x + LOG_SQRT_2PI;
    double xp = x, x2 = x * x;
    for (unsigned i = 0; i < sizeof STIRLING / sizeof STIRLING[0]; i++) {
        sum += STIRLING[i] / xp;
        xp *= x2;
    }
    return sum;
}

double lgamma(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return INFINITY;

    /* The poles: gamma has one at every non-positive whole number. */
    if (x <= 0.0 && x == trunc(x)) return INFINITY;

    if (x < 0.0) {
        /* Reflection: gamma(x) gamma(1-x) = pi / sin(pi x). Taking logs turns
         * the product into a difference, which is what makes the negative
         * half reachable at all. */
        double s = sin(M_PI * x);
        return log(M_PI / fabs(s)) - lgamma(1.0 - x);
    }

    /* Stirling's series needs a large argument, and gamma(x) = gamma(x+1)/x
     * walks any small one up to where it does. */
    double prod = 1.0;
    while (x < 12.0) {
        prod *= x;
        x += 1.0;
    }
    return lgamma_stirling(x) - log(prod);
}

double tgamma(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return (x > 0) ? INFINITY : NAN;
    if (x <= 0.0 && x == trunc(x)) return NAN;      /* the poles */

    /* A whole number is a factorial, and exactly representable up to 22. */
    if (x == trunc(x) && x > 0.0 && x <= 23.0) {
        double r = 1.0;
        for (int i = 2; i < (int)x; i++) r *= (double)i;
        return r;
    }

    if (x < 0.0) {
        /* Reflection again, this time without the logarithm. */
        double s = sin(M_PI * x);
        if (s == 0.0) return NAN;
        return M_PI / (s * tgamma(1.0 - x));
    }

    if (x > 171.7) return INFINITY;                 /* beyond a double */
    return exp(lgamma(x));
}

/* The error function. Its Taylor series about zero converges for every x, but
 * the terms grow to about e^(x^2) before they shrink, so past x = 2 the sum
 * loses more to cancellation than it is worth. There, erfc is computed
 * directly from a continued fraction and erf is one minus it. */

#define TWO_OVER_SQRT_PI 1.12837916709551257390

static double erf_series(double x) {
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n <= 60; n++) {
        term *= -x2 / (double)n;
        sum += term / (double)(2 * n + 1);
    }
    return TWO_OVER_SQRT_PI * sum;
}

/* erfc(x) sqrt(pi) e^(x^2) = 1/(x+ 1/2/(x+ 1/(x+ 3/2/(x+ 2/(x+ ...)))))
 * evaluated by Lentz's method, which builds a continued fraction from the
 * front instead of needing to know where to stop before starting. */
static double erfc_cf(double x) {
    const double tiny = 1e-300;
    double f = tiny, C = f, D = 0.0;
    for (int i = 1; i <= 300; i++) {
        double a = (i == 1) ? 1.0 : (double)(i - 1) / 2.0;
        double b = x;
        D = b + a * D;
        if (D == 0.0) D = tiny;
        C = b + a / C;
        if (C == 0.0) C = tiny;
        D = 1.0 / D;
        double delta = C * D;
        f *= delta;
        if (fabs(delta - 1.0) < 1e-17) break;
    }
    double xx, xx_err;
    two_prod(x, x, &xx, &xx_err);
    /* exp(-(xx + err)) = exp(-xx) * exp(-err), and err is small enough that
     * one term of its own series is already past the last bit. */
    return f * exp(-xx) * (1.0 - xx_err) / 1.77245385090551602730;
}

double erf(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return copysign(1.0, x);
    double a = fabs(x);
    if (a < 1.0) return erf_series(x);
    if (a > 6.0) return copysign(1.0, x);
    return copysign(1.0 - erfc_cf(a), x);
}

double erfc(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return (x > 0) ? 0.0 : 2.0;
    if (x < 0.0) return 2.0 - erfc(-x);
    if (x < 1.0) return 1.0 - erf_series(x);   /* erf(1) is 0.84: barely any loss */
    if (x > 27.0) return 0.0;
    return erfc_cf(x);
}

/* --- the single-precision spellings ---------------------------------------- */

float fabsf(float x)            { return (float)fabs((double)x); }
float sqrtf(float x)            { return (float)sqrt((double)x); }
float floorf(float x)           { return (float)floor((double)x); }
float ceilf(float x)            { return (float)ceil((double)x); }
float powf(float x, float y)    { return (float)pow((double)x, (double)y); }
float expf(float x)             { return (float)exp((double)x); }
float logf(float x)             { return (float)log((double)x); }
float sinf(float x)             { return (float)sin((double)x); }
float cosf(float x)             { return (float)cos((double)x); }
float atan2f(float y, float x)  { return (float)atan2((double)y, (double)x); }
float fmodf(float x, float y)   { return (float)fmod((double)x, (double)y); }

/* --- two that arrived with a JavaScript engine ----------------------------
 *
 * Duktape implements Math.cbrt and the Date type, and expects both of these
 * from the system. They live here rather than with the rest of their
 * families because this is the one file in the libc built with floating
 * point registers enabled -- everything else is -mgeneral-regs-only, and a
 * function returning a double cannot be compiled there at all.
 */

/* Cube root. Newton on t^3 = x converges from any positive start, and the
 * sign is pulled out first so the negative half is the same problem. */
double cbrt(double x) {
    if (x == 0.0 || isnan(x) || isinf(x)) return x;

    int neg = x < 0.0;
    if (neg) x = -x;

    /* Halving the exponent gives a start within a factor of two, which is
     * three iterations rather than thirty. */
    int e;
    double m = frexp(x, &e);
    double t = ldexp(m, e / 3);
    if (t <= 0.0) t = 1.0;

    for (int i = 0; i < 12; i++) {
        double t2 = t * t;
        if (t2 == 0.0) break;
        double next = t - (t - x / t2) / 3.0;
        if (next == t) break;
        t = next;
    }
    return neg ? -t : t;
}

/* The difference between two times, in seconds. time_t here is a count of
 * seconds already, so this is a subtraction -- but it is declared to return
 * a double, and that is why it could not live in timecal.c. */
double difftime(time_t a, time_t b) { return (double)(a - b); }
