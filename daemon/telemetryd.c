/* Author: Domen Puncer <domen@cba.si>.  License: WTFPL */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>


static int nodaemon;
static int baudrate = 115200;
static const char *uart = "/dev/ttyS0";
static int interval = 60;


static void help(const char *prog)
{
	printf("usage: %s [options]\n", prog);
	printf(
		"       -h              this text\n"
		"       -f              don't daemonize\n"
		"       -b baudrate     set baudrate (default 115200)\n"
		"       -d serial       set serial device (default /dev/ttyS0)\n"
		"       -i interval     set reading interval (in seconds, default 60)\n"
	      );
}

static int parse_options(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "hfb:d:i:")) != EOF) {
		switch (c) {
		case 'h':
			help(argv[0]);
			return -1;
		case 'f':
			nodaemon = 1;
			break;
		case 'b':
			baudrate = atoi(optarg);
			break;
		case 'd':
			uart = optarg;
			break;
		case 'i':
			interval = atoi(optarg);
			if (interval < 5) {
				fprintf(stderr, "%s: interval too small, setting to 5\n", argv[0]);
				interval = 5;
			}
			break;
		default:
			fprintf(stderr, "invalid option %c\n", c);
			help(argv[0]);
			return -1;
		}
	}
	return 0;
}

/* must use monotonic clock, if we're doing comparisons, so time changes don't affect us */
time_t time_mono(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		perror("clock_gettime");
		return -1;
	}

	return ts.tv_sec;
}

static void daemonize(void)
{
	/* source: http://www.enderunix.org/docs/eng/daemon.php */
	int i;

	if (getppid() == 1)
		return; /* already a daemon */

	i = fork();
	if (i<0)
		exit(1); /* fork error */
	if (i>0)
		exit(0); /* parent exits */

	/* child (daemon) continues */
	setsid(); /* obtain a new process group */

	/* blackhole reroute */
	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
}

static int readline(int fd, char *line, int maxlen)
{
	int pos = 0;
	line[pos] = '\0';

	while (1) {
		char c;
		int n;
		n = read(fd, &c, 1);
		if (n == -1) {
			if (errno == EAGAIN) {
				/* timeout, return partial line */
				if (pos) {
					line[pos] = '\0';
					return pos;
				}
				return -EAGAIN;
			}
			perror("read");
			return -errno;
		}
		else if (n == 0) {
			/* timeout */
			if (pos) {
				line[pos] = '\0';
				return pos;
			}
			//fprintf(stderr, "stdin closed\n");
			return 0;
		}

		if (c == '\n' || c == '\r') {
			/* newline, return the line */
			if (pos) {
				line[pos] = '\0';
				return pos;
			}
			/* otherwise ignore */
		}
		else if (pos+1 < maxlen) {
			line[pos++] = c;
		}
	}

	/* doesn't come this far */
}

int main(int argc, char **argv)
{
	int fd;
	struct termios tio, sio;
	char line[128];

	if (parse_options(argc, argv) < 0)
		return -1;

	//fd = open(uart, O_RDWR | O_NONBLOCK);
	fd = open(uart, O_RDWR);
	if (fd < 0) {
		perror(uart);
		return -1;
	}

	if (tcgetattr(fd, &sio) < 0)
		return -1;

	tio = sio;
	cfmakeraw(&tio);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 1;

	cfsetispeed(&tio, baudrate);
	cfsetospeed(&tio, baudrate);

	if (tcsetattr(fd, TCSAFLUSH, &tio) < 0) {
		perror("tcsetattr");
		close(fd);
		return -1;
	}

	if (!nodaemon)
		daemonize();

	int tries = 4;
	while (--tries) {
		write(fd, "atz\n", 4);
		readline(fd, line, sizeof(line));
		if (strcmp(line, "atz") == 0) /* discard echo */
			readline(fd, line, sizeof(line));
		printf("serial receive (atz reply): %s\n", line);

		if (strncmp(line, "OK", 2) == 0) {
			break;
		}
	}
	if (!tries) {
		fprintf(stderr, "didn't detect telemetry circuit on %s\n", uart);
	}

	time_t scan_read = time_mono();

	while (1) {
		/* send commands */
		time_t now = time_mono();
		if (now - scan_read >= interval) {
			const char *cmd = "1w scan_read\n";
			write(fd, cmd, strlen(cmd));
			scan_read = now;
		}

		/* parse replies */
		int n = readline(fd, line, sizeof(line));
		if (n < 0 && n != -EAGAIN)
			return -1;
		if (n < 1)
			continue;
		printf("serial receive: %s\n", line);

		/* 1WIRE DS18B20 288055bb03000052 20.75 */
		if (strncmp(line, "1WIRE DS18B20 ", 14) == 0) {
			FILE *f;
			char filename[128];
			char *id = &line[14];
			id[16] = '\0';
			if (strspn(id, "0123456789abcdef") != 16) {
				fprintf(stderr, "device id (%s) invalid\n", id);
				continue;
			}
			snprintf(filename, sizeof(filename), "/tmp/telemetry.ds18b20.%s", id);
			f = fopen(filename, "w");
			if (!f) {
				perror(filename);
				continue;
			}
			fprintf(f, "%s\n", id+17);
			fclose(f);
		}
	}

	tcsetattr(fd, TCSANOW, &sio);
	close(fd);

	return 0;
}
