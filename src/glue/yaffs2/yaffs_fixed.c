/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What five yaffs2 core files come to under the one configuration every
 * device has (wb_yaffs_add_blockdev(), S-12 section 3: yaffs2, in-band
 * tags, no tags ECC, no extended attributes, CPU byte order) and a trace
 * mask that stays zero. They are not built; these stand in for them
 * (src/third_party/README.md):
 *
 *   yaffs_tagscompat.c  yaffs1 tags: installed only when is_yaffs2 is clear
 *   yaffs_ecc.c         tags ECC: used only without in-band tags and with
 *                       no_tags_ecc clear
 *   yaffs_nameval.c     extended attributes: reached only from the xattr
 *                       calls, which nothing makes
 *   yaffs_endian.c      byte swaps: only when the stored order differs
 *                       from the CPU's
 *   yaffs_verify.c      checks that run only with a verify bit in
 *                       yaffs_trace_mask, and report only through traces
 *
 * A call that would need the real code goes through BUG(), which
 * wb_yaffs_bug_count counts, and fails where it can.
 */
#include "yaffs_port.h"

#include "yaffs_guts.h"
#include "yaffs_ecc.h"
#include "yaffs_endian.h"
#include "yaffs_nameval.h"
#include "yaffs_tagscompat.h"
#include "yaffs_trace.h"
#include "yaffs_verify.h"

/* yaffs_tagscompat.c. Without it a yaffs1 device has no tagger and its
 * mount fails yaffs_check_dev_fns(). */
/* cppcheck-suppress constParameterPointer ; yaffs2 fixes the type */
void yaffs_tags_compat_install(struct yaffs_dev *dev)
{
	if (!dev->param.is_yaffs2)
		BUG();
}

/* yaffs_ecc.c: tags ECC, for tags out of band only. */
void yaffs_ecc_calc_other(const unsigned char *data, unsigned n_bytes,
			  struct yaffs_ecc_other *ecc)
{
	(void)data;
	(void)n_bytes;
	memset(ecc, 0, sizeof(*ecc));
	BUG();
}

int yaffs_ecc_correct_other(unsigned char *data, unsigned n_bytes,
			    struct yaffs_ecc_other *read_ecc,
			    const struct yaffs_ecc_other *test_ecc)
{
	(void)data;
	(void)n_bytes;
	(void)read_ecc;
	(void)test_ecc;
	BUG();
	return -1;
}

/* yaffs_nameval.c: extended attributes. */
int nval_del(struct yaffs_dev *dev, char *xb, int xb_size, const YCHAR *name)
{
	(void)dev;
	(void)xb;
	(void)xb_size;
	(void)name;
	BUG();
	return -ENODATA;
}

int nval_set(struct yaffs_dev *dev, char *xb, int xb_size, const YCHAR *name,
	     const char *buf, int bsize, int flags)
{
	(void)dev;
	(void)xb;
	(void)xb_size;
	(void)name;
	(void)buf;
	(void)bsize;
	(void)flags;
	BUG();
	return -ENOSPC;
}

int nval_get(struct yaffs_dev *dev, const char *xb, int xb_size,
	     const YCHAR *name, char *buf, int bsize)
{
	(void)dev;
	(void)xb;
	(void)xb_size;
	(void)name;
	(void)buf;
	(void)bsize;
	BUG();
	return -ENODATA;
}

int nval_list(struct yaffs_dev *dev, const char *xb, int xb_size, char *buf,
	      int bsize)
{
	(void)dev;
	(void)xb;
	(void)xb_size;
	(void)buf;
	(void)bsize;
	BUG();
	return 0;
}

int nval_hasvalues(struct yaffs_dev *dev, const char *xb, int xb_size)
{
	(void)dev;
	(void)xb;
	(void)xb_size;
	BUG();
	return 0;
}

/* yaffs_endian.c. swap_endian stays zero: every stored order the build
 * accepts is the CPU's. */
void yaffs_endian_config(struct yaffs_dev *dev)
{
	const u32 one = 1;
	const int little = ((const unsigned char *)&one)[0] == 1;

	if (dev->tnode_size < 1)
		BUG();
	dev->swap_endian = 0;
	if (dev->param.stored_endian == (little ? 2 : 1))
		BUG();
}

void yaffs_do_endian_u32(struct yaffs_dev *dev, u32 *val)
{
	(void)dev;
	(void)val;
}

void yaffs_do_endian_s32(struct yaffs_dev *dev, s32 *val)
{
	(void)dev;
	(void)val;
}

void yaffs_do_endian_oh(struct yaffs_dev *dev, struct yaffs_obj_hdr *oh)
{
	(void)dev;
	(void)oh;
}

void yaffs_do_endian_packed_tags2(struct yaffs_dev *dev,
				  struct yaffs_packed_tags2_tags_only *ptt)
{
	(void)dev;
	(void)ptt;
}

/* yaffs_verify.c. The tests on a missing object or directory come before
 * the trace mask's, as there. */
int yaffs_skip_verification(struct yaffs_dev *dev)
{
	(void)dev;
	return !(yaffs_trace_mask &
		 (YAFFS_TRACE_VERIFY | YAFFS_TRACE_VERIFY_FULL));
}

void yaffs_verify_blk(struct yaffs_dev *dev, struct yaffs_block_info *bi,
		      int n)
{
	(void)dev;
	(void)bi;
	(void)n;
}

void yaffs_verify_collected_blk(struct yaffs_dev *dev,
				struct yaffs_block_info *bi, int n)
{
	(void)dev;
	(void)bi;
	(void)n;
}

void yaffs_verify_blocks(struct yaffs_dev *dev)
{
	(void)dev;
}

void yaffs_verify_oh(struct yaffs_obj *obj, struct yaffs_obj_hdr *oh,
		     struct yaffs_ext_tags *tags, int parent_check)
{
	(void)obj;
	(void)oh;
	(void)tags;
	(void)parent_check;
}

void yaffs_verify_file(struct yaffs_obj *obj)
{
	(void)obj;
}

void yaffs_verify_link(struct yaffs_obj *obj)
{
	(void)obj;
}

void yaffs_verify_symlink(struct yaffs_obj *obj)
{
	(void)obj;
}

void yaffs_verify_special(struct yaffs_obj *obj)
{
	(void)obj;
}

void yaffs_verify_obj(struct yaffs_obj *obj)
{
	(void)obj;
}

void yaffs_verify_objects(struct yaffs_dev *dev)
{
	(void)dev;
}

/* cppcheck-suppress constParameterPointer ; yaffs2 fixes the type */
void yaffs_verify_obj_in_dir(struct yaffs_obj *obj)
{
	if (!obj)
		BUG();
}

/* cppcheck-suppress constParameterPointer ; yaffs2 fixes the type */
void yaffs_verify_dir(struct yaffs_obj *directory)
{
	if (!directory || !directory->my_dev)
		BUG();
}

void yaffs_verify_free_chunks(struct yaffs_dev *dev)
{
	(void)dev;
}

int yaffs_verify_file_sane(struct yaffs_obj *obj)
{
	(void)obj;
	return YAFFS_OK;
}
