#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *concat(char *a, const char *b) {
	size_t a_len;
	size_t b_len;
	char *result;

	a_len = strlen(a);
	b_len = strlen(b);
	result = malloc(a_len + b_len + 1);
	memcpy(result, a, a_len);
	memcpy(result + a_len, b, b_len + 1);
	free(a);
	return result;
}

int main(void) {
	char *s;
	int n;
	int i;

	s = malloc(1);
	s[0] = '\0';
	n = 20000;

	for (i = 0; i < n; i++) {
		s = concat(s, "x");
	}

	printf("%zu\n", strlen(s));

	free(s);
	return 0;
}
