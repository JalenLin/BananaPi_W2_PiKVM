// SPDX-License-Identifier: GPL-2.0-only
/*
 * Physical-address aliasing test (docs/09 §7).
 *
 * The kernel's early memtest writes one fixed pattern everywhere and reads it
 * back, so it cannot see two physical addresses that are really the same
 * DRAM: both get the same value. This writes every 8-byte word with its OWN
 * physical address instead, then reads them all back. A word at X that
 * reads back Y means the write meant for Y landed at X -- which names both
 * ends of the alias.
 *
 *   aliastest <MiB>
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PS 4096UL
#define PFN_MASK ((1ULL << 55) - 1)
#define PRESENT (1ULL << 63)

static int read_pfns(uint8_t *buf, size_t np, uint64_t *pfn)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) { perror("pagemap"); return -1; }
	off_t off = ((uintptr_t)buf / PS) * 8;
	ssize_t want = np * 8, got = 0;
	while (got < want) {
		ssize_t r = pread(fd, (char *)pfn + got, want - got, off + got);
		if (r <= 0) { perror("pread"); close(fd); return -1; }
		got += r;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? strtoul(argv[1], NULL, 0) : 256;
	size_t len = mb << 20, np = len / PS, i, j;
	uint64_t *pfn, *pfn2, minp = ~0ULL, maxp = 0, bad = 0, moved = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	uint8_t *buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return 1; }
	for (i = 0; i < np; i++)
		buf[i * PS] = 1;			/* fault every page in */
	mlock(buf, len);

	pfn = malloc(np * 8);
	pfn2 = malloc(np * 8);
	if (!pfn || !pfn2 || read_pfns(buf, np, pfn))
		return 1;

	for (i = 0; i < np; i++) {
		uint64_t pa = (pfn[i] & PFN_MASK) * PS;
		uint64_t *w = (uint64_t *)(buf + i * PS);
		if (!(pfn[i] & PRESENT)) continue;
		if (pa < minp) minp = pa;
		if (pa > maxp) maxp = pa;
		for (j = 0; j < PS / 8; j++)
			w[j] = pa + j * 8;
	}
	printf("ALIAS %zu MiB: wrote %zu pages, phys 0x%llx..0x%llx\n", mb, np,
	       (unsigned long long)minp, (unsigned long long)maxp + PS);

	if (read_pfns(buf, np, pfn2))
		return 1;

	for (i = 0; i < np; i++) {
		uint64_t pa = (pfn[i] & PFN_MASK) * PS;
		uint64_t *w = (uint64_t *)(buf + i * PS);
		if (!(pfn[i] & PRESENT)) continue;
		if (pfn2[i] != pfn[i]) { moved++; continue; }	/* migrated */
		for (j = 0; j < PS / 8; j++) {
			uint64_t want = pa + j * 8;
			if (w[j] == want) continue;
			if (bad < 40)
				printf("ALIAS bad: phys 0x%09llx holds 0x%016llx  (delta %+lld)\n",
				       (unsigned long long)want,
				       (unsigned long long)w[j],
				       (long long)(w[j] - want));
			bad++;
			break;				/* one line per page */
		}
	}
	printf("ALIAS %zu MiB: %llu bad pages, %llu migrated, of %zu\n",
	       mb, (unsigned long long)bad, (unsigned long long)moved, np);
	return bad ? 2 : 0;
}
