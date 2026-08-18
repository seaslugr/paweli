#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	int64_t total;
	int64_t count;
} Accumulator;

static int64_t bump(Accumulator *acc, int64_t v) {
	return acc->total + v;
}

static int64_t compute(int64_t n) {
	Accumulator *acc;
	int64_t i;
	int64_t result;

	acc = malloc(sizeof(Accumulator));
	acc->total = 0;
	acc->count = 0;

	i = 0;
	while (i < n) {
		acc->total = bump(acc, i);
		acc->count = acc->count + 1;
		i = i + 1;
	}

	result = acc->total;
	free(acc);
	return result;
}

int main(void) {
	printf("%lld\n", (long long)compute(5000000));
	return 0;
}
