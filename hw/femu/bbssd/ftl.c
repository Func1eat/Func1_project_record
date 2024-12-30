#include "ftl.h"

//#define FEMU_DEBUG_FTL
//#define FDP_DEBUG

static void *ftl_thread(void *arg);
static void output_info_log(struct ssd *ssd);

// 获取ruh的模式为slc还是qlc
static inline int get_ruh_mode(struct ssd *ssd, int ruhid) {
	return ssd->ruhtbl[ruhid].mode;
}

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline int should_fdp_gc(struct ssd *ssd, uint16_t rg) 
{																
 	int slc_flag =  (ssd->rums_slc[rg].free_ru_cnt <= ssd->sp.gc_thres_rus_slc);
	int qlc_flag =  (ssd->rums_qlc[rg].free_ru_cnt <= ssd->sp.gc_thres_rus_qlc);
	return (qlc_flag << 1) + slc_flag; 
}
static inline int should_fdp_gc_high(struct ssd *ssd, uint16_t rg)
{
	int slc_flag =  (ssd->rums_slc[rg].free_ru_cnt <= ssd->sp.gc_thres_rus_high_slc);
	int qlc_flag =  (ssd->rums_qlc[rg].free_ru_cnt <= ssd->sp.gc_thres_rus_high_qlc);
	if (slc_flag || qlc_flag)
		printf("gc_flag: %d %d\n", slc_flag, qlc_flag);
	return (qlc_flag << 1) + slc_flag; 
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

// 受害RU队列的比较定义
static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri(void *a)		
{
    return ((struct ru *)a)->vpc;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct ru *)a)->vpc = pri;
}

static inline size_t victim_ru_get_pos(void *a)
{
    return ((struct ru *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((struct ru *)a)->pos = pos;
}																	

// 每个rg一个rum来管理free、victim ru list等
static void ssd_init_fdp_ru_mgmts(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
    struct fdp_ru_mgmt *rum_slc = NULL, *rum_qlc = NULL;
    struct ru *ru = NULL;
	int nrg = spp->tt_luns / RG_DEGREE;
	
	ssd->rus =  g_malloc0(sizeof(struct ru) * spp->tt_rus);
	ssd->rums_slc = g_malloc(sizeof(struct fdp_ru_mgmt) * nrg);
	ssd->rums_qlc = g_malloc(sizeof(struct fdp_ru_mgmt) * nrg);

	for (int i = 0; i < nrg; i++) {
		rum_slc = &ssd->rums_slc[i];
		rum_qlc = &ssd->rums_qlc[i];


		// todo 单RG
		rum_slc->tt_rus = spp->slc_op * 1.0 / (spp->slc_op + spp->qlc_op) * spp->blks_per_pl;
		rum_qlc->tt_rus = spp->tt_rus - rum_slc->tt_rus;

		QTAILQ_INIT(&rum_slc->free_ru_list);
		QTAILQ_INIT(&rum_qlc->free_ru_list);
		
		// 固定大小SLC
		rum_slc->victim_ru_pq = pqueue_init(rum_slc->tt_rus, victim_ru_cmp_pri,
            victim_ru_get_pri, victim_ru_set_pri,
            victim_ru_get_pos, victim_ru_set_pos);

		rum_qlc->victim_ru_pq = pqueue_init(rum_qlc->tt_rus, victim_ru_cmp_pri,
            victim_ru_get_pri, victim_ru_set_pri,
            victim_ru_get_pos, victim_ru_set_pos);

		QTAILQ_INIT(&rum_slc->full_ru_list);
		QTAILQ_INIT(&rum_slc->bad_ru_list);
		QTAILQ_INIT(&rum_qlc->full_ru_list);
		QTAILQ_INIT(&rum_qlc->bad_ru_list);

		rum_slc->free_ru_cnt = 0;
		rum_qlc->free_ru_cnt = 0;
		for (int j = 0; j < rum_slc->tt_rus; j++) {
			ru = &ssd->rus[j];
			ru->id = j;
			ru->wp.ch = i * RG_DEGREE / spp->luns_per_ch;
			ru->wp.lun = i * RG_DEGREE % spp->luns_per_ch; 
			ru->wp.pl = 0;
			ru->wp.blk = j;
			ru->wp.pg = 0;
			ru->ipc = 0;
			ru->vpc = 0;
			ru->pos = 0;
			ru->erase_cnt = 0;
			ru->mode = 0;
			ru->rut = RU_TYPE_NORMAL;

			QTAILQ_INSERT_TAIL(&rum_slc->free_ru_list, ru, entry);
			rum_slc->free_ru_cnt++;
		}

		for (int j = 0; j < rum_qlc->tt_rus; j++) {
			ru = &ssd->rus[j + rum_slc->tt_rus];
			ru->id = j + rum_slc->tt_rus;
			ru->wp.ch = i * RG_DEGREE / spp->luns_per_ch;
			ru->wp.lun = i * RG_DEGREE % spp->luns_per_ch; 
			ru->wp.pl = 0;
			ru->wp.blk = j + rum_slc->tt_rus;
			ru->wp.pg = 0;
			ru->ipc = 0;
			ru->vpc = 0;
			ru->pos = 0;
			ru->erase_cnt = 0;
			ru->mode = 1;
			ru->rut = RU_TYPE_NORMAL;

			QTAILQ_INSERT_TAIL(&rum_qlc->free_ru_list, ru, entry);
			rum_qlc->free_ru_cnt++;
		}

		rum_slc->victim_ru_cnt = 0;
		rum_slc->full_ru_cnt = 0; 
		rum_slc->bad_ru_cnt = 0;
		rum_qlc->victim_ru_cnt = 0;
		rum_qlc->full_ru_cnt = 0; 
		rum_qlc->bad_ru_cnt = 0;
		printf("free_ru_cnt:%d %d\n", rum_slc->free_ru_cnt, rum_qlc->free_ru_cnt);
	}
	printf("finish\n"); 
}

// 从该rg的free_ru_list里取下一个
static int get_next_free_ruid(struct ssd *ssd, struct fdp_ru_mgmt *rum, int ruhid)
{
#ifdef FDP_DEBUG
	printf("get_next_free_ruid() called -> ");
#endif
	struct ru *retru = NULL;

	retru = QTAILQ_FIRST(&rum->free_ru_list);
	if (!retru) {
		output_info_log(ssd);
		ftl_err("SSD reaches its end of the life!\n");
		abort();
	}
	
	struct ru *ru_tmp = retru;
	int hottest = retru->erase_cnt;
	
	//热读
	// if (ruhid == 0) {
	// 	if (ssd->cv_moderate == 1) {
	// 		for (int i = 1; i < rum->free_ru_cnt; i++) {
	// 			retru = QTAILQ_NEXT(retru, entry);
	// 			if (retru->erase_cnt > hottest) {
	// 				hottest = retru->erase_cnt;
	// 				ru_tmp = retru;
	// 			} else if (retru->erase_cnt == hottest && retru->id < ru_tmp->id) {
	// 				ru_tmp = retru;
	// 			}
	// 		}
	// 	} // youngest block first
	// 	else {
	// 		for (int i = 1; i < rum->free_ru_cnt; i++) {
	// 			retru = QTAILQ_NEXT(retru, entry);
	// 			if (retru->erase_cnt < hottest) {
	// 				hottest = retru->erase_cnt;
	// 				ru_tmp = retru;
	// 			}
	// 		}
	// 	}
	// } else if (ruhid == 1) {
	// 	if (ssd->cv_moderate == 0) {
	// 		for (int i = 1; i < rum->free_ru_cnt; i++) {
	// 			retru = QTAILQ_NEXT(retru, entry);
	// 			if (retru->erase_cnt > hottest) {
	// 				hottest = retru->erase_cnt;
	// 				ru_tmp = retru;
	// 			} else if (retru->erase_cnt == hottest && retru->id < ru_tmp->id) {
	// 				ru_tmp = retru;
	// 			}
	// 		}
	// 	} // youngest block first
	// 	else {
	// 		for (int i = 1; i < rum->free_ru_cnt; i++) {
	// 			retru = QTAILQ_NEXT(retru, entry);
	// 			if (retru->erase_cnt < hottest) {
	// 				hottest = retru->erase_cnt;
	// 				ru_tmp = retru;
	// 			}
	// 		}
	// 	}
	// } else {
	// 	for (int i = 1; i < rum->free_ru_cnt; i++) {
	// 		retru = QTAILQ_NEXT(retru, entry);
	// 		if (retru->erase_cnt < hottest) {
	// 			hottest = retru->erase_cnt;
	// 			ru_tmp = retru;
	// 		}
	// 	}
	// }

	// 动态磨损均衡，优先选择最年轻的RU进行写入
  	for (int i = 1; i < rum->free_ru_cnt; i++) {
		retru = QTAILQ_NEXT(retru, entry);
		if (retru->erase_cnt < hottest) {
			hottest = retru->erase_cnt;
			ru_tmp = retru;
		}
	}

	QTAILQ_REMOVE(&rum->free_ru_list, ru_tmp, entry);
	rum->free_ru_cnt--;

	ftl_log("ru_id:%d ruhid:%d\n", ru_tmp->id, ruhid);
	return ru_tmp->id; 
}

// 初始化所有ruh
static void ssd_init_fdp_ruhtbl(struct FemuCtrl *n, struct ssd *ssd)
{
	NvmeEnduranceGroup *endgrp = &n->endgrps[0];
	struct ruh *ruh = NULL;
	struct fdp_ru_mgmt *rum_slc = NULL, *rum_qlc = NULL;

	ssd->fdp_enabled = n->bb_params.fdp_enabled;
	ssd->ruhtbl = g_malloc0(sizeof(struct ruh) * (endgrp->fdp.nruh)); 
	

	// 初始化ruh的mode
	for (int i = 0; i < endgrp->fdp.nruh; i++) {
		ruh = &ssd->ruhtbl[i];
		if (i == 0 || i == 1)
			ruh->mode = 0; // slc
		else
			ruh->mode = 1;
	}

	for (int i = 0; i < endgrp->fdp.nruh; i++) {
		ruh = &ssd->ruhtbl[i];
		ruh->ruht = NVME_RUHT_PERSISTENTLY_ISOLATED;
		// 当前指向的ru，每个rg一个
		ruh->cur_ruids = g_malloc0(sizeof(int) * endgrp->fdp.nrg);
		ruh->pi_gc_ruids = g_malloc0(sizeof(int) * endgrp->fdp.nrg);
		for (int j = 0; j < endgrp->fdp.nrg; j++)  {
			if (get_ruh_mode(ssd, i) == 0) {
				rum_slc = &ssd->rums_slc[j];
				ruh->cur_ruids[j] = get_next_free_ruid(ssd, rum_slc, i);
				ftl_log("init ruh %d cur_ruid[%d] = %d in slc\n", i, j, ruh->cur_ruids[j]);
			} else {
				rum_qlc = &ssd->rums_qlc[j];
				ruh->cur_ruids[j] = get_next_free_ruid(ssd, rum_qlc, i);
				ftl_log("init ruh %d cur_ruid[%d] = %d in qlc\n", i, j, ruh->cur_ruids[j]);
			}
		} 
	}
	
	// 每个rg指定一个ii_gc的ru，不需要同ruh
	// for (int i = 0; i < endgrp->fdp.nrg; i++) {
	// 	rum = &ssd->rums[i];
	// 	rum->ii_gc_ruid = get_next_free_ruid(ssd, rum);
	// 	rum->rus[rum->ii_gc_ruid].rut = RU_TYPE_II_GC;
	// }

	
	// 指定每个RUH的垃圾回收所在
	for (int i = 0; i < endgrp->fdp.nrg; i++) {
		int pi_gc_ruid;
		rum_slc = &ssd->rums_slc[i]; 
		rum_qlc = &ssd->rums_qlc[i]; 
		for (int j = 0; j < MAX_RUHS; j++) {
			if (get_ruh_mode(ssd, j) == 0) {
				pi_gc_ruid = get_next_free_ruid(ssd, rum_slc, j);
				ssd->ruhtbl[j].pi_gc_ruids[i] = pi_gc_ruid;
				ssd->rus[pi_gc_ruid].rut = RU_TYPE_PI_GC;
			} else {
				pi_gc_ruid = get_next_free_ruid(ssd, rum_qlc, j);
				ssd->ruhtbl[j].pi_gc_ruids[i] = pi_gc_ruid;
				ssd->rus[pi_gc_ruid].rut = RU_TYPE_PI_GC;
			}
		} 
	}
}																

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}
static void ssd_advance_fdp_write_pointer(struct ssd *ssd, uint16_t rgid, int lpn, uint16_t ruhid, bool for_gc, int mode)
{
	struct ssdparams *spp = &ssd->sp;
	struct fdp_ru_mgmt *rum = NULL;
	if (mode == 0) {
		rum = &ssd->rums_slc[rgid];
	} else {
		rum = &ssd->rums_qlc[rgid];
	}

	struct ruh *ruh = &ssd->ruhtbl[ruhid];
	int max_ch = (rgid + 1) * (RG_DEGREE / spp->luns_per_ch);
	int ruid;
	struct ru *ru = NULL;

	// 若当前是gc_write，则写入的ru是迁移后的ru
	if (for_gc) {
		if (ruh->ruht == NVME_RUHT_INITIALLY_ISOLATED) {
			ruid = rum->ii_gc_ruid; 
		}
		else if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
			ruid = ruh->pi_gc_ruids[rgid];
		}
		else {
			if (ssd->gc_cnt[lpn] == 0)
				ruid = ruh->pi_gc_ruids[rgid];
			else 
				ruid = rum->ii_gc_ruid;
		}
	}
	else
		ruid = ruh->cur_ruids[rgid];

	ru = &ssd->rus[ruid]; 
	check_addr(ru->wp.ch, max_ch);
	ru->wp.ch++;
	if (ru->wp.ch == max_ch) {
		ru->wp.ch = rgid * (RG_DEGREE / spp->luns_per_ch);
		check_addr(ru->wp.lun, spp->luns_per_ch);
		ru->wp.lun++;
		/* in this case, we should go to next lun */
		if (ru->wp.lun == spp->luns_per_ch) {
			ru->wp.lun = 0;
			/* go to next page in the block */
			check_addr(ru->wp.pg, spp->pgs_per_blk);
			// slc 每隔四个页写一下
			if (mode == 0)
            	ru->wp.pg += 4;
			else
				ru->wp.pg += 1;
			if (ru->wp.pg == spp->pgs_per_blk) {
				ru->wp.pg = 0;
				if (ru->vpc == spp->pgs_per_ru) {
					ftl_assert(ru->ipc == 0);
					QTAILQ_INSERT_TAIL(&rum->full_ru_list, ru, entry);
					rum->full_ru_cnt++;
				} else {
					ftl_assert(ru->vpc >= 0 && ru->vpc < spp->pgs_per_ru);
					ftl_assert(ru->ipc > 0);
					pqueue_insert(rum->victim_ru_pq, ru);
					rum->victim_ru_cnt++;
				}

				check_addr(ru->wp.blk, spp->blks_per_pl); 
				// 存下写入该ru的ruhid，方便后续找迁移的ru
				ru->ruhid = ruhid; 
				// 若当前是迁移后的ru写完了，则要新找一个存放迁移数据的ru
				if (ru->rut == RU_TYPE_II_GC) {
					rum->ii_gc_ruid = get_next_free_ruid(ssd, rum, ruhid);
					ssd->rus[rum->ii_gc_ruid].rut = RU_TYPE_II_GC;
				}
				else if (ru->rut == RU_TYPE_PI_GC) {
					ruh->pi_gc_ruids[rgid] = get_next_free_ruid(ssd, rum, ruhid);
					ssd->rus[ruh->pi_gc_ruids[rgid]].rut = RU_TYPE_PI_GC;
				}
				else {
					// 若是正常写的ru完了，就要更新ruh当前指向的ru
					ruh->cur_ruids[rgid] = get_next_free_ruid(ssd, rum, ruhid);
					ssd->rus[ruh->cur_ruids[rgid]].rut = RU_TYPE_NORMAL;
				} 
				check_addr(ru->wp.blk, spp->blks_per_pl);
				ftl_assert(ru->wp.pg == 0);
				ftl_assert(ru->wp.lun == 0);
				ftl_assert(ru->wp.ch == rgid * (RG_DEGREE / spp->luns_per_ch));
				ftl_assert(ru->wp.pl == 0);
			}
		}
	} 
}
static struct ppa fdp_get_new_page(struct ssd *ssd, uint16_t rgid, 
		int lpn, uint16_t ruhid, bool for_gc, int mode)
{
	struct fdp_ru_mgmt *rum = NULL;
	if (mode == 0)
		rum = &ssd->rums_slc[rgid];
	else
		rum = &ssd->rums_qlc[rgid];
	struct ruh* ruh = &ssd->ruhtbl[ruhid];
	int ruid;
	struct ru *ru;
    struct ppa ppa; 
	if (for_gc) {
		if (ruh->ruht == NVME_RUHT_INITIALLY_ISOLATED)
			ruid = rum->ii_gc_ruid;
		else if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED)
			ruid = ruh->pi_gc_ruids[rgid];
		else {
			if (ssd->gc_cnt[lpn] == 0)
				ruid = ruh->pi_gc_ruids[rgid];
			else 
				ruid = rum->ii_gc_ruid;
		}
	}
	else
		ruid = ruh->cur_ruids[rgid];

	ppa.ppa = 0;

	ru = &ssd->rus[ruid];
	ppa.g.ch = ru->wp.ch;
	ppa.g.lun = ru->wp.lun;
	ppa.g.pg = ru->wp.pg;
	ppa.g.blk = ru->wp.blk;
	ppa.g.pl = ru->wp.pl; 

    ftl_assert(ppa.g.pl == 0);

    return ppa;
}																	

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
	ppa.g.ch = wpp->ch;
	ppa.g.lun = wpp->lun;
	ppa.g.pg = wpp->pg;
	ppa.g.blk = wpp->blk;
	ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;
    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

	spp->slc_op = n->bb_params.slc_op;
	spp->qlc_op = n->bb_params.qlc_op;
    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->blks_per_ru = RG_DEGREE; 								
    spp->pgs_per_ru = spp->blks_per_ru * spp->pgs_per_blk;
    spp->secs_per_ru = spp->pgs_per_ru * spp->secs_per_pg;
	spp->chs_per_ru = RG_DEGREE / spp->luns_per_ch;
	spp->luns_per_ru = spp->blks_per_ru;
    spp->tt_rus = spp->blks_per_lun;							
	
    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);

	int slc_rus = spp->slc_op * 1.0 / (spp->slc_op + spp->qlc_op) * spp->blks_per_pl;
	int qlc_rus = spp->tt_rus - slc_rus;

	spp->gc_thres_rus_slc = (int)((1 - spp->gc_thres_pcent) * slc_rus);
	spp->gc_thres_rus_qlc = (int)((1 - spp->gc_thres_pcent) * qlc_rus);
    spp->gc_thres_rus_high_slc = (int)((1 - spp->gc_thres_pcent_high) * slc_rus); 
	spp->gc_thres_rus_high_qlc = (int)((1 - spp->gc_thres_pcent_high) * qlc_rus);

    spp->enable_gc_delay = true; 

    spp->endurance_slc = 10000;
	spp->endurance_qlc = 300;

    spp->op = 0.0625;
	//ftl_log("%lf\n", spp->op * spp->tt_secs);
    spp->ecc_corr_str = 50;
    spp->epsilon = 0.00048;
    spp->alpha = 0.000000516375983;
    spp->k = 2.05;
	spp->read_retry = 0;

	spp->gap = 10;

	spp->pages_from_host = 0;
    spp->pages_from_gc = 0;
    spp->pages_from_wl = 0;

	spp->gc_slc_to_qlc_threshold = 0.2;

    check_params(spp);
}

// 给blk分配mode
static int get_blk_mode(struct ssdparams *spp, int blk_id) {
	int offset = spp->slc_op * 1.0 / (spp->slc_op + spp->qlc_op) * spp->blks_per_pl;
	if (blk_id < offset)
		return 0;
	return 1;
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

// 增加blk rber的随机生成
static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);

	double min = 0.2;
	double max = 1.0;
	srand(time(NULL));
    for (int i = 0; i < pl->nblks; i++) {
		if (get_blk_mode(spp, i) == 0)
			pl->blk[i].mode = 0;
		else
			pl->blk[i].mode = 1;
		pl->blk[i].rand_rate = min + (double) rand() / (double)RAND_MAX * (max - min);
		//ftl_log("blk %d rand_rate %lf\n", i, pl->blk[i].rand_rate);
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    } 

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

	ssd->gc_cnt = g_malloc0(sizeof(int) * spp->tt_pgs);

    /* initialize all the lines */
    ssd_init_lines(ssd);

	/* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

	ssd_init_fdp_ru_mgmts(ssd); 						

	ssd_init_fdp_ruhtbl(n, ssd);				

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct ru *get_ru(struct ssd *ssd, struct ppa *ppa)
{
	//struct ssdparams *spp = &ssd->sp;
	//uint16_t rgid = ppa->g.ch * (spp->luns_per_ch / RG_DEGREE) + (ppa->g.lun / RG_DEGREE);
	// uint16_t rgid = (ppa->g.ch * spp->luns_per_ch + ppa->g.lun) / RG_DEGREE;
	// struct fdp_ru_mgmt *rum = &ssd->rums[rgid];
    return &ssd->rus[ppa->g.blk];
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
	struct nand_block *blk = get_blk(ssd, ppa);
    uint64_t lat = 0, operation_lat = 0;

	if (c == NAND_SLC_READ)
		operation_lat = NAND_SLC_READ_LAT;
    else if (c == NAND_SLC_PROG)
		operation_lat = NAND_SLC_PROG_LAT;
    else if (c == NAND_SLC_ERASE)
		operation_lat = NAND_SLC_ERASE_LAT;
    else if (c == NAND_QLC_READ_L)
		operation_lat = NAND_QLC_READ_L_LAT;
    else if (c == NAND_QLC_READ_CL)
		operation_lat = NAND_QLC_READ_CL_LAT;
    else if (c == NAND_QLC_READ_CU)
		operation_lat = NAND_QLC_READ_CU_LAT;
    else if (c == NAND_QLC_READ_U)
		operation_lat = NAND_QLC_READ_U_LAT;
    else if (c == NAND_QLC_PROG_L)
		operation_lat = NAND_QLC_PROG_L_LAT;
    else if (c == NAND_QLC_PROG_CL)
		operation_lat = NAND_QLC_PROG_CL_LAT;
    else if (c == NAND_QLC_PROG_CU)
		operation_lat = NAND_QLC_PROG_CU_LAT;
    else if (c == NAND_QLC_PROG_U)
		operation_lat = NAND_QLC_PROG_U_LAT;
	else if (c == NAND_QLC_PROG_TOTAL)
		operation_lat = NAND_QLC_PROG_TOTAL_LAT;
    else if (c == NAND_QLC_ERASE)
		operation_lat = NAND_QLC_ERASE_LAT;

	if (c == NAND_SLC_READ || c == NAND_QLC_READ_U || c == NAND_QLC_READ_CU || c == NAND_QLC_READ_CL || c == NAND_QLC_READ_L){
		// 算读重试次数
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
		
		// 磨损次数乘以随机值，显示不同闪存块的随机耐磨特性
        double ec = (double)blk->erase_cnt * blk->rand_rate;
		
		// 分别计算不同类型页的磨损程度 qlc根据页类型，slc要乘以磨损比例
		if (c == NAND_SLC_READ) {
			ec = ec / spp->endurance_slc * spp->endurance_qlc;
		} else if (c == NAND_QLC_READ_CL) {
			ec = ec * 2;
		} else if (c == NAND_QLC_READ_CU) {
			ec = ec * 6;
		} else if (c == NAND_QLC_READ_U) {
			ec = ec * 6;
		}

		double rber = spp->epsilon + spp->alpha*pow(ec,spp->k);
		rber = rber > 1.0 ? 1.0 : rber;
		int bits_count = spp->secs_per_pg * spp->secsz * 8;

		int read_retry = 0;
		while ((int)(bits_count * rber) > spp->ecc_corr_str) {
			rber /= 2.0;
			read_retry += 1;
		}
		spp->read_retry += read_retry;
        lun->next_lun_avail_time = nand_stime + operation_lat * (1 + read_retry);
        lat = lun->next_lun_avail_time - cmd_stime;	
	} else {
		nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + operation_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
	}

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa, uint16_t rgid)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    bool was_full_ru = false;	
    struct line *line;
    struct ru *ru; 

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
	struct fdp_ru_mgmt *rum;
	int pages_per_wl = 1;
	if (blk->mode == 0) {
		rum = &ssd->rums_slc[rgid];
		pages_per_wl = 4;
	} else {
		rum = &ssd->rums_qlc[rgid];
	}

    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc+=pages_per_wl;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc-=pages_per_wl;

	if (ssd->fdp_enabled)
	{ 
		/* update corresponding ru status */
		ru = get_ru(ssd, ppa);
		ftl_assert(ru->ipc >= 0 && ru->ipc < spp->pgs_per_ru);
		if (ru->vpc == spp->pgs_per_ru) {
			ftl_assert(ru->ipc == 0);
			was_full_ru = true;
		}
		ru->ipc+=pages_per_wl;
		ftl_assert(ru->vpc > 0 && ru->vpc <= spp->pgs_per_ru);
		/* Adjust the position of the victime ru in the pq under over-writes */
		if (ru->pos) {
			/* Note that ru->vpc will be updated by this call */
			pqueue_change_priority(rum->victim_ru_pq, ru->vpc - pages_per_wl, ru);
		} else {
			ru->vpc-=pages_per_wl;
		}

		if (was_full_ru) {
			/* move ru: "full" -> "victim" */
			QTAILQ_REMOVE(&rum->full_ru_list, ru, entry);
			rum->full_ru_cnt--;
			pqueue_insert(rum->victim_ru_pq, ru);
			rum->victim_ru_cnt++;
		}
	}
	else
	{
		/* update corresponding line status */
		line = get_line(ssd, ppa);
		ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
		if (line->vpc == spp->pgs_per_line) {
			ftl_assert(line->ipc == 0);
			was_full_line = true;
		}
		line->ipc++;
		ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
		/* Adjust the position of the victime line in the pq under over-writes */
		if (line->pos) {
			/* Note that line->vpc will be updated by this call */
			pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
		} else {
			line->vpc--;
		}

		if (was_full_line) {
			/* move line: "full" -> "victim" */
			QTAILQ_REMOVE(&lm->full_line_list, line, entry);
			lm->full_line_cnt--;
			pqueue_insert(lm->victim_line_pq, line);
			lm->victim_line_cnt++;
		} 
	}
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;
    struct ru *ru;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;
 
    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
	int page_per_wl = 1;
	if (blk->mode == 0) 
		page_per_wl = 4;
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc+=page_per_wl;

    /* update corresponding ru status */
	if (ssd->fdp_enabled) {
		ru = get_ru(ssd, ppa);
		ftl_assert(ru->vpc >= 0 && ru->vpc < ssd->sp.pgs_per_ru);
		ru->vpc+=page_per_wl; 
	}
    /* update corresponding line status */
	else {
		line = get_line(ssd, ppa);
		ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
		line->vpc++;
	}
} 

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
	struct nand_block *cur_blk = get_blk(ssd, ppa);
	int mode = cur_blk->mode;

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        if (mode == 0)
        	gcr.cmd = NAND_SLC_READ;
		else {
			int page_type = ppa->g.pg % 4;
			if (page_type == 0)
            	gcr.cmd = NAND_QLC_READ_L;
          	else if (page_type == 1)
            	gcr.cmd = NAND_QLC_READ_CL;
          	else if (page_type == 2)
            	gcr.cmd = NAND_QLC_READ_CU;
          	else
            	gcr.cmd = NAND_QLC_READ_U;
		}
		gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t fdp_gc_write_page(struct ssd *ssd, struct ppa *old_ppa, uint16_t rgid, uint16_t ruhid, int gc_flag)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));

	int mode = get_ruh_mode(ssd, ruhid);

	// 当gc效率低于阈值时将数据迁往qlc
	if (mode == 0 && gc_flag == 1)
		mode = 1;

	new_ppa = fdp_get_new_page(ssd, rgid, lpn, ruhid, true, mode);

    /* update maptbl */

    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

	mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
	ssd_advance_fdp_write_pointer(ssd, rgid, lpn, ruhid, true, mode);

	ssd->gc_cnt[lpn]++;
	
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        if (mode == 0)
        	gcw.cmd = NAND_SLC_READ;
		else {
			int page_type = new_ppa.g.pg % 4;
			if (page_type == 0)
            	gcw.cmd = NAND_QLC_READ_L;
          	else if (page_type == 1)
            	gcw.cmd = NAND_QLC_READ_CL;
          	else if (page_type == 2)
            	gcw.cmd = NAND_QLC_READ_CU;
          	else
            	gcw.cmd = NAND_QLC_READ_U;
		}
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
	new_ppa = get_new_page(ssd);
    /* update maptbl */

#ifdef FEMU_DEBUG_FTL
	printf("ch: %d, lun: %d, blk: %d, pg: %d\n", 
			new_ppa.g.ch, new_ppa.g.lun, new_ppa.g.blk, new_ppa.g.pg);
#endif

    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

	mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
	ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_QLC_PROG_U;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

static struct ru *select_victim_ru_slc(struct ssd *ssd, bool force, int rgid)
{
    struct fdp_ru_mgmt *rum = &ssd->rums_slc[rgid];
    struct ru *victim_ru = NULL;

    victim_ru = pqueue_peek(rum->victim_ru_pq);
    if (!victim_ru) {
        return NULL;
    } 

    if (!force && victim_ru->ipc < ssd->sp.pgs_per_ru / 8) {
        return NULL;
    }

    pqueue_pop(rum->victim_ru_pq);
    victim_ru->pos = 0;
    rum->victim_ru_cnt--;

    /* victim_ru is a danggling node now */
	ftl_log("slc victim_ru: %d\n", victim_ru->id);
    return victim_ru;
}

static struct ru *select_victim_ru_qlc(struct ssd *ssd, bool force, int rgid)
{
    struct fdp_ru_mgmt *rum = &ssd->rums_qlc[rgid];
    struct ru *victim_ru = NULL;

    victim_ru = pqueue_peek(rum->victim_ru_pq);
    if (!victim_ru) {
        return NULL;
    } 

    if (!force && victim_ru->ipc < ssd->sp.pgs_per_ru / 8) {
        return NULL;
    }

    pqueue_pop(rum->victim_ru_pq);
    victim_ru->pos = 0;
    rum->victim_ru_cnt--;

    /* victim_ru is a danggling node now */
	ftl_log("qlc victim_ru: %d\n", victim_ru->id);
    return victim_ru;
}

static int fdp_clean_one_block(struct ssd *ssd, struct ppa *ppa, uint16_t rgid, uint16_t ruhid)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
	
	struct ru *victim_ru = get_ru(ssd, &ppa);

	int gc_flag = 0; // flag=0表示迁移数据仍放置在slc，否则表示迁移数据放置在qlc
	double gc_ratio = victim_ru->ipc / (double)spp->pgs_per_ru; // ipc小于阈值说明当前存在大部分数据要做迁移
	if (gc_ratio < spp->gc_slc_to_qlc_threshold) {
		gc_flag = 1;
	}

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
#ifdef FDP_DEBUG
	printf("old_ch: %d old_lun: %d old_pl: %d old_blk: %d old_pg: %d\n",
			ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);
#endif
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
			// 当gc效率较低时，将gc需要迁移的数据放置在qlc中
            fdp_gc_write_page(ssd, ppa, rgid, ruhid, gc_flag);
            cnt++;
        }
    }

	(ssd->sp).pages_from_gc += cnt;

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
	return cnt;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static void output_info_log(struct ssd *ssd) {
	// ftl_log("output the info log\n");
  	// FILE *fp_wa = NULL;
    // struct fdp_ru_mgmt *rum = ssd->rums;
  
    // double wa = ((ssd->sp).pages_from_wl + (ssd->sp).pages_from_gc + (ssd->sp).pages_from_host) * 1.0 / (ssd->sp).pages_from_host;
    // struct ru *ru;
    // unsigned long long util = 0;
    // for (int i = 0; i < rum->tt_rus; i++) {
    //     ru = &rum->rus[i];
    //     util += ru->vpc;
    // }

    // char path2wa[80] = "wa.log.";
    // char path2ec[80] = "ec.log.";
    // strcat(path2wa, ssd->ssdname);
    // strcat(path2ec, ssd->ssdname);
    // fp_wa = fopen(path2wa, "a+");
    // fprintf(fp_wa, "WA=%.3f, util: %.1f(GB), pages from Host: %"PRIu64", pages from GC: %"PRIu64", pages from WL: %"PRIu64", read retry: %"PRIu64", bad_ru_cnt = %d, pages_from_host_read=%"PRIu64", host_read_block=%"PRIu64", host_write_block=%"PRIu64"\n", wa, util*4.0/1024/1024, (ssd->sp).pages_from_host, (ssd->sp).pages_from_gc, (ssd->sp).pages_from_wl, (ssd->sp).read_retry, rum->bad_ru_cnt, (ssd->sp).pages_from_host_read, (ssd->sp).host_read_block, (ssd->sp).host_write_block);
    // fclose(fp_wa);

    // ftl_log("Free_ru_cnt = %d, util: %.1f(GB), full_ru_cnt = %d, victim_ru_cnt = %d, bad_ru_cnt = %d, read_retry_cnt=%"PRIu64", pages_from_host_read=%"PRIu64", host_read_block=%"PRIu64", host_write_block=%"PRIu64"\n",rum->free_ru_cnt, util*4.0/1024/1024, rum->full_ru_cnt, rum->victim_ru_cnt, rum->bad_ru_cnt, (ssd->sp).read_retry, (ssd->sp).pages_from_host_read, (ssd->sp).host_read_block, (ssd->sp).host_write_block);

    // FILE *fp= fopen(path2ec, "a+");
    // if (fp != NULL) {
    //     int* records = g_malloc0(sizeof(int) * rum->tt_rus);
	// 	for (int i = 0; i < rum->tt_rus; i ++) {
	// 		ru = &rum->rus[i];
	// 		records[i] = ru->erase_cnt;
	// 	}
    //     for (int i = 0; i < rum->tt_rus; i++) {
    //         fprintf(fp, "%d ", records[i]);
    //     }
    //     fprintf(fp, "\n");
    //     fclose(fp);
    //     free(records);

    // } else {
    //     perror("Error");
    //     printf("Endurance log file open error!\n");
    // }
	return;
}


static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_QLC_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

      /* update line status */
      mark_line_free(ssd, &ppa);

    return 0;
}

static void erase_victim_ru(struct ssd *ssd, int victim_ru_id, int mode,  uint16_t rgid, NvmeRequest *req) { 
	struct ru *victim_ru = NULL;
	struct ssdparams *spp = &ssd->sp;
	struct nand_lun *lunp; 
	struct ppa ppa;
	struct fdp_ru_mgmt *rum;

	if (mode == 0)
		rum = &ssd->rums_slc[rgid];
	else
		rum = &ssd->rums_qlc[rgid];

	NvmeRuHandle *ruh;
	NvmeFdpEvent *e = NULL;
	int start_lunidx = rgid * RG_DEGREE;
	uint16_t ruhid;

	int gc_pgs = 0;
	ppa.g.blk = victim_ru_id;
	ppa.g.ch = start_lunidx / spp->luns_per_ch;
	ppa.g.lun = start_lunidx % spp->luns_per_ch;
	ppa.g.pl = 0;

	victim_ru = get_ru(ssd, &ppa);

	ruhid = victim_ru->ruhid; 
	ruh = &req->ns->endgrp->fdp.ruhs[ruhid];	

    ftl_log("GC-ing ru:%d,ipc=%d,vpc=%d,victim=%d,full=%d,free=%d,bad=%d,ruhid=%d\n", victim_ru->id,
              victim_ru->ipc, victim_ru->vpc, rum->victim_ru_cnt, rum->full_ru_cnt, rum->free_ru_cnt, rum->bad_ru_cnt, ruhid); 

	for (int lunidx = start_lunidx; lunidx < start_lunidx + RG_DEGREE; lunidx++) {
		ppa.g.ch = lunidx / spp->luns_per_ch;
		ppa.g.lun = lunidx % spp->luns_per_ch;
		ppa.g.pl = 0;
		lunp = get_lun(ssd, &ppa);
		gc_pgs += fdp_clean_one_block(ssd, &ppa, rgid, ruhid);
		mark_block_free(ssd, &ppa);

		if (spp->enable_gc_delay)
		{
			struct nand_cmd gce;
			gce.type = GC_IO;
			if (mode == 0)
				gce.cmd = NAND_SLC_ERASE;
			else 
				gce.cmd = NAND_QLC_ERASE;
			gce.stime = 0;
			ssd_advance_status(ssd, &ppa, &gce);
		} 

		lunp->gc_endtime = lunp->next_lun_avail_time;
	}
	victim_ru->erase_cnt += spp->gap;

	if (ruh->ruht == NVME_RUHT_INITIALLY_ISOLATED && log_event(ruh, FDP_EVT_MEDIA_REALLOC)) {
		struct nvme_fdp_event_realloc mr;
		e = nvme_fdp_alloc_event(req->ns->ctrl, &req->ns->endgrp->fdp.ctrl_events);
		e->type = FDP_EVT_MEDIA_REALLOC;
		e->flags = FDPEF_PIV | FDPEF_NSIDV | FDPEF_LV;
		e->pid = cpu_to_le16(ruhid);
		e->nsid = cpu_to_le32(req->ns->id);
		mr.flags = 1 << 0; // LIV on
		mr.nlbam = gc_pgs * 8;
		mr.lba = 0;
		memcpy(e->type_specific, &mr, sizeof(mr));
		e->rgid = cpu_to_le16(rgid);
		e->ruhid = cpu_to_le16(ruhid);
	}
	ftl_log("ru:%d erase_cnt:%d\n", victim_ru->id, victim_ru->erase_cnt);

	// 统计当前有效页数
	struct ru* tmp;
	double util = 0.0;
	for (int i = 0; i < rum->tt_rus; i++) {
		if (mode == 0)
			tmp = &ssd->rus[i];
		else
			tmp = &ssd->rus[ssd->rums_slc[rgid].tt_rus + i];
		util += tmp->vpc;
	}
	ftl_log("valid page:%lf\n", util);

	/* reset wp of victim ru */
	victim_ru->wp.ch = start_lunidx / spp->luns_per_ch;
	victim_ru->wp.lun = start_lunidx % spp->luns_per_ch;
	victim_ru->wp.pl = 0;
	victim_ru->wp.blk = victim_ru->id;
	victim_ru->wp.pg = 0;
	victim_ru->ipc = 0;
	victim_ru->vpc = 0;
	// victim_ru->pos = 0;
	
    // 块到达磨损上限，弃用整个超级块
    if ((mode == 0 && victim_ru->erase_cnt >= spp->endurance_slc) || (mode == 1 && victim_ru->erase_cnt >= spp->endurance_qlc)) {
		QTAILQ_INSERT_TAIL(&rum->bad_ru_list, victim_ru, entry);
		rum->bad_ru_cnt++;
		ftl_log("Ru %d becomes bad!\n", victim_ru->id);

		if (ssd->cv_moderate == 0) {
            double eop = ((rum->tt_rus - rum->bad_ru_cnt)*ssd->sp.pgs_per_line - util)/util;
			ftl_log("eop:%lf\n", eop);
            if (eop < ssd->sp.op) {
            //if (eop < 1) {    
				ssd->cv_moderate = 1;
				output_info_log(ssd);
            }
		}
    } else {
    	QTAILQ_INSERT_TAIL(&rum->free_ru_list, victim_ru, entry);
		rum->free_ru_cnt++;
    }

	return;
}
static int do_fdp_gc(struct ssd *ssd, uint16_t rgid, bool force, NvmeRequest *req, int gc_flag)
{
	struct ru *victim_ru_slc, *victim_ru_qlc = NULL;
	if (gc_flag >= 2) {
    	victim_ru_qlc = select_victim_ru_qlc(ssd, force, rgid);
		if(victim_ru_qlc)
			erase_victim_ru(ssd, victim_ru_qlc->id, 1, rgid, req);
	}
	if (gc_flag == 3 || gc_flag == 1) {
		victim_ru_slc = select_victim_ru_slc(ssd, force, rgid);
		if(victim_ru_slc)
			erase_victim_ru(ssd, victim_ru_slc->id, 0, rgid, req);
	}
    if (!victim_ru_qlc && !victim_ru_slc) {
        return -1;
    }

    return 0;
}
	
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
	(ssd->sp).pages_from_host_read += (end_lpn - start_lpn) + 1;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;

		struct nand_block *cur_blk = get_blk(ssd, &ppa);
		int mode = cur_blk->mode;

		if (mode == 0)
			srd.cmd = NAND_SLC_READ;
		else {
			int page_type = ppa.g.pg % 4;
			if (page_type == 0)
            	srd.cmd = NAND_QLC_READ_L;
          	else if (page_type == 1)
            	srd.cmd = NAND_QLC_READ_CL;
          	else if (page_type == 2)
            	srd.cmd = NAND_QLC_READ_CU;
          	else
            	srd.cmd = NAND_QLC_READ_U;
		}

        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }
    return maxlat; 
}

// 当ruhid为0或者1时，写入被定向到SLC
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
	uint64_t lba = req->slba;
	struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;

	(ssd->sp).pages_from_host += (end_lpn - start_lpn) + 1;
	
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

	NvmeRwCmd *rw = (NvmeRwCmd*)&req->cmd;
	NvmeNamespace *ns = req->ns;
	NvmeEnduranceGroup *endgrp = ns->endgrp;
	bool fdp_enabled = ssd->fdp_enabled;
    uint32_t dw12 = le32_to_cpu(req->cmd.cdw12);
    uint8_t dtype = (dw12 >> 20) & 0xf;
	uint16_t pid = le16_to_cpu(rw->dspec);
	uint16_t rgif = endgrp->fdp.rgif;						
	uint16_t rgid = pid >> (16 - rgif);
	uint16_t ph = pid & ((1 << (15 - rgif)) - 1);
	uint16_t ruhid;										

	if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT) {
		ph = 0;
		rgid = 0;
	}
	ruhid = ns->fdp.phs[ph];
	//ftl_log("%d\n", ruhid);

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    } 

	if (fdp_enabled) {
		int gc_flag = 0;
		/* perform GC here until !should_fdp_gc(ssd, rgid) */
		while ((gc_flag = should_fdp_gc_high(ssd, rgid))) {
			r = do_fdp_gc(ssd, rgid, true, req, gc_flag);
			if (r == -1)
				break;
		}
	}
	else {
		/* perform GC here until !should_gc(ssd) */
		while (should_gc_high(ssd)) {
			r = do_gc(ssd, true);
			if (r == -1)
				break;
		}
	}

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
			uint16_t old_rgid = (ppa.g.ch * spp->luns_per_ch + ppa.g.lun) / RG_DEGREE;
			mark_page_invalid(ssd, &ppa, old_rgid); 
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
			ssd->gc_cnt[lpn] = 0;
        }

        /* new write */
		int mode = get_ruh_mode(ssd, ruhid);
		ppa = (fdp_enabled ? fdp_get_new_page(ssd, rgid, 0, ruhid, false, mode) : get_new_page(ssd));

#ifdef FDP_DEBUG
		printf("pid: %10d lpn: %10ld rgid: %5d ruhid: %5d ch: %5d, lun: %5d, blk: %5d, pg: %5d\n", 
				pid, lpn, rgid, ruhid, ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pg);
#endif

        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
		if (fdp_enabled)  {
			ssd_advance_fdp_write_pointer(ssd, rgid, 0, ruhid, false, mode);
		}
		else
			ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;

		if (mode == 0)
			swr.cmd = NAND_SLC_PROG;
		else {
			int page_type = ppa.g.pg % 4;
			if (page_type == 0)
            	swr.cmd = NAND_QLC_PROG_L;
          	else if (page_type == 1)
            	swr.cmd = NAND_QLC_PROG_CL;
          	else if (page_type == 2)
            	swr.cmd = NAND_QLC_PROG_CU;
          	else
            	swr.cmd = NAND_QLC_PROG_U;
		}

        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
			}

			ftl_assert(req);
			switch (req->cmd.opcode) {
				case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
			// if (should_gc(ssd)) {
			// 	do_gc(ssd, false);
			// }
        }
    }

    return NULL;
}
