#include "ftl.h"
#include "stdbool.h"
// #include "uthash.h"
//#define FEMU_DEBUG_FTL
//#define FDP_DEBUG、

// LRU buffer
// 双向链表节点
typedef struct DLinkedNode {
    uint64_t lpn;
    bool dirty;
    struct DLinkedNode* prev;
    struct DLinkedNode* next;
} DLinkedNode;

// LRU缓存结构
typedef struct {
    int capacity;
    int size;
    DLinkedNode* head;
    DLinkedNode* tail;
    DLinkedNode** cache;
} LRUCache;

// 创建新节点
static DLinkedNode* newNode(uint64_t key, bool value) {
    DLinkedNode* node = (DLinkedNode*)malloc(sizeof(DLinkedNode));
    node->lpn = key;
    node->dirty = value;
    node->prev = NULL;
    node->next = NULL;
    return node;
}

// 创建LRU缓存
static LRUCache* lRUCacheCreate(int capacity) {
    LRUCache* cache = (LRUCache*)malloc(sizeof(LRUCache));
    cache->capacity = capacity;
    cache->size = 0;
    cache->head = newNode(0, 0);
    cache->tail = newNode(0, 0);
    cache->head->next = cache->tail;
    cache->tail->prev = cache->head;
    cache->cache = (DLinkedNode**)calloc(7864320, sizeof(DLinkedNode*));
    return cache;
}

// 添加节点到链表头部
static void addToHead(LRUCache* obj, DLinkedNode* node) {
    node->prev = obj->head;
    node->next = obj->head->next;
    obj->head->next->prev = node;
    obj->head->next = node;
}

// 删除节点
static void removeNode(DLinkedNode* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

// 将节点移到链表头部
static void moveToHead(LRUCache* obj, DLinkedNode* node) {
    removeNode(node);
    addToHead(obj, node);
}

// 删除链表尾部节点
static DLinkedNode* removeTail(LRUCache* obj) {
    DLinkedNode* node = obj->tail->prev;
    removeNode(node);
    return node;
}

// 获取缓存中的值
static int lRUCacheGet(LRUCache* obj, uint64_t key) {
    if (obj->cache[key] == NULL) {
        return -1;
    }
    DLinkedNode* node = obj->cache[key];
    moveToHead(obj, node);
    return node->dirty;
}

// 放入缓存，返回淘汰的点
static void lRUCachePut(LRUCache* obj, uint64_t key, bool value) {
    if (obj->cache[key] != NULL) {
        DLinkedNode* node = obj->cache[key];
        node->dirty = value;
        moveToHead(obj, node);
    } else {
        DLinkedNode* node = newNode(key, value);
        obj->cache[key] = node;
        addToHead(obj, node);
        obj->size++;
		if (obj->size > obj->capacity) {
            DLinkedNode* removed = removeTail(obj);
            obj->cache[removed->lpn] = NULL;
            obj->size--;
			free(removed);
        }
    }
	return;
}

// 释放LRU缓存
// static void lRUCacheFree(LRUCache* obj) {
//     DLinkedNode* node = obj->head;
//     while (node != NULL) {
//         DLinkedNode* temp = node;
//         node = node->next;
//         free(temp);
//     }
//     free(obj->cache);
//     free(obj);
// }

// 定义写缓冲区和读缓冲区
static LRUCache* write_buffer;
static LRUCache* read_buffer;

// 初始化写缓冲区和读缓冲区
static void ssd_init_lru_buffers(struct ssd *ssd) {
    int write_buffer_capacity = ssd->sp.write_buffer_capacity;  
    int read_buffer_capacity = ssd->sp.read_buffer_capacity; 
    write_buffer = lRUCacheCreate(write_buffer_capacity);
    read_buffer = lRUCacheCreate(read_buffer_capacity);
}

// 释放写缓冲区和读缓冲区
// static void free_lru_buffers() {
//     lRUCacheFree(write_buffer);
//     lRUCacheFree(read_buffer);
// }

static void *ftl_thread(void *arg);
static void output_info_log(struct ssd *ssd);
static void read_req_migrate(struct ssd *ssd, uint64_t lpn);
static int do_fdp_gc(struct ssd *ssd, uint16_t rgid, bool force, int gc_flag);
static inline struct ru *get_ru(struct ssd *ssd, struct ppa *ppa);
static void ssd_aged(struct ssd *ssd, double age_rate);

static double get_ru_victim_pre(struct ssd *ssd, struct ru *ru) {
	double victim_pre = 0;
	// if (ssd->sp.write_mode == 2) {
	// 	victim_pre = (ru->read_hotness * (ssd->avg_qlc_read_lat - NAND_SLC_READ_LAT) + ru->write_hotness * (NAND_QLC_PROG_CL_LAT - NAND_SLC_PROG_LAT)) / ru->vpc + ru->vpc;
	// }
	// else if (ssd->sp.write_mode == 1)
	// 	victim_pre = ru->write_hotness;
	// else if (ssd->sp.write_mode == 0)
		victim_pre = ru->vpc;
	return victim_pre;
}

static inline double add_hotness (double h, int thre) {
	return h < thre - 1 ? h + 1 : thre;
}

static inline double sub_hotness (double h) {
	int i = h;
	return (double) (i >> 1);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

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
	int slc_flag =  ssd->sp.gc_thres_rus_high_slc != 0 ? ssd->rums_slc[rg].free_ru_cnt <= ssd->sp.gc_thres_rus_high_slc : 0;
	int qlc_flag =  (ssd->rums_qlc[rg].free_ru_cnt <= ssd->sp.gc_thres_rus_high_qlc);
	//if (slc_flag || qlc_flag)
		//printf("gc_flag: %d %d\n", slc_flag, qlc_flag);
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
	//ftl_log("lpn:%"PRIu64", , ruid:%d, read_hotness: %lf, write_hotness: %lf\n", lpn, new_ru->id, new_ru->read_hotness, new_ru->write_hotness);
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

// static inline pqueue_pri_t victim_ru_get_pri(void *a)		
// {
//     return ((struct ru *)a)->vpc;
// }

// static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
// {
//     ((struct ru *)a)->vpc = pri;
// }

// 综合考虑热度和无效页个数
static inline pqueue_pri_t victim_ru_get_pri(void *a)		
{
    return ((struct ru *)a)->victim_pre;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct ru *)a)->victim_pre = pri;
}

static inline size_t victim_ru_get_pos(void *a)
{
    return ((struct ru *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((struct ru *)a)->pos = pos;
}																	

// 获取slc区域中第i个ru的ru_id
int get_slc_ru_id(struct ssd *ssd, int i)
{
	int index = 0;
	if (ssd->sp.ru_mode == 0) {
		index = i;
	} else if (ssd->sp.ru_mode == 1) {
		index = ssd->indices[i + ssd->rums_qlc->tt_rus];
	} else
		index = ssd->indices[i];
	return index;
}

// 获取qlc区域中第i个ru的ru_id
int get_qlc_ru_id(struct ssd *ssd, int i)
{
	int index = 0;
	if (ssd->sp.ru_mode == 0) {
		index = i + ssd->rums_slc->tt_rus;
	} else if (ssd->sp.ru_mode == 1) {
		index = ssd->indices[i];
	} else
		index = ssd->indices[i + ssd->rums_slc->tt_rus];
	return index;
}

// 每个rg一个rum来管理free、victim ru list等
// 增加每个ru中rand_rate的初始化
static void ssd_init_fdp_ru_mgmts(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
    struct fdp_ru_mgmt *rum_slc = NULL, *rum_qlc = NULL;
    struct ru *ru = NULL;
	int nrg = spp->tt_luns / RG_DEGREE;
	
	ssd->rus =  g_malloc0(sizeof(struct ru) * spp->tt_rus);
	ssd->rums_slc = g_malloc(sizeof(struct fdp_ru_mgmt) * nrg);
	ssd->rums_qlc = g_malloc(sizeof(struct fdp_ru_mgmt) * nrg);
	ssd->rand_rate = g_malloc(sizeof(double) * spp->tt_rus);

	// rand_rate取值范围
	double min = 0.5;
	double max = 1.5;
	srand(0); // 可复现的随机序列

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

		rum_slc->read_cnt = 0;
		rum_slc->write_cnt = 0;
		rum_qlc->read_cnt = 0;
		rum_qlc->write_cnt = 0;

		rum_slc->read_ru_cnt = 0;
		rum_qlc->read_ru_cnt = 0;

		rum_slc->write_ru_cnt = 0;
		rum_qlc->write_ru_cnt = 0;

		// 生成所有ru的信息
		for (int j = 0; j < spp->tt_rus; j++) {
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
			ru->erase = 0;
			ru->pe_slc = 0;
			ru->pe_qlc = 0;
			ru->read_hotness = 0;
			ru->write_hotness = 0;
			ru->victim_pre = 0;
			ru->rut = RU_TYPE_NORMAL;
			ru->rand_rate = min + (double) rand() / (double)RAND_MAX * (max - min);
			// to do 先全保持一致
			ru->rand_rate = 1;
			ssd->rand_rate[j] = ru->rand_rate;
		}

		// 对rand_rate进行排序，记录索引
		ssd->indices = g_malloc(sizeof(int) * spp->tt_rus);
		double *tmp = g_malloc(sizeof(double) * spp->tt_rus);
		for (int j = 0; j < spp->tt_rus; j ++) {
			ssd->indices[j] = j;
			tmp[j] = ssd->rand_rate[j];
		}
		// 从大到小排序
		for (int j = 0; j < spp->tt_rus - 1; j++) {
			for (int k = 0; k < spp->tt_rus - j - 1; k++) {
				if (tmp[k] < tmp[k + 1]) {
					int temp = ssd->indices[k];
					ssd->indices[k] = ssd->indices[k + 1];
					ssd->indices[k + 1] = temp;

					double t = tmp[k];
					tmp[k] = tmp[k + 1];
					tmp[k + 1] = t;
				}
			}
		}

		// 将ru放置在不同区域
		for (int j = 0; j < rum_slc->tt_rus; j ++) {
			ru = &ssd->rus[get_slc_ru_id(ssd, j)];
			ru->mode = 0;
			QTAILQ_INSERT_TAIL(&rum_slc->free_ru_list, ru, entry);
			rum_slc->free_ru_cnt++;
		}
		for (int j = 0; j < rum_qlc->tt_rus; j ++) {
			ru = &ssd->rus[get_qlc_ru_id(ssd, j)];
			ru->mode = 1;
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
	struct ru *retru = NULL;

	retru = QTAILQ_FIRST(&rum->free_ru_list);
	if (!retru) {
		output_info_log(ssd);
		ftl_err("SSD reaches its end of the life!\n");
		abort();
	}
	
	struct ru *ru_tmp = retru;
	if (ssd->sp.enable_dwl) {
		// 动态磨损均衡，优先选择最年轻的RU进行写入
		double hottest = retru->erase;
		for (int i = 1; i < rum->free_ru_cnt; i++) {
			retru = QTAILQ_NEXT(retru, entry);
			if (retru->erase < hottest) {
				hottest = retru->erase;
				ru_tmp = retru;
			}
		}
	}
	
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
	if (ru_tmp->mode == 0 && ruhid == 0) {
		rum->write_ru_cnt ++;
		ru_tmp->ruhid = 0;
	}
	else if (ru_tmp->mode == 0 && ruhid == 1) {
		rum->read_ru_cnt ++;
		ru_tmp->ruhid = 1;
	}
	else if (ru_tmp->mode == 1) {
		ru_tmp->ruhid = ruhid;
	}
	QTAILQ_REMOVE(&rum->free_ru_list, ru_tmp, entry);
	rum->free_ru_cnt--;
	
	if (ruhid == 1 && rum->read_ru_cnt >= ssd->sp.ra_max_cnt)
		ssd->ra_full_flag = 1;
	//ftl_log("ru_id:%d ruhid:%d\n", ru_tmp->id, ruhid);
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
			if (get_ruh_mode(ssd, i) == 0 && ssd->sp.slc_op != 0) {
				rum_slc = &ssd->rums_slc[j];
				ruh->cur_ruids[j] = get_next_free_ruid(ssd, rum_slc, i);
				ftl_log("init ruh %d cur_ruid[%d] = %d in slc\n", i, j, ruh->cur_ruids[j]);
			} else if (get_ruh_mode(ssd, i) == 1 && ssd->sp.qlc_op != 0) {
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
			if (get_ruh_mode(ssd, j) == 0  && ssd->sp.slc_op != 0) {
				pi_gc_ruid = get_next_free_ruid(ssd, rum_slc, j);
				ssd->ruhtbl[j].pi_gc_ruids[i] = pi_gc_ruid;
				ssd->rus[pi_gc_ruid].rut = RU_TYPE_PI_GC;
			} else if (get_ruh_mode(ssd, j) == 1 && ssd->sp.qlc_op != 0) {
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
				// 若当前是迁移后的ru写完了，则要新找一个存放迁移数据的ru
				int new_ru_id = get_next_free_ruid(ssd, rum, ruhid);
				// if (new_ru_id == -1) {
				// 	ssd->read_area_full_flag = 0;
				// 	return;
				// }
				if (ru->rut == RU_TYPE_II_GC) {
					rum->ii_gc_ruid = new_ru_id;
					//("ii\n");
					ssd->rus[rum->ii_gc_ruid].rut = RU_TYPE_II_GC;
				}
				else if (ru->rut == RU_TYPE_PI_GC) {
					ruh->pi_gc_ruids[rgid] = new_ru_id;
					//ftl_log("pi\n");
					ssd->rus[ruh->pi_gc_ruids[rgid]].rut = RU_TYPE_PI_GC;
				}
				else {
					// 若是正常写的ru完了，就要更新ruh当前指向的ru
					ruh->cur_ruids[rgid] = new_ru_id;
					//ftl_log("normal\n");
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

	spp->util_ratio_high = n->bb_params.util_ratio_high * 1.0 / 100;
	spp->util_ratio_low = n->bb_params.util_ratio_low * 1.0 / 100;

	spp->balance_ratio = n->bb_params.balance_ratio * 1.0 / 100;
	spp->unbalance_ratio = 1 - spp->balance_ratio;
	
	spp->write_buffer_capacity = n->bb_params.write_buffer_capacity;
	spp->read_buffer_capacity = n->bb_params.read_buffer_capacity;

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
	spp->gc_thres_rus_qlc = (int)((1 - 0.9) * qlc_rus);
    spp->gc_thres_rus_high_slc = (int)((1 - spp->gc_thres_pcent_high) * slc_rus); 
	spp->gc_thres_rus_high_qlc = (int)((1 - 0.99) * qlc_rus);

    spp->enable_gc_delay = true; 

    spp->endurance_slc = 40000;
	spp->endurance_qlc = 1500;

    spp->op = 0.0625;
	//ftl_log("%lf\n", spp->op * spp->tt_secs);
    spp->ecc_corr_str = 0;
    spp->epsilon = 0.00048;
    spp->alpha = 0.000000516375983;
    spp->k = 2.05;
	spp->read_retry = 0;

	spp->slc_alpha = 0.5;

	spp->gap = 10;

	spp->pages_from_host = 0;
    spp->pages_from_gc = 0;
    spp->pages_from_wl = 0;
	spp->pages_from_migrate = 0;

	spp->gc_slc_to_qlc_threshold = 0.3;
	spp->enable_dwl = 1;
	spp->enable_swl = 0;

	spp->ru_mode = n->bb_params.ru_mode;
	spp->read_migration = n->bb_params.read_migration;
	spp->write_mode = n->bb_params.write_mode;
	spp->wl_mode = n->bb_params.wl_mode;
	spp->read_latency_threshold = 2 * NAND_QLC_READ_CU_LAT;
	spp->ra_max_cnt = n->bb_params.ra_max_cnt;
	spp->dynamic_ra_flag = n->bb_params.dynamic_ra_flag;
    check_params(spp);
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

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
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

	ssd_init_lru_buffers(ssd);

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

	// 初始化lpntbl
	ssd->lpnrtbl = g_malloc0(spp->tt_pgs * sizeof(int));
	ssd->lpnwtbl = g_malloc0(spp->tt_pgs * sizeof(int));

	ssd->read_hotness = g_malloc0(spp->tt_pgs * sizeof(double));
	ssd->write_hotness = g_malloc0(spp->tt_pgs * sizeof(double));
	ssd->gc_cnt_before_update = g_malloc0(spp->tt_pgs * sizeof(int));
	ssd->age = 2;
	ssd->hotless_ru_hotness = 0;
	ssd->hotless_ru_id = 0;
	ssd->wr_hotless_ru_hotness = 0;
	ssd->wr_hotless_ru_id = 0;
	ssd->migrate_count = 0;
	ssd->write_migrate_count = 0;
	ssd->pe_qlc = 0;
	ssd->pe_slc = 0;
	ssd->hot_ratio = 1;
	ssd->page_size_thre = 1;
	ssd->qlc_gc_eff = 1;
	ssd->slc_gc_eff = 1;
	// ssd->gc_func_left = 0;
	// ssd->gc_func_right = 0;
	// ssd->has_do_qlc_gc = 0;
	// ssd->has_do_slc_gc = 0;
	ssd->cnt_window = 0;
	ssd->ra_full_flag = 0;
	ssd->gc_to_slc_cnt = 0;
	ssd->gc_to_qlc_cnt = 0;

	ssd->status_0_cnt = 0;
	ssd->status_1_cnt = 0;
	ssd->status_2_cnt = 0;
	ssd->status_3_cnt = 0;
	ssd->status_4_cnt = 0;
	ssd->status_5_cnt = 0;
	ssd->gc_cnt_before_update_thre = 3;
	ssd->write_hotness_thre = 0;

	if (ssd->age == 2) {
		double avg_qlc_unbalance_read_lat = (QLC_U_1_R + QLC_U_2_R + MID_READ_U_RETRY * QLC_U_3_R  + MID_READ_U_RETRY * QLC_U_4_R) * 1.0/ 4;
		double avg_qlc_balance_read_lat = MID_READ_RETRY * (QLC_1_R + QLC_2_R + QLC_3_R  + QLC_4_R) * 1.0/ 4;
		ssd->avg_qlc_read_lat = avg_qlc_balance_read_lat * ssd->sp.balance_ratio + avg_qlc_unbalance_read_lat * ssd->sp.unbalance_ratio;
		ftl_log("avg_qlc_read_lat:%lf,avg_qlc_balance_read_lat:%lf,avg_qlc_unbalance_read_lat:%lf\n", ssd->avg_qlc_read_lat, avg_qlc_balance_read_lat, avg_qlc_unbalance_read_lat);
	}

	double age_rate = 0.8;
	ssd_aged(ssd, age_rate);
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

static inline bool if_satisfy_migrate(struct ssd *ssd, struct ppa *ppa) {
	int migrate_mode = ssd->sp.read_migration;
	if (migrate_mode == 0)
		return false;
	// 根据读取次数判断是否需要迁移
	uint64_t lpn = get_rmap_ent(ssd, ppa);
	if (migrate_mode == 1) {
		if (ssd->read_hotness[lpn] >= 2)
			return true;
		return false;
	}
	// 同时根据读取次数和页面类型判断是否需要迁移
	if (migrate_mode == 2) {
		int page_type = ppa->g.pg % 4;
		if (ssd->read_hotness[lpn] >= 2 && (page_type == 2 || page_type == 3))
			return true;
		return false;
	}

	// 根据自定义的热度判断是否需要迁移
	if (migrate_mode == 3) {
		int page_type = ppa->g.pg % 4;
		int page_balance_type = (ppa->g.pg / 4) % 100 < ssd->sp.balance_ratio * 100 ? 0 : 1;
		double read_page_lat;
		if (page_balance_type == 0) {
			if (page_type == 0)
				read_page_lat = QLC_1_R * MID_READ_RETRY;
			else if (page_type == 1)
				read_page_lat = QLC_2_R * MID_READ_RETRY;
			else if (page_type == 2)
				read_page_lat = QLC_3_R * MID_READ_RETRY;
			else
				read_page_lat = QLC_4_R * MID_READ_RETRY;
		} else {
			if (page_type == 0)
				read_page_lat = QLC_1_R * MID_READ_RETRY;
			else if (page_type == 1)
				read_page_lat = QLC_1_R * MID_READ_RETRY;
			else if (page_type == 2)
				read_page_lat = QLC_1_R * MID_READ_RETRY;
			else
				read_page_lat = QLC_1_R * MID_READ_RETRY;
		}

		// 被迁移的页热度
		double cur_hotness = ssd->read_hotness[lpn] * (read_page_lat - SLC_R) + ssd->write_hotness[lpn] * (QLC_W - SLC_W);
		// if (ssd->v_write <= ssd->v_gc) {
		// 	if (cur_hotness > NAND_SLC_PROG_LAT)
		// 		return true;
		// 	else
		// 		return false;
		// } else {
		double cur_slc_ra_avg_hotness =  ssd->slc_valid_cnt == 0 ? 0 : ssd->total_slc_ra_read_hotness  / ssd->slc_valid_cnt * (ssd->avg_qlc_read_lat - SLC_R) + ssd->total_slc_ra_write_hotness/ ssd->slc_valid_cnt * (QLC_W - SLC_W);

		if ((ssd->ra_full_flag == 0 && ssd->read_hotness[lpn] >= 2 && cur_hotness >= cur_slc_ra_avg_hotness + SLC_W) ||(ssd->ra_full_flag != 0 && cur_hotness >= cur_slc_ra_avg_hotness + QLC_W + SLC_R + SLC_W))
			return true;
		else
			return false;
		// }
	}
	return false;
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
	//struct ru *cur_ru = get_ru(ssd, ppa);
    uint64_t lat = 0, operation_lat = 0;

	if (c == NAND_SLC_READ)
		operation_lat = NAND_SLC_READ_LAT;
    else if (c == NAND_SLC_PROG)
		operation_lat = NAND_SLC_PROG_LAT;
    else if (c == NAND_SLC_ERASE)
		operation_lat = NAND_SLC_ERASE_LAT;
    else if (c == NAND_QLC_READ) {
		operation_lat = NAND_QLC_READ_CL_LAT;
	}
    else if (c == NAND_QLC_PROG) {
		operation_lat = NAND_QLC_PROG_CL_LAT;
	}	
    else if (c == NAND_QLC_ERASE)
		operation_lat = NAND_QLC_ERASE_LAT;

	if (c == NAND_SLC_READ || c == NAND_QLC_READ){
		// 算读重试次数
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;

		int read_retry = 0;
		if(ssd->age == 2 && c == NAND_QLC_READ) {
			int page_type = ppa->g.pg % 4;
			int page_balance_type = (ppa->g.pg / 4) % 100 < ssd->sp.balance_ratio * 100 ? 0 : 1;
			if (page_balance_type == 0) {
				if (page_type == 0) {
					operation_lat = NAND_QLC_READ_L_LAT * 2;
					read_retry = 1;
				}
				else if (page_type == 1) {
					operation_lat = NAND_QLC_READ_CL_LAT * 3;
					read_retry = 2;
				}
				else if (page_type == 2) {
					operation_lat = NAND_QLC_READ_CU_LAT * 3;
					read_retry = 2;
				}
				else {
					operation_lat = NAND_QLC_READ_U_LAT * 3;
					read_retry = 2;
				}
			} else {
				if (page_type == 0)
					operation_lat = NAND_QLC_READ_L_LAT;
				else if (page_type == 1)
					operation_lat = NAND_QLC_READ_CL_LAT;
				else if (page_type == 2) {
					operation_lat = NAND_QLC_READ_CU_LAT * 7;
					read_retry = 6;
				}
				else {
 					operation_lat = NAND_QLC_READ_U_LAT * 7;
					read_retry = 6;
				}
			}
		}

		spp->read_retry += read_retry;
        lun->next_lun_avail_time = nand_stime + operation_lat;

		// 读取操作发生在QLC区域且满足条件
		if (c != NAND_SLC_READ && if_satisfy_migrate(ssd, ppa)){
			uint64_t lpn = get_rmap_ent(ssd, ppa);
			//ftl_log("lpn: %"PRIu64" read latency:%"PRIu64"\n", lpn, req_lat);
			//ftl_log("begin read migrate\n");
			ssd->migrate_count ++;
			spp->pages_from_migrate ++;
			read_req_migrate(ssd, lpn);
		}
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
    struct ru *ru = get_ru(ssd, ppa); 
	uint64_t lpn = get_rmap_ent(ssd, ppa);

	ru->read_hotness -= ssd->read_hotness[lpn];
	ru->write_hotness -= ssd->write_hotness[lpn];

	if (ru->mode == 0) { 
		if (ru->ruhid == 0) {
			ssd->total_slc_wa_write_hotness -= ssd->write_hotness[lpn];
			ssd->total_slc_wa_read_hotness -= ssd->read_hotness[lpn];
		} else if (ru->ruhid == 1) {
			ssd->total_slc_ra_read_hotness -= ssd->read_hotness[lpn];
			ssd->total_slc_ra_write_hotness -= ssd->write_hotness[lpn];
		}
		ssd->slc_valid_cnt -= 1;
	}
	//ftl_log("lpn:%"PRIu64", ruid:%d read_hotness: %lf, write_hotness: %lf\n", lpn, ru->id, ru->read_hotness, ru->write_hotness);

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
	struct fdp_ru_mgmt *rum;
	int pages_per_wl = 1;
	if (ru->mode == 0) {
		rum = &ssd->rums_slc[rgid];
		pages_per_wl = 4;
	} else {
		rum = &ssd->rums_qlc[rgid];
	}

	// 有效页 - 1
	rum->valid_page_num -= 1;

    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc+=pages_per_wl;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc-=pages_per_wl;

	if (ssd->fdp_enabled)
	{ 
		/* update corresponding ru status */
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
			ru->vpc -= pages_per_wl;
			double cur_victim_pre = get_ru_victim_pre(ssd, ru);
			pqueue_change_priority(rum->victim_ru_pq, cur_victim_pre, ru);
		} else {
			ru->vpc -= pages_per_wl;
			ru->victim_pre = get_ru_victim_pre(ssd, ru);
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
	struct ru *cur_ru = get_ru(ssd, ppa);
	int page_per_wl = 1;
	struct fdp_ru_mgmt* rum;
	if (cur_ru->mode == 0) {
		rum = &ssd->rums_slc[0];
		page_per_wl = 4;
	} else {
		rum = &ssd->rums_qlc[0];
	}
	rum->valid_page_num += 1;
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc+=page_per_wl;

    /* update corresponding ru status */
	if (ssd->fdp_enabled) {
		uint64_t lpn = get_rmap_ent(ssd, ppa);
		ru = get_ru(ssd, ppa);
		ftl_assert(ru->vpc >= 0 && ru->vpc < ssd->sp.pgs_per_ru);
		ru->vpc+=page_per_wl;
		ru->read_hotness += ssd->read_hotness[lpn];
		ru->write_hotness += ssd->write_hotness[lpn];
		ru->victim_pre = get_ru_victim_pre(ssd, ru);
		if (ru->mode == 0) {
			if (ru->ruhid == 0) {
				ssd->total_slc_wa_write_hotness += ssd->write_hotness[lpn];
				ssd->total_slc_wa_read_hotness += ssd->read_hotness[lpn];
			} else if (ru->ruhid == 1) {
				ssd->total_slc_ra_write_hotness += ssd->write_hotness[lpn];
				ssd->total_slc_ra_read_hotness += ssd->read_hotness[lpn];
			}
			ssd->slc_valid_cnt += 1;
		}
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
	struct ru *cur_ru = get_ru(ssd, ppa);
	int mode = cur_ru->mode;

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        if (mode == 0)
        	gcr.cmd = NAND_SLC_READ;
		else {
			gcr.cmd = NAND_QLC_READ;
		}
		gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

// 将lpn对应的数据迁移到slc中
static void read_req_migrate(struct ssd *ssd, uint64_t lpn) {
	int gc_flag = 0;
	int r;
	/* perform GC here until !should_fdp_gc(ssd, rgid) */
	while ((gc_flag = should_fdp_gc_high(ssd, 0))) {
		r = do_fdp_gc(ssd, 0, true, gc_flag);
		if (r == -1)
			break;
	}

	// 因为是在读请求过程中触发的，所以避免了读操作，直接进行写操作迁移数据
	struct ppa ppa;
	ppa = get_maptbl_ent(ssd, lpn);
	struct ssdparams *spp = &ssd->sp;

	// 更新rmap
	if (mapped_ppa(&ppa)) {
		uint16_t old_rgid = (ppa.g.ch * spp->luns_per_ch + ppa.g.lun) / RG_DEGREE;
		mark_page_invalid(ssd, &ppa, old_rgid); 
		set_rmap_ent(ssd, INVALID_LPN, &ppa);
		ssd->gc_cnt[lpn] = 0;
	}

	/* new write */
	int mode = 0;
	// to do ruhid定为1，rg定为0
	if (spp->read_migration == 3) {
		ppa = fdp_get_new_page(ssd, 0, 0, 1, false, mode);
	} else 
		ppa = fdp_get_new_page(ssd, 0, 0, 0, false, mode);

	ssd->slc_write_cnt ++;

	/* update maptbl */
	set_maptbl_ent(ssd, lpn, &ppa);
	/* update rmap */
	set_rmap_ent(ssd, lpn, &ppa);

	mark_page_valid(ssd, &ppa);

	if (spp->read_migration == 3) {
		ssd_advance_fdp_write_pointer(ssd, 0, 0, 1, false, mode);
	} else 
		ssd_advance_fdp_write_pointer(ssd, 0, 0, 0, false, mode);

	struct nand_cmd swr;
	swr.type = USER_IO;

	if (mode == 0)
		swr.cmd = NAND_SLC_PROG;
	else {
		swr.cmd = NAND_QLC_PROG;
	}

	swr.stime = 0;
	ssd_advance_status(ssd, &ppa, &swr);
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t fdp_gc_write_page(struct ssd *ssd, struct ppa *old_ppa, uint16_t rgid, uint16_t ruhid)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

	// 旧物理地址置为无效
	struct nand_page * pg = get_pg(ssd, old_ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

	set_rmap_ent(ssd, INVALID_LPN, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));

	int mode = get_ruh_mode(ssd, ruhid);

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
			gcw.cmd = NAND_SLC_PROG;
		else {
            gcw.cmd = NAND_QLC_PROG;
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
        gcw.cmd = NAND_QLC_PROG;
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

static struct ru *select_hotless_ra_ru(struct ssd *ssd) {
	struct fdp_ru_mgmt *rum = &ssd->rums_slc[0];
	struct ru *ra_victim_ru = NULL;
	double cur_ru_hotness = 0;
	double ra_victim_ru_hotness = 1000000000;
	for (int j = 1; j < rum->victim_ru_cnt; j++) {
		struct ru * tmp_ru = rum->victim_ru_pq->d[j + 1]; 
		// if (tmp_ru->ruhid == 0 && wa_victim_second_ru == NULL) {
		// 	wa_victim_second_ru = tmp_ru;
		// 	ftl_log("wa_victim_second_ru:%d\n", wa_victim_second_ru->vpc);
		// 	continue;
		// }
		if (tmp_ru->ruhid == 1) {
			cur_ru_hotness = tmp_ru->read_hotness * (ssd->avg_qlc_read_lat - SLC_R) + tmp_ru->write_hotness * (QLC_W - SLC_W);
			ftl_log("%d cur_ru_hotness:%lf\n", tmp_ru->id, cur_ru_hotness);
			if (cur_ru_hotness < ra_victim_ru_hotness) {
				ra_victim_ru = tmp_ru;
				ra_victim_ru_hotness = cur_ru_hotness;
			}
		}
	}
	return ra_victim_ru;
}
static struct ru *if_increase_ra(struct ssd *ssd) {
	struct fdp_ru_mgmt *rum = &ssd->rums_slc[0];
    struct ru *wa_victim_ru = NULL;
	struct ru *ra_victim_ru = NULL;
	//struct ru *wa_victim_second_ru = NULL;

    wa_victim_ru = pqueue_peek(rum->victim_ru_pq);
	double wa_victim_ru_hotness = wa_victim_ru->read_hotness * (ssd->avg_qlc_read_lat - SLC_R) + wa_victim_ru->write_hotness * (QLC_W - SLC_W);
	ftl_log("wa_victim_ru_hotness:%lf\n", wa_victim_ru_hotness);

	ra_victim_ru = select_hotless_ra_ru(ssd);
	double ra_victim_ru_hotness = ra_victim_ru->read_hotness * (ssd->avg_qlc_read_lat - SLC_R) + ra_victim_ru->write_hotness * (QLC_W - SLC_W);
	
	double slc_after_gc_eff = ssd->slc_gc_eff * 2.0 / 3  +  wa_victim_ru->ipc / ssd->sp.pgs_per_ru / 3;
	double slc_cur_valid = (1 - ssd->slc_gc_eff) * ssd->sp.pgs_per_ru / 4;
	double slc_after_valid = (1 - slc_after_gc_eff) * ssd->sp.pgs_per_ru / 4;
	double cur_gc_cost =  slc_cur_valid / (ssd->sp.pgs_per_ru / 4.0 - slc_cur_valid);
	double after_gc_cost = slc_after_valid / (ssd->sp.pgs_per_ru / 4.0 - slc_after_valid);
	ftl_log("cur_gc_cost:%lf after_gc_cost:%lf\n", cur_gc_cost, after_gc_cost);

	double not_increase_benefit = ssd->total_slc_wa_write_hotness * (after_gc_cost - cur_gc_cost);

	ftl_log("not_increase_benefit:%lf\n", not_increase_benefit);
	if (ssd->slc_gc_eff >= ssd->sp.util_ratio_high) {
		not_increase_benefit = not_increase_benefit * (SLC_R + SLC_W);
		not_increase_benefit += wa_victim_ru->vpc / 4.0 * (SLC_R + SLC_W);
	} else {
		not_increase_benefit = not_increase_benefit * (SLC_R + QLC_W);
		not_increase_benefit += wa_victim_ru->vpc / 4.0 * (SLC_R + QLC_W);
	}

	not_increase_benefit += wa_victim_ru_hotness;
	
	double increase_benefit = ra_victim_ru_hotness + ra_victim_ru->vpc / 4.0 * (SLC_R + QLC_W);
	ftl_log("left:%lf right:%lf\n", not_increase_benefit, increase_benefit);
	if (not_increase_benefit < increase_benefit)
		return wa_victim_ru;
	else
		return ra_victim_ru;
}

static struct ru *select_victim_ru_slc(struct ssd *ssd, bool force, int rgid)
{
    struct fdp_ru_mgmt *rum = &ssd->rums_slc[rgid];
    struct ru *victim_ru = NULL;
	//struct ssdparams *spp = &ssd->sp;
    victim_ru = pqueue_peek(rum->victim_ru_pq);

    if (!victim_ru) {
		return NULL;
    } 

    if (!force && victim_ru->ipc < ssd->sp.pgs_per_ru / 8) {
        return NULL;
    }

	// 当前应该挑选热度最低的READ 超级块被淘汰
	if (victim_ru->ruhid == 0 && ssd->ra_full_flag == 1 && (ssd->sp.write_mode == 1 || ssd->sp.write_mode == 2)) {
		if (ssd->sp.dynamic_ra_flag) {
			victim_ru = if_increase_ra(ssd);
			if (victim_ru->ruhid == 0)
			ssd->sp.ra_max_cnt ++;
		}
		else
			victim_ru = select_hotless_ra_ru(ssd);
	}

    /* victim_ru is a danggling node now */
	// ftl_log("slc victim_ru: %d\n", victim_ru->id);
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

    // pqueue_pop(rum->victim_ru_pq);
    // victim_ru->pos = 0;
    // rum->victim_ru_cnt--;

    /* victim_ru is a danggling node now */
	// ftl_log("qlc victim_ru: %d\n", victim_ru->id);
    return victim_ru;
}

static int fdp_clean_one_block(struct ssd *ssd, struct ppa *ppa, uint16_t rgid, uint16_t ruhid)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
	int mode = get_ruh_mode(ssd, ruhid);

	// 根据GC效率判断是否将当前GC数据迁移到QLC区域
	int migrate_flag = 0;
	if (ssd->sp.write_mode == 0) 
		migrate_flag = 1;
	else if (ssd->sp.write_mode == 2){
		if (ssd->slc_gc_eff >= ssd->sp.util_ratio_high && ruhid == 0)
			migrate_flag = 0;
		else
			migrate_flag = 1;
	} else if (ssd->sp.write_mode == 1)
		migrate_flag = 1;

    for (int pg = 0; pg < spp->pgs_per_blk; ) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            /* delay the maptbl update until "write" happens */
			// 当gc效率较低时，将gc需要迁移的数据放置在qlc中
			gc_read_page(ssd, ppa);
            cnt++;
        }

		// SLC块只需要扫描四分之一的块
		if (mode == 0)
			pg += 4;
		else
			pg ++;
    }

	for (int pg = 0; pg < spp->pgs_per_blk; ) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
			uint64_t cur_lpn = get_rmap_ent(ssd, ppa);
			if ( migrate_flag == 1 && mode == 0) {
				// if (ssd->sp.write_mode == 1 || ssd->sp.write_mode == 2) {
				// 	ssd->gc_valid_cnt ++;
				// }
				// if (ssd->sp.write_mode == 1)
				// 	ssd->gc_hotness += ssd->write_hotness[cur_lpn];
				// else if (ssd->sp.write_mode == 2){
				// 	ssd->gc_hotness += ssd->read_hotness[cur_lpn] * (ssd->avg_qlc_read_lat - NAND_SLC_READ_LAT) + ssd->write_hotness[cur_lpn] * (NAND_QLC_PROG_CL_LAT - NAND_SLC_PROG_LAT);
				// }

				if (ssd->sp.write_mode == 2) {
					double cur_read_hotness = 0;
					double cur_write_hotness = 0;
					double cur_slc_avg_hotness = 0;
					if (ruhid == 0)
						cur_slc_avg_hotness = ssd->total_slc_wa_read_hotness / ssd->slc_valid_cnt * (ssd->avg_qlc_read_lat - SLC_R) + ssd->total_slc_wa_write_hotness / ssd->slc_valid_cnt * (QLC_W - SLC_W);
					else if (ruhid == 1)
						cur_slc_avg_hotness = ssd->total_slc_ra_read_hotness / ssd->slc_valid_cnt * (ssd->avg_qlc_read_lat - SLC_R) + ssd->total_slc_ra_write_hotness / ssd->slc_valid_cnt * (QLC_W - SLC_W);
					cur_write_hotness = ssd->write_hotness[cur_lpn] * (QLC_W - SLC_W);
					cur_read_hotness = ssd->read_hotness[cur_lpn] * (ssd->avg_qlc_read_lat - SLC_R);
					if (cur_read_hotness + cur_write_hotness <= cur_slc_avg_hotness) {
						ssd->write_migrate_count ++;
						ssd->status_4_cnt ++;
						fdp_gc_write_page(ssd, ppa, rgid, 2);
						ssd->qlc_migrate_cnt ++;
						ssd->gc_to_qlc_cnt ++;
					} else {
						ssd->gc_cnt_before_update[cur_lpn] = add_hotness(ssd->gc_cnt_before_update[cur_lpn], 3);
						fdp_gc_write_page(ssd, ppa, rgid, ruhid);
						ssd->gc_to_slc_cnt ++;
					}
					// if (ssd->gc_cnt_before_update[cur_lpn] >= 2) {
					// 	ssd->write_migrate_count ++;
					// 	fdp_gc_write_page(ssd, ppa, rgid, 2);
					// 	ssd->qlc_migrate_cnt ++;
					// 	ssd->gc_to_qlc_cnt ++;
					// 	ssd->gc_cnt_before_update[cur_lpn] = 0;
					// } else {
					// 	ssd->gc_cnt_before_update[cur_lpn] = add_hotness(ssd->gc_cnt_before_update[cur_lpn], 3);
					// 	fdp_gc_write_page(ssd, ppa, rgid, ruhid);
					// 	ssd->gc_to_slc_cnt ++;
					// }
				} else if (ssd->sp.write_mode == 1) {
					// to do 修改阈值
					if (ssd->gc_cnt_before_update[cur_lpn] >= ssd->gc_cnt_before_update_thre) {
						ssd->write_migrate_count ++;
						fdp_gc_write_page(ssd, ppa, rgid, 2);
						ssd->status_4_cnt ++;
						ssd->qlc_migrate_cnt ++;
						ssd->gc_to_qlc_cnt ++;
					} else {
						ssd->gc_cnt_before_update[cur_lpn] = add_hotness(ssd->gc_cnt_before_update[cur_lpn], 3);
						fdp_gc_write_page(ssd, ppa, rgid, ruhid);
						ssd->gc_to_slc_cnt ++;
					}
				} else if (ssd->sp.write_mode == 0) {
					ssd->write_migrate_count ++;
					fdp_gc_write_page(ssd, ppa, rgid, 2);
					ssd->qlc_migrate_cnt ++;
				}
			} else
				fdp_gc_write_page(ssd, ppa, rgid, ruhid);
        }

		// SLC块只需要扫描四分之一的块
		if (mode == 0)
			pg += 4;
		else
			pg ++;
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
	ftl_log("output the info log\n");
    // char path2wa[80] = "wa.log.";
    char path2ec[80] = "ec.log";
	char path2rwtbl[80] = "rwtbl.log";
	char path2wara[80] = "wara.log";
    char path2ecinfo[80] = "ec_info.log";

	// 打印两个区域的平均pe次数
	FILE *fp_ec = fopen(path2ec, "a+");
    FILE *fp_ec_info = fopen(path2ecinfo, "w+");
	struct fdp_ru_mgmt *rum_slc = ssd->rums_slc, *rum_qlc = ssd->rums_qlc;
	struct ru *ru;
	double erase_slc_cnt = 0, erase_qlc_cnt = 0;
	fprintf(fp_ec_info, "slc:\n");
	for (int j = 0; j < rum_slc->tt_rus; j ++) {
		ru = &ssd->rus[get_slc_ru_id(ssd, j)];
		erase_slc_cnt += ru->erase;
        fprintf(fp_ec_info, "%lf\n", ru->erase);
	}
    fprintf(fp_ec_info, "qlc:\n");
	for (int j = 0; j < rum_qlc->tt_rus; j ++) {
		ru = &ssd->rus[get_qlc_ru_id(ssd, j)];
		erase_qlc_cnt += ru->erase;
        fprintf(fp_ec_info, "%lf\n", ru->erase);
	}
	double erase_ratio = erase_slc_cnt * 27.0 / (erase_qlc_cnt * 80.0);
	fprintf(fp_ec, "%lf %lf %lf\n", erase_slc_cnt, erase_qlc_cnt, erase_ratio);
	fclose(fp_ec);

	// 打印rwtbl
	FILE *fp_rwtbl = fopen(path2rwtbl, "w+");
	for (int i = 0; i < ssd->sp.tt_pgs; i ++) {
		if (ssd->lpnrtbl[i] != 0 || ssd->lpnwtbl[i] != 0)
			fprintf(fp_rwtbl, "%d %d %d\n", i, ssd->lpnrtbl[i], ssd->lpnwtbl[i]);
	}
	fclose(fp_rwtbl);

	// 打印写放大
	// double wa = ((ssd->sp).pages_from_wl + (ssd->sp).pages_from_gc + (ssd->sp).pages_from_host - (ssd->sp).pages_from_wl_pre - (ssd->sp).pages_from_gc_pre - (ssd->sp).pages_from_host_pre) * 1.0 / ((ssd->sp).pages_from_host - (ssd->sp).pages_from_host_pre);
	double wa = ((ssd->sp).pages_from_wl + (ssd->sp).pages_from_gc + (ssd->sp).pages_from_host + (ssd->sp).pages_from_migrate) * 1.0 / ((ssd->sp).pages_from_host);
	double ra = ((ssd->sp).pages_from_host_read + (ssd->sp).read_retry + (ssd->sp).pages_from_gc) * 1.0 / ((ssd->sp).pages_from_host_read);
	FILE *fp_wara = fopen(path2wara, "a+");
	fprintf(fp_wara, "wa:%lf ra:%lf migrate_count:%"PRIu64" write_migrate_count:%"PRIu64" slc_wc:%"PRIu64" slc_rc:%"PRIu64" qlc_wc:%"PRIu64" qlc_rc:%"PRIu64"\n", wa, ra, ssd->migrate_count, ssd->write_migrate_count, ssd->rums_slc[0].write_cnt, ssd->rums_slc[0].read_cnt, ssd->rums_qlc[0].write_cnt, ssd->rums_qlc[0].read_cnt);
	fclose(fp_wara);
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

// migrate_flag 0表示正常GC 1表示迁移
static void erase_victim_ru(struct ssd *ssd, int victim_ru_id, int mode,  uint16_t rgid) { 
	struct ru *victim_ru = NULL;
	struct ssdparams *spp = &ssd->sp;
	struct nand_lun *lunp; 
	struct ppa ppa;
	struct fdp_ru_mgmt *rum;

	if (mode == 0)
		rum = &ssd->rums_slc[rgid];
	else
		rum = &ssd->rums_qlc[rgid];

	victim_ru = &ssd->rus[victim_ru_id];
	// NvmeRuHandle *ruh;
	// NvmeFdpEvent *e = NULL;
	int start_lunidx = rgid * RG_DEGREE;
	uint16_t ruhid;

	// 假如是主动迁移，则受害者块修改为hotless_ru并从victim_ru和full_ru中去除
	// if (migrate_flag == 2) {
	// 	ftl_log("begin migrate!\n");
	// 	if (spp->write_mode == 2 || spp->write_mode == 5) {
	// 		victim_ru = &ssd->rus[ssd->hotless_ru_id];
	// 		victim_ru_id = ssd->hotless_ru_id;
	// 	} else {
	// 		victim_ru = &ssd->rus[ssd->wr_hotless_ru_id];
	// 		victim_ru_id = ssd->wr_hotless_ru_id;
	// 	}
	// }

	// 是victim_ru
	if (victim_ru->vpc != spp->pgs_per_ru) {
		pqueue_remove(rum->victim_ru_pq, victim_ru);
		victim_ru->pos = 0;
		rum->victim_ru_cnt--;
	} else {
		// 是full_ru
		QTAILQ_REMOVE(&rum->full_ru_list, victim_ru, entry);
		rum->full_ru_cnt--;
	}
	
	int gc_pgs = 0;
	ppa.g.blk = victim_ru_id;
	ppa.g.ch = start_lunidx / spp->luns_per_ch;
	ppa.g.lun = start_lunidx % spp->luns_per_ch;
	ppa.g.pl = 0;

	victim_ru = get_ru(ssd, &ppa);

	ruhid = victim_ru->ruhid; 
	//ruh = &req->ns->endgrp->fdp.ruhs[ruhid];	
	double ru_read_hotness = 0;
	double ru_write_hotness = 0;
	if (victim_ru->mode == 0) {
		if (victim_ru->write_hotness < 0) {

		}
		ru_write_hotness = victim_ru->write_hotness * (QLC_W - SLC_W) * 4.0 / victim_ru->vpc;
		ru_read_hotness = victim_ru->read_hotness * (ssd->avg_qlc_read_lat - SLC_R) * 4.0 / victim_ru->vpc;
	}
	ftl_log("GC-ing ru:%d,ipc=%d,vpc=%d,r_hotness:%lf,w_hotness:%lf victim=%d,full=%d,free=%d,bad=%d,read=%d,write=%d,ruhid=%d\n", victim_ru->id,
              victim_ru->ipc, victim_ru->vpc, ru_read_hotness, ru_write_hotness, rum->victim_ru_cnt, rum->full_ru_cnt, rum->free_ru_cnt, rum->bad_ru_cnt, rum->read_ru_cnt, rum->write_ru_cnt, ruhid);
	
	// 最近三次的平均GC效率
	if (victim_ru->mode == 0){
		ssd->slc_gc_eff = ssd->slc_gc_eff * 2.0 / 3 + victim_ru->ipc * 1.0 / ssd->sp.pgs_per_ru / 3;
	} else {
		ssd->qlc_gc_eff = ssd->qlc_gc_eff * 2.0 / 3 + victim_ru->ipc * 1.0 / ssd->sp.pgs_per_ru / 3;
	}
	// double slc_valid = ssd->slc_gc_eff * ssd->sp.pgs_per_ru / 4;
	// ssd->gc_func_left = NAND_SLC_PROG_LAT + slc_valid / (ssd->sp.pgs_per_ru / 4 - slc_valid) * (NAND_SLC_PROG_LAT + NAND_SLC_READ_LAT);
	// if (ssd->has_do_slc_gc)
	// 	ssd->gc_func_left +=  NAND_SLC_ERASE_LAT * RG_DEGREE * 1.0 / (ssd->sp.pgs_per_ru / 4 - slc_valid);
	// double qlc_valid = ssd->qlc_gc_eff * ssd->sp.pgs_per_ru;
	// ssd->gc_func_right = NAND_QLC_PROG_L_LAT + qlc_valid / (ssd->sp.pgs_per_ru - qlc_valid) * (NAND_QLC_PROG_L_LAT + ssd->avg_qlc_read_lat);
	// if (ssd->has_do_qlc_gc)
	// 	ssd->gc_func_right += NAND_QLC_ERASE_LAT * RG_DEGREE * 1.0 / (ssd->sp.pgs_per_ru - qlc_valid); 
	// ftl_log("slc_valid:%lf qlc_valid:%lf gc_func_left:%lf gc_func_right:%lf\n", slc_valid, qlc_valid, ssd->gc_func_left, ssd->gc_func_right);

	// if (ssd->sp.write_mode == 1 || ssd->sp.write_mode == 2) {
	// 	// ssd->qlc_migrate_cnt = 0;
	// 	ssd->avg_qlc_gc_hotness = ssd->gc_valid_cnt == 0 ? 0 : ssd->gc_hotness / ssd->gc_valid_cnt;
    // 	ftl_log("avg_qlc:%lf gc_valid_cnt:%d gc_hotness:%lf\n", ssd->avg_qlc_gc_hotness, ssd->gc_valid_cnt, ssd->gc_hotness);
	// 	ssd->gc_valid_cnt = 0;
	// 	ssd->gc_hotness = 0;
	// }

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

	// 调整准入阈值和迁移阈值
	ssd->cnt_window ++;
	if (ssd->cnt_window == 3) {
		ftl_log("0:%d 1:%d 2:%d 3:%d 4:%d 5:%d\n", ssd->status_0_cnt, ssd->status_1_cnt, ssd->status_2_cnt, ssd->status_3_cnt, ssd->status_4_cnt, ssd->status_5_cnt);
		double goodness = ssd->status_0_cnt * 17.0 + ssd->status_1_cnt * 13.0 + ssd->status_2_cnt * 9.0 + ssd->status_3_cnt * 5.0 - ssd->status_4_cnt * (4.0 * ssd->gc_cnt_before_update_thre + 4.0);
		goodness /= ssd->status_0_cnt + ssd->status_1_cnt + ssd->status_2_cnt + ssd->status_3_cnt + ssd->status_4_cnt + ssd->status_5_cnt;
		if (ssd->status_0_cnt + ssd->status_1_cnt + ssd->status_2_cnt + ssd->status_3_cnt + ssd->status_4_cnt + ssd->status_5_cnt != 0) {
			ssd->goodness[ssd->write_hotness_thre] = goodness;
			ssd->gap[ssd->write_hotness_thre] = 0;
			for (int i = 0; i < 4; i ++) {
				if (i != ssd->write_hotness_thre) {
					ssd->gap[i] ++;
					if (ssd->gap[i] >= 10) {
						ssd->gap[i] = 0;
						ssd->goodness[i] = 0;
					}
				}
			}
			ftl_log("goodness:%lf\n", goodness);

			
			// 尝试调整
			double left_goodness = ssd->write_hotness_thre == 0? -1 : ssd->goodness[ssd->write_hotness_thre - 1];
			double right_goodness = ssd->write_hotness_thre == 3? -1 : ssd->goodness[ssd->write_hotness_thre + 1];
			if (goodness < left_goodness || goodness < right_goodness) {
				if (left_goodness > right_goodness) {
					ssd->write_hotness_thre --;
					ssd->gc_cnt_before_update_thre = 3;
				} else {
					ssd->write_hotness_thre ++;
					ssd->gc_cnt_before_update_thre = 3;
				}
			} else if (ssd->status_4_cnt < ssd->status_5_cnt && left_goodness == 0) {
				// 说明当前写入阈值大了，大量请求进入QLC
				ssd->write_hotness_thre --;
				ssd->gc_cnt_before_update_thre = 3;
			} else if (ssd->status_4_cnt >= ssd->status_5_cnt && right_goodness == 0) {
				// 说明当前写入阈值小了，大量冷请求进入SLC又被迁移
				ssd->write_hotness_thre ++;
				ssd->gc_cnt_before_update_thre = 3;
			} else {
				int flag = 0;
				while (flag == 0) {
					switch(ssd->gc_cnt_before_update_thre) {
						case 3 :
							if (ssd->status_4_cnt * 4 <= ssd->status_3_cnt * 17)
								flag = 1;
							break;
						case 2:
							if (ssd->status_4_cnt * 4 <= ssd->status_2_cnt * 17)
								flag = 1;
							break;
						case 1:
							if (ssd->status_4_cnt * 4 <= ssd->status_1_cnt * 17)
								flag = 1;
							break;
						case 0:
							if (ssd->status_4_cnt * 4 <= ssd->status_0_cnt * 17) {
								flag = 1;
							}
							break;
					}
					if (flag == 0) {
						ssd->gc_cnt_before_update_thre --;
						if (ssd->gc_cnt_before_update < 0) {
							ssd->write_hotness_thre ++;
							ssd->gc_cnt_before_update_thre = 3;
							flag = 1;
						}
					}	
				}
			}
			ftl_log("write_hotness_thre:%d gc_cnt_before_update_thre:%d\n", ssd->write_hotness_thre, ssd->gc_cnt_before_update_thre);
			ssd->status_0_cnt = 0;
			ssd->status_1_cnt = 0;
			ssd->status_2_cnt = 0;
			ssd->status_3_cnt = 0;
			ssd->status_4_cnt = 0;
			ssd->status_5_cnt = 0;
		}
		ssd->cnt_window = 0;
	}

	// 修改ru的相关信息
	if (mode == 0) {
		victim_ru->pe_slc += spp->gap;
		ssd->pe_slc += spp->gap;
		rum->valid_page_num -= victim_ru->vpc / 4;
	}	
	else {
		victim_ru->pe_qlc += spp->gap;
		ssd->pe_qlc += spp->gap;
		rum->valid_page_num -= victim_ru->vpc;
	}

	victim_ru->erase = victim_ru->pe_slc * spp->slc_alpha + victim_ru->pe_qlc;
	if (victim_ru->mode == 0) {
		if (victim_ru->ruhid == 0) {
			ssd->total_slc_wa_read_hotness -= victim_ru->read_hotness;
			ssd->total_slc_wa_write_hotness -= victim_ru->write_hotness;
		} else if (victim_ru->ruhid == 1) {
			ssd->total_slc_ra_read_hotness -= victim_ru->read_hotness;
			ssd->total_slc_ra_write_hotness -= victim_ru->write_hotness;
		}
		ssd->slc_valid_cnt -= victim_ru->vpc / 4;
	}
	victim_ru->write_hotness = 0;
	victim_ru->read_hotness = 0;
	victim_ru->victim_pre = 0;
	
	ftl_log("ru:%d erase_cnt:%lf\n", victim_ru->id, victim_ru->erase);

	// 统计当前有效页数
	struct ru* tmp;
	double util = 0.0;
	for (int i = 0; i < rum->tt_rus; i++) {
		if (mode == 0)
			tmp = &ssd->rus[get_slc_ru_id(ssd, i)];
		else
			tmp = &ssd->rus[get_qlc_ru_id(ssd, i)];
		util += 
		tmp->vpc;
	}

	/* reset wp of victim ru */
	victim_ru->wp.ch = start_lunidx / spp->luns_per_ch;
	victim_ru->wp.lun = start_lunidx % spp->luns_per_ch;
	victim_ru->wp.pl = 0;
	victim_ru->wp.blk = victim_ru->id;
	victim_ru->wp.pg = 0;
	victim_ru->ipc = 0;
	victim_ru->vpc = 0;
	
    // 块到达磨损上限，弃用整个超级块
	double cur_endurance = mode == 0? victim_ru->rand_rate * (double) spp->endurance_slc : victim_ru->rand_rate * (double) spp->endurance_qlc;
    if (victim_ru->erase >= cur_endurance) {
		QTAILQ_INSERT_TAIL(&rum->bad_ru_list, victim_ru, entry);
		rum->bad_ru_cnt++;
		ftl_log("Ru %d becomes bad!\n", victim_ru->id);
		int cur_slc_ru = ssd->rums_slc[0].tt_rus - ssd->rums_slc[0].bad_ru_cnt;
		int cur_qlc_ru = ssd->rums_qlc[0].tt_rus - ssd->rums_qlc[0].bad_ru_cnt;
		double eop = (cur_slc_ru / 4.0 + cur_qlc_ru) / ssd->sp.tt_rus;
		ftl_log("eop:%lf\n", eop);
		if (eop < (spp->qlc_op * 1.0 / (spp->slc_op + spp->qlc_op))) {
			output_info_log(ssd);
			ftl_err("SSD reaches its end of the life!\n");
			abort();	
		}
	} else {
		// 静态磨损均衡，把最低磨损的ru数据迁移到当前已被gc的ru中，然后低磨损ru插入free_ru_list
		// SLC区域和QLC区域单独执行静态磨损均衡
		if (ssd->sp.wl_mode == 0 || ssd->sp.wl_mode == 1 || ssd->sp.wl_mode == 2) {
			int youngest_ru_id = -1;
			double youngest_ru_erase = 1000000;
			double erase_sum = 0; // 磨损总次数
			int wl_rus = 0; // 磨损ru数量
			struct ru *cur_ru;

			int endurance = (mode == 0) ? spp->endurance_slc : spp->endurance_qlc;
			// 只在victim_ru所在的区域执行静态磨损均衡
			for (int i = 0; i < rum->tt_rus; i++) {
				if (mode == 0)
					cur_ru = &ssd->rus[get_slc_ru_id(ssd, i)];
				else
					cur_ru = &ssd->rus[get_qlc_ru_id(ssd, i)];
				// 统计没有坏的ru的总磨损次数
				if ((cur_ru->erase < cur_ru->rand_rate * (double)endurance)) {
					wl_rus++;
					erase_sum += cur_ru->erase;
				}
				// 找到磨损次数最少的ru
				if ((cur_ru->erase < youngest_ru_erase)) {
					youngest_ru_id = cur_ru->id;
					youngest_ru_erase = cur_ru->erase;
				}
			}

			if (youngest_ru_id != -1 && youngest_ru_id != victim_ru->id) { 
				double threshold = erase_sum / wl_rus / 2 + endurance / 3; // 交换阈值（热ru磨损次数大于平均磨损次数+...）
				if (victim_ru->erase > threshold) {
					struct nand_block *block_cold = NULL;
					int counter = 0;
					for (int ch = 0; ch < spp->nchs; ch++) {
						for (int lun = 0; lun < spp->luns_per_ch; lun++) {
							// 读冷ru中的所有数据
							ppa.g.ch = ch;
							ppa.g.lun = lun;
							ppa.g.pl = 0;
							lunp = get_lun(ssd, &ppa);
							ppa.g.blk = youngest_ru_id;
							block_cold = get_blk(ssd, &ppa);
							int block_cold_vpc = block_cold->vpc;
							(ssd->sp).pages_from_wl += block_cold_vpc;
							counter = block_cold_vpc;
							if (spp->enable_gc_delay) {
								struct nand_cmd gcr;
								gcr.type = WL_IO;
								if (mode == 0)
									gcr.cmd = NAND_SLC_READ;
								else {
									gcr.cmd = NAND_QLC_READ;
								}
								gcr.stime = 0;
								while (counter > 0) {
									ssd_advance_status(ssd, &ppa, &gcr);
									lunp->gc_endtime = lunp->next_lun_avail_time;
									if (mode == 0)
										counter-=4;
									else
										counter--;
								}
							}

							// 迁移到刚gc的ru中
							ppa.g.blk = victim_ru->id;
							counter = block_cold_vpc;
							if (ssd->sp.enable_gc_delay) {
								struct nand_cmd gcw;
								gcw.type = WL_IO;
								if (mode == 0)
									gcw.cmd = NAND_SLC_PROG;
								else {
									gcw.cmd = NAND_QLC_PROG;
								}
								gcw.stime = 0;
								while (counter > 0) {
									ssd_advance_status(ssd, &ppa, &gcw);
									lunp->gc_endtime = lunp->next_lun_avail_time;
									if (mode == 0)
										counter-=4;
									else
										counter--;
								}
							}

							// 擦除冷ru
							ppa.g.blk = youngest_ru_id;
							if (spp->enable_gc_delay) {
								struct nand_cmd gce;
								gce.type = WL_IO;
								if (mode == 0)
									gce.cmd = NAND_SLC_ERASE;
								else 
									gce.cmd = NAND_QLC_ERASE;
								gce.stime = 0;
								ssd_advance_status(ssd, &ppa, &gce);
								lunp->gc_endtime = lunp->next_lun_avail_time;
							}
						}
					}
					if (mode == 0) {
						ssd->rus[youngest_ru_id].pe_slc += spp->gap;
					} else {
						ssd->rus[youngest_ru_id].pe_qlc += spp->gap;
					}
					youngest_ru_erase = ssd->rus[youngest_ru_id].pe_slc * spp->slc_alpha + ssd->rus[youngest_ru_id].pe_qlc;
					ftl_log("WL complete between ru %d (ec = %lf) and ru %d (ec = %lf)\n", victim_ru->id, victim_ru->erase, youngest_ru_id, youngest_ru_erase);
					// 只是模拟了时延，数据实质上没有交换，后续冷ru上的数据读还是会定位到冷ru上来，因此交换erase_cnt信息
					ssd->rus[youngest_ru_id].erase = victim_ru->erase;
					victim_ru->erase = youngest_ru_erase;
				}
			}
		}

		// wl_mode = 2 表明执行两个区域全局的静态磨损均衡，带上块的动态转化
		// 受害块得能参与动态转化
		if (ssd->sp.wl_mode == 2 && victim_ru->erase < spp->endurance_qlc && victim_ru->mode == 0) {
			int youngest_ru_id = -1;
			double youngest_ru_erase = 1000000;
			double erase_sum = 0; // 磨损总次数
			int wl_rus = 0; // 磨损ru数量
			struct ru *cur_ru;

			// 全局寻找
			for (int i = 0; i < ssd->sp.tt_rus; i++) {
				cur_ru = &ssd->rus[i];
				// 统计能参与转化ru的总磨损次数
				if (cur_ru->erase < spp->endurance_qlc) {
					wl_rus++;
					erase_sum += cur_ru->erase;
				}
				// 找到磨损次数最少的ru
				if ((cur_ru->erase < youngest_ru_erase)) {
					youngest_ru_id = cur_ru->id;
					youngest_ru_erase = cur_ru->erase;
				}
			}
			ftl_log("youngest_ru:%d erase:%lf\n", youngest_ru_id, youngest_ru_erase);
			// 受害者块在slc区 youngest块在qlc区时才进行交换
			if (youngest_ru_id != -1 && youngest_ru_id != victim_ru->id && ssd->rus[youngest_ru_id].mode == 1) { 
				double threshold = erase_sum / wl_rus / 2 + spp->endurance_qlc / 3; // 交换阈值
				ftl_log("threshold:%lf erase:%lf\n", threshold, victim_ru->erase);
				if (victim_ru->erase > threshold) {
					struct nand_block *block_cold = NULL;
					int counter = 0;
					for (int ch = 0; ch < spp->nchs; ch++) {
						for (int lun = 0; lun < spp->luns_per_ch; lun++) {
							// 读冷ru中的所有数据
							ppa.g.ch = ch;
							ppa.g.lun = lun;
							ppa.g.pl = 0;
							lunp = get_lun(ssd, &ppa);
							ppa.g.blk = youngest_ru_id;
							block_cold = get_blk(ssd, &ppa);
							int block_cold_vpc = block_cold->vpc;
							(ssd->sp).pages_from_wl += block_cold_vpc;
							counter = block_cold_vpc;
							if (spp->enable_gc_delay) {
								struct nand_cmd gcr;
								gcr.type = WL_IO;
								gcr.cmd = NAND_QLC_READ;
								gcr.stime = 0;
								while (counter > 0) {
									ssd_advance_status(ssd, &ppa, &gcr);
									lunp->gc_endtime = lunp->next_lun_avail_time;
									counter--;
								}
							}

							// 迁移到刚gc的ru中
							ppa.g.blk = victim_ru->id;
							counter = block_cold_vpc;
							if (ssd->sp.enable_gc_delay) {
								struct nand_cmd gcw;
								gcw.type = WL_IO;
								gcw.cmd = NAND_QLC_PROG;
								gcw.stime = 0;
								while (counter > 0) {
									ssd_advance_status(ssd, &ppa, &gcw);
									lunp->gc_endtime = lunp->next_lun_avail_time;
									counter--;
								}
							}

							// 擦除冷ru
							ppa.g.blk = youngest_ru_id;
							if (spp->enable_gc_delay) {
								struct nand_cmd gce;
								gce.type = WL_IO;
								gce.cmd = NAND_QLC_ERASE;
								gce.stime = 0;
								ssd_advance_status(ssd, &ppa, &gce);
								lunp->gc_endtime = lunp->next_lun_avail_time;
							}
						}
					}
					ssd->rus[youngest_ru_id].pe_qlc += spp->gap;
					youngest_ru_erase = ssd->rus[youngest_ru_id].pe_slc * spp->slc_alpha + ssd->rus[youngest_ru_id].pe_qlc;
					int youngest_ru_peslc = ssd->rus[youngest_ru_id].pe_slc;
					int youngest_ru_peqlc = ssd->rus[youngest_ru_id].pe_qlc;

					ftl_log("global WL complete between ru %d (ec = %lf) and ru %d (ec = %lf)\n", victim_ru->id, victim_ru->erase, youngest_ru_id, youngest_ru_erase);
					// 只是模拟了时延，数据实质上没有交换，后续冷ru上的数据读还是会定位到冷ru上来，因此交换erase_cnt信息
					ssd->rus[youngest_ru_id].erase = victim_ru->erase;
					ssd->rus[youngest_ru_id].pe_qlc = victim_ru->pe_qlc;
					ssd->rus[youngest_ru_id].pe_slc = victim_ru->pe_slc;
					//ssd->rus[youngest_ru_id].mode = 1;
					victim_ru->erase = youngest_ru_erase;
					victim_ru->pe_slc = youngest_ru_peslc;
					victim_ru->pe_qlc = youngest_ru_peqlc;
					//victim_ru->mode = 0;
				}
			}
		}
		
		if (victim_ru->mode == 0 && victim_ru->ruhid == 0)
			rum->write_ru_cnt --;
		else if (victim_ru->mode == 0 && victim_ru->ruhid == 1)
			rum->read_ru_cnt --;
		if (rum->read_ru_cnt < ssd->sp.ra_max_cnt && ssd->ra_full_flag == 1) {
			ftl_log("ra_full_flag:%d\n", ssd->ra_full_flag);
			ssd->ra_full_flag = 2;
		}
    	QTAILQ_INSERT_TAIL(&rum->free_ru_list, victim_ru, entry);
		rum->free_ru_cnt++;
    }
	return;
}

static int do_fdp_gc(struct ssd *ssd, uint16_t rgid, bool force, int gc_flag)
{
	struct ru *victim_ru_slc, *victim_ru_qlc = NULL;
	if (gc_flag >= 2) {
    	victim_ru_qlc = select_victim_ru_qlc(ssd, force, rgid);
		if(victim_ru_qlc) {;
			erase_victim_ru(ssd, victim_ru_qlc->id, 1, rgid);
		}
	}
	if (gc_flag == 3 || gc_flag == 1) {
		victim_ru_slc = select_victim_ru_slc(ssd, force, rgid);
		if(victim_ru_slc) {
			erase_victim_ru(ssd, victim_ru_slc->id, 0, rgid);
		}
	}
    if (!victim_ru_qlc && !victim_ru_slc) {
        return -1;
    }

    return 0;
}

// 只修改映射，不模拟时延
static void ssd_pre_read(struct ssd *ssd, uint64_t lpn, int page_num) {
	int ruhid = 2;

	// double cur_slc_avg_hotness = 0;
	// if (ssd->sp.write_mode == 0) {
	// 	if (ssd->v_write <= ssd->v_gc) {
	// 		ruhid = 0;
	// 	} else {
	// 		if (page_num < 16) {
	// 			ruhid = 0;
	// 		}
	// 	}
	// }
	
	// if (ssd->sp.write_mode == 1) {
	// 	// if (ssd->v_write <= ssd->v_gc)
	// 	// 	ruhid = 0;
	// 	// else {
	// 		cur_slc_avg_hotness =  ssd->slc_valid_cnt == 0 ? 0 : ssd->total_slc_write_hotness / ssd->slc_valid_cnt;
	// 		if (ssd->write_hotness[lpn] >= cur_slc_avg_hotness)
	// 			ruhid = 0;
	// 	// }
	// }

	// if (ssd->sp.write_mode == 2) {
	// 	cur_slc_avg_hotness =  ssd->slc_valid_cnt == 0 ? 0 : ssd->total_slc_read_hotness  / ssd->slc_valid_cnt * (ssd->avg_qlc_read_lat - NAND_SLC_READ_LAT) + ssd->total_slc_write_hotness / ssd->slc_valid_cnt * (NAND_QLC_PROG_CL_LAT - NAND_SLC_PROG_LAT);
	// 	double cur_hotness = ssd->read_hotness[lpn] * (ssd->avg_qlc_read_lat - NAND_SLC_READ_LAT) + ssd->write_hotness[lpn] * (NAND_QLC_PROG_CL_LAT - NAND_SLC_PROG_LAT);
	// 	// if (ssd->v_write <= ssd->v_gc) {
	// 	// 	ruhid = 0;
	// 	// } else {
	// 		if (cur_hotness >= 2.0 * cur_slc_avg_hotness)
	// 			ruhid = 0;
	// 	// }
	// }

	int mode = get_ruh_mode(ssd, ruhid);
	struct ppa ppa = fdp_get_new_page(ssd, 0, 0, ruhid, false, mode);

	/* update maptbl */
	set_maptbl_ent(ssd, lpn, &ppa);
	/* update rmap */
	set_rmap_ent(ssd, lpn, &ppa);

	mark_page_valid(ssd, &ppa);

	/* need to advance the write pointer here */
	ssd_advance_fdp_write_pointer(ssd, 0, 0, ruhid, false, mode);
	return;
	// } else {
	// 	int gc_flag = 0;
	// 	int r;
	// 	/* perform GC here until !should_fdp_gc(ssd, rgid) */
	// 	while ((gc_flag = should_fdp_gc_high(ssd, 0))) {
	// 		r = do_fdp_gc(ssd, 0, true, gc_flag);
	// 		if (r == -1)
	// 			break;
	// 	}
	// 	int ruhid;
	// 	uint64_t avg_qlc_read_lat = (NAND_QLC_READ_CL_LAT + NAND_QLC_READ_L_LAT + NAND_QLC_READ_CU_LAT * ssd->age + NAND_QLC_READ_U_LAT * ssd->age) / 4;
	// 	double cur_hotness = (avg_qlc_read_lat - NAND_SLC_READ_LAT);
	// 	if (cur_hotness > ssd->hotless_ru_hotness + NAND_SLC_PROG_LAT + NAND_SLC_READ_LAT)
	// 		ruhid = 1;
	// 	else
	// 		ruhid = 3;

	// 	int mode = get_ruh_mode(ssd, ruhid);
	// 	struct ppa ppa = fdp_get_new_page(ssd, 0, 0, ruhid, false, mode);

	// 	/* update maptbl */
	// 	set_maptbl_ent(ssd, lpn, &ppa);
	// 	/* update rmap */
	// 	set_rmap_ent(ssd, lpn, &ppa);

	// 	mark_page_valid(ssd, &ppa);

	// 	/* need to advance the write pointer here */
	// 	ssd_advance_fdp_write_pointer(ssd, 0, 0, ruhid, false, mode);
	// 	return;
	// }
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
		ssd->lpnrtbl[lpn] ++;

		int hit_read = lRUCacheGet(read_buffer, lpn);
		if (hit_read != -1) {
			sublat = 1000;
			continue;
		}

        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
			// 把读取的数据提前写入
			int gc_flag = 0;
			int r = 0;
			/* perform GC here until !should_fdp_gc(ssd, rgid) */
			while ((gc_flag = should_fdp_gc_high(ssd, 0))) {
				r = do_fdp_gc(ssd, 0, true, gc_flag);
				if (r == -1)
					break;
			}
            ssd_pre_read(ssd, lpn, end_lpn - start_lpn + 1);
        }
		// 重新获取
		ppa = get_maptbl_ent(ssd, lpn);
		
        struct nand_cmd srd;
        srd.type = USER_IO;

		struct ru *cur_ru = get_ru(ssd, &ppa);
		double pre_read_hotness = ssd->read_hotness[lpn];
		ssd->read_hotness[lpn] = add_hotness(ssd->read_hotness[lpn], 15);
		cur_ru->read_hotness = cur_ru->read_hotness + ssd->read_hotness[lpn] - pre_read_hotness;
		if (cur_ru->mode == 0) {
			if (cur_ru->ruhid == 0)
				ssd->total_slc_wa_read_hotness = ssd->total_slc_wa_read_hotness + ssd->read_hotness[lpn] - pre_read_hotness;
			else if (cur_ru->ruhid == 1)
				ssd->total_slc_ra_read_hotness = ssd->total_slc_ra_read_hotness + ssd->read_hotness[lpn] - pre_read_hotness;
		}
		cur_ru->victim_pre = get_ru_victim_pre(ssd, cur_ru);

		int mode = cur_ru->mode;
		
		if (mode == 0) {
			srd.cmd = NAND_SLC_READ;
			ssd->rums_slc[0].read_cnt ++;
		}
		else {
			srd.cmd = NAND_QLC_READ;
			ssd->rums_qlc[0].high_read_cnt ++;
			ssd->rums_qlc[0].read_cnt ++;
		}
		
		// 未命中则写入读buffer, 读buffer驱逐的页不用写入闪存
		lRUCachePut(read_buffer, lpn, 0);

        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
		sublat += 1000;
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }
    return maxlat; 
}

static uint64_t ssd_write_flush(struct ssd *ssd, NvmeRequest *req) {
	uint64_t maxlat = 0;
	uint64_t curlat = 0;
	struct ppa ppa;
	struct ssdparams *spp = &ssd->sp;
	int ruhid = 0;

	int r = 0;
	int gc_flag = 0;
	/* perform GC here until !should_fdp_gc(ssd, rgid) */
	while ((gc_flag = should_fdp_gc_high(ssd, 0))) {
		r = do_fdp_gc(ssd, 0, true, gc_flag);
		if (r == -1)
			break;
	}
	
	if (0) {
		while (should_gc_high(ssd)) {
		r = do_gc(ssd, true);
		if (r == -1)
			break;
		}
	}
	
	for (int i = 0; i < RG_DEGREE; i++) {
		DLinkedNode *tail = removeTail(write_buffer);
		uint64_t lpn = tail->lpn;

		// 释放空间
		write_buffer->cache[tail->lpn] = NULL;
		write_buffer->size--;
		free(tail);

		int write_flag = 1;
        ppa = get_maptbl_ent(ssd, lpn);
	
		int ppa_map_flag = 0;
        if (mapped_ppa(&ppa)) {
			if (get_ru(ssd, &ppa)->mode == 0 && get_ru(ssd, &ppa)->ruhid == 0) {
				ppa_map_flag = 1;
				if (ssd->gc_cnt_before_update[lpn] == 0) {
					ssd->status_0_cnt ++;
				} else if (ssd->gc_cnt_before_update[lpn] == 1) {
					ssd->status_1_cnt ++;
				} else if (ssd->gc_cnt_before_update[lpn] == 2) {
					ssd->status_2_cnt ++;
				} else if (ssd->gc_cnt_before_update[lpn] == 3) {
					ssd->status_3_cnt ++;
				}
			}
			else if (get_ru(ssd, &ppa)->mode == 0  && get_ru(ssd, &ppa)->ruhid == 1)
				ppa_map_flag = 2;
            /* update old page information first */
			uint16_t old_rgid = (ppa.g.ch * spp->luns_per_ch + ppa.g.lun) / RG_DEGREE;
			mark_page_invalid(ssd, &ppa, old_rgid); 
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
			ssd->gc_cnt[lpn] = 0;
        }

		// 统计热度
		if (spp->write_mode == 1) {
			if (ppa_map_flag == 1 || ssd->write_hotness[lpn] >= ssd->write_hotness_thre) {
				write_flag = 0;
			} else {
				write_flag = 1;
			}
		}

		ssd->lpnwtbl[lpn]++;
		ssd->write_hotness[lpn] = add_hotness(ssd->write_hotness[lpn], 3);
		ssd->gc_cnt_before_update[lpn] = 0;

		// VIS工作
		if (spp->write_mode == 0) {
			if (ssd->v_write <= ssd->v_gc) {
				write_flag = 0;
			} else {
				if (ppa_map_flag == 1) {
					write_flag = 0;
				} else {
					write_flag = 1;
				}
			}
		}
		
		if (spp->write_mode == 4) {
			write_flag = 1;
		}

		double cur_slc_wa_avg_hotness = 0;
		//double cur_slc_ra_avg_hotness = 0;
		double cur_write_hotness = 0;
		double cur_read_hotness = 0;
	
		if (spp->write_mode == 2) {
			cur_slc_wa_avg_hotness = ssd->slc_valid_cnt == 0 ? 0 : ssd->total_slc_wa_read_hotness / ssd->slc_valid_cnt * (ssd->avg_qlc_read_lat - SLC_R) + ssd->total_slc_wa_write_hotness / ssd->slc_valid_cnt * (QLC_W - SLC_W);
			//cur_slc_ra_avg_hotness = ssd->slc_valid_cnt == 0 ? 0 : ssd->total_slc_ra_read_hotness / ssd->slc_valid_cnt * (ssd->avg_qlc_read_lat - SLC_R) + ssd->total_slc_ra_write_hotness / ssd->slc_valid_cnt * (QLC_W - SLC_W);
			cur_write_hotness = ssd->write_hotness[lpn] * (QLC_W - SLC_W);
			cur_read_hotness = ssd->read_hotness[lpn] * (ssd->avg_qlc_read_lat - SLC_R);

			if (ppa_map_flag == 1) {
				write_flag = 0;
				ruhid = 0;
			} else if (ppa_map_flag == 2){
				write_flag = 0;
				ruhid = 1;
			// } else if (cur_read_hotness + cur_write_hotness >= cur_slc_ra_avg_hotness + SLC_R + SLC_W && cur_read_hotness > cur_write_hotness) {
			// 	write_flag = 0;
			// 	ruhid = 1;
			} else if (ssd->slc_gc_eff >= ssd->sp.util_ratio_high) {
				write_flag = 0;
				ruhid = 0;
			} else if (cur_write_hotness + cur_read_hotness >= cur_slc_wa_avg_hotness + QLC_W - SLC_W) {
				write_flag = 0;
				ruhid = 0;
				// }
			}
		}

		if (write_flag) {
			ruhid = 2;
			ssd->status_5_cnt ++;
		}

		if (ruhid == 2)
			ssd->rums_qlc[0].write_cnt ++;
		else {
			ssd->rums_slc[0].write_cnt ++;
			if (spp->write_mode == 0)
				ssd->slc_write_cnt ++;
		}

		//ftl_log("len:%d, ruhid:%d\n", len, ruhid);

        /* new write */
		int mode = get_ruh_mode(ssd, ruhid);
		ppa = fdp_get_new_page(ssd, 0, 0, ruhid, false, mode);

        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
		ssd_advance_fdp_write_pointer(ssd, 0, 0, ruhid, false, mode);


        struct nand_cmd swr;
        swr.type = USER_IO;

		if (mode == 0)
			swr.cmd = NAND_SLC_PROG;
		else {
            swr.cmd = NAND_QLC_PROG;
		}

        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
	}
	maxlat = (curlat > maxlat) ? curlat : maxlat;
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
	
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    } 

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		int hit_write = lRUCacheGet(write_buffer, lpn);
		if (hit_write != -1) {
			curlat = 1000;
        	maxlat = (curlat > maxlat)? curlat : maxlat;
			continue;
		}
		
		curlat = 1000;
		if (write_buffer->size + 1 > write_buffer->capacity) {
			// 写buffer满了，把buffer中的数据写入闪存
			curlat += ssd_write_flush(ssd, req);
		}
		lRUCachePut(write_buffer, lpn, 0);
		maxlat = (curlat > maxlat)? curlat : maxlat;
    }

    return maxlat;
}

//预先填充QLC区域
static void ssd_aged(struct ssd *ssd, double age_rate) {
	uint64_t tt_lpn = 30720 * 256;
	uint64_t start_lpn = 0;
	uint64_t end_lpn = age_rate * tt_lpn;
	// ftl_log("start_lpn:%"PRIu64" end_lpn:%"PRIu64"\n", start_lpn, end_lpn);
	struct ppa ppa;
	for (int i = 0; i < 1; i ++) {
		for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
			ppa = get_maptbl_ent(ssd, lpn);
			if (mapped_ppa(&ppa)) {
				mark_page_invalid(ssd, &ppa, 0); 
				set_rmap_ent(ssd, INVALID_LPN, &ppa);
			}

			int ruhid = 3;
			/* new write */
			int mode = get_ruh_mode(ssd, ruhid);
			ppa = fdp_get_new_page(ssd, 0, 0, ruhid, false, mode);

			/* update maptbl */
			set_maptbl_ent(ssd, lpn, &ppa);
			/* update rmap */
			set_rmap_ent(ssd, lpn, &ppa);

			mark_page_valid(ssd, &ppa);

			/* need to advance the write pointer here */
			ssd_advance_fdp_write_pointer(ssd, 0, 0, ruhid, false, mode);
		}
	}
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
	
	uint64_t start_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	uint64_t start_time2 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	//uint64_t start_time3 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	uint64_t cur_time;

	// 每一秒更新热度
	uint64_t time_gap = 50000000;
	uint64_t time_gap2 = 2000000000;
	//uint64_t time_gap3 = 100000000;
    while (1) {
		cur_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
		if (ssd->sp.write_mode == 0 && cur_time - start_time >= time_gap) {
			ssd->v_write = ssd->slc_write_cnt * 1.0;
			ssd->v_gc = ssd->qlc_migrate_cnt * 1.0;
			//ftl_log("v_write:%lf v_gc:%lf\n", ssd->v_write, ssd->v_gc);
			ssd->slc_write_cnt = 0;
			ssd->qlc_migrate_cnt = 0;
			start_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
		}

		// if ((ssd->sp.write_mode == 1 || ssd->sp.write_mode == 2) && cur_time - start_time3 >= time_gap3) {
		// 	ssd->avg_qlc_gc_hotness = ssd->qlc_migrate_cnt == 0 ? ssd->avg_qlc_gc_hotness : ssd->qlc_migrate_hotness / ssd->qlc_migrate_cnt;
		// 	ssd->gc_ratio = ssd->gc_total_cnt == 0 ? ssd->gc_ratio : (ssd->gc_total_cnt - ssd->gc_valid_cnt) * 1.0 / ssd->gc_total_cnt;
		// 	ssd->qlc_migrate_hotness = 0;
		// 	ssd->qlc_migrate_cnt = 0;
		// 	ssd->gc_total_cnt = 0;
		// 	ssd->gc_valid_cnt = 0;
		// 	ftl_log("avg_qlc_gc_hotness:%lf gc_ratio:%lf\n", ssd->avg_qlc_gc_hotness, ssd->gc_ratio);
		// 	start_time3 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
		// }

		if (cur_time - start_time2 >= time_gap2) {
			struct ru* ru;
			struct ppa ppa;
			for (int j = 0; j < ssd->sp.tt_rus; j++) {
				ru = &ssd->rus[j];
				ru->write_hotness = 0;
				ru->read_hotness = 0;
				ru->victim_pre = get_ru_victim_pre(ssd, ru);
			}

			ssd->total_slc_wa_read_hotness = 0;
			ssd->total_slc_wa_write_hotness = 0;
			ssd->total_slc_ra_read_hotness = 0;
			ssd->total_slc_ra_write_hotness = 0;


			for (int j = 0; j < ssd->sp.tt_pgs; j++) {
				ppa = get_maptbl_ent(ssd, j);
				if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
					continue;
				}
				ssd->read_hotness[j] = sub_hotness(ssd->read_hotness[j]);
				ssd->write_hotness[j] = sub_hotness(ssd->write_hotness[j]);
				ru = get_ru(ssd, &ppa);
				ru->write_hotness += ssd->write_hotness[j];
				ru->read_hotness += ssd->read_hotness[j];
				ru->victim_pre = get_ru_victim_pre(ssd, ru);
				if (ru->mode == 0 && ru->ruhid == 0) {
					ssd->total_slc_wa_read_hotness += ssd->read_hotness[j];
					ssd->total_slc_wa_write_hotness += ssd->write_hotness[j];
				} else if (ru->mode == 0 && ru->ruhid == 1) {
					ssd->total_slc_ra_read_hotness += ssd->read_hotness[j];
					ssd->total_slc_ra_write_hotness += ssd->write_hotness[j];
				}
			}

			start_time2 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
			// ftl_log("%"PRIu64"\n", start_time2 - cur_time);
		}

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
