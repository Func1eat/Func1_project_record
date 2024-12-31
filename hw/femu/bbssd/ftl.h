#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"
#include <math.h>

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

enum {
    NAND_SLC_READ = 0,
    NAND_SLC_PROG = 1,
    NAND_SLC_ERASE = 2,
    NAND_QLC_READ_L = 3,
    NAND_QLC_READ_CL = 4,
    NAND_QLC_READ_CU = 5,
    NAND_QLC_READ_U = 6,
    NAND_QLC_PROG_L = 7,
    NAND_QLC_PROG_CL = 8,
    NAND_QLC_PROG_CU = 9,
    NAND_QLC_PROG_U = 10,
	NAND_QLC_PROG_TOTAL = 11,
    NAND_QLC_ERASE = 12,
    NAND_SLC_READ_LAT = 30000,
    NAND_SLC_PROG_LAT = 160000,
    NAND_SLC_ERASE_LAT = 3000000,
    NAND_QLC_READ_L_LAT = 85000,
    NAND_QLC_READ_CL_LAT = 170000,
    NAND_QLC_READ_CU_LAT = 510000,
    NAND_QLC_READ_U_LAT = 510000,
    NAND_QLC_PROG_L_LAT = 510000,
    NAND_QLC_PROG_CL_LAT = 510000,
    NAND_QLC_PROG_CU_LAT = 510000,
    NAND_QLC_PROG_U_LAT = 510000,
	NAND_QLC_PROG_TOTAL_LAT = 1000000,
    NAND_QLC_ERASE_LAT = 3500000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
	WL_IO = 2,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};

enum {						
	RU_TYPE_NORMAL = 0, // 该RU用作正常写入
	RU_TYPE_II_GC = 1, //该RU用作II GC写入
	RU_TYPE_PI_GC = 2, //该RU用作PI GC写入
};

#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
	int mode; // slc = 0  qlc = 1
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
	int gc_thres_rus_slc;
	int gc_thres_rus_qlc;
    int gc_thres_rus_high_slc;
    int gc_thres_rus_high_qlc;

	int slc_op;
	int qlc_op;

    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */
#ifdef DEVICE_UTIL_DEBUG
	int tt_valid_pgs;	
#endif

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int secs_per_ru;
    int pgs_per_ru;
    int blks_per_ru;
	int chs_per_ru;
	int luns_per_ru;
    int tt_rus;	

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */

    // 磨损相关
    int endurance_slc;
	int endurance_qlc;

    double op;
    int enable_swl; // 是否开启静态磨损均衡
	int enable_dwl; // 是否开启动态磨损均衡

    // ecc相关
    int ecc_corr_str;
    double epsilon;
    double alpha;
    double k;
    int gap; // 每次擦写增长的擦写次数（方便快速测试）

    // 写放大相关
    uint64_t read_retry;
    uint64_t pages_from_host;
    uint64_t pages_from_gc;
    uint64_t pages_from_wl;
    uint64_t pages_from_host_read;

    uint64_t read_retry_pre;
    uint64_t pages_from_host_pre;
    uint64_t pages_from_gc_pre;
    uint64_t pages_from_wl_pre;
    uint64_t pages_from_host_read_pre;
    
    uint64_t host_read_block;
    uint64_t host_write_block;

    // gc效率阈值，低于该阈值表示当前需要将数据驱逐到qlc
    double gc_slc_to_qlc_threshold;
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

typedef struct ru {		
	int id;
	struct {
		int ch;
		int lun;
		int pl;
		int blk;
		int pg;
	} wp;
	int ipc;
	int vpc;
	QTAILQ_ENTRY(ru) entry;		/* in either {free, victim, full} list */
	size_t pos;					/* position in the priority queue for victim ru */
	int ruhid;					/* needed for gc */
	int rut;					/* ru type: normal, ii_gc, pi_gc */

	int erase_cnt;
	int mode;

	double rand_rate; // rand_rate表示该ru中每个块的擦写次数上限等于标准的endurance * rand_rate
} ru; 					

struct ruh {				
	int ruht;					/* ruh type: ii_gc, pi_gc */
	int mode;                  // ruh指向的介质类型
	int* cur_ruids;
	int* pi_gc_ruids;
};						

struct fdp_ru_mgmt {	
	QTAILQ_HEAD(free_ru_list, ru) free_ru_list;
	pqueue_t *victim_ru_pq;
    //QTAILQ_HEAD(victim_blk_list, blk) victim_blk_list;
	QTAILQ_HEAD(full_ru_list, ru) full_ru_list;
	QTAILQ_HEAD(bad_ru_list, ru) bad_ru_list;
	int tt_rus;
	int free_ru_cnt;
	int victim_ru_cnt;
	int full_ru_cnt; 
	int bad_ru_cnt;
	int ii_gc_ruid;			/* recalim unit for initially isolated gc */
};							

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct line_mgmt lm;
	struct ruh *ruhtbl;			/* ruh table */	
	struct ru *rus;					
	struct fdp_ru_mgmt *rums_slc; 	/* raclaim unit managements */		
	struct fdp_ru_mgmt *rums_qlc;
	int *gc_cnt;				/* for two-level isolation gc */		
	int fdp_enabled;

	int cv_moderate;
    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
};

void ssd_init(FemuCtrl *n);

#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)


/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif
