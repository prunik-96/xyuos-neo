#include "kmath.h"

double k_sqrt(double x) {
    double r;
    __asm__ ("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double k_fabs(double x) {
    return x < 0.0 ? -x : x;
}

double k_floor(double x) {
    double t = (double)(long long)x;
    return (t > x) ? t - 1.0 : t;
}

double k_ceil(double x) {
    double t = (double)(long long)x;
    return (t < x) ? t + 1.0 : t;
}

double k_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = x / y;
    // truncate toward zero
    double t = (double)(long long)q;
    return x - t * y;
}

// --- pow/cos/acos: only referenced by stb code paths we never call. Provided
//     for link/compile correctness; accuracy is not important here. ---

static double k_ln(double x) {
    if (x <= 0.0) return 0.0;
    // range-reduce to [1,2): x = m * 2^e
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0)  { x *= 2.0; e--; }
    // atanh series around 1: ln(x) = 2*(u + u^3/3 + u^5/5 + ...), u=(x-1)/(x+1)
    double u = (x - 1.0) / (x + 1.0);
    double u2 = u * u, term = u, sum = 0.0;
    for (int k = 1; k < 20; k += 2) { sum += term / k; term *= u2; }
    return 2.0 * sum + (double)e * 0.6931471805599453;
}

static double k_exp(double x) {
    // exp(x) = 2^(x/ln2); split integer/frac, series for frac
    double y = x / 0.6931471805599453;
    long long i = (long long)k_floor(y);
    double f = (y - (double)i) * 0.6931471805599453;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k < 18; k++) { term *= f / k; sum += term; }
    // 2^i
    double p = 1.0;
    if (i >= 0) { for (long long k = 0; k < i; k++) p *= 2.0; }
    else        { for (long long k = 0; k < -i; k++) p *= 0.5; }
    return sum * p;
}

double k_pow(double x, double y) {
    if (x <= 0.0) return 0.0;
    return k_exp(y * k_ln(x));
}

double k_cos(double x) {
    x = k_fmod(x, 6.283185307179586);
    if (x < 0) x += 6.283185307179586;
    if (x > 3.141592653589793) x -= 6.283185307179586;
    double x2 = x * x, term = 1.0, sum = 1.0;
    for (int k = 1; k < 10; k++) { term *= -x2 / ((2 * k - 1) * (2 * k)); sum += term; }
    return sum;
}

double k_acos(double x) {
    if (x < -1.0) x = -1.0;
    if (x > 1.0)  x = 1.0;
    int neg = x < 0.0;
    double a = k_fabs(x);
    double r = k_sqrt(1.0 - a) *
               (1.5707288 + a * (-0.2121144 + a * (0.0742610 + a * -0.0187293)));
    return neg ? (3.141592653589793 - r) : r;
}

void *k_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *k_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

size_t k_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
