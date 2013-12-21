#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>

#if RAND_MAX < (1 << 16)
#error Code assumes at least two bytes of randomness
#endif

/* Padding is because current algo doesn't quite go to end of file. */
#define END_PADDING 1024

static void randomize(uint16_t *data, unsigned int datasize)
{
	unsigned int i;

	for (i = 0; i < datasize; i++)
		data[i] = random();
}

static int write_random(int fd, unsigned int len)
{
	uint16_t data[512];
	unsigned int done, amount;

	for (done = 0; done < len; done += amount) {
		randomize(data, sizeof(data)/2);
		amount = len - done;
		if (amount > sizeof(data))
			amount = sizeof(data);

		if (write(fd, data, amount) != amount) {
			perror("write");
			return 0;
		}
	}
	return 1;
}

int main(int argc, char *argv[])
{
	unsigned int i, size, repsize;
	uint16_t *repeat;

	if (argc != 4) {
		fprintf(stderr,
			"Usage: %s <mbytes> <repeatsize> <numrepeats>\n",
			argv[0]);
		exit(1);
	}

	/* Layout is [random][repeat][padding] [random][repeat][padding]... */
	size = atol(argv[1]) * 1024 * 1024 / (atol(argv[3]) + 1);
	repsize = atoi(argv[2]);

	size -= repsize + END_PADDING;

	repeat = malloc(repsize + 2);
	randomize(repeat, repsize/2 + 1);

	for (i = 0; i <= atol(argv[3]); i++) {
		if (!write_random(STDOUT_FILENO, size))
			return 1;
		if (write(STDOUT_FILENO, repeat, repsize) != repsize) {
			perror("write");
			return 1;
		}
		if (!write_random(STDOUT_FILENO, END_PADDING))
			return 1;
	}
	free(repeat);
	return 0;
}
