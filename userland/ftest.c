/* Does userland floating point work at all?
 *
 * Every program so far has been built with -mgeneral-regs-only, which forbids
 * the SSE registers doubles are passed in, so nothing has ever tried. The
 * kernel saves and restores FPU state per process (fxsave), so it should --
 * but "should" is not an answer, and Python without floats is not Python.
 *
 * This also exercises the calling convention across a process switch: the
 * loop runs long enough to be preempted many times, and a scheduler that
 * dropped the FPU state would show up as an answer that drifts. */

#include <stdio.h>
#include <math.h>

int main(void) {
    double a = 1.0 / 3.0;
    printf("1/3        = %f\n", a);
    printf("a*3        = %f\n", a * 3.0);

    float f = 2.5f;
    printf("float 2.5*4= %f\n", (double)(f * 4.0f));

    printf("fabs(-7.25)= %f\n", fabs(-7.25));
    printf("ldexp(3,4) = %f\n", ldexp(3.0, 4));

    /* Long enough to be preempted repeatedly. Every term is exact in binary
     * floating point, so the sum is exact too: if it comes out anything but
     * 2.0, state was lost across a switch. */
    double sum = 0.0;
    for (int i = 0; i < 4000000; i++) sum += 0.5 / 1000000.0;
    printf("sum        = %f  (expected 2.000000)\n", sum);

    /* Integers and doubles in the same expression, which is where a broken
     * calling convention usually shows first. */
    int n = 7;
    printf("n/2.0      = %f\n", n / 2.0);

    printf("%s\n", (sum > 1.999 && sum < 2.001) ? "FLOAT OK" : "FLOAT BROKEN");
    return 0;
}
