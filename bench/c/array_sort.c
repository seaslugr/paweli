#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int64_t lcg_next(int64_t seed) {
	return (seed * 1103515245 + 12345) % 2147483647;
}

static void push(int64_t **values, size_t *count, size_t *capacity, int64_t value) {
	if (*count + 1 > *capacity) {
		*capacity = *capacity < 8 ? 8 : *capacity * 2;
		*values = realloc(*values, *capacity * sizeof(int64_t));
	}
	(*values)[(*count)++] = value;
}

static int compare(const void *a, const void *b) {
	int64_t x;
	int64_t y;

	x = *(const int64_t *)a;
	y = *(const int64_t *)b;
	if (x < y) {
		return -1;
	}
	if (x > y) {
		return 1;
	}
	return 0;
}

int main(void) {
	int64_t n;
	int64_t *nums;
	size_t count;
	size_t capacity;
	int64_t seed;
	int64_t i;

	n = 20000;
	nums = NULL;
	count = 0;
	capacity = 0;
	seed = 42;

	for (i = 0; i < n; i++) {
		seed = lcg_next(seed);
		push(&nums, &count, &capacity, seed % 1000000);
	}

	qsort(nums, count, sizeof(int64_t), compare);

	printf("%lld\n", (long long)nums[0]);
	printf("%lld\n", (long long)nums[n - 1]);

	free(nums);
	return 0;
}
