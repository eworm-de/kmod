/*
 * libkmod - interface to kernel module operations
 *
 * Copyright (C) 2011-2013  ProFUSION embedded systems
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef ENABLE_ZSTD
#include <zstd.h>
#endif
#ifdef ENABLE_XZ
#include <lzma.h>
#endif
#ifdef ENABLE_ZLIB
#include <zlib.h>
#endif

#include <shared/util.h>

#include "libkmod.h"
#include "libkmod-internal.h"

struct kmod_file;
struct file_ops {
	int (*load)(struct kmod_file *file);
	void (*unload)(struct kmod_file *file);
};

struct kmod_file {
	int fd;
	enum kmod_file_compression_type compression;
	off_t size;
	void *memory;
	const struct file_ops *ops;
	const struct kmod_ctx *ctx;
	struct kmod_elf *elf;
};

#ifdef ENABLE_ZSTD
static int zstd_read_block(struct kmod_file *file, size_t block_size,
			   ZSTD_inBuffer *input, size_t *input_capacity)
{
	ssize_t rdret;
	int ret;

	if (*input_capacity < block_size) {
		free((void *)input->src);
		input->src = malloc(block_size);
		if (input->src == NULL) {
			ret = -errno;
			ERR(file->ctx, "zstd: %m\n");
			return ret;
		}
		*input_capacity = block_size;
	}

	rdret = read(file->fd, (void *)input->src, block_size);
	if (rdret < 0) {
		ret = -errno;
		ERR(file->ctx, "zstd: %m\n");
		return ret;
	}

	input->pos = 0;
	input->size = rdret;
	return 0;
}

static int zstd_ensure_outbuffer_space(ZSTD_outBuffer *buffer, size_t min_free)
{
	uint8_t *old_buffer = buffer->dst;
	int ret = 0;

	if (buffer->size - buffer->pos >= min_free)
		return 0;

	buffer->size += min_free;
	buffer->dst = realloc(buffer->dst, buffer->size);
	if (buffer->dst == NULL) {
		ret = -errno;
		free(old_buffer);
	}

	return ret;
}

static int zstd_decompress_block(struct kmod_file *file, ZSTD_DStream *dstr,
				 ZSTD_inBuffer *input, ZSTD_outBuffer *output,
				 size_t *next_block_size)
{
	size_t out_buf_min_size = ZSTD_DStreamOutSize();
	int ret = 0;

	do {
		ssize_t dsret;

		ret = zstd_ensure_outbuffer_space(output, out_buf_min_size);
		if (ret) {
			ERR(file->ctx, "zstd: %s\n", strerror(-ret));
			break;
		}

		dsret = ZSTD_decompressStream(dstr, output, input);
		if (ZSTD_isError(dsret)) {
			ret = -EINVAL;
			ERR(file->ctx, "zstd: %s\n", ZSTD_getErrorName(dsret));
			break;
		}
		if (dsret > 0)
			*next_block_size = (size_t)dsret;
	} while (input->pos < input->size
		 || output->pos > output->size
		 || output->size - output->pos < out_buf_min_size);

	return ret;
}

static int load_zstd(struct kmod_file *file)
{
	ZSTD_DStream *dstr;
	size_t next_block_size;
	size_t zst_inb_capacity = 0;
	ZSTD_inBuffer zst_inb = { 0 };
	ZSTD_outBuffer zst_outb = { 0 };
	int ret;

	dstr = ZSTD_createDStream();
	if (dstr == NULL) {
		ret = -EINVAL;
		ERR(file->ctx, "zstd: Failed to create decompression stream\n");
		goto out;
	}

	next_block_size = ZSTD_initDStream(dstr);

	while (true) {
		ret = zstd_read_block(file, next_block_size, &zst_inb,
				      &zst_inb_capacity);
		if (ret != 0)
			goto out;
		if (zst_inb.size == 0) /* EOF */
			break;

		ret = zstd_decompress_block(file, dstr, &zst_inb, &zst_outb,
					    &next_block_size);
		if (ret != 0)
			goto out;
	}

	ZSTD_freeDStream(dstr);
	free((void *)zst_inb.src);
	file->memory = zst_outb.dst;
	file->size = zst_outb.pos;
	return 0;
out:
	if (dstr != NULL)
		ZSTD_freeDStream(dstr);
	free((void *)zst_inb.src);
	free((void *)zst_outb.dst);
	return ret;
}

static void unload_zstd(struct kmod_file *file)
{
	free(file->memory);
}

static const char magic_zstd[] = {0x28, 0xB5, 0x2F, 0xFD};
#endif

#ifdef ENABLE_XZ
static void xz_uncompress_belch(struct kmod_file *file, lzma_ret ret)
{
	switch (ret) {
	case LZMA_MEM_ERROR:
		ERR(file->ctx, "xz: %s\n", strerror(ENOMEM));
		break;
	case LZMA_FORMAT_ERROR:
		ERR(file->ctx, "xz: File format not recognized\n");
		break;
	case LZMA_OPTIONS_ERROR:
		ERR(file->ctx, "xz: Unsupported compression options\n");
		break;
	case LZMA_DATA_ERROR:
		ERR(file->ctx, "xz: File is corrupt\n");
		break;
	case LZMA_BUF_ERROR:
		ERR(file->ctx, "xz: Unexpected end of input\n");
		break;
	default:
		ERR(file->ctx, "xz: Internal error (bug)\n");
		break;
	}
}

static int xz_uncompress(lzma_stream *strm, struct kmod_file *file)
{
	uint8_t in_buf[BUFSIZ], out_buf[BUFSIZ];
	lzma_action action = LZMA_RUN;
	lzma_ret ret;
	void *p = NULL;
	size_t total = 0;

	strm->avail_in  = 0;
	strm->next_out  = out_buf;
	strm->avail_out = sizeof(out_buf);

	while (true) {
		if (strm->avail_in == 0) {
			ssize_t rdret = read(file->fd, in_buf, sizeof(in_buf));
			if (rdret < 0) {
				ret = -errno;
				goto out;
			}
			strm->next_in  = in_buf;
			strm->avail_in = rdret;
			if (rdret == 0)
				action = LZMA_FINISH;
		}
		ret = lzma_code(strm, action);
		if (strm->avail_out == 0 || ret != LZMA_OK) {
			size_t write_size = BUFSIZ - strm->avail_out;
			char *tmp = realloc(p, total + write_size);
			if (tmp == NULL) {
				ret = -errno;
				goto out;
			}
			memcpy(tmp + total, out_buf, write_size);
			total += write_size;
			p = tmp;
			strm->next_out = out_buf;
			strm->avail_out = BUFSIZ;
		}
		if (ret == LZMA_STREAM_END)
			break;
		if (ret != LZMA_OK) {
			xz_uncompress_belch(file, ret);
			ret = -EINVAL;
			goto out;
		}
	}
	file->memory = p;
	file->size = total;
	return 0;
 out:
	free(p);
	return ret;
}

static int load_xz(struct kmod_file *file)
{
	lzma_stream strm = LZMA_STREAM_INIT;
	lzma_ret lzret;
	int ret;

	lzret = lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
	if (lzret == LZMA_MEM_ERROR) {
		ERR(file->ctx, "xz: %s\n", strerror(ENOMEM));
		return -ENOMEM;
	} else if (lzret != LZMA_OK) {
		ERR(file->ctx, "xz: Internal error (bug)\n");
		return -EINVAL;
	}
	ret = xz_uncompress(&strm, file);
	lzma_end(&strm);
	return ret;
}

static void unload_xz(struct kmod_file *file)
{
	free(file->memory);
}

static const char magic_xz[] = {0xfd, '7', 'z', 'X', 'Z', 0};
#endif

#ifdef ENABLE_ZLIB
#define READ_STEP (4 * 1024 * 1024)
static int load_zlib(struct kmod_file *file)
{
	int err = 0;
	off_t did = 0, total = 0;
	_cleanup_free_ unsigned char *p = NULL;
	gzFile gzf;
	int gzfd;

	errno = 0;
	gzfd = fcntl(file->fd, F_DUPFD_CLOEXEC, 3);
	if (gzfd < 0)
		return -errno;

	gzf = gzdopen(gzfd, "rb"); /* takes ownership of the fd */
	if (gzf == NULL) {
		close(gzfd);
		return -errno;
	}

	for (;;) {
		int r;

		if (did == total) {
			void *tmp = realloc(p, total + READ_STEP);
			if (tmp == NULL) {
				err = -errno;
				goto error;
			}
			total += READ_STEP;
			p = tmp;
		}

		r = gzread(gzf, p + did, total - did);
		if (r == 0)
			break;
		else if (r < 0) {
			int gzerr;
			const char *gz_errmsg = gzerror(gzf, &gzerr);

			ERR(file->ctx, "gzip: %s\n", gz_errmsg);

			/* gzip might not set errno here */
			err = gzerr == Z_ERRNO ? -errno : -EINVAL;
			goto error;
		}
		did += r;
	}

	file->memory = p;
	file->size = did;
	p = NULL;
	gzclose(gzf);
	return 0;

error:
	gzclose(gzf); /* closes the gzfd */
	return err;
}

static void unload_zlib(struct kmod_file *file)
{
	free(file->memory);
}

static const char magic_zlib[] = {0x1f, 0x8b};
#endif

static const struct comp_type {
	size_t magic_size;
	enum kmod_file_compression_type compression;
	const char *magic_bytes;
	const struct file_ops ops;
} comp_types[] = {
#ifdef ENABLE_ZSTD
	{sizeof(magic_zstd),	KMOD_FILE_COMPRESSION_ZSTD, magic_zstd, {load_zstd, unload_zstd}},
#endif
#ifdef ENABLE_XZ
	{sizeof(magic_xz),	KMOD_FILE_COMPRESSION_XZ, magic_xz, {load_xz, unload_xz}},
#endif
#ifdef ENABLE_ZLIB
	{sizeof(magic_zlib),	KMOD_FILE_COMPRESSION_ZLIB, magic_zlib, {load_zlib, unload_zlib}},
#endif
	{0,			KMOD_FILE_COMPRESSION_NONE, NULL, {NULL, NULL}}
};

static int load_reg(struct kmod_file *file)
{
	struct stat st;

	if (fstat(file->fd, &st) < 0)
		return -errno;

	file->size = st.st_size;
	file->memory = mmap(NULL, file->size, PROT_READ, MAP_PRIVATE,
			    file->fd, 0);
	if (file->memory == MAP_FAILED)
		return -errno;

	return 0;
}

static void unload_reg(struct kmod_file *file)
{
	munmap(file->memory, file->size);
}

static const struct file_ops reg_ops = {
	load_reg, unload_reg
};

struct kmod_elf *kmod_file_get_elf(struct kmod_file *file)
{
	if (file->elf)
		return file->elf;

	kmod_file_load_contents(file);
	file->elf = kmod_elf_new(file->memory, file->size);
	return file->elf;
}

struct kmod_file *kmod_file_open(const struct kmod_ctx *ctx,
						const char *filename)
{
	struct kmod_file *file = calloc(1, sizeof(struct kmod_file));
	const struct comp_type *itr;
	size_t magic_size_max = 0;
	int err = 0;

	if (file == NULL)
		return NULL;

	file->fd = open(filename, O_RDONLY|O_CLOEXEC);
	if (file->fd < 0) {
		err = -errno;
		goto error;
	}

	for (itr = comp_types; itr->ops.load != NULL; itr++) {
		if (magic_size_max < itr->magic_size)
			magic_size_max = itr->magic_size;
	}

	if (magic_size_max > 0) {
		char *buf = alloca(magic_size_max + 1);
		ssize_t sz;

		if (buf == NULL) {
			err = -errno;
			goto error;
		}
		sz = read_str_safe(file->fd, buf, magic_size_max + 1);
		lseek(file->fd, 0, SEEK_SET);
		if (sz != (ssize_t)magic_size_max) {
			if (sz < 0)
				err = sz;
			else
				err = -EINVAL;
			goto error;
		}

		for (itr = comp_types; itr->ops.load != NULL; itr++) {
			if (memcmp(buf, itr->magic_bytes, itr->magic_size) == 0) {
				file->ops = &itr->ops;
				file->compression = itr->compression;
				break;
			}
		}
	}

	if (file->ops == NULL) {
		file->ops = &reg_ops;
		file->compression = KMOD_FILE_COMPRESSION_NONE;
	}

	file->ctx = ctx;

error:
	if (err < 0) {
		if (file->fd >= 0)
			close(file->fd);
		free(file);
		errno = -err;
		return NULL;
	}

	return file;
}

/*
 *  Callers should just check file->memory got updated
 */
void kmod_file_load_contents(struct kmod_file *file)
{
	if (file->memory)
		return;

	/*  The load functions already log possible errors. */
	file->ops->load(file);
}

void *kmod_file_get_contents(const struct kmod_file *file)
{
	return file->memory;
}

off_t kmod_file_get_size(const struct kmod_file *file)
{
	return file->size;
}

enum kmod_file_compression_type kmod_file_get_compression(const struct kmod_file *file)
{
	return file->compression;
}

int kmod_file_get_fd(const struct kmod_file *file)
{
	return file->fd;
}

void kmod_file_unref(struct kmod_file *file)
{
	if (file->elf)
		kmod_elf_unref(file->elf);

	if (file->memory)
		file->ops->unload(file);

	close(file->fd);
	free(file);
}
