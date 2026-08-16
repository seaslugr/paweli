#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Mirrors bench/pw/array_index.pw: the same start-at-8/double growth
// object_array_push() itself uses (see speedtest.sh's note on matching
// allocation strategy), then the same three read/write loops.
static void push(int64_t **values, size_t *count, size_t *capacity, int64_t value) {
	if (*count + 1 > *capacity) {
		*capacity = *capacity < 8 ? 8 : *capacity * 2;
		*values = realloc(*values, *capacity * sizeof(int64_t));
	}
	(*values)[(*count)++] = value;
}

int main(void) {
	int64_t n = 300000;
	int64_t *arr = NULL;
	size_t count = 0;
	size_t capacity = 0;
	int64_t total = 0;
	int64_t i;

	for (i = 0; i < n; i++) {
		push(&arr, &count, &capacity, i);
	}

	for (i = 0; i < n; i++) {
		total += arr[i];
	}
	for (i = 0; i < n; i++) {
		arr[i] = arr[i] + 1;
	}
	for (i = 0; i < n; i++) {
		total += arr[i];
	}

	printf("%lld\n", (long long)total);
	printf("%lld\n", (long long)arr[0]);
	printf("%lld\n", (long long)arr[n - 1]);

	free(arr);
	return 0;
}
