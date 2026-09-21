/* Measure our elementary functions against the host's.
 *
 * Our math.c is compiled here with the host compiler and every symbol
 * renamed, so both implementations exist in one program and can be handed the
 * same inputs. The number that matters is the error in units in the last
 * place: 0 means bit-identical, 1 means the last bit differs, and anything
 * under about 4 is fine for a language that prints 17 digits at most. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

double my_sqrt(double), my_exp(double), my_log(double), my_log2(double),
       my_log10(double), my_log1p(double), my_expm1(double),
       my_sin(double), my_cos(double), my_tan(double),
       my_asin(double), my_acos(double), my_atan(double),
       my_sinh(double), my_cosh(double), my_tanh(double),
       my_asinh(double), my_acosh(double), my_atanh(double),
       my_floor(double), my_ceil(double), my_trunc(double), my_round(double),
       my_fabs(double);
double my_pow(double, double), my_atan2(double, double), my_fmod(double, double);
double my_ldexp(double, int);
double my_frexp(double, int *);
double my_modf(double, double *);
double my_copysign(double, double);

typedef union { double d; uint64_t u; } dbits;

/* Distance between two doubles, counted in representable values. */
static double ulps(double a, double b) {
    if (a == b) return 0.0;
    if (isnan(a) && isnan(b)) return 0.0;
    if (isnan(a) || isnan(b)) return 1e18;
    if (isinf(a) || isinf(b)) return (a == b) ? 0.0 : 1e18;
    dbits x, y;
    x.d = a; y.d = b;
    if ((a < 0) != (b < 0)) return 1e18;
    int64_t d = (int64_t)(x.u > y.u ? x.u - y.u : y.u - x.u);
    return (double)d;
}

static int failures;

static void report(const char *name, double worst, double at, double got, double want) {
    const char *verdict = worst <= 4.0 ? "exact-ish" : (worst <= 16.0 ? "ok" : "BAD");
    if (worst > 16.0) failures++;
    printf("  %-8s worst %10.1f ulp   %s", name, worst, verdict);
    if (worst > 16.0) printf("   at %.17g: got %.17g want %.17g", at, got, want);
    printf("\n");
}

#define SWEEP(name, mine, theirs, lo, hi, n)                       \
    do {                                                           \
        double worst = 0, at = 0, got = 0, want = 0;               \
        for (int i = 0; i <= (n); i++) {                           \
            double x = (lo) + ((hi) - (lo)) * (double)i / (n);     \
            double a = mine(x), b = theirs(x);                     \
            double u = ulps(a, b);                                 \
            if (u > worst) { worst = u; at = x; got = a; want = b; }\
        }                                                          \
        report(name, worst, at, got, want);                        \
    } while (0)

int main(void) {
    printf("elementary functions, ours against the host's:\n\n");

    SWEEP("sqrt",  my_sqrt,  sqrt,  0.0, 1000.0, 200000);
    SWEEP("exp",   my_exp,   exp,   -700.0, 700.0, 200000);
    SWEEP("expm1", my_expm1, expm1, -2.0, 2.0, 200000);
    SWEEP("log",   my_log,   log,   1e-300, 1e300, 200000);
    SWEEP("log2",  my_log2,  log2,  1e-30, 1e30, 200000);
    SWEEP("log10", my_log10, log10, 1e-30, 1e30, 200000);
    SWEEP("log1p", my_log1p, log1p, -0.9, 10.0, 200000);
    SWEEP("sin",   my_sin,   sin,   -100.0, 100.0, 200000);
    SWEEP("cos",   my_cos,   cos,   -100.0, 100.0, 200000);
    SWEEP("tan",   my_tan,   tan,   -1.5, 1.5, 200000);
    SWEEP("asin",  my_asin,  asin,  -1.0, 1.0, 200000);
    SWEEP("acos",  my_acos,  acos,  -1.0, 1.0, 200000);
    SWEEP("atan",  my_atan,  atan,  -1000.0, 1000.0, 200000);
    SWEEP("sinh",  my_sinh,  sinh,  -20.0, 20.0, 200000);
    SWEEP("cosh",  my_cosh,  cosh,  -20.0, 20.0, 200000);
    SWEEP("tanh",  my_tanh,  tanh,  -10.0, 10.0, 200000);
    SWEEP("asinh", my_asinh, asinh, -100.0, 100.0, 200000);
    SWEEP("acosh", my_acosh, acosh, 1.0, 1000.0, 200000);
    SWEEP("atanh", my_atanh, atanh, -0.999, 0.999, 200000);
    SWEEP("floor", my_floor, floor, -1000.0, 1000.0, 200000);
    SWEEP("ceil",  my_ceil,  ceil,  -1000.0, 1000.0, 200000);
    SWEEP("round", my_round, round, -1000.0, 1000.0, 200000);
    SWEEP("trunc", my_trunc, trunc, -1000.0, 1000.0, 200000);

    /* Two arguments, on a grid. */
    {
        double worst = 0, at = 0, got = 0, want = 0;
        for (int i = 0; i <= 400; i++)
            for (int j = 0; j <= 400; j++) {
                double x = 0.01 + 40.0 * i / 400.0;
                double y = -20.0 + 40.0 * j / 400.0;
                double a = my_pow(x, y), b = pow(x, y);
                double u = ulps(a, b);
                if (u > worst) { worst = u; at = x; got = a; want = b; }
            }
        report("pow", worst, at, got, want);
    }
    {
        double worst = 0, at = 0, got = 0, want = 0;
        for (int i = 0; i <= 600; i++)
            for (int j = 0; j <= 600; j++) {
                double y = -30.0 + 60.0 * i / 600.0;
                double x = -30.0 + 60.0 * j / 600.0;
                double a = my_atan2(y, x), b = atan2(y, x);
                double u = ulps(a, b);
                if (u > worst) { worst = u; at = y; got = a; want = b; }
            }
        report("atan2", worst, at, got, want);
    }
    {
        double worst = 0, at = 0, got = 0, want = 0;
        for (int i = 1; i <= 600; i++)
            for (int j = 1; j <= 600; j++) {
                double x = -300.0 + 600.0 * i / 600.0;
                double y = -30.0 + 60.0 * j / 600.0;
                if (y == 0) continue;
                double a = my_fmod(x, y), b = fmod(x, y);
                double u = ulps(a, b);
                if (u > worst) { worst = u; at = x; got = a; want = b; }
            }
        report("fmod", worst, at, got, want);
    }

    /* The ones with an out parameter, and the exact cases. */
    {
        int e1, e2;
        double worst = 0;
        for (int i = 0; i < 100000; i++) {
            double x = (double)(i - 50000) * 1e-3;
            if (x == 0) continue;
            double a = my_frexp(x, &e1), b = frexp(x, &e2);
            if (e1 != e2) worst = 1e18;
            double u = ulps(a, b);
            if (u > worst) worst = u;
        }
        report("frexp", worst, 0, 0, 0);
    }
    {
        double i1, i2, worst = 0;
        for (int i = 0; i < 100000; i++) {
            double x = (double)(i - 50000) * 1e-3;
            double a = my_modf(x, &i1), b = modf(x, &i2);
            if (i1 != i2) worst = 1e18;
            double u = ulps(a, b);
            if (u > worst) worst = u;
        }
        report("modf", worst, 0, 0, 0);
    }

    /* Values that must come out exactly right, not merely close. */
    printf("\nexact cases:\n");
    struct { const char *what; double got, want; } ex[] = {
        { "sqrt(144)",      my_sqrt(144.0),          12.0 },
        { "pow(2,10)",      my_pow(2.0, 10.0),       1024.0 },
        { "pow(2,-2)",      my_pow(2.0, -2.0),       0.25 },
        { "pow(-2,3)",      my_pow(-2.0, 3.0),       -8.0 },
        { "pow(x,0)",       my_pow(3.7, 0.0),        1.0 },
        { "exp(0)",         my_exp(0.0),             1.0 },
        { "log(1)",         my_log(1.0),             0.0 },
        { "sin(0)",         my_sin(0.0),             0.0 },
        { "cos(0)",         my_cos(0.0),             1.0 },
        { "floor(-2.5)",    my_floor(-2.5),          -3.0 },
        { "ceil(-2.5)",     my_ceil(-2.5),           -2.0 },
        { "round(-2.5)",    my_round(-2.5),          -3.0 },
        { "round(2.5)",     my_round(2.5),           3.0 },
        { "trunc(-2.9)",    my_trunc(-2.9),          -2.0 },
        { "fmod(10,3)",     my_fmod(10.0, 3.0),      1.0 },
        { "fmod(-10,3)",    my_fmod(-10.0, 3.0),     -1.0 },
        { "copysign(3,-1)", my_copysign(3.0, -1.0),  -3.0 },
        { "ldexp(3,4)",     my_ldexp(3.0, 4),        48.0 },
        { "atan2(1,1)*4",   my_atan2(1.0, 1.0) * 4.0, M_PI },
    };
    for (unsigned i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        int ok = (ex[i].got == ex[i].want);
        if (!ok) failures++;
        printf("  %-16s %-8s %.17g\n", ex[i].what, ok ? "exact" : "WRONG", ex[i].got);
    }

    printf("\n%s\n", failures ? "SOMETHING IS WRONG" : "all within tolerance");
    return failures ? 1 : 0;
}
