// SPDX-License-Identifier: GPL-2.0-only
/*
 * xnudrmd: the Linux end of the DRM bridge. It serves the hosted macOS's GPU clients (kexts/XnuDrm, libxnudrm) from
 * Linux's DRM render node (virtio-gpu or Qualcomm MSM / Adreno): each request comes as a host call on /dev/xnu-hostcall
 * (fki), is carried out with an ordinary DRM ioctl, mmap or poll, and answered with the outputs. Mappings are translated
 * into physical addresses (fki's XNU_HOSTCALL_XLATE) so the hosted kernel maps the very same pages into its process: no copies.
 *
 * Usage: xnudrmd [render node, default /dev/dri/renderD128]. Run as root (the hostcall device is 0600).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/virtgpu_drm.h>
#include <drm/msm_drm.h>
#include "xnudrm_proto.h"

/* Newer MSM DRM definitions (Linux >= 6.15 / 7.x) if building against older libc/kernel headers */
#ifndef DRM_MSM_VM_BIND
#define DRM_MSM_VM_BIND 0x0D
struct xnu_drm_msm_vm_bind_op {
	__u32 op;
	__u32 handle;
	__u64 obj_offset;
	__u64 iova;
	__u64 range;
	__u32 flags;
	__u32 pad;
};
struct xnu_drm_msm_vm_bind {
	__u32 flags;
	__u32 nr_ops;
	__s32 fence_fd;
	__u32 queue_id;
	__u64 in_syncobjs;
	__u64 out_syncobjs;
	__u32 nr_in_syncobjs;
	__u32 nr_out_syncobjs;
	__u32 syncobj_stride;
	__u32 op_stride;
	union {
		struct xnu_drm_msm_vm_bind_op op;
		__u64 ops;
	};
};
#define DRM_IOCTL_MSM_VM_BIND DRM_IOWR(DRM_COMMAND_BASE + DRM_MSM_VM_BIND, struct xnu_drm_msm_vm_bind)
#else
#define xnu_drm_msm_vm_bind_op drm_msm_vm_bind_op
#define xnu_drm_msm_vm_bind drm_msm_vm_bind
#endif

#ifndef MSM_VM_BIND_FENCE_FD_IN
#define MSM_VM_BIND_FENCE_FD_IN  0x00000001
#define MSM_VM_BIND_FENCE_FD_OUT 0x00000002
#endif

struct xnu_hostcall_xlate { uint64_t va, len, out, max; };
#define XNU_HOSTCALL_XLATE _IOWR('X', 1, struct xnu_hostcall_xlate)

#define MAX_CLIENTS 256
#define MAX_MAPS 65536
#define MAX_FENCES 65536

static const char *node = "/dev/dri/renderD128";
static int hc = -1;
static int verbose;
static int is_msm = -1;			/* 1 if render node is Qualcomm msm, 0 if virtio_gpu */
static int clients[MAX_CLIENTS];	/* DRM fd + 1 (0: free) */
static struct map { int client; void *va; size_t len; } maps[MAX_MAPS];
static struct fence { int client; int fd; } fences[MAX_FENCES];	/* fd + 1 */

static uint8_t *msg, *rep;
#define PAYLOAD(m) ((m) + 8 + sizeof(struct xnudrm_hdr))

static int client_fd(uint32_t c) { return c && c < MAX_CLIENTS && clients[c] ? clients[c] - 1 : -1; }

static int detect_driver(int fd)
{
	if (is_msm >= 0) return is_msm;
	char name[64] = {};
	struct drm_version v = { .name_len = sizeof(name) - 1, .name = name };
	if (ioctl(fd, DRM_IOCTL_VERSION, &v) == 0) {
		is_msm = (strcmp(name, "msm") == 0) ? 1 : 0;
		fprintf(stderr, "xnudrmd: driver '%s' v%d.%d.%d (is_msm=%d)\n",
			name, v.version_major, v.version_minor, v.version_patchlevel, is_msm);
	} else {
		is_msm = 0;
	}
	return is_msm;
}

static int alloc_fence(uint32_t client, int fd)
{
	int i;
	if (fd < 0) return -1;
	for (i = 1; i < MAX_FENCES && fences[i].fd; i++) ;
	if (i == MAX_FENCES) { close(fd); return -EMFILE; }
	fences[i].fd = fd + 1;
	fences[i].client = (int)client;
	return i;
}

static int lookup_fence_fd(uint32_t client, int id)
{
	if (id <= 0 || id >= MAX_FENCES || !fences[id].fd || fences[id].client != (int)client)
		return -1;
	return fences[id].fd - 1;
}

/* A pointer field of an ioctl struct: an offset into the payload, checked to lie inside it with len bytes. */
static int ptr_field(uint8_t *payload, uint32_t plen, __u64 *field, uint64_t len)
{
	uint64_t off = *field;
	if (len == 0) { *field = 0; return 0; }
	if (off > plen || len > plen - off) return -EINVAL;
	*field = (uint64_t)(uintptr_t)(payload + off);
	return 0;
}
static void unptr_field(uint8_t *payload, __u64 *field)
{
	if (*field) *field = (uint64_t)((uint8_t *)(uintptr_t)*field - payload);
}

static int do_ioctl_msm(int fd, uint32_t client, uint32_t nr, uint8_t *p, uint32_t plen)
{
	int r = 0;
#define NEED(type) do { if (plen < sizeof(type)) return -EINVAL; } while (0)
	switch (nr) {
	case DRM_COMMAND_BASE + DRM_MSM_GET_PARAM: {
		struct drm_msm_param *a = (void *)p; NEED(*a);
		if (a->len) {
			if ((r = ptr_field(p, plen, &a->value, a->len))) return r;
		}
		r = ioctl(fd, DRM_IOCTL_MSM_GET_PARAM, a);
		if (a->len) unptr_field(p, &a->value);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_SET_PARAM: {
		struct drm_msm_param *a = (void *)p; NEED(*a);
		if (a->len) {
			if ((r = ptr_field(p, plen, &a->value, a->len))) return r;
		}
		/* Mesa's drmCommandWriteRead uses _IOWR even though msm_drm.h defines SET_PARAM as _IOW */
		r = ioctl(fd, DRM_IOWR(DRM_COMMAND_BASE + DRM_MSM_SET_PARAM, struct drm_msm_param), a);
		if (r < 0 && errno == ENOTTY)
			r = ioctl(fd, DRM_IOCTL_MSM_SET_PARAM, a);
		if (a->len) unptr_field(p, &a->value);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_NEW: {
		NEED(struct drm_msm_gem_new);
		r = ioctl(fd, DRM_IOCTL_MSM_GEM_NEW, p);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO: {
		struct drm_msm_gem_info *a = (void *)p; NEED(*a);
		if (a->len) {
			if ((r = ptr_field(p, plen, &a->value, a->len))) return r;
		}
		r = ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, a);
		if (a->len) unptr_field(p, &a->value);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP: {
		struct drm_msm_gem_cpu_prep *a = (void *)p; NEED(*a);
		/* Never block inside a hostcall; force non-blocking / immediate timeout */
		struct timespec ts = {};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		a->timeout.tv_sec = ts.tv_sec;
		a->timeout.tv_nsec = ts.tv_nsec;
		r = ioctl(fd, DRM_IOCTL_MSM_GEM_CPU_PREP, a);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI: {
		NEED(struct drm_msm_gem_cpu_fini);
		r = ioctl(fd, DRM_IOCTL_MSM_GEM_CPU_FINI, p);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE: {
		NEED(struct drm_msm_gem_madvise);
		r = ioctl(fd, DRM_IOCTL_MSM_GEM_MADVISE, p);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW: {
		NEED(struct drm_msm_submitqueue);
		r = ioctl(fd, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, p);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_CLOSE: {
		NEED(__u32);
		r = ioctl(fd, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, p);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY: {
		struct drm_msm_submitqueue_query *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->data, a->len))) return r;
		r = ioctl(fd, DRM_IOCTL_MSM_SUBMITQUEUE_QUERY, a);
		unptr_field(p, &a->data);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE: {
		struct drm_msm_wait_fence *a = (void *)p; NEED(*a);
		/* Poll non-blocking on Linux; macOS libxnudrm sleeps between polls */
		struct timespec ts = {};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		a->timeout.tv_sec = ts.tv_sec;
		a->timeout.tv_nsec = ts.tv_nsec;
		r = ioctl(fd, DRM_IOCTL_MSM_WAIT_FENCE, a);
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT: {
		struct drm_msm_gem_submit *a = (void *)p; NEED(*a);
		if (a->flags & MSM_SUBMIT_FENCE_FD_IN) {
			int ffd = lookup_fence_fd(client, a->fence_fd);
			if (ffd < 0) return -EINVAL;
			a->fence_fd = ffd;
		}
		if (a->nr_bos && (r = ptr_field(p, plen, &a->bos, (uint64_t)a->nr_bos * sizeof(struct drm_msm_gem_submit_bo)))) return r;
		if (a->nr_cmds && (r = ptr_field(p, plen, &a->cmds, (uint64_t)a->nr_cmds * sizeof(struct drm_msm_gem_submit_cmd)))) return r;
		uint32_t s_stride = a->syncobj_stride ? a->syncobj_stride : sizeof(struct drm_msm_gem_submit_syncobj);
		if (a->nr_in_syncobjs && (r = ptr_field(p, plen, &a->in_syncobjs, (uint64_t)a->nr_in_syncobjs * s_stride))) return r;
		if (a->nr_out_syncobjs && (r = ptr_field(p, plen, &a->out_syncobjs, (uint64_t)a->nr_out_syncobjs * s_stride))) return r;
		r = ioctl(fd, DRM_IOCTL_MSM_GEM_SUBMIT, a);
		if (a->nr_bos) unptr_field(p, &a->bos);
		if (a->nr_cmds) unptr_field(p, &a->cmds);
		if (a->nr_in_syncobjs) unptr_field(p, &a->in_syncobjs);
		if (a->nr_out_syncobjs) unptr_field(p, &a->out_syncobjs);
		if (r == 0 && (a->flags & MSM_SUBMIT_FENCE_FD_OUT)) {
			int id = alloc_fence(client, a->fence_fd);
			if (id < 0) { a->fence_fd = -1; return id; }
			a->fence_fd = id;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_VM_BIND: {
		struct xnu_drm_msm_vm_bind *a = (void *)p; NEED(*a);
		if (a->flags & MSM_VM_BIND_FENCE_FD_IN) {
			int ffd = lookup_fence_fd(client, a->fence_fd);
			if (ffd < 0) return -EINVAL;
			a->fence_fd = ffd;
		}
		uint32_t op_stride = a->op_stride ? a->op_stride : sizeof(struct xnu_drm_msm_vm_bind_op);
		if (a->nr_ops > 1 && (r = ptr_field(p, plen, &a->ops, (uint64_t)a->nr_ops * op_stride))) return r;
		uint32_t s_stride = a->syncobj_stride ? a->syncobj_stride : sizeof(struct drm_msm_gem_submit_syncobj);
		if (a->nr_in_syncobjs && (r = ptr_field(p, plen, &a->in_syncobjs, (uint64_t)a->nr_in_syncobjs * s_stride))) return r;
		if (a->nr_out_syncobjs && (r = ptr_field(p, plen, &a->out_syncobjs, (uint64_t)a->nr_out_syncobjs * s_stride))) return r;
		r = ioctl(fd, DRM_IOCTL_MSM_VM_BIND, a);
		if (a->nr_ops > 1) unptr_field(p, &a->ops);
		if (a->nr_in_syncobjs) unptr_field(p, &a->in_syncobjs);
		if (a->nr_out_syncobjs) unptr_field(p, &a->out_syncobjs);
		if (r == 0 && (a->flags & MSM_VM_BIND_FENCE_FD_OUT)) {
			int id = alloc_fence(client, a->fence_fd);
			if (id < 0) { a->fence_fd = -1; return id; }
			a->fence_fd = id;
		}
		break;
	}
	default:
		return -ENOTTY;
	}
	return r < 0 ? -errno : 0;
}

static int do_ioctl(int fd, uint32_t client, uint32_t nr, uint8_t *p, uint32_t plen)
{
	int r = 0;
	/* Generic DRM ioctls (GEM_CLOSE, GET_CAP, SYNCOBJ_*) */
	switch (nr) {
	case 0x00: {	/* DRM_IOCTL_VERSION: query whether render node is msm or virtio_gpu */
		/* Custom lightweight version query: payload is 64-byte char name + 3 ints (major, minor, patch) */
		if (plen >= 12) {
			struct drm_version v = {};
			char name[64] = {};
			v.name = name;
			v.name_len = sizeof(name) - 1;
			r = ioctl(fd, DRM_IOCTL_VERSION, &v);
			if (r == 0) {
				int32_t *ver = (int32_t *)p;
				ver[0] = v.version_major;
				ver[1] = v.version_minor;
				ver[2] = v.version_patchlevel;
				if (plen >= 12 + 64) memcpy(p + 12, name, 64);
			}
			return r < 0 ? -errno : 0;
		}
		return -EINVAL;
	}
	case 0x09: {	/* DRM_IOCTL_GEM_CLOSE */
		NEED(struct drm_gem_close);
		r = ioctl(fd, DRM_IOCTL_GEM_CLOSE, p);
		return r < 0 ? -errno : 0;
	}
	case 0x0c: {	/* DRM_IOCTL_GET_CAP */
		NEED(struct drm_get_cap);
		r = ioctl(fd, DRM_IOCTL_GET_CAP, p);
		return r < 0 ? -errno : 0;
	}
	case 0xbf: {	/* DRM_IOCTL_SYNCOBJ_CREATE */
		NEED(struct drm_syncobj_create);
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, p);
		return r < 0 ? -errno : 0;
	}
	case 0xc0: {	/* DRM_IOCTL_SYNCOBJ_DESTROY */
		NEED(struct drm_syncobj_destroy);
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, p);
		return r < 0 ? -errno : 0;
	}
	case 0xc1: {	/* DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD */
		struct drm_syncobj_handle *a = (void *)p; NEED(*a);
		a->fd = -1;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, a);
		if (r == 0) {
			int id = alloc_fence(client, a->fd);
			if (id < 0) { a->fd = -1; return id; }
			a->fd = id;
		}
		return r < 0 ? -errno : 0;
	}
	case 0xc2: {	/* DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE */
		struct drm_syncobj_handle *a = (void *)p; NEED(*a);
		int ffd = lookup_fence_fd(client, a->fd);
		if (ffd < 0) return -EINVAL;
		int saved_id = a->fd;
		a->fd = ffd;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, a);
		a->fd = saved_id;
		return r < 0 ? -errno : 0;
	}
	case 0xc3: {	/* DRM_IOCTL_SYNCOBJ_WAIT */
		struct drm_syncobj_wait *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->handles, (uint64_t)a->count_handles * 4))) return r;
		/* Non-blocking poll on Linux; macOS libxnudrm loops with nanosleep backoff */
		a->timeout_nsec = 0;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, a);
		unptr_field(p, &a->handles);
		return r < 0 ? -errno : 0;
	}
	case 0xc4: {	/* DRM_IOCTL_SYNCOBJ_RESET */
		struct drm_syncobj_array *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->handles, (uint64_t)a->count_handles * 4))) return r;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, a);
		unptr_field(p, &a->handles);
		return r < 0 ? -errno : 0;
	}
	case 0xc5: {	/* DRM_IOCTL_SYNCOBJ_SIGNAL */
		struct drm_syncobj_array *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->handles, (uint64_t)a->count_handles * 4))) return r;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, a);
		unptr_field(p, &a->handles);
		return r < 0 ? -errno : 0;
	}
	case 0xca: {	/* DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT */
		struct drm_syncobj_timeline_wait *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->handles, (uint64_t)a->count_handles * 4))) return r;
		if ((r = ptr_field(p, plen, &a->points, (uint64_t)a->count_handles * 8))) return r;
		/* Non-blocking poll on Linux; macOS libxnudrm loops with nanosleep backoff */
		a->timeout_nsec = 0;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, a);
		unptr_field(p, &a->handles);
		unptr_field(p, &a->points);
		return r < 0 ? -errno : 0;
	}
	case 0xcb:	/* DRM_IOCTL_SYNCOBJ_QUERY */
	case 0xcd: {	/* DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL */
		struct drm_syncobj_timeline_array *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->handles, (uint64_t)a->count_handles * 4))) return r;
		if ((r = ptr_field(p, plen, &a->points, (uint64_t)a->count_handles * 8))) return r;
		r = ioctl(fd, nr == 0xcb ? DRM_IOCTL_SYNCOBJ_QUERY : DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, a);
		unptr_field(p, &a->handles);
		unptr_field(p, &a->points);
		return r < 0 ? -errno : 0;
	}
	case 0xcc: {	/* DRM_IOCTL_SYNCOBJ_TRANSFER */
		NEED(struct drm_syncobj_transfer);
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, p);
		return r < 0 ? -errno : 0;
	}
	}

	if (detect_driver(fd))
		return do_ioctl_msm(fd, client, nr, p, plen);

	switch (nr) {
	case DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM: {
		struct drm_virtgpu_getparam *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->value, sizeof(int)))) return r;
		r = ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, a);
		unptr_field(p, &a->value);
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_GET_CAPS: {
		struct drm_virtgpu_get_caps *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->addr, a->size))) return r;
		r = ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, a);
		unptr_field(p, &a->addr);
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT: {
		struct drm_virtgpu_context_init *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->ctx_set_params, (uint64_t)a->num_params * sizeof(struct drm_virtgpu_context_set_param)))) return r;
		r = ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, a);
		unptr_field(p, &a->ctx_set_params);
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB: {
		struct drm_virtgpu_resource_create_blob *a = (void *)p; NEED(*a);
		if ((r = ptr_field(p, plen, &a->cmd, a->cmd_size))) return r;
		r = ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, a);
		unptr_field(p, &a->cmd);
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_INFO: {
		NEED(struct drm_virtgpu_resource_info);
		r = ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, p);
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT: {
		NEED(struct drm_virtgpu_3d_wait);
		r = ioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, p);
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER: {
		struct drm_virtgpu_execbuffer *a = (void *)p; NEED(*a);
		if (a->num_in_syncobjs || a->num_out_syncobjs || (a->flags & VIRTGPU_EXECBUF_FENCE_FD_IN)) return -EOPNOTSUPP;
		if ((r = ptr_field(p, plen, &a->command, a->size))) return r;
		if ((r = ptr_field(p, plen, &a->bo_handles, (uint64_t)a->num_bo_handles * 4))) return r;
		a->fence_fd = -1;
		r = ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, a);
		unptr_field(p, &a->command);
		unptr_field(p, &a->bo_handles);
		if (r == 0 && (a->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT)) {
			int id = alloc_fence(client, a->fence_fd);
			if (id < 0) { a->fence_fd = -1; return id; }
			a->fence_fd = id;	/* the fence's id */
		}
		break;
	}
	default:
		return -ENOTTY;
	}
#undef NEED
	return r < 0 ? -errno : 0;
}

static int do_map(int fd, uint32_t client, struct xnudrm_hdr *h, uint8_t *out, uint32_t *outlen)
{
	uint32_t handle = (uint32_t)h->arg0;
	uint64_t mmap_offset = 0;
	size_t len = (h->arg1 + 4095) & ~(size_t)4095;
	struct xnudrm_range *rg = (void *)out;
	uint32_t nr = 0, maxr = (XNUDRM_MAX_MSG - sizeof(*h)) / sizeof(*rg);
	uint64_t *pa;
	size_t i, n = len >> 12;
	int id;
	void *va;

	if (!len) return -EINVAL;
	for (id = 1; id < MAX_MAPS && maps[id].va; id++) ;
	if (id == MAX_MAPS) return -ENOMEM;
	if (detect_driver(fd)) {
		struct drm_msm_gem_info gi = { .handle = handle, .info = MSM_INFO_GET_OFFSET };
		if (ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, &gi)) return -errno;
		mmap_offset = gi.value;
	} else {
		struct drm_virtgpu_map m = { .handle = handle };
		if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &m)) return -errno;
		mmap_offset = m.offset;
	}
	va = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mmap_offset);
	if (va == MAP_FAILED) return -errno;
	for (i = 0; i < n; i++) (void)*(volatile uint8_t *)((uint8_t *)va + (i << 12));	/* fault every page in */
	pa = malloc(n * 8);
	if (!pa) { munmap(va, len); return -ENOMEM; }
	struct xnu_hostcall_xlate x = { (uintptr_t)va, len, (uintptr_t)pa, n };
	if (ioctl(hc, XNU_HOSTCALL_XLATE, &x)) { int e = -errno; free(pa); munmap(va, len); return e; }
	for (i = 0; i < n; i++) {
		if (nr && rg[nr - 1].pa + rg[nr - 1].len == pa[i]) { rg[nr - 1].len += 4096; continue; }
		if (nr == maxr) { free(pa); munmap(va, len); return -E2BIG; }
		rg[nr].pa = pa[i]; rg[nr].len = 4096; nr++;
	}
	free(pa);
	maps[id] = (struct map){ client, va, len };
	h->arg0 = id;
	*outlen = nr * sizeof(*rg);
	if (verbose) fprintf(stderr, "map %d: handle %u, %zu bytes, %u ranges, first pa %#llx\n", id, handle, len, nr, (unsigned long long)rg[0].pa);
	return 0;
}

static void drop_client(uint32_t c)
{
	int i;
	for (i = 1; i < MAX_MAPS; i++) if (maps[i].va && maps[i].client == (int)c) { munmap(maps[i].va, maps[i].len); maps[i].va = NULL; }
	for (i = 1; i < MAX_FENCES; i++) if (fences[i].fd && fences[i].client == (int)c) { close(fences[i].fd - 1); fences[i].fd = 0; }
	if (clients[c]) close(clients[c] - 1);
	clients[c] = 0;
}

static void serve(uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	struct xnudrm_hdr *h = (void *)(in + 8), *rh = (void *)(out + 8);
	uint8_t *payload = PAYLOAD(in), *rpayload = PAYLOAD(out);
	uint32_t rlen = 0;
	int fd, st = 0;

	memcpy(out, in, 8);	/* the call's sequence number */
	if (inlen < 8 + sizeof(*h) || h->magic != XNUDRM_MAGIC || h->payload_len > inlen - 8 - sizeof(*h)) {
		*rh = (struct xnudrm_hdr){ .magic = XNUDRM_MAGIC, .status = -EINVAL };
		*outlen = 8 + sizeof(*rh);
		return;
	}
	*rh = *h;
	rh->payload_len = 0;
	fd = client_fd(h->client);
	switch (h->op) {
	case XNUDRM_OPEN: {
		int c, f;
		if (h->arg0 != XNUDRM_VERSION) { st = -EPROTO; break; }
		for (c = 1; c < MAX_CLIENTS && clients[c]; c++) ;
		if (c == MAX_CLIENTS) { st = -EMFILE; break; }
		f = open(node, O_RDWR | O_CLOEXEC);
		if (f < 0) { st = -errno; break; }
		detect_driver(f);
		clients[c] = f + 1;
		rh->client = c;
		if (verbose) fprintf(stderr, "open: client %d\n", c);
		break;
	}
	case XNUDRM_CLOSE:
		if (fd < 0) { st = -EBADF; break; }
		drop_client(h->client);
		if (verbose) fprintf(stderr, "close: client %u\n", h->client);
		break;
	case XNUDRM_IOCTL:
		if (fd < 0) { st = -EBADF; break; }
		memcpy(rpayload, payload, h->payload_len);
		st = do_ioctl(fd, h->client, (uint32_t)h->arg0, rpayload, h->payload_len);
		rlen = h->payload_len;
		if (verbose > 1) fprintf(stderr, "ioctl %#x: %d\n", (unsigned)h->arg0, st);
		break;
	case XNUDRM_MAP:
		if (fd < 0) { st = -EBADF; break; }
		st = do_map(fd, h->client, rh, rpayload, &rlen);
		break;
	case XNUDRM_UNMAP:
		if (h->arg0 == 0 || h->arg0 >= MAX_MAPS || !maps[h->arg0].va || maps[h->arg0].client != (int)h->client) { st = -EINVAL; break; }
		munmap(maps[h->arg0].va, maps[h->arg0].len);
		maps[h->arg0].va = NULL;
		break;
	case XNUDRM_FENCE_WAIT:
	case XNUDRM_FENCE_CLOSE: {
		struct fence *f = h->arg0 && h->arg0 < MAX_FENCES ? &fences[h->arg0] : NULL;
		if (!f || !f->fd || f->client != (int)h->client) { st = -EINVAL; break; }
		if (h->op == XNUDRM_FENCE_CLOSE) { close(f->fd - 1); f->fd = 0; break; }
		struct pollfd p = { .fd = f->fd - 1, .events = POLLIN };
		int ms = h->arg1 ? (int)((h->arg1 + 999999) / 1000000) : 0;
		int r = poll(&p, 1, ms > 50 ? 50 : ms);	/* never hold the hosted CPU in a host call longer than 50 ms */
		st = r < 0 ? -errno : r == 0 ? -XNUDRM_ETIME : 0;
		break;
	}
	default:
		st = -ENOSYS;
	}
	rh->status = st;
	rh->payload_len = st == 0 || h->op == XNUDRM_IOCTL ? rlen : 0;
	*outlen = 8 + sizeof(*rh) + rh->payload_len;
}

int main(int argc, char **argv)
{
	int a;
	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-v")) verbose++;
		else node = argv[a];
	}
	hc = open("/dev/xnu-hostcall", O_RDWR | O_CLOEXEC);
	if (hc < 0) { perror("/dev/xnu-hostcall"); return 1; }
	msg = malloc(8 + XNUDRM_MAX_MSG + sizeof(struct xnudrm_hdr));
	rep = malloc(8 + XNUDRM_MAX_MSG + sizeof(struct xnudrm_hdr));
	if (!msg || !rep) return 1;
	fprintf(stderr, "xnudrmd: serving %s\n", node);
	for (;;) {
		ssize_t n = read(hc, msg, 8 + XNUDRM_MAX_MSG + sizeof(struct xnudrm_hdr));
		size_t rn;
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			perror("read");
			return 1;
		}
		serve(msg, (size_t)n, rep, &rn);
		if (write(hc, rep, rn) < 0 && errno != ESTALE) perror("write");
	}
}
