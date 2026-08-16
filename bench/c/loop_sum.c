#include <stdint.h>
#include <stdio.h>

static int64_t compute(int64_t n) {
	int64_t sum;
	int64_t i;

	sum = 0;
	i = 0;
	while (i < n) {
		if (i % 7 == 0) {
			sum = sum + i * 3;
		} else {
			sum = sum + i - 1;
		}
		i = i + 1;
	}
	return sum;
}

int main(void) {
	printf("%lld\n", (long long)compute(20000000));
	return 0;
}
