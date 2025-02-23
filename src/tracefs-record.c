// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2022 Google Inc, Steven Rostedt <rostedt@goodmis.org>
 */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <kbuffer.h>

#include "tracefs.h"
#include "tracefs-local.h"

enum {
	TC_STOP			= 1 << 0,   /* Stop reading */
	TC_PERM_NONBLOCK	= 1 << 1,   /* read is always non blocking */
	TC_NONBLOCK		= 1 << 2,   /* read is non blocking */
};

struct tracefs_cpu {
	int		fd;
	int		flags;
	int		nfds;
	int		ctrl_pipe[2];
	int		splice_pipe[2];
	int		pipe_size;
	int		subbuf_size;
	int		buffered;
	int		splice_read_flags;
	struct kbuffer	*kbuf;
	void		*buffer;
	void		*mapping;
};

/**
 * tracefs_cpu_alloc_fd - create a tracefs_cpu instance for an existing fd
 * @fd: The file descriptor to attach the tracefs_cpu to
 * @subbuf_size: The expected size to read the subbuffer with
 * @nonblock: If true, the file will be opened in O_NONBLOCK mode
 *
 * Return a descriptor that can read the tracefs trace_pipe_raw file
 * that is associated with the given @fd and must be read in @subbuf_size.
 *
 * Returns NULL on error.
 */
struct tracefs_cpu *
tracefs_cpu_alloc_fd(int fd, int subbuf_size, bool nonblock)
{
	struct tracefs_cpu *tcpu;
	int mode = O_RDONLY;
	int ret;

	tcpu = calloc(1, sizeof(*tcpu));
	if (!tcpu)
		return NULL;

	if (nonblock) {
		mode |= O_NONBLOCK;
		tcpu->flags |= TC_NONBLOCK | TC_PERM_NONBLOCK;
	}

	tcpu->splice_pipe[0] = -1;
	tcpu->splice_pipe[1] = -1;

	tcpu->fd = fd;

	tcpu->subbuf_size = subbuf_size;

	if (tcpu->flags & TC_PERM_NONBLOCK) {
		tcpu->ctrl_pipe[0] = -1;
		tcpu->ctrl_pipe[1] = -1;
	} else {
		/* ctrl_pipe is used to break out of blocked reads */
		ret = pipe(tcpu->ctrl_pipe);
		if (ret < 0)
			goto fail;
		if (tcpu->ctrl_pipe[0] > tcpu->fd)
			tcpu->nfds = tcpu->ctrl_pipe[0] + 1;
		else
			tcpu->nfds = tcpu->fd + 1;
	}

	return tcpu;
 fail:
	free(tcpu);
	return NULL;
}

static struct tracefs_cpu *cpu_open(struct tracefs_instance *instance,
				    const char *path_fmt, int cpu, bool nonblock)
{
	struct tracefs_cpu *tcpu;
	struct tep_handle *tep;
	struct kbuffer *kbuf;
	char path[128];
	int mode = O_RDONLY;
	int subbuf_size;
	int ret;
	int fd;

	if (nonblock)
		mode |= O_NONBLOCK;

	sprintf(path, path_fmt, cpu);

	fd = tracefs_instance_file_open(instance, path, mode);
	if (fd < 0)
		return NULL;

	tep = tep_alloc();
	if (!tep)
		goto fail;

	/* Get the size of the page */
	ret = tracefs_load_headers(NULL, tep);
	if (ret < 0)
		goto fail;

	subbuf_size = tep_get_sub_buffer_size(tep);

	kbuf = tep_kbuffer(tep);
	if (!kbuf)
		goto fail;

	tep_free(tep);
	tep = NULL;

	tcpu = tracefs_cpu_alloc_fd(fd, subbuf_size, nonblock);
	if (!tcpu)
		goto fail;

	tcpu->kbuf = kbuf;

	return tcpu;
 fail:
	tep_free(tep);
	close(fd);
	return NULL;
}

/**
 * tracefs_cpu_open - open an instance raw trace file
 * @instance: the instance (NULL for toplevel) of the cpu raw file to open
 * @cpu: The CPU that the raw trace file is associated with
 * @nonblock: If true, the file will be opened in O_NONBLOCK mode
 *
 * Return a descriptor that can read the tracefs trace_pipe_raw file
 * for a give @cpu in a given @instance.
 *
 * Returns NULL on error.
 */
struct tracefs_cpu *
tracefs_cpu_open(struct tracefs_instance *instance, int cpu, bool nonblock)
{
	return cpu_open(instance, "per_cpu/cpu%d/trace_pipe_raw", cpu, nonblock);
}

/**
 * tracefs_cpu_snapshot_open - open an instance snapshot raw trace file
 * @instance: the instance (NULL for toplevel) of the cpu raw file to open
 * @cpu: The CPU that the raw trace file is associated with
 * @nonblock: If true, the file will be opened in O_NONBLOCK mode
 *
 * Return a descriptor that can read the tracefs snapshot_raw file
 * for a give @cpu in a given @instance.
 *
 * In nonblock mode, it will block if the snapshot is empty and wake up
 * when there's a new snapshot.
 *
 * Returns NULL on error.
 */
struct tracefs_cpu *
tracefs_cpu_snapshot_open(struct tracefs_instance *instance, int cpu, bool nonblock)
{
	return cpu_open(instance, "per_cpu/cpu%d/snapshot_raw", cpu, nonblock);
}

/**
 * tracefs_snapshot_snap - takes a snapshot (allocates if necessary)
 * @instance: The instance to take a snapshot on
 *
 * Takes a snapshot of the current ring buffer.
 *
 * Returns 0 on success, -1 on error.
 */
int tracefs_snapshot_snap(struct tracefs_instance *instance)
{
	int ret;

	ret = tracefs_instance_file_write(instance, "snapshot", "1");
	return ret < 0 ? -1 : 0;
}

/**
 * tracefs_snapshot_clear - clears the snapshot
 * @instance: The instance to clear the snapshot
 *
 * Clears the snapshot buffer for the @instance.
 *
 * Returns 0 on success, -1 on error.
 */
int tracefs_snapshot_clear(struct tracefs_instance *instance)
{
	int ret;

	ret = tracefs_instance_file_write(instance, "snapshot", "2");
	return ret < 0 ? -1 : 0;
}

/**
 * tracefs_snapshot_free - frees the snapshot
 * @instance: The instance to free the snapshot
 *
 * Frees the snapshot for the given @instance.
 *
 * Returns 0 on success, -1 on error.
 */
int tracefs_snapshot_free(struct tracefs_instance *instance)
{
	int ret;

	ret = tracefs_instance_file_write(instance, "snapshot", "0");
	return ret < 0 ? -1 : 0;
}

/**
 * tracefs_cpu_open_mapped - open an instance raw trace file and map it
 * @instance: the instance (NULL for toplevel) of the cpu raw file to open
 * @cpu: The CPU that the raw trace file is associated with
 * @nonblock: If true, the file will be opened in O_NONBLOCK mode
 *
 * Return a descriptor that can read the tracefs trace_pipe_raw file
 * for a give @cpu in a given @instance.
 *
 * Returns NULL on error.
 */
struct tracefs_cpu *
tracefs_cpu_open_mapped(struct tracefs_instance *instance, int cpu, bool nonblock)
{
	struct tracefs_cpu *tcpu;

	tcpu = tracefs_cpu_open(instance, cpu, nonblock);
	if (!tcpu)
		return NULL;

	tracefs_cpu_map(tcpu);

	return tcpu;
}

static void close_fd(int fd)
{
	if (fd < 0)
		return;
	close(fd);
}

/**
 * tracefs_cpu_free_fd - clean up the tracefs_cpu descriptor
 * @tcpu: The descriptor created with tracefs_cpu_alloc_fd()
 *
 * Closes all the internal file descriptors that were opened by
 * tracefs_cpu_alloc_fd(), and frees the descriptor.
 */
void tracefs_cpu_free_fd(struct tracefs_cpu *tcpu)
{
	close_fd(tcpu->ctrl_pipe[0]);
	close_fd(tcpu->ctrl_pipe[1]);
	close_fd(tcpu->splice_pipe[0]);
	close_fd(tcpu->splice_pipe[1]);

	tfs_unmap(tcpu->mapping);
	kbuffer_free(tcpu->kbuf);
	free(tcpu);
}

/**
 * tracefs_cpu_close - clean up and close a raw trace descriptor
 * @tcpu: The descriptor created with tracefs_cpu_open()
 *
 * Closes all the file descriptors associated to the trace_pipe_raw
 * opened by tracefs_cpu_open().
 */
void tracefs_cpu_close(struct tracefs_cpu *tcpu)
{
	if (!tcpu)
		return;

	close(tcpu->fd);
	tracefs_cpu_free_fd(tcpu);
}

/**
 * tracefs_cpu_read_size - Return the size of the sub buffer
 * @tcpu: The descriptor that holds the size of the sub buffer
 *
 * A lot of the functions that read the data from the trace_pipe_raw
 * expect the caller to have allocated enough space to store a full
 * subbuffer. Calling this function is a requirement to do so.
 */
int tracefs_cpu_read_size(struct tracefs_cpu *tcpu)
{
	if (!tcpu)
		return -1;
	return tcpu->subbuf_size;
}

bool tracefs_cpu_is_mapped(struct tracefs_cpu *tcpu)
{
	return tcpu->mapping != NULL;
}

/**
 * tracefs_mapped_is_supported - find out if memory mapping is supported
 *
 * Return true if the ring buffer can be memory mapped, or false on
 * error or it cannot be.
 */
bool tracefs_mapped_is_supported(void)
{
	struct tracefs_cpu *tcpu;
	bool ret;

	tcpu = tracefs_cpu_open_mapped(NULL, 0, false);
	if (!tcpu)
		return false;
	ret = tracefs_cpu_is_mapped(tcpu);
	tracefs_cpu_close(tcpu);
	return ret;
}

int tracefs_cpu_map(struct tracefs_cpu *tcpu)
{
	if (tcpu->mapping)
		return 0;

	tcpu->mapping = tfs_mmap(tcpu->fd, tcpu->kbuf);
	return tcpu->mapping ? 0 : -1;
}

void tracefs_cpu_unmap(struct tracefs_cpu *tcpu)
{
	if (!tcpu->mapping)
		return;

	tfs_unmap(tcpu->mapping);
}

static void set_nonblock(struct tracefs_cpu *tcpu)
{
	long flags;

	if (tcpu->flags & TC_NONBLOCK)
		return;

	flags = fcntl(tcpu->fd, F_GETFL);
	fcntl(tcpu->fd, F_SETFL, flags | O_NONBLOCK);
	tcpu->flags |= TC_NONBLOCK;
}

static void unset_nonblock(struct tracefs_cpu *tcpu)
{
	long flags;

	if (!(tcpu->flags & TC_NONBLOCK))
		return;

	flags = fcntl(tcpu->fd, F_GETFL);
	flags &= ~O_NONBLOCK;
	fcntl(tcpu->fd, F_SETFL, flags);
	tcpu->flags &= ~TC_NONBLOCK;
}

/*
 * If set to blocking mode, block until the watermark has been
 * reached, or the control has said to stop. If the contol is
 * set, then nonblock will be set to true on the way out.
 */
static int wait_on_input(struct tracefs_cpu *tcpu, bool nonblock)
{
	fd_set rfds;
	int ret;

	if (tcpu->flags & TC_PERM_NONBLOCK)
		return 1;

	if (nonblock) {
		set_nonblock(tcpu);
		return 1;
	} else {
		unset_nonblock(tcpu);
	}

	FD_ZERO(&rfds);
	FD_SET(tcpu->fd, &rfds);
	FD_SET(tcpu->ctrl_pipe[0], &rfds);

	ret = select(tcpu->nfds, &rfds, NULL, NULL, NULL);

	/* Let the application decide what to do with signals and such */
	if (ret < 0)
		return ret;

	if (FD_ISSET(tcpu->ctrl_pipe[0], &rfds)) {
		/* Flush the ctrl pipe */
		read(tcpu->ctrl_pipe[0], &ret, 1);

		/* Make nonblock as it is now stopped */
		set_nonblock(tcpu);
		/* Permanently set unblock */
		tcpu->flags |= TC_PERM_NONBLOCK;
	}

	return FD_ISSET(tcpu->fd, &rfds);
}

/* If nonblock is set, set errno to EAGAIN on no data */
static int mmap_read(struct tracefs_cpu *tcpu, void *buffer, bool nonblock)
{
	void *mapping = tcpu->mapping;
	int ret;

	ret = tfs_mmap_read(mapping, buffer);
	if (ret <= 0) {
		if (!ret && nonblock)
			errno = EAGAIN;
		return ret;
	}

	/* Write full sub-buffer size, but zero out empty space */
	if (ret < tcpu->subbuf_size)
		memset(buffer + ret, 0, tcpu->subbuf_size - ret);
	return tcpu->subbuf_size;
}

/**
 * tracefs_cpu_read - read from the raw trace file
 * @tcpu: The descriptor representing the raw trace file
 * @buffer: Where to read into (must be at least the size of the subbuffer)
 * @nonblock: Hint to not block on the read if there's no data.
 *
 * Reads the trace_pipe_raw files associated to @tcpu into @buffer.
 * @buffer must be at least the size of the sub buffer of the ring buffer,
 * which is returned by tracefs_cpu_read_size().
 *
 * If @nonblock is set, and there's no data available, it will return
 * immediately. Otherwise depending on how @tcpu was opened, it will
 * block. If @tcpu was opened with nonblock set, then this @nonblock
 * will make no difference.
 *
 * Returns the amount read or -1 on error.
 */
int tracefs_cpu_read(struct tracefs_cpu *tcpu, void *buffer, bool nonblock)
{
	int ret;

	/*
	 * If nonblock is set, then the wait_on_input() will return
	 * immediately, if there's nothing in the buffer, with
	 * ret == 0.
	 */
	ret = wait_on_input(tcpu, nonblock);
	if (ret <= 0)
		return ret;

	if (tcpu->mapping)
		return mmap_read(tcpu, buffer, nonblock);

	ret = read(tcpu->fd, buffer, tcpu->subbuf_size);

	/* It's OK if there's no data to read */
	if (ret < 0 && errno == EAGAIN) {
		/* Reset errno */
		errno = 0;
		ret = 0;
	}

	return ret;
}

static bool get_buffer(struct tracefs_cpu *tcpu)
{
	if (!tcpu->buffer) {
		tcpu->buffer = malloc(tcpu->subbuf_size);
		if (!tcpu->buffer)
			return false;
	}
	return true;
}

/**
 * tracefs_cpu_read_buf - read from the raw trace file and return kbuffer
 * @tcpu: The descriptor representing the raw trace file
 * @nonblock: Hint to not block on the read if there's no data.
 *
 * Reads the trace_pipe_raw files associated to @tcpu and returns a kbuffer
 * associated with the read that can be used to parse events.
 *
 * If @nonblock is set, and there's no data available, it will return
 * immediately. Otherwise depending on how @tcpu was opened, it will
 * block. If @tcpu was opened with nonblock set, then this @nonblock
 * will make no difference.
 *
 * Returns a kbuffer associated to the next sub-buffer or NULL on error
 * or no data to read with nonblock set (EAGAIN will be set).
 *
 * The kbuffer returned should not be freed!
 */
struct kbuffer *tracefs_cpu_read_buf(struct tracefs_cpu *tcpu, bool nonblock)
{
	int ret;

	/* If mapping is enabled, just use it directly */
	if (tcpu->mapping) {
		ret = wait_on_input(tcpu, nonblock);
		if (ret <= 0)
			return NULL;

		ret = tfs_mmap_load_subbuf(tcpu->mapping, tcpu->kbuf);
		return ret > 0 ? tcpu->kbuf : NULL;
	}

	if (!get_buffer(tcpu))
		return NULL;

	ret = tracefs_cpu_read(tcpu, tcpu->buffer, nonblock);
	if (ret <= 0)
		return NULL;

	kbuffer_load_subbuffer(tcpu->kbuf, tcpu->buffer);
	return tcpu->kbuf;
}

static int init_splice(struct tracefs_cpu *tcpu)
{
	char *buf;
	int ret;

	if (tcpu->splice_pipe[0] >= 0)
		return 0;

	ret = pipe(tcpu->splice_pipe);
	if (ret < 0)
		return ret;

	if (tfs_str_read_file("/proc/sys/fs/pipe-max-size", &buf, false)) {
		int size = atoi(buf);
		fcntl(tcpu->splice_pipe[0], F_SETPIPE_SZ, &size);
		free(buf);
	}

	ret = fcntl(tcpu->splice_pipe[0], F_GETPIPE_SZ, &tcpu->pipe_size);
	/*
	 * F_GETPIPE_SZ was introduced in 2.6.35, ftrace was introduced
	 * in 2.6.31. If we are running on an older kernel, just fall
	 * back to using subbuf_size for splice(). It could also return
	 * the size of the pipe and not set pipe_size.
	 */
	if (ret > 0 && !tcpu->pipe_size)
		tcpu->pipe_size = ret;
	else if (ret < 0)
		tcpu->pipe_size = tcpu->subbuf_size;

	tcpu->splice_read_flags = SPLICE_F_MOVE;
	if (tcpu->flags & TC_NONBLOCK)
		tcpu->splice_read_flags |= SPLICE_F_NONBLOCK;

	return 0;
}

/**
 * tracefs_cpu_buffered_read - Read the raw trace data buffering through a pipe
 * @tcpu: The descriptor representing the raw trace file
 * @buffer: Where to read into (must be at least the size of the subbuffer)
 * @nonblock: Hint to not block on the read if there's no data.
 *
 * This is basically the same as tracefs_cpu_read() except that it uses
 * a pipe through splice to buffer reads. This will batch reads keeping
 * the reading from the ring buffer less intrusive to the system, as
 * just reading all the time can cause quite a disturbance.
 *
 * Note, one difference between this and tracefs_cpu_read() is that it
 * will read only in sub buffer pages. If the ring buffer has not filled
 * a page, then it will not return anything, even with @nonblock set.
 * Calls to tracefs_cpu_flush() should be done to read the rest of
 * the file at the end of the trace.
 *
 * Returns the amount read or -1 on error.
 */
int tracefs_cpu_buffered_read(struct tracefs_cpu *tcpu, void *buffer, bool nonblock)
{
	int mode = SPLICE_F_MOVE;
	int ret;

	if (tcpu->buffered < 0)
		tcpu->buffered = 0;

	if (tcpu->buffered)
		goto do_read;

	ret = wait_on_input(tcpu, nonblock);
	if (ret <= 0)
		return ret;

	if (tcpu->mapping)
		return mmap_read(tcpu, buffer, nonblock);

	if (tcpu->flags & TC_NONBLOCK)
		mode |= SPLICE_F_NONBLOCK;

	ret = init_splice(tcpu);
	if (ret < 0)
		return ret;

	ret = splice(tcpu->fd, NULL, tcpu->splice_pipe[1], NULL,
		     tcpu->pipe_size, mode);
	if (ret <= 0)
		return ret;

	tcpu->buffered = ret;

 do_read:
	ret = read(tcpu->splice_pipe[0], buffer, tcpu->subbuf_size);
	if (ret > 0)
		tcpu->buffered -= ret;
	return ret;
}

/**
 * tracefs_cpu_buffered_read_buf - Read the raw trace data buffering through a pipe
 * @tcpu: The descriptor representing the raw trace file
 * @nonblock: Hint to not block on the read if there's no data.
 *
 * This is basically the same as tracefs_cpu_read() except that it uses
 * a pipe through splice to buffer reads. This will batch reads keeping
 * the reading from the ring buffer less intrusive to the system, as
 * just reading all the time can cause quite a disturbance.
 *
 * Note, one difference between this and tracefs_cpu_read() is that it
 * will read only in sub buffer pages. If the ring buffer has not filled
 * a page, then it will not return anything, even with @nonblock set.
 * Calls to tracefs_cpu_flush() should be done to read the rest of
 * the file at the end of the trace.
 *
 * Returns a kbuffer associated to the next sub-buffer or NULL on error
 * or no data to read with nonblock set (EAGAIN will be set).
 *
 * The kbuffer returned should not be freed!
 */
struct kbuffer *tracefs_cpu_buffered_read_buf(struct tracefs_cpu *tcpu, bool nonblock)
{
	int ret;

	/* If mapping is enabled, just use it directly */
	if (tcpu->mapping) {
		ret = wait_on_input(tcpu, nonblock);
		if (ret <= 0)
			return NULL;

		ret = tfs_mmap_load_subbuf(tcpu->mapping, tcpu->kbuf);
		return ret > 0 ? tcpu->kbuf : NULL;
	}

	if (!get_buffer(tcpu))
		return NULL;

	ret = tracefs_cpu_buffered_read(tcpu, tcpu->buffer, nonblock);
	if (ret <= 0)
		return NULL;

	kbuffer_load_subbuffer(tcpu->kbuf, tcpu->buffer);
	return tcpu->kbuf;
}

/**
 * tracefs_cpu_stop - Stop a blocked read of the raw tracing file
 * @tcpu: The descriptor representing the raw trace file
 *
 * This will attempt to unblock a task blocked on @tcpu reading it.
 * On older kernels, it may not do anything for the pipe reads, as
 * older kernels do not wake up tasks waiting on the ring buffer.
 *
 * Returns 0 if the tasks reading the raw tracing file does not
 * need a nudge.
 *
 * Returns 1 if that tasks may need a nudge (send a signal).
 *
 * Returns negative on error.
 */
int tracefs_cpu_stop(struct tracefs_cpu *tcpu)
{
	int ret = 1;

	if (tcpu->flags & TC_PERM_NONBLOCK)
		return 0;

	ret = write(tcpu->ctrl_pipe[1], &ret, 1);
	if (ret < 0)
		return ret;

	/* Calling ioctl() on recent kernels will wake up the waiters */
	ret = ioctl(tcpu->fd, 0);
	if (ret < 0)
		ret = 1;
	else
		ret = 0;

	set_nonblock(tcpu);

	return ret;
}

/**
 * tracefs_cpu_flush - Finish out and read the rest of the raw tracing file
 * @tcpu: The descriptor representing the raw trace file
 * @buffer: Where to read into (must be at least the size of the subbuffer)
 *
 * Reads the trace_pipe_raw file associated by the @tcpu and puts it
 * into @buffer, which must be the size of the sub buffer which is retrieved.
 * by tracefs_cpu_read_size(). This should be called at the end of tracing
 * to get the rest of the data.
 *
 * This will set the file descriptor for reading to non-blocking mode.
 *
 * Returns the number of bytes read, or negative on error.
 */
int tracefs_cpu_flush(struct tracefs_cpu *tcpu, void *buffer)
{
	int ret;

	/* Make sure that reading is now non blocking */
	set_nonblock(tcpu);

	if (tcpu->buffered < 0)
		tcpu->buffered = 0;

	if (tcpu->mapping)
		return mmap_read(tcpu, buffer, false);

	if (tcpu->buffered) {
		ret = read(tcpu->splice_pipe[0], buffer, tcpu->subbuf_size);
		if (ret > 0)
			tcpu->buffered -= ret;
		return ret;
	}

	ret = read(tcpu->fd, buffer, tcpu->subbuf_size);
	if (ret > 0 && tcpu->buffered)
		tcpu->buffered -= ret;

	/* It's OK if there's no data to read */
	if (ret < 0 && errno == EAGAIN) {
		/* Reset errno */
		errno = 0;
		ret = 0;
	}

	return ret;
}

/**
 * tracefs_cpu_flush_buf - Finish out and read the rest of the raw tracing file
 * @tcpu: The descriptor representing the raw trace file
 *
 * Reads the trace_pipe_raw file associated by the @tcpu and puts it
 * into @buffer, which must be the size of the sub buffer which is retrieved.
 * by tracefs_cpu_read_size(). This should be called at the end of tracing
 * to get the rest of the data.
 *
 * This will set the file descriptor for reading to non-blocking mode.
 */
struct kbuffer *tracefs_cpu_flush_buf(struct tracefs_cpu *tcpu)
{
	int ret;

	if (!get_buffer(tcpu))
		return NULL;

	if (tcpu->mapping) {
		/* Make sure that reading is now non blocking */
		set_nonblock(tcpu);
		ret = tfs_mmap_load_subbuf(tcpu->mapping, tcpu->kbuf);
		return ret > 0 ? tcpu->kbuf : NULL;
	}

	ret = tracefs_cpu_flush(tcpu, tcpu->buffer);
	if (ret <= 0)
		return NULL;

	kbuffer_load_subbuffer(tcpu->kbuf, tcpu->buffer);
	return tcpu->kbuf;
}

/**
 * tracefs_cpu_flush_write - Finish out and read the rest of the raw tracing file
 * @tcpu: The descriptor representing the raw trace file
 * @wfd: The write file descriptor to write the data to
 *
 * Reads the trace_pipe_raw file associated by the @tcpu and writes it to
 * @wfd. This should be called at the end of tracing to get the rest of the data.
 *
 * Returns the number of bytes written, or negative on error.
 */
int tracefs_cpu_flush_write(struct tracefs_cpu *tcpu, int wfd)
{
	char buffer[tcpu->subbuf_size];
	int ret;

	ret = tracefs_cpu_flush(tcpu, buffer);
	if (ret > 0)
		ret = write(wfd, buffer, ret);

	/* It's OK if there's no data to read */
	if (ret < 0 && errno == EAGAIN)
		ret = 0;

	return ret;
}

/**
 * tracefs_cpu_write - Write the raw trace file into a file descriptor
 * @tcpu: The descriptor representing the raw trace file
 * @wfd: The write file descriptor to write the data to
 * @nonblock: Hint to not block on the read if there's no data.
 *
 * This will pipe the data from the trace_pipe_raw file associated with @tcpu
 * into the @wfd file descriptor. If @nonblock is set, then it will not
 * block on if there's nothing to write. Note, it will only write sub buffer
 * size data to @wfd. Calls to tracefs_cpu_flush_write() are needed to
 * write out the rest.
 *
 * Returns the number of bytes read or negative on error.
 */
int tracefs_cpu_write(struct tracefs_cpu *tcpu, int wfd, bool nonblock)
{
	char buffer[tcpu->subbuf_size];
	int mode = SPLICE_F_MOVE;
	int tot_write = 0;
	int tot;
	int ret;

	if (tcpu->mapping) {
		int r = tracefs_cpu_read(tcpu, buffer, nonblock);
		if (r < 0)
			return r;
		do {
			ret = write(wfd, buffer, r);
			if (ret < 0)
				return ret;
			r -= ret;
			tot_write += ret;
		} while (r > 0);
		return tot_write;
	}

	ret = wait_on_input(tcpu, nonblock);
	if (ret <= 0)
		return ret;

	if (tcpu->flags & TC_NONBLOCK)
		mode |= SPLICE_F_NONBLOCK;

	ret = init_splice(tcpu);
	if (ret < 0)
		return ret;

	tot = splice(tcpu->fd, NULL, tcpu->splice_pipe[1], NULL,
		     tcpu->pipe_size, mode);
	if (tot < 0)
		return tot;

	if (tot == 0)
		return 0;

	ret = splice(tcpu->splice_pipe[0], NULL, wfd, NULL,
		     tot, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

	if (ret >= 0)
		return ret;

	/* Some file systems do not allow splicing, try writing instead */
	do {
		int r = tcpu->subbuf_size;

		if (r > tot)
			r = tot;

		ret = read(tcpu->splice_pipe[0], buffer, r);
		if (ret > 0) {
			tot -= ret;
			ret = write(wfd, buffer, ret);
		}
		if (ret > 0)
			tot_write += ret;
	} while (ret > 0);

	if (ret < 0)
		return ret;

	return tot_write;
}

/**
 * tracefs_cpu_pipe - Write the raw trace file into a pipe descriptor
 * @tcpu: The descriptor representing the raw trace file
 * @wfd: The write file descriptor to write the data to (must be a pipe)
 * @nonblock: Hint to not block on the read if there's no data.
 *
 * This will splice directly the file descriptor of the trace_pipe_raw
 * file to the given @wfd, which must be a pipe. This can also be used
 * if @tcpu was created with tracefs_cpu_create_fd() where the passed
 * in @fd there was a pipe, then @wfd does not need to be a pipe.
 *
 * Returns the number of bytes read or negative on error.
 */
int tracefs_cpu_pipe(struct tracefs_cpu *tcpu, int wfd, bool nonblock)
{
	int mode = SPLICE_F_MOVE;
	int ret;

	if (tcpu->mapping)
		return tracefs_cpu_write(tcpu, wfd, nonblock);

	ret = wait_on_input(tcpu, nonblock);
	if (ret <= 0)
		return ret;

	if (tcpu->flags & TC_NONBLOCK)
		mode |= SPLICE_F_NONBLOCK;

	ret = splice(tcpu->fd, NULL, wfd, NULL,
		     tcpu->pipe_size, mode);
	return ret;
}
