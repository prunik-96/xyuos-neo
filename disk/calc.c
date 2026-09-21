/* Calculator -- reads an operator and two numbers, prints the result.
 * Build and run on xyuOS with:   run /calc.c
 *
 * Note the two operands are actually READ here; the original version used
 * num1 and num2 without ever assigning them. */

#include <stdio.h>

int main(void) {
    char operator;
    double num1, num2, result;

    printf("calculator -- operators + - * / , or q to quit\n");

    while (1) {
        printf("\noperator: ");
        if (scanf(" %c", &operator) != 1) break;

        if (operator == 'q' || operator == 'Q') {
            printf("goodbye.\n");
            break;
        }

        if (operator != '+' && operator != '-' &&
            operator != '*' && operator != '/') {
            printf("not an operator: %c\n", operator);
            continue;
        }

        printf("two numbers: ");
        if (scanf("%lf %lf", &num1, &num2) != 2) {
            printf("could not read two numbers\n");
            continue;
        }

        switch (operator) {
            case '+':
                result = num1 + num2;
                printf("%.6f + %.6f = %.6f\n", num1, num2, result);
                break;
            case '-':
                result = num1 - num2;
                printf("%.6f - %.6f = %.6f\n", num1, num2, result);
                break;
            case '*':
                result = num1 * num2;
                printf("%.6f * %.6f = %.6f\n", num1, num2, result);
                break;
            case '/':
                if (num2 != 0) {
                    result = num1 / num2;
                    printf("%.6f / %.6f = %.6f\n", num1, num2, result);
                } else {
                    printf("division by zero\n");
                }
                break;
        }
    }

    return 0;
}
