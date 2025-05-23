// SPDX-License-Identifier: GPL-2.0
/*
 * f3fs shrinker support
 *   the basic infra was copied from fs/ubifs/shrinker.c
 *
 * Copyright (c) 2015 Motorola Mobility
 * Copyright (c) 2015 Jaegeuk Kim <jaegeuk@kernel.org>
 */
#include <linux/fs.h>
#include <linux/f3fs_fs.h>

#include "f3fs.h"
#include "node.h"

static LIST_HEAD(f3fs_list);
static DEFINE_SPINLOCK(f3fs_list_lock);
static unsigned int shrinker_run_no;

static unsigned long __count_nat_entries(struct f3fs_sb_info *sbi)
{
	return NM_I(sbi)->nat_cnt[RECLAIMABLE_NAT];
}

static unsigned long __count_free_nids(struct f3fs_sb_info *sbi)
{
	long count = NM_I(sbi)->nid_cnt[FREE_NID] - MAX_FREE_NIDS;

	return count > 0 ? count : 0;
}

static unsigned long __count_extent_cache(struct f3fs_sb_info *sbi)
{
	return atomic_read(&sbi->total_zombie_tree) +
				atomic_read(&sbi->total_ext_node);
}

unsigned long f3fs_shrink_count(struct shrinker *shrink,
				struct shrink_control *sc)
{
	struct f3fs_sb_info *sbi;
	struct list_head *p;
	unsigned long count = 0;

	spin_lock(&f3fs_list_lock);
	p = f3fs_list.next;
	while (p != &f3fs_list) {
		sbi = list_entry(p, struct f3fs_sb_info, s_list);

		/* stop f3fs_put_super */
		if (!mutex_trylock(&sbi->umount_mutex)) {
			p = p->next;
			continue;
		}
		spin_unlock(&f3fs_list_lock);

		/* count extent cache entries */
		count += __count_extent_cache(sbi);

		/* count clean nat cache entries */
		count += __count_nat_entries(sbi);

		/* count free nids cache entries */
		count += __count_free_nids(sbi);

		spin_lock(&f3fs_list_lock);
		p = p->next;
		mutex_unlock(&sbi->umount_mutex);
	}
	spin_unlock(&f3fs_list_lock);
	return count;
}

unsigned long f3fs_shrink_scan(struct shrinker *shrink,
				struct shrink_control *sc)
{
	unsigned long nr = sc->nr_to_scan;
	struct f3fs_sb_info *sbi;
	struct list_head *p;
	unsigned int run_no;
	unsigned long freed = 0;

	spin_lock(&f3fs_list_lock);
	do {
		run_no = ++shrinker_run_no;
	} while (run_no == 0);
	p = f3fs_list.next;
	while (p != &f3fs_list) {
		sbi = list_entry(p, struct f3fs_sb_info, s_list);

		if (sbi->shrinker_run_no == run_no)
			break;

		/* stop f3fs_put_super */
		if (!mutex_trylock(&sbi->umount_mutex)) {
			p = p->next;
			continue;
		}
		spin_unlock(&f3fs_list_lock);

		sbi->shrinker_run_no = run_no;

		/* shrink extent cache entries */
		freed += f3fs_shrink_extent_tree(sbi, nr >> 1);

		/* shrink clean nat cache entries */
		if (freed < nr)
			freed += f3fs_try_to_free_nats(sbi, nr - freed);

		/* shrink free nids cache entries */
		if (freed < nr)
			freed += f3fs_try_to_free_nids(sbi, nr - freed);

		spin_lock(&f3fs_list_lock);
		p = p->next;
		list_move_tail(&sbi->s_list, &f3fs_list);
		mutex_unlock(&sbi->umount_mutex);
		if (freed >= nr)
			break;
	}
	spin_unlock(&f3fs_list_lock);
	return freed;
}

void f3fs_join_shrinker(struct f3fs_sb_info *sbi)
{
	spin_lock(&f3fs_list_lock);
	list_add_tail(&sbi->s_list, &f3fs_list);
	spin_unlock(&f3fs_list_lock);
}

void f3fs_leave_shrinker(struct f3fs_sb_info *sbi)
{
	f3fs_shrink_extent_tree(sbi, __count_extent_cache(sbi));

	spin_lock(&f3fs_list_lock);
	list_del_init(&sbi->s_list);
	spin_unlock(&f3fs_list_lock);
}
