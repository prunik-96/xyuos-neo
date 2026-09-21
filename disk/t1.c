int main(void)
{
    int a = 9, b = 7, s = 0, i;
    for (i = 0; i < b; i++) s += a;   /* 63 via a loop */
    if (s > 100) return 1;
    return s;
}
