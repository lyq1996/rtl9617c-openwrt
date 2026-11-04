// Minimal i2c-dev random-read tool for the PON A2h EEPROM.
// usage: i2cpeek <bus> <addr> <page|-1> <offset> <len>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <unistd.h>

static int xfer(int fd, unsigned char addr, unsigned char *w, int wn,
		unsigned char *r, int rn)
{
	struct i2c_msg msg[2];
	struct i2c_rdwr_ioctl_data data;
	int n = 0;

	msg[n].addr = addr;
	msg[n].flags = 0;
	msg[n].len = wn;
	msg[n].buf = w;
	n++;
	if (rn) {
		msg[n].addr = addr;
		msg[n].flags = I2C_M_RD;
		msg[n].len = rn;
		msg[n].buf = r;
		n++;
	}
	data.msgs = msg;
	data.nmsgs = n;
	return ioctl(fd, I2C_RDWR, &data) < 0 ? -errno : 0;
}

int main(int argc, char **argv)
{
	int fd, page, offset, len, i, ret, raw = 0, arg = 1;
	unsigned char addr, reg, buf[256];

	if (argc > 1 && !strcmp(argv[1], "-r")) {
		raw = 1;
		arg = 2;
	}
	if (argc != arg + 5) {
		fprintf(stderr, "usage: %s [-r] bus addr page offset len\n", argv[0]);
		return 2;
	}
	fd = open(argv[arg], O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	addr = strtoul(argv[arg + 1], NULL, 0);
	page = strtol(argv[arg + 2], NULL, 0);
	offset = strtol(argv[arg + 3], NULL, 0);
	len = strtol(argv[arg + 4], NULL, 0);
	if (len <= 0 || len > (int)sizeof(buf)) return 2;
	if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
		perror("I2C_SLAVE_FORCE");
		return 1;
	}
	if (page >= 0) {
		unsigned char p[2] = {0x7f, (unsigned char)page};
		ret = xfer(fd, addr, p, 2, NULL, 0);
		if (ret) { fprintf(stderr, "page: %s\n", strerror(-ret)); return 1; }
	}
	reg = (unsigned char)offset;
	ret = xfer(fd, addr, &reg, 1, buf, len);
	if (ret) { fprintf(stderr, "read: %s\n", strerror(-ret)); return 1; }
	if (raw) {
		if (fwrite(buf, 1, len, stdout) != (size_t)len) return 1;
		close(fd);
		return 0;
	}
	for (i = 0; i < len; i++) {
		if (!(i % 16)) printf("%02x:", offset + i);
		printf(" %02x", buf[i]);
		if ((i % 16) == 15 || i == len - 1) printf("\n");
	}
	close(fd);
	return 0;
}
