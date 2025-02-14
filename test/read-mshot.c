/* SPDX-License-Identifier: MIT */
/*
 * Description: test multishot read (IORING_OP_READ_MULTISHOT) on pipes,
 *		using ring provided buffers
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

#define BUF_SIZE	32
#define NR_BUFS		64
#define BUF_BGID	1

#define BR_MASK		(NR_BUFS - 1)

#define NR_OVERFLOW	(NR_BUFS / 4)

static int no_buf_ring, no_read_mshot;

static int test(int first_good, int async, int overflow)
{
	struct io_uring_buf_ring *br;
	struct io_uring_params p = { };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, fds[2], i;
	char tmp[32];
	char *buf;
	void *ptr;

	p.flags = IORING_SETUP_CQSIZE;
	if (!overflow)
		p.cq_entries = NR_BUFS + 1;
	else
		p.cq_entries = NR_OVERFLOW;
	ret = io_uring_queue_init_params(1, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}

	if (posix_memalign((void **) &buf, 4096, NR_BUFS * BUF_SIZE))
		return 1;

	br = io_uring_setup_buf_ring(&ring, NR_BUFS, BUF_BGID, 0, &ret);
	if (!br) {
		if (ret == -EINVAL) {
			no_buf_ring = 1;
			return 0;
		}
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	ptr = buf;
	for (i = 0; i < NR_BUFS; i++) {
		io_uring_buf_ring_add(br, buf, BUF_SIZE, i + 1, BR_MASK, i);
		buf += BUF_SIZE;
	}
	io_uring_buf_ring_advance(br, NR_BUFS);

	if (first_good) {
		sprintf(tmp, "this is buffer %d\n", 0);
		ret = write(fds[1], tmp, strlen(tmp));
	}

	sqe = io_uring_get_sqe(&ring);
	/* len == 0 means just use the defined provided buffer length */
	io_uring_prep_read_multishot(sqe, fds[0], 0, 0, BUF_BGID);
	if (async)
		sqe->flags |= IOSQE_ASYNC;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	/* write NR_BUFS + 1, or if first_good is set, NR_BUFS */
	for (i = 0; i < NR_BUFS + !first_good; i++) {
		/* prevent pipe buffer merging */
		usleep(1000);
		sprintf(tmp, "this is buffer %d\n", i + 1);
		ret = write(fds[1], tmp, strlen(tmp));
		if (ret != strlen(tmp)) {
			printf("write ret %d\n", ret);
			return 1;
		}
	}

	for (i = 0; i < NR_BUFS + 1; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait cqe failed %d\n", ret);
			return 1;
		}
		if (cqe->res < 0) {
			/* expected failure as we try to read one too many */
			if (cqe->res == -ENOBUFS && i == NR_BUFS)
				break;
			if (!i && cqe->res == -EINVAL) {
				no_read_mshot = 1;
				break;
			}
			fprintf(stderr, "%d: cqe res %d\n", i, cqe->res);
			return 1;
		}
		if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
			fprintf(stderr, "no buffer selected\n");
			return 1;
		}
		if (!(cqe->flags & IORING_CQE_F_MORE)) {
			/* we expect this on overflow */
			if (overflow && (i - 1 == NR_OVERFLOW))
				break;
			fprintf(stderr, "no more cqes\n");
			return 1;
		}
		/* should've overflown! */
		if (overflow && i > NR_OVERFLOW) {
			fprintf(stderr, "Expected overflow!\n");
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	free(ptr);
	return 0;
}

static int test_invalid(int async)
{
	struct io_uring_buf_ring *br;
	struct io_uring_params p = { };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	char fname[32] = ".mshot.%d.XXXXXX";
	int ret, fd;
	char *buf;

	p.flags = IORING_SETUP_CQSIZE;
	p.cq_entries = NR_BUFS;
	ret = io_uring_queue_init_params(1, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	fd = mkstemp(fname);
	if (fd < 0) {
		perror("mkstemp");
		return 1;
	}
	unlink(fname);

	if (posix_memalign((void **) &buf, 4096, BUF_SIZE))
		return 1;

	br = io_uring_setup_buf_ring(&ring, 1, BUF_BGID, 0, &ret);
	if (!br) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	io_uring_buf_ring_add(br, buf, BUF_SIZE, 1, BR_MASK, 0);
	io_uring_buf_ring_advance(br, 1);

	sqe = io_uring_get_sqe(&ring);
	/* len == 0 means just use the defined provided buffer length */
	io_uring_prep_read_multishot(sqe, fd, 0, 0, BUF_BGID);
	if (async)
		sqe->flags |= IOSQE_ASYNC;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait cqe failed %d\n", ret);
		return 1;
	}
	if (cqe->res != -EBADFD) {
		fprintf(stderr, "Got cqe res %d, wanted -EBADFD\n", cqe->res);
		return 1;
	}

	io_uring_cqe_seen(&ring, cqe);
	io_uring_queue_exit(&ring);
	free(buf);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = test(0, 0, 0);
	if (ret) {
		fprintf(stderr, "test 0 0 0 failed\n");
		return T_EXIT_FAIL;
	}
	if (no_buf_ring || no_read_mshot)
		return T_EXIT_SKIP;

	ret = test(0, 1, 0);
	if (ret) {
		fprintf(stderr, "test 0 1 0, failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(1, 0, 0);
	if (ret) {
		fprintf(stderr, "test 1 0 0 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(0, 0, 1);
	if (ret) {
		fprintf(stderr, "test 0 0 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(0, 1, 1);
	if (ret) {
		fprintf(stderr, "test 0 1 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(1, 0, 1);
	if (ret) {
		fprintf(stderr, "test 1 0 1, failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(1, 0, 1);
	if (ret) {
		fprintf(stderr, "test 1 0 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(1, 1, 1);
	if (ret) {
		fprintf(stderr, "test 1 1 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_invalid(0);
	if (ret) {
		fprintf(stderr, "test_invalid 0 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_invalid(1);
	if (ret) {
		fprintf(stderr, "test_invalid 1 failed\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
