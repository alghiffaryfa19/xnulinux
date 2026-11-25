// SPDX-License-Identifier: MIT
/*
 * libxnudrm: Linux's DRM render node (virtio-gpu or Qualcomm MSM / Adreno) for Mesa's Venus and Turnip drivers inside
 * the hosted macOS. The node is a user client of kexts/XnuDrm; each DRM ioctl becomes one message to xnudrmd on the Linux
 * host (xnudrm_proto.h), with the ioctl struct's user pointers turned into offsets into the message. Buffer mappings are
 * Linux's own pages, mapped into this process by the kext. Fences are pipes whose read end stands for the fence (so dup'ed
 * descriptors stay the same fence); poll() on one asks xnudrmd, and once the fence signals the pipe is made readable for good.
 */
#ifdef XNUDRM_COMPAT_H	/* force-included into the whole driver: this file means the system's own calls */
#undef open
#undef close
#undef mmap
#undef munmap
#undef stat
#undef poll
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "drm-uapi/drm.h"
#include "drm-uapi/virtgpu_drm.h"
#include "drm-uapi/msm_drm.h"
#include "xnudrm_proto.h"
#include "xf86drm.h"

#define NODE_RENDER "/dev/dri/renderD128"
#define NODE_PRIMARY "/dev/dri/card0"

/* Linux errno values that differ from Darwin's */
#define XNUDRM_EAGAIN 11
#define XNUDRM_EBUSY  16
#define XNUDRM_ETIMEDOUT 110

static int linux_to_darwin_errno(int e)
{
	switch (e) {
	case XNUDRM_EAGAIN: return EAGAIN;
	case XNUDRM_EBUSY: return EBUSY;
	case XNUDRM_ETIME: return ETIME;
	case XNUDRM_ETIMEDOUT: return ETIMEDOUT;
	default: return e;
	}
}

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t conn;		/* the user client of the call in progress (under lk) */
static struct xnode { int fd; io_connect_t conn; int is_msm; int major, minor, patch; char name[64]; } nodes[64];
static unsigned nnodes;
static struct xmap { void *va; size_t len; uint64_t id; io_connect_t conn; } *maps;
static unsigned nmaps;
static struct xfence { dev_t dev; ino_t ino; int wfd; uint64_t id; io_connect_t conn; } *fences;
static unsigned nfences;
static uint8_t *buf;			/* message, then reply */

static int debug(void) { static int d = -1; if (d < 0) d = getenv("XNUDRM_DEBUG") != NULL; return d; }

static int open_conn(io_connect_t *c)
{
	/* the user client hangs off the Metal accelerator (an IOAccelerator: sandboxes let drawing processes open it) */
	io_service_t sv = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("XnuCpuMetalAccelerator"));
	if (!sv) sv = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("XnuDrm"));
	if (!sv) { if (debug()) fprintf(stderr, "xnudrm: no XnuDrm service\n"); return -ENODEV; }
	kern_return_t kr = IOServiceOpen(sv, mach_task_self(), XNUDRM_CONNECT_TYPE, c);
	IOObjectRelease(sv);
	if (kr != KERN_SUCCESS) { if (debug()) fprintf(stderr, "xnudrm: IOServiceOpen %#x\n", kr); return -EIO; }
	if (!buf) buf = malloc(sizeof(struct xnudrm_hdr) + XNUDRM_MAX_MSG);
	if (!buf) { IOServiceClose(*c); return -ENOMEM; }
	return 0;
}

/* The render node entry behind a file descriptor (NULL: not one). Called with lk held. */
static struct xnode *find_node(int fd)
{
	for (unsigned i = 0; i < nnodes; i++) if (nodes[i].fd == fd) return &nodes[i];
	return NULL;
}

static io_connect_t node_conn(int fd)
{
	struct xnode *n = find_node(fd);
	return n ? n->conn : 0;
}

/* buf holds a header and plen bytes of payload; the reply replaces it. Returns the reply's status or -errno. */
static int call(uint32_t plen)
{
	uint64_t in[4] = { (uintptr_t)buf, sizeof(struct xnudrm_hdr) + plen, (uintptr_t)buf, sizeof(struct xnudrm_hdr) + XNUDRM_MAX_MSG };
	uint64_t out = 0;
	uint32_t nout = 1;
	kern_return_t kr = IOConnectCallScalarMethod(conn, 0, in, 4, &out, &nout);
	if (kr != KERN_SUCCESS) { if (debug()) fprintf(stderr, "xnudrm: call %#x\n", kr); return -EIO; }
	int st = ((struct xnudrm_hdr *)buf)->status;
	return st < 0 ? -linux_to_darwin_errno(-st) : st;
}

static struct xnudrm_hdr *hdr(uint32_t op, uint64_t a0, uint64_t a1)
{
	struct xnudrm_hdr *h = (void *)buf;
	*h = (struct xnudrm_hdr){ .magic = XNUDRM_MAGIC, .op = op, .arg0 = a0, .arg1 = a1 };
	return h;
}

static void query_node_version(struct xnode *n)
{
	conn = n->conn;
	hdr(XNUDRM_IOCTL, 0x00, 0);
	uint8_t *pl = buf + sizeof(struct xnudrm_hdr);
	memset(pl, 0, 80);
	if (call(80) == 0) {
		int32_t *ver = (int32_t *)pl;
		n->major = ver[0];
		n->minor = ver[1];
		n->patch = ver[2];
		memcpy(n->name, pl + 12, 63);
		n->name[63] = '\0';
		n->is_msm = strcmp(n->name, "msm") == 0;
	} else {
		strcpy(n->name, "virtio_gpu");
		n->is_msm = 0;
	}
}

/* ---- the render node as a file ---- */

int xnudrm_open(const char *path, int flags, ...)
{
	if (path && !strcmp(path, NODE_RENDER)) {
		io_connect_t c = 0;
		pthread_mutex_lock(&lk);
		int r = nnodes < sizeof(nodes) / sizeof(nodes[0]) ? open_conn(&c) : -EMFILE;
		int fd = r ? -1 : open("/dev/null", O_RDWR | O_CLOEXEC);
		if (!r && fd < 0) { IOServiceClose(c); r = -errno; }
		if (!r) {
			struct xnode *n = &nodes[nnodes++];
			memset(n, 0, sizeof(*n));
			n->fd = fd;
			n->conn = c;
			query_node_version(n);
		}
		pthread_mutex_unlock(&lk);
		if (r) { errno = -r; return -1; }
		return fd;
	}
	if (flags & O_CREAT) {
		va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
		return open(path, flags, mode);
	}
	return open(path, flags);
}

int xnudrm_close(int fd)
{
	pthread_mutex_lock(&lk);
	for (unsigned i = 0; i < nnodes; i++) {
		if (nodes[i].fd == fd) {
			/* closing the user client drops what is still mapped from it and its DRM file on Linux */
			io_connect_t c = nodes[i].conn;
			nodes[i] = nodes[--nnodes];
			for (unsigned j = 0; j < nmaps; ) { if (maps[j].conn == c) maps[j] = maps[--nmaps]; else j++; }
			for (unsigned j = 0; j < nfences; j++) if (fences[j].conn == c && fences[j].id) fences[j].id = 0;
			IOServiceClose(c);
			break;
		}
	}
	for (unsigned j = 0; j < nfences; j++) {
		/* If caller closed a fence read-end pipe fd, release itsLinux fence in xnudrmd */
	}
	pthread_mutex_unlock(&lk);
	return close(fd);
}

int xnudrm_stat(const char *path, struct stat *st)
{
	if (path && !strcmp(path, NODE_RENDER)) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFCHR | 0666;
		st->st_rdev = makedev(226, 128);
		return 0;
	}
	if (path && !strcmp(path, NODE_PRIMARY)) { errno = ENOENT; return -1; }
	return stat(path, st);
}

/* ---- fences ---- */

static struct xfence *fence_of(int fd)
{
	if (fd < 0) return NULL;
	struct stat st;
	if (fstat(fd, &st)) return NULL;
	for (unsigned i = 0; i < nfences; i++)
		if (fences[i].ino == st.st_ino && fences[i].dev == st.st_dev) return &fences[i];
	return NULL;
}

static int new_fence(uint64_t id)	/* for conn */
{
	int p[2];
	if (pipe(p)) return -1;
	fcntl(p[0], F_SETFD, FD_CLOEXEC); fcntl(p[1], F_SETFD, FD_CLOEXEC);
	struct stat st; fstat(p[0], &st);
	for (unsigned i = 0; i < nfences; ) {
		if (fences[i].ino == st.st_ino && fences[i].dev == st.st_dev) {
			if (fences[i].wfd >= 0) close(fences[i].wfd);
			fences[i] = fences[--nfences];
		} else i++;
	}
	struct xfence *nf = realloc(fences, (nfences + 1) * sizeof(*fences));
	if (!nf) { close(p[0]); close(p[1]); return -1; }
	fences = nf;
	fences[nfences++] = (struct xfence){ st.st_dev, st.st_ino, p[1], id, conn };
	return p[0];
}

/* ---- ioctls ---- */

/* Append len bytes at p (or zeros) to the payload; the field becomes the offset. */
static int put(uint32_t *plen, uint64_t *field, const void *p, uint64_t len)
{
	if (!len) { *field = 0; return 0; }
	if (*plen + len > XNUDRM_MAX_MSG) return -E2BIG;
	uint8_t *pl = buf + sizeof(struct xnudrm_hdr);
	if (p) memcpy(pl + *plen, p, len); else memset(pl + *plen, 0, len);
	*field = *plen;
	*plen += (uint32_t)((len + 7) & ~7ull);
	return 0;
}

static int do_ioctl_msm(uint32_t nr, uint32_t size, void *arg)
{
	uint32_t plen = (size + 7) & ~7u;
	uint8_t *pl = buf + sizeof(struct xnudrm_hdr);
	uint8_t saved[256];
	int r;

	/* On MSM, MSM_INFO_GET_OFFSET is used only to pass an offset cookie into mmap(fd, offset):
	 * encode the GEM handle as (handle << 12) so xnudrm_mmap knows which GEM handle to map! */
	if (nr == DRM_COMMAND_BASE + DRM_MSM_GEM_INFO) {
		struct drm_msm_gem_info *gi = arg;
		if (gi->info == MSM_INFO_GET_OFFSET) {
			gi->value = (uint64_t)gi->handle << 12;
			return 0;
		}
	}

	hdr(XNUDRM_IOCTL, nr, 0);
	memcpy(pl, arg, size);
	memcpy(saved, arg, size);

	switch (nr) {
	case DRM_COMMAND_BASE + DRM_MSM_GET_PARAM:
	case DRM_COMMAND_BASE + DRM_MSM_SET_PARAM: {
		struct drm_msm_param *a = (void *)pl;
		if (a->len) {
			const void *src = (nr == DRM_COMMAND_BASE + DRM_MSM_SET_PARAM) ? (const void *)(uintptr_t)a->value : NULL;
			if ((r = put(&plen, &a->value, src, a->len))) return r;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO: {
		struct drm_msm_gem_info *a = (void *)pl;
		if (a->len) {
			const void *src = (a->info == MSM_INFO_SET_NAME || a->info == MSM_INFO_SET_METADATA)
				? (const void *)(uintptr_t)a->value : NULL;
			if ((r = put(&plen, &a->value, src, a->len))) return r;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY: {
		struct drm_msm_submitqueue_query *a = (void *)pl;
		if ((r = put(&plen, &a->data, NULL, a->len))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT: {
		struct drm_msm_gem_submit *a = (void *)pl;
		if (a->flags & MSM_SUBMIT_FENCE_FD_IN) {
			struct xfence *xf = fence_of(a->fence_fd);
			if (!xf || !xf->id) return -EINVAL;
			a->fence_fd = (int32_t)xf->id;
		}
		if (a->nr_bos && (r = put(&plen, &a->bos, (const void *)(uintptr_t)a->bos, (uint64_t)a->nr_bos * sizeof(struct drm_msm_gem_submit_bo)))) return r;
		if (a->nr_cmds && (r = put(&plen, &a->cmds, (const void *)(uintptr_t)a->cmds, (uint64_t)a->nr_cmds * sizeof(struct drm_msm_gem_submit_cmd)))) return r;
		uint32_t s_stride = a->syncobj_stride ? a->syncobj_stride : sizeof(struct drm_msm_syncobj);
		if (a->nr_in_syncobjs && (r = put(&plen, &a->in_syncobjs, (const void *)(uintptr_t)a->in_syncobjs, (uint64_t)a->nr_in_syncobjs * s_stride))) return r;
		if (a->nr_out_syncobjs && (r = put(&plen, &a->out_syncobjs, (const void *)(uintptr_t)a->out_syncobjs, (uint64_t)a->nr_out_syncobjs * s_stride))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_VM_BIND: {
		struct drm_msm_vm_bind *a = (void *)pl;
		if (a->flags & MSM_VM_BIND_FENCE_FD_IN) {
			struct xfence *xf = fence_of(a->fence_fd);
			if (!xf || !xf->id) return -EINVAL;
			a->fence_fd = (int32_t)xf->id;
		}
		uint32_t op_stride = a->op_stride ? a->op_stride : sizeof(struct drm_msm_vm_bind_op);
		if (a->nr_ops > 1 && (r = put(&plen, &a->ops, (const void *)(uintptr_t)a->ops, (uint64_t)a->nr_ops * op_stride))) return r;
		uint32_t s_stride = a->syncobj_stride ? a->syncobj_stride : sizeof(struct drm_msm_syncobj);
		if (a->nr_in_syncobjs && (r = put(&plen, &a->in_syncobjs, (const void *)(uintptr_t)a->in_syncobjs, (uint64_t)a->nr_in_syncobjs * s_stride))) return r;
		if (a->nr_out_syncobjs && (r = put(&plen, &a->out_syncobjs, (const void *)(uintptr_t)a->out_syncobjs, (uint64_t)a->nr_out_syncobjs * s_stride))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_NEW:
	case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP:
	case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI:
	case DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE:
	case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW:
	case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_CLOSE:
	case DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE:
		break;
	default:
		return -ENOTTY;
	}

	r = call(plen);
	if (r && r != -EINTR && r != -ETIMEDOUT && r != -EBUSY) {
		if (debug()) fprintf(stderr, "xnudrm: msm ioctl %#x: %d\n", nr, r);
	}

	switch (nr) {
	case DRM_COMMAND_BASE + DRM_MSM_GET_PARAM:
	case DRM_COMMAND_BASE + DRM_MSM_SET_PARAM: {
		struct drm_msm_param *a = (void *)pl, *s = (void *)saved;
		if (a->len) {
			if (!r && nr == DRM_COMMAND_BASE + DRM_MSM_GET_PARAM && s->value)
				memcpy((void *)(uintptr_t)s->value, pl + a->value, a->len);
			a->value = s->value;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO: {
		struct drm_msm_gem_info *a = (void *)pl, *s = (void *)saved;
		if (a->len) {
			if (!r && (a->info == MSM_INFO_GET_NAME || a->info == MSM_INFO_GET_METADATA) && s->value)
				memcpy((void *)(uintptr_t)s->value, pl + a->value, a->len);
			a->value = s->value;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY: {
		struct drm_msm_submitqueue_query *a = (void *)pl, *s = (void *)saved;
		if (!r && s->data && a->len) memcpy((void *)(uintptr_t)s->data, pl + a->data, a->len);
		a->data = s->data;
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT: {
		struct drm_msm_gem_submit *a = (void *)pl, *s = (void *)saved;
		a->bos = s->bos;
		a->cmds = s->cmds;
		a->in_syncobjs = s->in_syncobjs;
		a->out_syncobjs = s->out_syncobjs;
		if (!r && (a->flags & MSM_SUBMIT_FENCE_FD_OUT))
			a->fence_fd = new_fence((uint32_t)a->fence_fd);
		else
			a->fence_fd = s->fence_fd;
		break;
	}
	case DRM_COMMAND_BASE + DRM_MSM_VM_BIND: {
		struct drm_msm_vm_bind *a = (void *)pl, *s = (void *)saved;
		if (a->nr_ops > 1) a->ops = s->ops;
		a->in_syncobjs = s->in_syncobjs;
		a->out_syncobjs = s->out_syncobjs;
		if (!r && (a->flags & MSM_VM_BIND_FENCE_FD_OUT))
			a->fence_fd = new_fence((uint32_t)a->fence_fd);
		else
			a->fence_fd = s->fence_fd;
		break;
	}
	}
	memcpy(arg, pl, size);
	return r;
}

static int do_ioctl(struct xnode *node, unsigned long request, void *arg)
{
	uint32_t nr = request & 0xff, size = (request >> 16) & 0x1fff, plen = 0;
	uint8_t *pl = buf + sizeof(struct xnudrm_hdr);
	uint8_t saved[256];
	uint64_t f0 = 0;
	int r;

	if (size > 256) return -EINVAL;

	/* Generic DRM ioctls */
	switch (nr) {
	case 0x09:	/* GEM_CLOSE */
	case 0x0c:	/* GET_CAP */
	case 0xbf:	/* SYNCOBJ_CREATE */
	case 0xc0:	/* SYNCOBJ_DESTROY */
	case 0xcc: {	/* SYNCOBJ_TRANSFER */
		hdr(XNUDRM_IOCTL, nr, 0);
		memcpy(pl, arg, size);
		plen = (size + 7) & ~7u;
		r = call(plen);
		memcpy(arg, pl, size);
		return r;
	}
	case 0xc1: {	/* SYNCOBJ_HANDLE_TO_FD */
		hdr(XNUDRM_IOCTL, nr, 0);
		memcpy(pl, arg, size);
		plen = (size + 7) & ~7u;
		r = call(plen);
		struct drm_syncobj_handle *a = (void *)pl;
		if (!r) a->fd = new_fence((uint32_t)a->fd);
		memcpy(arg, pl, size);
		return r;
	}
	case 0xc2: {	/* SYNCOBJ_FD_TO_HANDLE */
		struct drm_syncobj_handle *in_a = arg;
		struct xfence *xf = fence_of(in_a->fd);
		if (!xf || !xf->id) return -EINVAL;
		hdr(XNUDRM_IOCTL, nr, 0);
		memcpy(pl, arg, size);
		((struct drm_syncobj_handle *)pl)->fd = (int32_t)xf->id;
		plen = (size + 7) & ~7u;
		r = call(plen);
		((struct drm_syncobj_handle *)pl)->fd = in_a->fd;
		memcpy(arg, pl, size);
		return r;
	}
	case 0xc3: {	/* SYNCOBJ_WAIT */
		hdr(XNUDRM_IOCTL, nr, 0);
		memcpy(pl, arg, size);
		memcpy(saved, arg, size);
		plen = (size + 7) & ~7u;
		struct drm_syncobj_wait *a = (void *)pl, *s = (void *)saved;
		if ((r = put(&plen, &a->handles, (const void *)(uintptr_t)a->handles, (uint64_t)a->count_handles * 4))) return r;
		r = call(plen);
		a->handles = s->handles;
		memcpy(arg, pl, size);
		return r;
	}
	case 0xc4:	/* SYNCOBJ_RESET */
	case 0xc5: {	/* SYNCOBJ_SIGNAL */
		hdr(XNUDRM_IOCTL, nr, 0);
		memcpy(pl, arg, size);
		memcpy(saved, arg, size);
		plen = (size + 7) & ~7u;
		struct drm_syncobj_array *a = (void *)pl, *s = (void *)saved;
		if ((r = put(&plen, &a->handles, (const void *)(uintptr_t)a->handles, (uint64_t)a->count_handles * 4))) return r;
		r = call(plen);
		a->handles = s->handles;
		memcpy(arg, pl, size);
		return r;
	}
	case 0xca: {	/* SYNCOBJ_TIMELINE_WAIT */
		hdr(XNUDRM_IOCTL, nr, 0);
		memcpy(pl, arg, size);
		memcpy(saved, arg, size);
		plen = (size + 7) & ~7u;
		struct drm_syncobj_timeline_wait *a = (void *)pl, *s = (void *)saved;
		if ((r = put(&plen, &a->handles, (const void *)(uintptr_t)a->handles, (uint64_t)a->count_handles * 4))) return r;
		if ((r = put(&plen, &a->points, (const void *)(uintptr_t)a->points, (uint64_t)a->count_handles * 8))) return r;
		r = call(plen);
		a->handles = s->handles;
		a->points = s->points;
		memcpy(arg, pl, size);
		return r;
	}
	case 0xcb:	/* SYNCOBJ_QUERY */
	case 0xcd: {	/* SYNCOBJ_TIMELINE_SIGNAL */
		hdr(XNUDRM_IOCTL, nr, 0);
		memcpy(pl, arg, size);
		memcpy(saved, arg, size);
		plen = (size + 7) & ~7u;
		struct drm_syncobj_timeline_array *a = (void *)pl, *s = (void *)saved;
		if ((r = put(&plen, &a->handles, (const void *)(uintptr_t)a->handles, (uint64_t)a->count_handles * 4))) return r;
		const void *pts_src = (nr == 0xcd) ? (const void *)(uintptr_t)a->points : NULL;
		if ((r = put(&plen, &a->points, pts_src, (uint64_t)a->count_handles * 8))) return r;
		r = call(plen);
		if (!r && nr == 0xcb && s->points)
			memcpy((void *)(uintptr_t)s->points, pl + a->points, (uint64_t)a->count_handles * 8);
		a->handles = s->handles;
		a->points = s->points;
		memcpy(arg, pl, size);
		return r;
	}
	}

	if (node->is_msm)
		return do_ioctl_msm(nr, size, arg);

	if (nr == DRM_COMMAND_BASE + DRM_VIRTGPU_MAP) {	/* answered here: see xnudrm_mmap */
		struct drm_virtgpu_map *m = arg;
		m->offset = (uint64_t)m->handle << 12;
		return 0;
	}
	hdr(XNUDRM_IOCTL, nr, 0);
	memcpy(pl, arg, size);
	plen = (size + 7) & ~7u;
	switch (nr) {
	case DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM: {
		struct drm_virtgpu_getparam *a = (void *)pl;
		f0 = a->value; if ((r = put(&plen, &a->value, NULL, 8))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_GET_CAPS: {
		struct drm_virtgpu_get_caps *a = (void *)pl;
		f0 = a->addr; if ((r = put(&plen, &a->addr, NULL, a->size))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT: {
		struct drm_virtgpu_context_init *a = (void *)pl;
		if ((r = put(&plen, &a->ctx_set_params, (void *)(uintptr_t)a->ctx_set_params, (uint64_t)a->num_params * sizeof(struct drm_virtgpu_context_set_param)))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB: {
		struct drm_virtgpu_resource_create_blob *a = (void *)pl;
		if ((r = put(&plen, &a->cmd, (void *)(uintptr_t)a->cmd, a->cmd_size))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER: {
		struct drm_virtgpu_execbuffer *a = (void *)pl;
		if (a->num_in_syncobjs || a->num_out_syncobjs || (a->flags & VIRTGPU_EXECBUF_FENCE_FD_IN)) return -EOPNOTSUPP;
		if ((r = put(&plen, &a->command, (void *)(uintptr_t)a->command, a->size))) return r;
		if ((r = put(&plen, &a->bo_handles, (void *)(uintptr_t)a->bo_handles, (uint64_t)a->num_bo_handles * 4))) return r;
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_INFO:
	case DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT:
		break;
	default:
		return -ENOTTY;
	}
	r = call(plen);
	if (r && r != -EINTR) { if (debug()) fprintf(stderr, "xnudrm: ioctl %#x: %d\n", nr, r); }
	/* copy the outputs back: the struct, then the out-buffers its pointers named */
	switch (nr) {
	case DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM: {
		struct drm_virtgpu_getparam *a = (void *)pl;
		if (!r) memcpy((void *)(uintptr_t)f0, pl + a->value, sizeof(int));
		a->value = f0;
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_GET_CAPS: {
		struct drm_virtgpu_get_caps *a = (void *)pl;
		if (!r) memcpy((void *)(uintptr_t)f0, pl + a->addr, a->size);
		a->addr = f0;
		break;
	}
	case DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT:
	case DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB:
	case DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER: {
		/* pointer fields go back to the caller's values */
		memcpy(saved, arg, size);
		if (nr == DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT) ((struct drm_virtgpu_context_init *)pl)->ctx_set_params = ((struct drm_virtgpu_context_init *)saved)->ctx_set_params;
		if (nr == DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB) ((struct drm_virtgpu_resource_create_blob *)pl)->cmd = ((struct drm_virtgpu_resource_create_blob *)saved)->cmd;
		if (nr == DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER) {
			struct drm_virtgpu_execbuffer *a = (void *)pl, *s = (void *)saved;
			a->command = s->command; a->bo_handles = s->bo_handles;
			if (!r && (a->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT)) a->fence_fd = new_fence((uint32_t)a->fence_fd);
			if (debug()) fprintf(stderr, "xnudrm: execbuffer flags %#x size %u ring %u -> %d, fence fd %d\n", a->flags, a->size, a->ring_idx, r, a->fence_fd);
		}
		break;
	}
	}
	memcpy(arg, pl, size);
	return r;
}

static uint64_t now_mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
	uint32_t nr = request & 0xff;

	/* Never block a hosted XNU CPU inside a hostcall (deaf to IPIs):
	 * poll non-blocking on Linux and sleep in macOS userspace with exponential backoff up to 2 ms. */
	if (nr == 0xc3 || nr == 0xca) {	/* DRM_IOCTL_SYNCOBJ_WAIT / TIMELINE_WAIT */
		int64_t deadline = (nr == 0xca)
			? ((struct drm_syncobj_timeline_wait *)arg)->timeout_nsec
			: ((struct drm_syncobj_wait *)arg)->timeout_nsec;
		long delay_ns = 100000;
		for (;;) {
			pthread_mutex_lock(&lk);
			struct xnode *n = find_node(fd);
			conn = n ? n->conn : 0;
			int r = n ? do_ioctl(n, request, arg) : -EBADF;
			pthread_mutex_unlock(&lk);
			if (r != -ETIME) {
				if (r) { errno = -r; return -1; }
				return 0;
			}
			if (deadline <= 0 || (uint64_t)deadline <= now_mono_ns()) {
				errno = ETIME;
				return -1;
			}
			struct timespec d = { 0, delay_ns };
			nanosleep(&d, NULL);
			if (delay_ns < 2000000) {
				delay_ns <<= 1;
				if (delay_ns > 2000000) delay_ns = 2000000;
			}
		}
	}

	pthread_mutex_lock(&lk);
	struct xnode *n = find_node(fd);
	int is_msm = n ? n->is_msm : 0;
	pthread_mutex_unlock(&lk);

	if (!is_msm && nr == DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT && arg &&
	    !(((struct drm_virtgpu_3d_wait *)arg)->flags & VIRTGPU_WAIT_NOWAIT)) {
		long delay_ns = 100000;	/* 100 us -> 2 ms */
		for (;;) {
			struct drm_virtgpu_3d_wait nw = *(struct drm_virtgpu_3d_wait *)arg;
			nw.flags |= VIRTGPU_WAIT_NOWAIT;
			pthread_mutex_lock(&lk);
			struct xnode *nd = find_node(fd);
			conn = nd ? nd->conn : 0;
			int r = nd ? do_ioctl(nd, request, &nw) : -EBADF;
			pthread_mutex_unlock(&lk);
			if (r != -EBUSY) {
				if (r) { errno = -r; return -1; }
				return 0;
			}
			struct timespec d = { 0, delay_ns };
			nanosleep(&d, NULL);
			if (delay_ns < 2000000) {
				delay_ns <<= 1;
				if (delay_ns > 2000000) delay_ns = 2000000;
			}
		}
	}

	if (is_msm && nr == DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE && arg) {
		struct drm_msm_wait_fence *wf = arg;
		uint64_t deadline_ns = (uint64_t)wf->timeout.tv_sec * 1000000000ULL + (uint64_t)wf->timeout.tv_nsec;
		long delay_ns = 100000;
		for (;;) {
			pthread_mutex_lock(&lk);
			struct xnode *nd = find_node(fd);
			conn = nd ? nd->conn : 0;
			int r = nd ? do_ioctl(nd, request, arg) : -EBADF;
			pthread_mutex_unlock(&lk);
			if (r != -ETIMEDOUT) {
				if (r) { errno = -r; return -1; }
				return 0;
			}
			if (deadline_ns <= now_mono_ns()) {
				errno = ETIMEDOUT;
				return -1;
			}
			struct timespec d = { 0, delay_ns };
			nanosleep(&d, NULL);
			if (delay_ns < 2000000) {
				delay_ns <<= 1;
				if (delay_ns > 2000000) delay_ns = 2000000;
			}
		}
	}

	pthread_mutex_lock(&lk);
	n = find_node(fd);
	conn = n ? n->conn : 0;
	int r = n ? do_ioctl(n, request, arg) : -EBADF;
	pthread_mutex_unlock(&lk);
	if (r) { errno = -r; return -1; }
	return 0;
}

int drmCommandRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
	unsigned long req = _IOC(IOC_OUT, DRM_IOCTL_BASE, DRM_COMMAND_BASE + drmCommandIndex, size);
	return drmIoctl(fd, req, data) ? -errno : 0;
}

int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
	unsigned long req = _IOC(IOC_IN, DRM_IOCTL_BASE, DRM_COMMAND_BASE + drmCommandIndex, size);
	return drmIoctl(fd, req, data) ? -errno : 0;
}

int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
	unsigned long req = _IOC(IOC_INOUT, DRM_IOCTL_BASE, DRM_COMMAND_BASE + drmCommandIndex, size);
	return drmIoctl(fd, req, data) ? -errno : 0;
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	struct drm_get_cap c = { .capability = capability };
	if (drmIoctl(fd, DRM_IOCTL_GET_CAP, &c)) return -errno;
	*value = c.value;
	return 0;
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	errno = ENOSYS;
	return -1;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd; (void)prime_fd; (void)handle;
	errno = ENOSYS;
	return -1;
}

/* ---- syncobj helpers ---- */

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	struct drm_syncobj_create a = { .flags = flags };
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &a)) return -errno;
	*handle = a.handle;
	return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy a = { .handle = handle };
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &a) ? -errno : 0;
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	struct drm_syncobj_handle a = { .handle = handle };
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &a)) return -errno;
	*obj_fd = a.fd;
	return 0;
}

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	struct drm_syncobj_handle a = { .fd = obj_fd };
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &a)) return -errno;
	*handle = a.handle;
	return 0;
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	struct drm_syncobj_handle a = {
		.handle = handle,
		.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
		.fd = sync_file_fd,
	};
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &a) ? -errno : 0;
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	struct drm_syncobj_handle a = {
		.handle = handle,
		.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
	};
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &a)) return -errno;
	*sync_file_fd = a.fd;
	return 0;
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
		   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_wait a = {
		.handles = (uintptr_t)handles,
		.timeout_nsec = timeout_nsec,
		.count_handles = num_handles,
		.flags = flags,
	};
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &a)) return -errno;
	if (first_signaled) *first_signaled = a.first_signaled;
	return 0;
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array a = {
		.handles = (uintptr_t)handles,
		.count_handles = handle_count,
	};
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &a) ? -errno : 0;
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array a = {
		.handles = (uintptr_t)handles,
		.count_handles = handle_count,
	};
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &a) ? -errno : 0;
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
			     uint64_t *points, uint32_t handle_count)
{
	struct drm_syncobj_timeline_array a = {
		.handles = (uintptr_t)handles,
		.points = (uintptr_t)points,
		.count_handles = handle_count,
	};
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a) ? -errno : 0;
}

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
			   unsigned num_handles, int64_t timeout_nsec,
			   unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_timeline_wait a = {
		.handles = (uintptr_t)handles,
		.points = (uintptr_t)points,
		.timeout_nsec = timeout_nsec,
		.count_handles = num_handles,
		.flags = flags,
	};
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &a)) return -errno;
	if (first_signaled) *first_signaled = a.first_signaled;
	return 0;
}

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	struct drm_syncobj_timeline_array a = {
		.handles = (uintptr_t)handles,
		.points = (uintptr_t)points,
		.count_handles = handle_count,
		.flags = flags,
	};
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &a) ? -errno : 0;
}

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
		       uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	struct drm_syncobj_transfer a = {
		.src_handle = src_handle,
		.dst_handle = dst_handle,
		.src_point = src_point,
		.dst_point = dst_point,
		.flags = flags,
	};
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &a) ? -errno : 0;
}

/* ---- mappings: mmap(node fd, offset) after DRM_IOCTL_VIRTGPU_MAP or MSM_INFO_GET_OFFSET ---- */

int xnudrm_munmap(void *addr, size_t len);

/*
 * VIRTGPU_MAP's / MSM_INFO_GET_OFFSET's offset is the GEM handle's mmap cookie on Linux; xnudrmd maps by handle,
 * so the shim answers MAP / GET_OFFSET itself with (handle << 12) as the offset and mmap() maps that handle.
 */
void *xnudrm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	pthread_mutex_lock(&lk);
	io_connect_t c = fd >= 0 ? node_conn(fd) : 0;
	if (!c) { pthread_mutex_unlock(&lk); return mmap(addr, len, prot, flags, fd, off); }
	uint64_t in[2] = { (uint64_t)off >> 12, len }, out[2] = { 0, 0 };
	uint32_t nout = 2;
	kern_return_t kr = IOConnectCallScalarMethod(c, 1, in, 2, out, &nout);
	if (kr == KERN_SUCCESS) {
		struct xmap *nm = realloc(maps, (nmaps + 1) * sizeof(*maps));
		if (nm) { maps = nm; maps[nmaps++] = (struct xmap){ (void *)(uintptr_t)out[0], len, out[1], c }; }
	}
	pthread_mutex_unlock(&lk);
	if (kr != KERN_SUCCESS) { if (debug()) fprintf(stderr, "xnudrm: map handle %llu: %#x\n", (unsigned long long)in[0], kr); errno = ENOMEM; return MAP_FAILED; }
	if (addr && (flags & MAP_FIXED) && addr != (void *)(uintptr_t)out[0]) {
		/* a placed mapping (VkPhysicalDeviceMapMemoryPlacedFeaturesEXT): not supported over the bridge */
		xnudrm_munmap((void *)(uintptr_t)out[0], len);
		errno = EINVAL;
		return MAP_FAILED;
	}
	return (void *)(uintptr_t)out[0];
}

int xnudrm_munmap(void *addr, size_t len)
{
	pthread_mutex_lock(&lk);
	for (unsigned i = 0; i < nmaps; i++) {
		if (maps[i].va == addr) {
			uint64_t id = maps[i].id;
			io_connect_t c = maps[i].conn;
			maps[i] = maps[--nmaps];
			IOConnectCallScalarMethod(c, 2, &id, 1, NULL, NULL);
			pthread_mutex_unlock(&lk);
			return 0;
		}
	}
	pthread_mutex_unlock(&lk);
	return munmap(addr, len);
}

/* ---- fence polling ---- */

/* 1 signalled, 0 not yet, <0 error. Called with lk held. */
static int fence_check(struct xfence *f)
{
	if (!f->id) return 1;
	conn = f->conn;
	hdr(XNUDRM_FENCE_WAIT, f->id, 0);
	int r = call(0);
	if (r == -ETIME) return 0;
	if (debug() && r) fprintf(stderr, "xnudrm: fence %llu wait: %d\n", (unsigned long long)f->id, r);
	hdr(XNUDRM_FENCE_CLOSE, f->id, 0);
	call(0);
	f->id = 0;
	(void)!write(f->wfd, "s", 1);	/* signalled for good */
	close(f->wfd);
	f->wfd = -1;
	return r ? r : 1;
}

int xnudrm_poll(struct pollfd *fds, nfds_t n, int timeout)
{
	struct timespec t0, now;
	long delay_ns = 100000;	/* 100 us -> 2 ms */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		int pending = 0;
		pthread_mutex_lock(&lk);
		for (nfds_t i = 0; i < n; i++) {
			struct xfence *f = nfences ? fence_of(fds[i].fd) : NULL;
			if (f && f->id && fence_check(f) == 0) pending = 1;
		}
		pthread_mutex_unlock(&lk);
		if (debug()) {
			fprintf(stderr, "xnudrm: poll %u fds, timeout %d: %s;", (unsigned)n, timeout, pending ? "pending" : "ready");
			for (nfds_t i = 0; i < n; i++) { struct xfence *f = nfences ? fence_of(fds[i].fd) : NULL; fprintf(stderr, " fd %d -> %s id %llu", fds[i].fd, f ? "fence" : "other", f ? (unsigned long long)f->id : 0ull); }
			fprintf(stderr, "\n");
		}
		if (!pending) return poll(fds, n, timeout);	/* signalled fences are readable pipes now */
		clock_gettime(CLOCK_MONOTONIC, &now);
		long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
		if (timeout >= 0 && ms >= timeout) return poll(fds, n, 0);
		/* each check stops the hosted CPU for one host call: back off exponentially up to 2 ms */
		struct timespec d = { 0, delay_ns };
		nanosleep(&d, NULL);
		if (delay_ns < 2000000) {
			delay_ns <<= 1;
			if (delay_ns > 2000000) delay_ns = 2000000;
		}
	}
}

/* ---- device discovery ---- */

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max)
{
	(void)flags;
	if (!devices) return 1;
	if (max < 1) return 0;
	drmDevicePtr d = calloc(1, sizeof(*d) + DRM_NODE_MAX * sizeof(char *) + sizeof(drmPciBusInfo) + sizeof(drmPciDeviceInfo));
	if (!d) return -ENOMEM;
	d->nodes = (char **)(d + 1);
	d->nodes[DRM_NODE_PRIMARY] = NODE_PRIMARY;
	d->nodes[DRM_NODE_RENDER] = NODE_RENDER;
	d->available_nodes = 1 << DRM_NODE_RENDER;
	d->bustype = DRM_BUS_PCI;
	d->businfo.pci = (drmPciBusInfoPtr)(d->nodes + DRM_NODE_MAX);
	d->deviceinfo.pci = (drmPciDeviceInfoPtr)(d->businfo.pci + 1);
	d->deviceinfo.pci->vendor_id = 0x1af4;	/* virtio (Turnip ignores bus/vendor and checks drmGetVersion) */
	d->deviceinfo.pci->device_id = 0x1050;	/* virtio-gpu */
	devices[0] = d;
	return 1;
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; i < count; i++) free(devices[i]);
}

drmVersionPtr drmGetVersion(int fd)
{
	pthread_mutex_lock(&lk);
	struct xnode *n = find_node(fd);
	int is_msm = n ? n->is_msm : 0;
	int major = n ? n->major : 0;
	int minor = n ? n->minor : 0;
	int patch = n ? n->patch : 0;
	pthread_mutex_unlock(&lk);
	if (!n) return NULL;
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (!v) return NULL;
	if (is_msm) {
		v->version_major = major ? major : 1;
		v->version_minor = minor ? minor : 13;
		v->version_patchlevel = patch;
		v->name = "msm";
		v->name_len = 3;
		v->desc = "Qualcomm Adreno MSM DRM over the xnulinux DRM bridge";
	} else {
		v->version_major = major;
		v->version_minor = minor;
		v->version_patchlevel = patch;
		v->name = "virtio_gpu";
		v->name_len = 10;
		v->desc = "virtio GPU over the xnulinux DRM bridge";
	}
	v->date = "0";
	return v;
}

void drmFreeVersion(drmVersionPtr v) { free(v); }
