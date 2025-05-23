// SPDX-License-Identifier: GPL-2.0
/*
 * f3fs sysfs interface
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 * Copyright (c) 2017 Chao Yu <chao@kernel.org>
 */
#include <linux/compiler.h>
#include <linux/proc_fs.h>
#include <linux/f3fs_fs.h>
#include <linux/seq_file.h>
#include <linux/unicode.h>
#include <linux/ioprio.h>
#include <linux/sysfs.h>

#include "f3fs.h"
#include "segment.h"
#include "gc.h"
#include "iostat.h"
#include <trace/events/f3fs.h>

static struct proc_dir_entry *f3fs_proc_root;

/* Sysfs support for f3fs */
enum {
	GC_THREAD,	/* struct f3fs_gc_thread */
	SM_INFO,	/* struct f3fs_sm_info */
	DCC_INFO,	/* struct discard_cmd_control */
	NM_INFO,	/* struct f3fs_nm_info */
	F3FS_SBI,	/* struct f3fs_sb_info */
#ifdef CONFIG_F3FS_STAT_FS
	STAT_INFO,	/* struct f3fs_stat_info */
#endif
#ifdef CONFIG_F3FS_FAULT_INJECTION
	FAULT_INFO_RATE,	/* struct f3fs_fault_info */
	FAULT_INFO_TYPE,	/* struct f3fs_fault_info */
#endif
	RESERVED_BLOCKS,	/* struct f3fs_sb_info */
	CPRC_INFO,	/* struct ckpt_req_control */
	ATGC_INFO,	/* struct atgc_management */
};

static const char *gc_mode_names[MAX_GC_MODE] = {
	"GC_NORMAL",
	"GC_IDLE_CB",
	"GC_IDLE_GREEDY",
	"GC_IDLE_AT",
	"GC_URGENT_HIGH",
	"GC_URGENT_LOW",
	"GC_URGENT_MID"
};

struct f3fs_attr {
	struct attribute attr;
	ssize_t (*show)(struct f3fs_attr *, struct f3fs_sb_info *, char *);
	ssize_t (*store)(struct f3fs_attr *, struct f3fs_sb_info *,
			 const char *, size_t);
	int struct_type;
	int offset;
	int id;
};

static ssize_t f3fs_sbi_show(struct f3fs_attr *a,
			     struct f3fs_sb_info *sbi, char *buf);

static unsigned char *__struct_ptr(struct f3fs_sb_info *sbi, int struct_type)
{
	if (struct_type == GC_THREAD)
		return (unsigned char *)sbi->gc_thread;
	else if (struct_type == SM_INFO)
		return (unsigned char *)SM_I(sbi);
	else if (struct_type == DCC_INFO)
		return (unsigned char *)SM_I(sbi)->dcc_info;
	else if (struct_type == NM_INFO)
		return (unsigned char *)NM_I(sbi);
	else if (struct_type == F3FS_SBI || struct_type == RESERVED_BLOCKS)
		return (unsigned char *)sbi;
#ifdef CONFIG_F3FS_FAULT_INJECTION
	else if (struct_type == FAULT_INFO_RATE ||
					struct_type == FAULT_INFO_TYPE)
		return (unsigned char *)&F3FS_OPTION(sbi).fault_info;
#endif
#ifdef CONFIG_F3FS_STAT_FS
	else if (struct_type == STAT_INFO)
		return (unsigned char *)F3FS_STAT(sbi);
#endif
	else if (struct_type == CPRC_INFO)
		return (unsigned char *)&sbi->cprc_info;
	else if (struct_type == ATGC_INFO)
		return (unsigned char *)&sbi->am;
	return NULL;
}

static ssize_t dirty_segments_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu %llu %llu %llu %llu\n",
    //	(unsigned long long)(dirty_segments(sbi)));
    (unsigned long long)(gc_read_blocks(sbi)),
    (unsigned long long)(gc_written_blocks(sbi)),
    (unsigned long long)(total_written_blocks(sbi)),
    (unsigned long long)(total_written_request_blocks(sbi)),
    (unsigned long long)(total_written_direct_request_blocks(sbi))
    );
}

static ssize_t free_segments_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu\n",
			(unsigned long long)(free_segments(sbi)));
}

static ssize_t ovp_segments_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu\n",
			(unsigned long long)(overprovision_segments(sbi)));
}

static ssize_t lifetime_write_kbytes_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu\n",
			(unsigned long long)(sbi->kbytes_written +
			((f3fs_get_sectors_written(sbi) -
				sbi->sectors_written_start) >> 1)));
}

static ssize_t sb_status_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%lx\n", sbi->s_flag);
}

static ssize_t pending_discard_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	if (!SM_I(sbi)->dcc_info)
		return -EINVAL;
	return sprintf(buf, "%llu\n", (unsigned long long)atomic_read(
				&SM_I(sbi)->dcc_info->discard_cmd_cnt));
}

static ssize_t features_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	int len = 0;

	if (f3fs_sb_has_encrypt(sbi))
		len += scnprintf(buf, PAGE_SIZE - len, "%s",
						"encryption");
	if (f3fs_sb_has_blkzoned(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "blkzoned");
	if (f3fs_sb_has_extra_attr(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "extra_attr");
	if (f3fs_sb_has_project_quota(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "projquota");
	if (f3fs_sb_has_inode_chksum(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "inode_checksum");
	if (f3fs_sb_has_flexible_inline_xattr(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "flexible_inline_xattr");
	if (f3fs_sb_has_quota_ino(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "quota_ino");
	if (f3fs_sb_has_inode_crtime(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "inode_crtime");
	if (f3fs_sb_has_lost_found(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "lost_found");
	if (f3fs_sb_has_verity(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "verity");
	if (f3fs_sb_has_sb_chksum(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "sb_checksum");
	if (f3fs_sb_has_casefold(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "casefold");
	if (f3fs_sb_has_readonly(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "readonly");
	if (f3fs_sb_has_compression(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "compression");
	len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "pin_file");
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static ssize_t current_reserved_blocks_show(struct f3fs_attr *a,
					struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%u\n", sbi->current_reserved_blocks);
}

static ssize_t unusable_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	block_t unusable;

	if (test_opt(sbi, DISABLE_CHECKPOINT))
		unusable = sbi->unusable_block_count;
	else
		unusable = f3fs_get_unusable_blocks(sbi);
	return sprintf(buf, "%llu\n", (unsigned long long)unusable);
}

static ssize_t encoding_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
#if IS_ENABLED(CONFIG_UNICODE)
	struct super_block *sb = sbi->sb;

	if (f3fs_sb_has_casefold(sbi))
		return sysfs_emit(buf, "UTF-8 (%d.%d.%d)\n",
			(sb->s_encoding->version >> 16) & 0xff,
			(sb->s_encoding->version >> 8) & 0xff,
			sb->s_encoding->version & 0xff);
#endif
	return sprintf(buf, "(none)");
}

static ssize_t mounted_time_sec_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu", SIT_I(sbi)->mounted_time);
}

#ifdef CONFIG_F3FS_STAT_FS
static ssize_t moved_blocks_foreground_show(struct f3fs_attr *a,
				struct f3fs_sb_info *sbi, char *buf)
{
	struct f3fs_stat_info *si = F3FS_STAT(sbi);

	return sprintf(buf, "%llu\n",
		(unsigned long long)(si->tot_blks -
			(si->bg_data_blks + si->bg_node_blks)));
}

static ssize_t moved_blocks_background_show(struct f3fs_attr *a,
				struct f3fs_sb_info *sbi, char *buf)
{
	struct f3fs_stat_info *si = F3FS_STAT(sbi);

	return sprintf(buf, "%llu\n",
		(unsigned long long)(si->bg_data_blks + si->bg_node_blks));
}

static ssize_t avg_vblocks_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	struct f3fs_stat_info *si = F3FS_STAT(sbi);

	si->dirty_count = dirty_segments(sbi);
	f3fs_update_sit_info(sbi);
	return sprintf(buf, "%llu\n", (unsigned long long)(si->avg_vblocks));
}
#endif

static ssize_t main_blkaddr_show(struct f3fs_attr *a,
				struct f3fs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%llu\n",
			(unsigned long long)MAIN_BLKADDR(sbi));
}

static ssize_t f3fs_sbi_show(struct f3fs_attr *a,
			struct f3fs_sb_info *sbi, char *buf)
{
	unsigned char *ptr = NULL;
	unsigned int *ui;

	ptr = __struct_ptr(sbi, a->struct_type);
	if (!ptr)
		return -EINVAL;

	if (!strcmp(a->attr.name, "extension_list")) {
		__u8 (*extlist)[F3FS_EXTENSION_LEN] =
					sbi->raw_super->extension_list;
		int cold_count = le32_to_cpu(sbi->raw_super->extension_count);
		int hot_count = sbi->raw_super->hot_ext_count;
		int len = 0, i;

		len += scnprintf(buf + len, PAGE_SIZE - len,
						"cold file extension:\n");
		for (i = 0; i < cold_count; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n",
								extlist[i]);

		len += scnprintf(buf + len, PAGE_SIZE - len,
						"hot file extension:\n");
		for (i = cold_count; i < cold_count + hot_count; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n",
								extlist[i]);
		return len;
	}

	if (!strcmp(a->attr.name, "ckpt_thread_ioprio")) {
		struct ckpt_req_control *cprc = &sbi->cprc_info;
		int len = 0;
		int class = IOPRIO_PRIO_CLASS(cprc->ckpt_thread_ioprio);
		int data = IOPRIO_PRIO_DATA(cprc->ckpt_thread_ioprio);

		if (class == IOPRIO_CLASS_RT)
			len += scnprintf(buf + len, PAGE_SIZE - len, "rt,");
		else if (class == IOPRIO_CLASS_BE)
			len += scnprintf(buf + len, PAGE_SIZE - len, "be,");
		else
			return -EINVAL;

		len += scnprintf(buf + len, PAGE_SIZE - len, "%d\n", data);
		return len;
	}

#ifdef CONFIG_F3FS_FS_COMPRESSION
	if (!strcmp(a->attr.name, "compr_written_block"))
		return sysfs_emit(buf, "%llu\n", sbi->compr_written_block);

	if (!strcmp(a->attr.name, "compr_saved_block"))
		return sysfs_emit(buf, "%llu\n", sbi->compr_saved_block);

	if (!strcmp(a->attr.name, "compr_new_inode"))
		return sysfs_emit(buf, "%u\n", sbi->compr_new_inode);
#endif

	if (!strcmp(a->attr.name, "gc_urgent"))
		return sysfs_emit(buf, "%s\n",
				gc_mode_names[sbi->gc_mode]);

	if (!strcmp(a->attr.name, "gc_segment_mode"))
		return sysfs_emit(buf, "%s\n",
				gc_mode_names[sbi->gc_segment_mode]);

	if (!strcmp(a->attr.name, "gc_reclaimed_segments")) {
		return sysfs_emit(buf, "%u\n",
			sbi->gc_reclaimed_segs[sbi->gc_segment_mode]);
	}

	if (!strcmp(a->attr.name, "current_atomic_write")) {
		s64 current_write = atomic64_read(&sbi->current_atomic_write);

		return sysfs_emit(buf, "%lld\n", current_write);
	}

	if (!strcmp(a->attr.name, "peak_atomic_write"))
		return sysfs_emit(buf, "%lld\n", sbi->peak_atomic_write);

	if (!strcmp(a->attr.name, "committed_atomic_block"))
		return sysfs_emit(buf, "%llu\n", sbi->committed_atomic_block);

	if (!strcmp(a->attr.name, "revoked_atomic_block"))
		return sysfs_emit(buf, "%llu\n", sbi->revoked_atomic_block);

	ui = (unsigned int *)(ptr + a->offset);

	return sprintf(buf, "%u\n", *ui);
}

static ssize_t __sbi_store(struct f3fs_attr *a,
			struct f3fs_sb_info *sbi,
			const char *buf, size_t count)
{
	unsigned char *ptr;
	unsigned long t;
	unsigned int *ui;
	ssize_t ret;

	ptr = __struct_ptr(sbi, a->struct_type);
	if (!ptr)
		return -EINVAL;

	if (!strcmp(a->attr.name, "extension_list")) {
		const char *name = strim((char *)buf);
		bool set = true, hot;

		if (!strncmp(name, "[h]", 3))
			hot = true;
		else if (!strncmp(name, "[c]", 3))
			hot = false;
		else
			return -EINVAL;

		name += 3;

		if (*name == '!') {
			name++;
			set = false;
		}

		if (!strlen(name) || strlen(name) >= F3FS_EXTENSION_LEN)
			return -EINVAL;

		f3fs_down_write(&sbi->sb_lock);

		ret = f3fs_update_extension_list(sbi, name, hot, set);
		if (ret)
			goto out;

		ret = f3fs_commit_super(sbi, false);
		if (ret)
			f3fs_update_extension_list(sbi, name, hot, !set);
out:
		f3fs_up_write(&sbi->sb_lock);
		return ret ? ret : count;
	}

	if (!strcmp(a->attr.name, "ckpt_thread_ioprio")) {
		const char *name = strim((char *)buf);
		struct ckpt_req_control *cprc = &sbi->cprc_info;
		int class;
		long data;
		int ret;

		if (!strncmp(name, "rt,", 3))
			class = IOPRIO_CLASS_RT;
		else if (!strncmp(name, "be,", 3))
			class = IOPRIO_CLASS_BE;
		else
			return -EINVAL;

		name += 3;
		ret = kstrtol(name, 10, &data);
		if (ret)
			return ret;
		if (data >= IOPRIO_NR_LEVELS || data < 0)
			return -EINVAL;

		cprc->ckpt_thread_ioprio = IOPRIO_PRIO_VALUE(class, data);
		if (test_opt(sbi, MERGE_CHECKPOINT)) {
			ret = set_task_ioprio(cprc->f3fs_issue_ckpt,
					cprc->ckpt_thread_ioprio);
			if (ret)
				return ret;
		}

		return count;
	}

	ui = (unsigned int *)(ptr + a->offset);

	ret = kstrtoul(skip_spaces(buf), 0, &t);
	if (ret < 0)
		return ret;
#ifdef CONFIG_F3FS_FAULT_INJECTION
	if (a->struct_type == FAULT_INFO_TYPE && t >= (1 << FAULT_MAX))
		return -EINVAL;
	if (a->struct_type == FAULT_INFO_RATE && t >= UINT_MAX)
		return -EINVAL;
#endif
	if (a->struct_type == RESERVED_BLOCKS) {
		spin_lock(&sbi->stat_lock);
		if (t > (unsigned long)(sbi->user_block_count -
				F3FS_OPTION(sbi).root_reserved_blocks -
				sbi->blocks_per_seg *
				SM_I(sbi)->additional_reserved_segments)) {
			spin_unlock(&sbi->stat_lock);
			return -EINVAL;
		}
		*ui = t;
		sbi->current_reserved_blocks = min(sbi->reserved_blocks,
				sbi->user_block_count - valid_user_blocks(sbi));
		spin_unlock(&sbi->stat_lock);
		return count;
	}

	if (!strcmp(a->attr.name, "discard_granularity")) {
		if (t == 0 || t > MAX_PLIST_NUM)
			return -EINVAL;
		if (!f3fs_block_unit_discard(sbi))
			return -EINVAL;
		if (t == *ui)
			return count;
		*ui = t;
		return count;
	}

	if (!strcmp(a->attr.name, "migration_granularity")) {
		if (t == 0 || t > sbi->segs_per_sec)
			return -EINVAL;
	}

	if (!strcmp(a->attr.name, "trim_sections"))
		return -EINVAL;

	if (!strcmp(a->attr.name, "gc_urgent")) {
		if (t == 0) {
			sbi->gc_mode = GC_NORMAL;
		} else if (t == 1) {
			sbi->gc_mode = GC_URGENT_HIGH;
			if (sbi->gc_thread) {
				sbi->gc_thread->gc_wake = 1;
				wake_up_interruptible_all(
					&sbi->gc_thread->gc_wait_queue_head);
				wake_up_discard_thread(sbi, true);
			}
		} else if (t == 2) {
			sbi->gc_mode = GC_URGENT_LOW;
		} else if (t == 3) {
			sbi->gc_mode = GC_URGENT_MID;
			if (sbi->gc_thread) {
				sbi->gc_thread->gc_wake = 1;
				wake_up_interruptible_all(
					&sbi->gc_thread->gc_wait_queue_head);
			}
		} else {
			return -EINVAL;
		}
		return count;
	}
	if (!strcmp(a->attr.name, "gc_idle")) {
		if (t == GC_IDLE_CB) {
			sbi->gc_mode = GC_IDLE_CB;
		} else if (t == GC_IDLE_GREEDY) {
			sbi->gc_mode = GC_IDLE_GREEDY;
		} else if (t == GC_IDLE_AT) {
			if (!sbi->am.atgc_enabled)
				return -EINVAL;
			sbi->gc_mode = GC_IDLE_AT;
		} else {
			sbi->gc_mode = GC_NORMAL;
		}
		return count;
	}

	if (!strcmp(a->attr.name, "gc_urgent_high_remaining")) {
		spin_lock(&sbi->gc_urgent_high_lock);
		sbi->gc_urgent_high_limited = t != 0;
		sbi->gc_urgent_high_remaining = t;
		spin_unlock(&sbi->gc_urgent_high_lock);

		return count;
	}

#ifdef CONFIG_F3FS_IOSTAT
	if (!strcmp(a->attr.name, "iostat_enable")) {
		sbi->iostat_enable = !!t;
		if (!sbi->iostat_enable)
			f3fs_reset_iostat(sbi);
		return count;
	}

	if (!strcmp(a->attr.name, "iostat_period_ms")) {
		if (t < MIN_IOSTAT_PERIOD_MS || t > MAX_IOSTAT_PERIOD_MS)
			return -EINVAL;
		spin_lock(&sbi->iostat_lock);
		sbi->iostat_period_ms = (unsigned int)t;
		spin_unlock(&sbi->iostat_lock);
		return count;
	}
#endif

#ifdef CONFIG_F3FS_FS_COMPRESSION
	if (!strcmp(a->attr.name, "compr_written_block") ||
		!strcmp(a->attr.name, "compr_saved_block")) {
		if (t != 0)
			return -EINVAL;
		sbi->compr_written_block = 0;
		sbi->compr_saved_block = 0;
		return count;
	}

	if (!strcmp(a->attr.name, "compr_new_inode")) {
		if (t != 0)
			return -EINVAL;
		sbi->compr_new_inode = 0;
		return count;
	}
#endif

	if (!strcmp(a->attr.name, "atgc_candidate_ratio")) {
		if (t > 100)
			return -EINVAL;
		sbi->am.candidate_ratio = t;
		return count;
	}

	if (!strcmp(a->attr.name, "atgc_age_weight")) {
		if (t > 100)
			return -EINVAL;
		sbi->am.age_weight = t;
		return count;
	}

	if (!strcmp(a->attr.name, "gc_segment_mode")) {
		if (t < MAX_GC_MODE)
			sbi->gc_segment_mode = t;
		else
			return -EINVAL;
		return count;
	}

	if (!strcmp(a->attr.name, "gc_reclaimed_segments")) {
		if (t != 0)
			return -EINVAL;
		sbi->gc_reclaimed_segs[sbi->gc_segment_mode] = 0;
		return count;
	}

	if (!strcmp(a->attr.name, "seq_file_ra_mul")) {
		if (t >= MIN_RA_MUL && t <= MAX_RA_MUL)
			sbi->seq_file_ra_mul = t;
		else
			return -EINVAL;
		return count;
	}

	if (!strcmp(a->attr.name, "max_fragment_chunk")) {
		if (t >= MIN_FRAGMENT_SIZE && t <= MAX_FRAGMENT_SIZE)
			sbi->max_fragment_chunk = t;
		else
			return -EINVAL;
		return count;
	}

	if (!strcmp(a->attr.name, "max_fragment_hole")) {
		if (t >= MIN_FRAGMENT_SIZE && t <= MAX_FRAGMENT_SIZE)
			sbi->max_fragment_hole = t;
		else
			return -EINVAL;
		return count;
	}

	if (!strcmp(a->attr.name, "peak_atomic_write")) {
		if (t != 0)
			return -EINVAL;
		sbi->peak_atomic_write = 0;
		return count;
	}

	if (!strcmp(a->attr.name, "committed_atomic_block")) {
		if (t != 0)
			return -EINVAL;
		sbi->committed_atomic_block = 0;
		return count;
	}

	if (!strcmp(a->attr.name, "revoked_atomic_block")) {
		if (t != 0)
			return -EINVAL;
		sbi->revoked_atomic_block = 0;
		return count;
	}

	*ui = (unsigned int)t;

	return count;
}

static ssize_t f3fs_sbi_store(struct f3fs_attr *a,
			struct f3fs_sb_info *sbi,
			const char *buf, size_t count)
{
	ssize_t ret;
	bool gc_entry = (!strcmp(a->attr.name, "gc_urgent") ||
					a->struct_type == GC_THREAD);

	if (gc_entry) {
		if (!down_read_trylock(&sbi->sb->s_umount))
			return -EAGAIN;
	}
	ret = __sbi_store(a, sbi, buf, count);
	if (gc_entry)
		up_read(&sbi->sb->s_umount);

	return ret;
}

static ssize_t f3fs_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
								s_kobj);
	struct f3fs_attr *a = container_of(attr, struct f3fs_attr, attr);

	return a->show ? a->show(a, sbi, buf) : 0;
}

static ssize_t f3fs_attr_store(struct kobject *kobj, struct attribute *attr,
						const char *buf, size_t len)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
									s_kobj);
	struct f3fs_attr *a = container_of(attr, struct f3fs_attr, attr);

	return a->store ? a->store(a, sbi, buf, len) : 0;
}

static void f3fs_sb_release(struct kobject *kobj)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
								s_kobj);
	complete(&sbi->s_kobj_unregister);
}

/*
 * Note that there are three feature list entries:
 * 1) /sys/fs/f3fs/features
 *   : shows runtime features supported by in-kernel f3fs along with Kconfig.
 *     - ref. F3FS_FEATURE_RO_ATTR()
 *
 * 2) /sys/fs/f3fs/$s_id/features <deprecated>
 *   : shows on-disk features enabled by mkfs.f3fs, used for old kernels. This
 *     won't add new feature anymore, and thus, users should check entries in 3)
 *     instead of this 2).
 *
 * 3) /sys/fs/f3fs/$s_id/feature_list
 *   : shows on-disk features enabled by mkfs.f3fs per instance, which follows
 *     sysfs entry rule where each entry should expose single value.
 *     This list covers old feature list provided by 2) and beyond. Therefore,
 *     please add new on-disk feature in this list only.
 *     - ref. F3FS_SB_FEATURE_RO_ATTR()
 */
static ssize_t f3fs_feature_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "supported\n");
}

#define F3FS_FEATURE_RO_ATTR(_name)				\
static struct f3fs_attr f3fs_attr_##_name = {			\
	.attr = {.name = __stringify(_name), .mode = 0444 },	\
	.show	= f3fs_feature_show,				\
}

static ssize_t f3fs_sb_feature_show(struct f3fs_attr *a,
		struct f3fs_sb_info *sbi, char *buf)
{
	if (F3FS_HAS_FEATURE(sbi, a->id))
		return sprintf(buf, "supported\n");
	return sprintf(buf, "unsupported\n");
}

#define F3FS_SB_FEATURE_RO_ATTR(_name, _feat)			\
static struct f3fs_attr f3fs_attr_sb_##_name = {		\
	.attr = {.name = __stringify(_name), .mode = 0444 },	\
	.show	= f3fs_sb_feature_show,				\
	.id	= F3FS_FEATURE_##_feat,				\
}

#define F3FS_ATTR_OFFSET(_struct_type, _name, _mode, _show, _store, _offset) \
static struct f3fs_attr f3fs_attr_##_name = {			\
	.attr = {.name = __stringify(_name), .mode = _mode },	\
	.show	= _show,					\
	.store	= _store,					\
	.struct_type = _struct_type,				\
	.offset = _offset					\
}

#define F3FS_RO_ATTR(struct_type, struct_name, name, elname)	\
	F3FS_ATTR_OFFSET(struct_type, name, 0444,		\
		f3fs_sbi_show, NULL,				\
		offsetof(struct struct_name, elname))

#define F3FS_RW_ATTR(struct_type, struct_name, name, elname)	\
	F3FS_ATTR_OFFSET(struct_type, name, 0644,		\
		f3fs_sbi_show, f3fs_sbi_store,			\
		offsetof(struct struct_name, elname))

#define F3FS_GENERAL_RO_ATTR(name) \
static struct f3fs_attr f3fs_attr_##name = __ATTR(name, 0444, name##_show, NULL)

#define F3FS_STAT_ATTR(_struct_type, _struct_name, _name, _elname)	\
static struct f3fs_attr f3fs_attr_##_name = {			\
	.attr = {.name = __stringify(_name), .mode = 0444 },	\
	.show = f3fs_sbi_show,					\
	.struct_type = _struct_type,				\
	.offset = offsetof(struct _struct_name, _elname),       \
}

F3FS_RW_ATTR(GC_THREAD, f3fs_gc_kthread, gc_urgent_sleep_time,
							urgent_sleep_time);
F3FS_RW_ATTR(GC_THREAD, f3fs_gc_kthread, gc_min_sleep_time, min_sleep_time);
F3FS_RW_ATTR(GC_THREAD, f3fs_gc_kthread, gc_max_sleep_time, max_sleep_time);
F3FS_RW_ATTR(GC_THREAD, f3fs_gc_kthread, gc_no_gc_sleep_time, no_gc_sleep_time);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, gc_idle, gc_mode);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, gc_urgent, gc_mode);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, reclaim_segments, rec_prefree_segments);
F3FS_RW_ATTR(DCC_INFO, discard_cmd_control, max_small_discards, max_discards);
F3FS_RW_ATTR(DCC_INFO, discard_cmd_control, max_discard_request, max_discard_request);
F3FS_RW_ATTR(DCC_INFO, discard_cmd_control, min_discard_issue_time, min_discard_issue_time);
F3FS_RW_ATTR(DCC_INFO, discard_cmd_control, mid_discard_issue_time, mid_discard_issue_time);
F3FS_RW_ATTR(DCC_INFO, discard_cmd_control, max_discard_issue_time, max_discard_issue_time);
F3FS_RW_ATTR(DCC_INFO, discard_cmd_control, discard_granularity, discard_granularity);
F3FS_RW_ATTR(RESERVED_BLOCKS, f3fs_sb_info, reserved_blocks, reserved_blocks);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, batched_trim_sections, trim_sections);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, ipu_policy, ipu_policy);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, min_ipu_util, min_ipu_util);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, min_fsync_blocks, min_fsync_blocks);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, min_seq_blocks, min_seq_blocks);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, min_hot_blocks, min_hot_blocks);
F3FS_RW_ATTR(SM_INFO, f3fs_sm_info, min_ssr_sections, min_ssr_sections);
F3FS_RW_ATTR(NM_INFO, f3fs_nm_info, ram_thresh, ram_thresh);
F3FS_RW_ATTR(NM_INFO, f3fs_nm_info, ra_nid_pages, ra_nid_pages);
F3FS_RW_ATTR(NM_INFO, f3fs_nm_info, dirty_nats_ratio, dirty_nats_ratio);
F3FS_RW_ATTR(NM_INFO, f3fs_nm_info, max_roll_forward_node_blocks, max_rf_node_blocks);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, max_victim_search, max_victim_search);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, migration_granularity, migration_granularity);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, dir_level, dir_level);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, cp_interval, interval_time[CP_TIME]);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, idle_interval, interval_time[REQ_TIME]);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, discard_idle_interval,
					interval_time[DISCARD_TIME]);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, gc_idle_interval, interval_time[GC_TIME]);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info,
		umount_discard_timeout, interval_time[UMOUNT_DISCARD_TIMEOUT]);
#ifdef CONFIG_F3FS_IOSTAT
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, iostat_enable, iostat_enable);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, iostat_period_ms, iostat_period_ms);
#endif
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, readdir_ra, readdir_ra);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, max_io_bytes, max_io_bytes);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, gc_pin_file_thresh, gc_pin_file_threshold);
F3FS_RW_ATTR(F3FS_SBI, f3fs_super_block, extension_list, extension_list);
#ifdef CONFIG_F3FS_FAULT_INJECTION
F3FS_RW_ATTR(FAULT_INFO_RATE, f3fs_fault_info, inject_rate, inject_rate);
F3FS_RW_ATTR(FAULT_INFO_TYPE, f3fs_fault_info, inject_type, inject_type);
#endif
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, data_io_flag, data_io_flag);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, node_io_flag, node_io_flag);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, gc_urgent_high_remaining, gc_urgent_high_remaining);
F3FS_RW_ATTR(CPRC_INFO, ckpt_req_control, ckpt_thread_ioprio, ckpt_thread_ioprio);
F3FS_GENERAL_RO_ATTR(dirty_segments);
F3FS_GENERAL_RO_ATTR(free_segments);
F3FS_GENERAL_RO_ATTR(ovp_segments);
F3FS_GENERAL_RO_ATTR(lifetime_write_kbytes);
F3FS_GENERAL_RO_ATTR(features);
F3FS_GENERAL_RO_ATTR(current_reserved_blocks);
F3FS_GENERAL_RO_ATTR(unusable);
F3FS_GENERAL_RO_ATTR(encoding);
F3FS_GENERAL_RO_ATTR(mounted_time_sec);
F3FS_GENERAL_RO_ATTR(main_blkaddr);
F3FS_GENERAL_RO_ATTR(pending_discard);
#ifdef CONFIG_F3FS_STAT_FS
F3FS_STAT_ATTR(STAT_INFO, f3fs_stat_info, cp_foreground_calls, cp_count);
F3FS_STAT_ATTR(STAT_INFO, f3fs_stat_info, cp_background_calls, bg_cp_count);
F3FS_STAT_ATTR(STAT_INFO, f3fs_stat_info, gc_foreground_calls, call_count);
F3FS_STAT_ATTR(STAT_INFO, f3fs_stat_info, gc_background_calls, bg_gc);
F3FS_GENERAL_RO_ATTR(moved_blocks_background);
F3FS_GENERAL_RO_ATTR(moved_blocks_foreground);
F3FS_GENERAL_RO_ATTR(avg_vblocks);
#endif

#ifdef CONFIG_FS_ENCRYPTION
F3FS_FEATURE_RO_ATTR(encryption);
F3FS_FEATURE_RO_ATTR(test_dummy_encryption_v2);
#if IS_ENABLED(CONFIG_UNICODE)
F3FS_FEATURE_RO_ATTR(encrypted_casefold);
#endif
#endif /* CONFIG_FS_ENCRYPTION */
#ifdef CONFIG_BLK_DEV_ZONED
F3FS_FEATURE_RO_ATTR(block_zoned);
F3FS_RO_ATTR(F3FS_SBI, f3fs_sb_info, unusable_blocks_per_sec,
					unusable_blocks_per_sec);
#endif
F3FS_FEATURE_RO_ATTR(atomic_write);
F3FS_FEATURE_RO_ATTR(extra_attr);
F3FS_FEATURE_RO_ATTR(project_quota);
F3FS_FEATURE_RO_ATTR(inode_checksum);
F3FS_FEATURE_RO_ATTR(flexible_inline_xattr);
F3FS_FEATURE_RO_ATTR(quota_ino);
F3FS_FEATURE_RO_ATTR(inode_crtime);
F3FS_FEATURE_RO_ATTR(lost_found);
#ifdef CONFIG_FS_VERITY
F3FS_FEATURE_RO_ATTR(verity);
#endif
F3FS_FEATURE_RO_ATTR(sb_checksum);
#if IS_ENABLED(CONFIG_UNICODE)
F3FS_FEATURE_RO_ATTR(casefold);
#endif
F3FS_FEATURE_RO_ATTR(readonly);
#ifdef CONFIG_F3FS_FS_COMPRESSION
F3FS_FEATURE_RO_ATTR(compression);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, compr_written_block, compr_written_block);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, compr_saved_block, compr_saved_block);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, compr_new_inode, compr_new_inode);
#endif
F3FS_FEATURE_RO_ATTR(pin_file);

/* For ATGC */
F3FS_RW_ATTR(ATGC_INFO, atgc_management, atgc_candidate_ratio, candidate_ratio);
F3FS_RW_ATTR(ATGC_INFO, atgc_management, atgc_candidate_count, max_candidate_count);
F3FS_RW_ATTR(ATGC_INFO, atgc_management, atgc_age_weight, age_weight);
F3FS_RW_ATTR(ATGC_INFO, atgc_management, atgc_age_threshold, age_threshold);

F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, seq_file_ra_mul, seq_file_ra_mul);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, gc_segment_mode, gc_segment_mode);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, gc_reclaimed_segments, gc_reclaimed_segs);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, max_fragment_chunk, max_fragment_chunk);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, max_fragment_hole, max_fragment_hole);

/* For atomic write */
F3FS_RO_ATTR(F3FS_SBI, f3fs_sb_info, current_atomic_write, current_atomic_write);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, peak_atomic_write, peak_atomic_write);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, committed_atomic_block, committed_atomic_block);
F3FS_RW_ATTR(F3FS_SBI, f3fs_sb_info, revoked_atomic_block, revoked_atomic_block);

#define ATTR_LIST(name) (&f3fs_attr_##name.attr)
static struct attribute *f3fs_attrs[] = {
	ATTR_LIST(gc_urgent_sleep_time),
	ATTR_LIST(gc_min_sleep_time),
	ATTR_LIST(gc_max_sleep_time),
	ATTR_LIST(gc_no_gc_sleep_time),
	ATTR_LIST(gc_idle),
	ATTR_LIST(gc_urgent),
	ATTR_LIST(reclaim_segments),
	ATTR_LIST(main_blkaddr),
	ATTR_LIST(max_small_discards),
	ATTR_LIST(max_discard_request),
	ATTR_LIST(min_discard_issue_time),
	ATTR_LIST(mid_discard_issue_time),
	ATTR_LIST(max_discard_issue_time),
	ATTR_LIST(discard_granularity),
	ATTR_LIST(pending_discard),
	ATTR_LIST(batched_trim_sections),
	ATTR_LIST(ipu_policy),
	ATTR_LIST(min_ipu_util),
	ATTR_LIST(min_fsync_blocks),
	ATTR_LIST(min_seq_blocks),
	ATTR_LIST(min_hot_blocks),
	ATTR_LIST(min_ssr_sections),
	ATTR_LIST(max_victim_search),
	ATTR_LIST(migration_granularity),
	ATTR_LIST(dir_level),
	ATTR_LIST(ram_thresh),
	ATTR_LIST(ra_nid_pages),
	ATTR_LIST(dirty_nats_ratio),
	ATTR_LIST(max_roll_forward_node_blocks),
	ATTR_LIST(cp_interval),
	ATTR_LIST(idle_interval),
	ATTR_LIST(discard_idle_interval),
	ATTR_LIST(gc_idle_interval),
	ATTR_LIST(umount_discard_timeout),
#ifdef CONFIG_F3FS_IOSTAT
	ATTR_LIST(iostat_enable),
	ATTR_LIST(iostat_period_ms),
#endif
	ATTR_LIST(readdir_ra),
	ATTR_LIST(max_io_bytes),
	ATTR_LIST(gc_pin_file_thresh),
	ATTR_LIST(extension_list),
#ifdef CONFIG_F3FS_FAULT_INJECTION
	ATTR_LIST(inject_rate),
	ATTR_LIST(inject_type),
#endif
	ATTR_LIST(data_io_flag),
	ATTR_LIST(node_io_flag),
	ATTR_LIST(gc_urgent_high_remaining),
	ATTR_LIST(ckpt_thread_ioprio),
	ATTR_LIST(dirty_segments),
	ATTR_LIST(free_segments),
	ATTR_LIST(ovp_segments),
	ATTR_LIST(unusable),
	ATTR_LIST(lifetime_write_kbytes),
	ATTR_LIST(features),
	ATTR_LIST(reserved_blocks),
	ATTR_LIST(current_reserved_blocks),
	ATTR_LIST(encoding),
	ATTR_LIST(mounted_time_sec),
#ifdef CONFIG_F3FS_STAT_FS
	ATTR_LIST(cp_foreground_calls),
	ATTR_LIST(cp_background_calls),
	ATTR_LIST(gc_foreground_calls),
	ATTR_LIST(gc_background_calls),
	ATTR_LIST(moved_blocks_foreground),
	ATTR_LIST(moved_blocks_background),
	ATTR_LIST(avg_vblocks),
#endif
#ifdef CONFIG_BLK_DEV_ZONED
	ATTR_LIST(unusable_blocks_per_sec),
#endif
#ifdef CONFIG_F3FS_FS_COMPRESSION
	ATTR_LIST(compr_written_block),
	ATTR_LIST(compr_saved_block),
	ATTR_LIST(compr_new_inode),
#endif
	/* For ATGC */
	ATTR_LIST(atgc_candidate_ratio),
	ATTR_LIST(atgc_candidate_count),
	ATTR_LIST(atgc_age_weight),
	ATTR_LIST(atgc_age_threshold),
	ATTR_LIST(seq_file_ra_mul),
	ATTR_LIST(gc_segment_mode),
	ATTR_LIST(gc_reclaimed_segments),
	ATTR_LIST(max_fragment_chunk),
	ATTR_LIST(max_fragment_hole),
	ATTR_LIST(current_atomic_write),
	ATTR_LIST(peak_atomic_write),
	ATTR_LIST(committed_atomic_block),
	ATTR_LIST(revoked_atomic_block),
	NULL,
};
ATTRIBUTE_GROUPS(f3fs);

static struct attribute *f3fs_feat_attrs[] = {
#ifdef CONFIG_FS_ENCRYPTION
	ATTR_LIST(encryption),
	ATTR_LIST(test_dummy_encryption_v2),
#if IS_ENABLED(CONFIG_UNICODE)
	ATTR_LIST(encrypted_casefold),
#endif
#endif /* CONFIG_FS_ENCRYPTION */
#ifdef CONFIG_BLK_DEV_ZONED
	ATTR_LIST(block_zoned),
#endif
	ATTR_LIST(atomic_write),
	ATTR_LIST(extra_attr),
	ATTR_LIST(project_quota),
	ATTR_LIST(inode_checksum),
	ATTR_LIST(flexible_inline_xattr),
	ATTR_LIST(quota_ino),
	ATTR_LIST(inode_crtime),
	ATTR_LIST(lost_found),
#ifdef CONFIG_FS_VERITY
	ATTR_LIST(verity),
#endif
	ATTR_LIST(sb_checksum),
#if IS_ENABLED(CONFIG_UNICODE)
	ATTR_LIST(casefold),
#endif
	ATTR_LIST(readonly),
#ifdef CONFIG_F3FS_FS_COMPRESSION
	ATTR_LIST(compression),
#endif
	ATTR_LIST(pin_file),
	NULL,
};
ATTRIBUTE_GROUPS(f3fs_feat);

F3FS_GENERAL_RO_ATTR(sb_status);
static struct attribute *f3fs_stat_attrs[] = {
	ATTR_LIST(sb_status),
	NULL,
};
ATTRIBUTE_GROUPS(f3fs_stat);

F3FS_SB_FEATURE_RO_ATTR(encryption, ENCRYPT);
F3FS_SB_FEATURE_RO_ATTR(block_zoned, BLKZONED);
F3FS_SB_FEATURE_RO_ATTR(extra_attr, EXTRA_ATTR);
F3FS_SB_FEATURE_RO_ATTR(project_quota, PRJQUOTA);
F3FS_SB_FEATURE_RO_ATTR(inode_checksum, INODE_CHKSUM);
F3FS_SB_FEATURE_RO_ATTR(flexible_inline_xattr, FLEXIBLE_INLINE_XATTR);
F3FS_SB_FEATURE_RO_ATTR(quota_ino, QUOTA_INO);
F3FS_SB_FEATURE_RO_ATTR(inode_crtime, INODE_CRTIME);
F3FS_SB_FEATURE_RO_ATTR(lost_found, LOST_FOUND);
F3FS_SB_FEATURE_RO_ATTR(verity, VERITY);
F3FS_SB_FEATURE_RO_ATTR(sb_checksum, SB_CHKSUM);
F3FS_SB_FEATURE_RO_ATTR(casefold, CASEFOLD);
F3FS_SB_FEATURE_RO_ATTR(compression, COMPRESSION);
F3FS_SB_FEATURE_RO_ATTR(readonly, RO);

static struct attribute *f3fs_sb_feat_attrs[] = {
	ATTR_LIST(sb_encryption),
	ATTR_LIST(sb_block_zoned),
	ATTR_LIST(sb_extra_attr),
	ATTR_LIST(sb_project_quota),
	ATTR_LIST(sb_inode_checksum),
	ATTR_LIST(sb_flexible_inline_xattr),
	ATTR_LIST(sb_quota_ino),
	ATTR_LIST(sb_inode_crtime),
	ATTR_LIST(sb_lost_found),
	ATTR_LIST(sb_verity),
	ATTR_LIST(sb_sb_checksum),
	ATTR_LIST(sb_casefold),
	ATTR_LIST(sb_compression),
	ATTR_LIST(sb_readonly),
	NULL,
};
ATTRIBUTE_GROUPS(f3fs_sb_feat);

static const struct sysfs_ops f3fs_attr_ops = {
	.show	= f3fs_attr_show,
	.store	= f3fs_attr_store,
};

static struct kobj_type f3fs_sb_ktype = {
	.default_groups = f3fs_groups,
	.sysfs_ops	= &f3fs_attr_ops,
	.release	= f3fs_sb_release,
};

static struct kobj_type f3fs_ktype = {
	.sysfs_ops	= &f3fs_attr_ops,
};

static struct kset f3fs_kset = {
	.kobj	= {.ktype = &f3fs_ktype},
};

static struct kobj_type f3fs_feat_ktype = {
	.default_groups = f3fs_feat_groups,
	.sysfs_ops	= &f3fs_attr_ops,
};

static struct kobject f3fs_feat = {
	.kset	= &f3fs_kset,
};

static ssize_t f3fs_stat_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
								s_stat_kobj);
	struct f3fs_attr *a = container_of(attr, struct f3fs_attr, attr);

	return a->show ? a->show(a, sbi, buf) : 0;
}

static ssize_t f3fs_stat_attr_store(struct kobject *kobj, struct attribute *attr,
						const char *buf, size_t len)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
								s_stat_kobj);
	struct f3fs_attr *a = container_of(attr, struct f3fs_attr, attr);

	return a->store ? a->store(a, sbi, buf, len) : 0;
}

static void f3fs_stat_kobj_release(struct kobject *kobj)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
								s_stat_kobj);
	complete(&sbi->s_stat_kobj_unregister);
}

static const struct sysfs_ops f3fs_stat_attr_ops = {
	.show	= f3fs_stat_attr_show,
	.store	= f3fs_stat_attr_store,
};

static struct kobj_type f3fs_stat_ktype = {
	.default_groups = f3fs_stat_groups,
	.sysfs_ops	= &f3fs_stat_attr_ops,
	.release	= f3fs_stat_kobj_release,
};

static ssize_t f3fs_sb_feat_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
							s_feature_list_kobj);
	struct f3fs_attr *a = container_of(attr, struct f3fs_attr, attr);

	return a->show ? a->show(a, sbi, buf) : 0;
}

static void f3fs_feature_list_kobj_release(struct kobject *kobj)
{
	struct f3fs_sb_info *sbi = container_of(kobj, struct f3fs_sb_info,
							s_feature_list_kobj);
	complete(&sbi->s_feature_list_kobj_unregister);
}

static const struct sysfs_ops f3fs_feature_list_attr_ops = {
	.show	= f3fs_sb_feat_attr_show,
};

static struct kobj_type f3fs_feature_list_ktype = {
	.default_groups = f3fs_sb_feat_groups,
	.sysfs_ops	= &f3fs_feature_list_attr_ops,
	.release	= f3fs_feature_list_kobj_release,
};

static int __maybe_unused segment_info_seq_show(struct seq_file *seq,
						void *offset)
{
	struct super_block *sb = seq->private;
	struct f3fs_sb_info *sbi = F3FS_SB(sb);
	unsigned int total_segs =
			le32_to_cpu(sbi->raw_super->segment_count_main);
	int i;

	seq_puts(seq, "format: segment_type|valid_blocks\n"
		"segment_type(0:HD, 1:WD, 2:CD, 3:HN, 4:WN, 5:CN)\n");

	for (i = 0; i < total_segs; i++) {
		struct seg_entry *se = get_seg_entry(sbi, i);

		if ((i % 10) == 0)
			seq_printf(seq, "%-10d", i);
		seq_printf(seq, "%d|%-3u", se->type, se->valid_blocks);
		if ((i % 10) == 9 || i == (total_segs - 1))
			seq_putc(seq, '\n');
		else
			seq_putc(seq, ' ');
	}

	return 0;
}

static int __maybe_unused segment_bits_seq_show(struct seq_file *seq,
						void *offset)
{
	struct super_block *sb = seq->private;
	struct f3fs_sb_info *sbi = F3FS_SB(sb);
	unsigned int total_segs =
			le32_to_cpu(sbi->raw_super->segment_count_main);
	int i, j;

	seq_puts(seq, "format: segment_type|valid_blocks|bitmaps\n"
		"segment_type(0:HD, 1:WD, 2:CD, 3:HN, 4:WN, 5:CN)\n");

	for (i = 0; i < total_segs; i++) {
		struct seg_entry *se = get_seg_entry(sbi, i);

		seq_printf(seq, "%-10d", i);
		seq_printf(seq, "%d|%-3u|", se->type, se->valid_blocks);
		for (j = 0; j < SIT_VBLOCK_MAP_SIZE; j++)
			seq_printf(seq, " %.2x", se->cur_valid_map[j]);
		seq_putc(seq, '\n');
	}
	return 0;
}

static int __maybe_unused victim_bits_seq_show(struct seq_file *seq,
						void *offset)
{
	struct super_block *sb = seq->private;
	struct f3fs_sb_info *sbi = F3FS_SB(sb);
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	int i;

	seq_puts(seq, "format: victim_secmap bitmaps\n");

	for (i = 0; i < MAIN_SECS(sbi); i++) {
		if ((i % 10) == 0)
			seq_printf(seq, "%-10d", i);
		seq_printf(seq, "%d", test_bit(i, dirty_i->victim_secmap) ? 1 : 0);
		if ((i % 10) == 9 || i == (MAIN_SECS(sbi) - 1))
			seq_putc(seq, '\n');
		else
			seq_putc(seq, ' ');
	}
	return 0;
}

int __init f3fs_init_sysfs(void)
{
	int ret;

	kobject_set_name(&f3fs_kset.kobj, "f3fs");
	f3fs_kset.kobj.parent = fs_kobj;
	ret = kset_register(&f3fs_kset);
	if (ret)
		return ret;

	ret = kobject_init_and_add(&f3fs_feat, &f3fs_feat_ktype,
				   NULL, "features");
	if (ret) {
		kobject_put(&f3fs_feat);
		kset_unregister(&f3fs_kset);
	} else {
		f3fs_proc_root = proc_mkdir("fs/f3fs", NULL);
	}
	return ret;
}

void f3fs_exit_sysfs(void)
{
	kobject_put(&f3fs_feat);
	kset_unregister(&f3fs_kset);
	remove_proc_entry("fs/f3fs", NULL);
	f3fs_proc_root = NULL;
}

int f3fs_register_sysfs(struct f3fs_sb_info *sbi)
{
	struct super_block *sb = sbi->sb;
	int err;

	sbi->s_kobj.kset = &f3fs_kset;
	init_completion(&sbi->s_kobj_unregister);
	err = kobject_init_and_add(&sbi->s_kobj, &f3fs_sb_ktype, NULL,
				"%s", sb->s_id);
	if (err)
		goto put_sb_kobj;

	sbi->s_stat_kobj.kset = &f3fs_kset;
	init_completion(&sbi->s_stat_kobj_unregister);
	err = kobject_init_and_add(&sbi->s_stat_kobj, &f3fs_stat_ktype,
						&sbi->s_kobj, "stat");
	if (err)
		goto put_stat_kobj;

	sbi->s_feature_list_kobj.kset = &f3fs_kset;
	init_completion(&sbi->s_feature_list_kobj_unregister);
	err = kobject_init_and_add(&sbi->s_feature_list_kobj,
					&f3fs_feature_list_ktype,
					&sbi->s_kobj, "feature_list");
	if (err)
		goto put_feature_list_kobj;

	if (f3fs_proc_root)
		sbi->s_proc = proc_mkdir(sb->s_id, f3fs_proc_root);

	if (sbi->s_proc) {
		proc_create_single_data("segment_info", 0444, sbi->s_proc,
				segment_info_seq_show, sb);
		proc_create_single_data("segment_bits", 0444, sbi->s_proc,
				segment_bits_seq_show, sb);
#ifdef CONFIG_F3FS_IOSTAT
		proc_create_single_data("iostat_info", 0444, sbi->s_proc,
				iostat_info_seq_show, sb);
#endif
		proc_create_single_data("victim_bits", 0444, sbi->s_proc,
				victim_bits_seq_show, sb);
	}
	return 0;
put_feature_list_kobj:
	kobject_put(&sbi->s_feature_list_kobj);
	wait_for_completion(&sbi->s_feature_list_kobj_unregister);
put_stat_kobj:
	kobject_put(&sbi->s_stat_kobj);
	wait_for_completion(&sbi->s_stat_kobj_unregister);
put_sb_kobj:
	kobject_put(&sbi->s_kobj);
	wait_for_completion(&sbi->s_kobj_unregister);
	return err;
}

void f3fs_unregister_sysfs(struct f3fs_sb_info *sbi)
{
	if (sbi->s_proc) {
#ifdef CONFIG_F3FS_IOSTAT
		remove_proc_entry("iostat_info", sbi->s_proc);
#endif
		remove_proc_entry("segment_info", sbi->s_proc);
		remove_proc_entry("segment_bits", sbi->s_proc);
		remove_proc_entry("victim_bits", sbi->s_proc);
		remove_proc_entry(sbi->sb->s_id, f3fs_proc_root);
	}

	kobject_del(&sbi->s_stat_kobj);
	kobject_put(&sbi->s_stat_kobj);
	wait_for_completion(&sbi->s_stat_kobj_unregister);
	kobject_del(&sbi->s_feature_list_kobj);
	kobject_put(&sbi->s_feature_list_kobj);
	wait_for_completion(&sbi->s_feature_list_kobj_unregister);

	kobject_del(&sbi->s_kobj);
	kobject_put(&sbi->s_kobj);
	wait_for_completion(&sbi->s_kobj_unregister);
}
