#include <arcan_shmif.h>
#include <arcan_shmif_server.h>

#include <ftw.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <signal.h>

#include "../a12.h"
#include "../a12_int.h"
#include "anet_helper.h"
#include "directory.h"

#include <sys/types.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>

void anet_directory_ioloop(struct ioloop_shared* I)
{
	int errmask = POLLERR | POLLNVAL | POLLHUP;
	struct pollfd fds[3] =
	{
		{.fd = I->userfd, .events = POLLIN | errmask},
		{.fd = I->fdin, .events = POLLIN | errmask},
		{.fd = -1, .events = POLLOUT | errmask}
	};

	uint8_t inbuf[9000];
	uint8_t* outbuf = NULL;
	uint64_t ts = 0;

	fcntl(I->fdin, F_SETFD, FD_CLOEXEC);
	fcntl(I->fdout, F_SETFD, FD_CLOEXEC);

	size_t outbuf_sz = a12_flush(I->S, &outbuf, A12_FLUSH_ALL);

	if (outbuf_sz)
		fds[2].fd = I->fdout;

/* regular simple processing loop, wait for DIRECTORY-LIST command */
	while (a12_ok(I->S) && -1 != poll(fds, 3, -1)){
		if ((fds[0].revents | fds[1].revents | fds[2].revents) & errmask){
			if (fds[0].revents & errmask){
				I->on_userfd(I, false);
			}

			break;
		}

		if (fds[0].revents & POLLIN){
			I->on_userfd(I, true);
		}

		if ((fds[2].revents & POLLOUT) && outbuf_sz){
			ssize_t nw = write(I->fdout, outbuf, outbuf_sz);
			if (nw > 0){
				outbuf += nw;
				outbuf_sz -= nw;
			}
		}

		if (fds[1].revents & POLLIN){
			ssize_t nr = recv(I->fdin, inbuf, 9000, 0);
			if (-1 == nr && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR){
				a12int_trace(A12_TRACE_DIRECTORY, "shutdown:reason=rw_error");
				break;
			}
			else if (0 == nr){
				a12int_trace(A12_TRACE_DIRECTORY, "shutdown:reason=closed");
				break;
			}
			a12_unpack(I->S, inbuf, nr, I, I->on_event);

/* check if there has been a change to the directory state after each unpack */
			uint64_t new_ts;
			if (I->on_directory){
				struct appl_meta* dir = a12int_get_directory(I->S, &new_ts);
				if (new_ts != ts){
					ts = new_ts;
					if (!I->on_directory(I, dir))
						return;
				}
			}
		}

		if (!outbuf_sz){
			outbuf_sz = a12_flush(I->S, &outbuf, A12_FLUSH_ALL);
			if (!outbuf_sz && I->shutdown)
				break;
		}

		fds[0].revents = fds[1].revents = fds[2].revents = 0;
		fds[2].fd = outbuf_sz ? I->fdout : -1;
		fds[0].fd = I->userfd;
	}
}

FILE* file_to_membuf(FILE* applin, char** out, size_t* out_sz)
{
	if (!applin)
		return NULL;

	FILE* applbuf = open_memstream(out, out_sz);
	if (!applbuf){
		return NULL;
	}

	char buf[4096];
	size_t nr;
	bool ok = true;

	while ((nr = fread(buf, 1, 4096, applin))){
		if (1 != fwrite(buf, nr, 1, applbuf)){
			ok = false;
			break;
		}
	}

	if (!ok){
		fclose(applbuf);
		return NULL;
	}

/* actually keep both in order to allow appending elsewhere */
	fflush(applbuf);
	return applbuf;
}

bool build_appl_pkg(const char* name, struct appl_meta* dst, int fd)
{
	/* just want directories */
	fchdir(fd);

	struct stat sbuf;
	if (-1 == stat(name, &sbuf) || (sbuf.st_mode & S_IFMT) != S_IFDIR){
		a12int_trace(A12_TRACE_DIRECTORY, "kind=build_appl:name=%s:error=not_dir", name);
		return false;
	}
	chdir(name);

	struct appl_meta* res = malloc(sizeof(struct appl_meta));
	if (!res){
		fchdir(fd);
		return false;
	}

	*res = (struct appl_meta){0};

	size_t buf_sz;
	FILE* cmd = popen("tar cf - .", "r");

	dst->handle = file_to_membuf(cmd, &dst->buf, &buf_sz);
	if (cmd)
		pclose(cmd);

	if (!dst->handle){
		fchdir(fd);
		free(res);
		return false;
	}

	snprintf(dst->appl.name, COUNT_OF(dst->appl.name), "%s", name);
	dst->buf_sz = buf_sz;
	dst->next = res;

	blake3_hasher hash;
	blake3_hasher_init(&hash);
	blake3_hasher_update(&hash, dst->buf, dst->buf_sz);
	blake3_hasher_finalize(&hash, (uint8_t*)dst->hash, 4);

	fchdir(fd);

	return true;
}
