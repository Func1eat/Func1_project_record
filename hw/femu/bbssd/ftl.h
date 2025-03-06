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
    NAND_QLC_READ = 3,
    NAND_QLC_PROG = 4,
    NAND_QLC_ERASE = 5,
    NAND_SLC_READ_LAT = 23000,
    NAND_SLC_PROG_LAT = 80000,
    NAND_SLC_ERASE_LAT = 4000000,
    NAND_QLC_READ_L_LAT = 72000,
    NAND_QLC_READ_CL_LAT = 96000,
    NAND_QLC_READ_CU_LAT = 96000,
    NAND_QLC_READ_U_LAT = 96000,
	NAND_QLC_READ_L_U_LAT = 24000,
    NAND_QLC_READ_CL_U_LAT = 48000,
    NAND_QLC_READ_CU_U_LAT = 144000,
    NAND_QLC_READ_U_U_LAT = 144000,
    NAND_QLC_PROG_L_LAT = 460000,
    NAND_QLC_PROG_CL_LAT = 460000,
    NAND_QLC_PROG_CU_LAT = 460000,
    NAND_QLC_PROG_U_LAT = 460000,
	NAND_QLC_PROG_TOTAL_LAT = 1000000,
    NAND_QLC_ERASE_LAT = 10000000,
};

enum {
    SLC_R = 1,
    SLC_W = 3,
    QLC_1_R = 3,
    QLC_2_R = 4,
    QLC_3_R = 4,
    QLC_4_R = 4,
    QLC_W = 20,
    MID_READ_RETRY = 3,
    OLD_READ_RETRY = 7,
	QLC_U_1_R = 1,
    QLC_U_2_R = 2,
    QLC_U_3_R = 6,
    QLC_U_4_R = 6,
	MID_READ_U_RETRY = 7,
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

	double util_ratio_high;
	double util_ratio_low;

	double balance_ratio;
	double unbalance_ratio;

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
    double slc_alpha;

    double op;
    int enable_swl; // 是否开启静态磨损均衡
	int enable_dwl; // 是否开启动态磨损均衡

    // ecc相关
    int ecc_corr_str;
    double epsilon;
    double alpha;
    double k;
    int gap; // 每次擦写增长的擦写次数（方便快速测试）

    // 写放大和读放大相关
    uint64_t read_retry;
    uint64_t pages_from_host;
    uint64_t pages_from_gc;
    uint64_t pages_from_wl;
    uint64_t pages_from_migrate;
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

	// ru mode 0表示按照序号将ru分为slc qlc，1表示将耐磨度低的分为slc，2表示将耐磨度高的分为slc
	int ru_mode;
	//0表示不迁移，1表示根据读取次数做迁移，2表示根据页面类型和读取次数做迁移
	int read_migration;

	// 写入时决定以何种方式进行区域选择
	int write_mode;

    // 磨损均衡方式
    int wl_mode;
    
	// 读延迟阈值，高于此阈值的数据会被迁移到slc
	uint64_t read_latency_threshold;

	// read area最大超级块数量
	int ra_max_cnt;

	int dynamic_ra_flag;

	int dynamic_tm_flag;
	
	int dynamic_tw_flag;

	// 读写buffer大小
	int write_buffer_capacity;
	int read_buffer_capacity;
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

	double erase; // 该RU的磨损度，等于pe_slc * alpha + pe_qlc, alpha为slc的擦写系数
	int pe_slc; // 在slc模式下的擦写次数
	int pe_qlc; // 在qlc模式下的擦写次数
	int mode;

	double rand_rate; // rand_rate表示该ru中每个块的擦写次数上限等于标准的endurance * rand_rate
	double wear_condition; // 当前的磨损状况，由两种模式下的擦写次数计算而来

	// 统计该ru当前的热度
	double read_hotness;
	double write_hotness; 

  double victim_pre;
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
  int read_ru_cnt;
  int write_ru_cnt;
	int ii_gc_ruid;			/* recalim unit for initially isolated gc */
	uint64_t read_cnt;
	uint64_t low_read_cnt;
	uint64_t high_read_cnt;
	uint64_t write_cnt;
	uint64_t valid_page_num;
};							

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
	struct ppa *back_maptbl;
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

	// 存放所有ru的rand_rate,方便排序
	double *rand_rate;
	int *indices; // 记录从大到小rand_rate的ru的索引
  
    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;

	// 统计每个lpn的读写次数
	int *lpnrtbl;
    int *lpnwtbl;
	
	// 统计每个lpn的当前热度
	double *read_hotness;
	double *write_hotness;
	double *gc_cnt_before_update;
	
	// 记录当前热度最低的SLC块及其热度
	int hotless_ru_id;
	double hotless_ru_hotness;

	// 记录当前只考虑写热度最低的SLC块及其热度
	int wr_hotless_ru_id;
	double wr_hotless_ru_hotness;

	// 当前寿命时期,作为热度衡量参数
	int age;

	// 记录read_req_migrate的迁移总数量
	uint64_t migrate_count;

  // 记录slc到qlc migrate的迁移总数量
	uint64_t write_migrate_count;

	// 记录当前写入判定是否进入SLC区域的请求大小阈值
	int page_size_thre;

	// 两个区域当前的磨损程度，用于计算磨损速率
	int pe_slc;
	int pe_qlc;

	// 热度比例，用于改变写入两个区域的速率
	double hot_ratio;

	double total_slc_wa_write_hotness;
	double total_slc_wa_read_hotness;
	int slc_valid_cnt;
	double total_slc_ra_write_hotness;
	double total_slc_ra_read_hotness;
	
	// int gc_total_cnt;
	// int gc_valid_cnt;
	// double gc_ratio;

	// qlc区域平均读延迟
	double avg_qlc_read_lat;

	double v_gc;
	double v_write;
	int slc_write_cnt;
  	int qlc_migrate_cnt;

  	// 当前存储的SLC区域的GC效率
	double slc_util;
	double slc_gc_eff;
	double qlc_gc_eff;
	// double gc_func_left;
	// double gc_func_right;
	// int has_do_slc_gc;
	// int has_do_qlc_gc;

	// 当前读热区已满
	int ra_full_flag;

	// 统计当前周期的正常GC数量
	int gc_to_slc_cnt;
	// 统计当前周期的迁移数量
	int gc_to_qlc_cnt;

	// 统计不同状态机在当前周期的数量
	double status_0_cnt;
	double status_00_cnt;
	double status_01_cnt;
	double status_12_cnt;
	double status_10_cnt;
	double status_23_cnt;
	double status_20_cnt;
	double status_34_cnt;
	double status_30_cnt;
	double status_4_cnt;
	double status_24_cnt;
	double status_14_cnt;
	double status_04_cnt;

	// double pre_status_0_cnt;
	// double pre_status_00_cnt;
	// double pre_status_01_cnt;
	// double pre_status_12_cnt;
	// double pre_status_10_cnt;
	// double pre_status_23_cnt;
	// double pre_status_20_cnt;
	// double pre_status_34_cnt;
	// double pre_status_30_cnt;
	// double pre_status_4_cnt;
	// double pre_status_24_cnt;
	// double pre_status_14_cnt;
	// double pre_status_04_cnt;

	// uint64_t status_0_total_cnt;
	// uint64_t status_1_total_cnt;
	// uint64_t status_2_total_cnt;
	// uint64_t status_3_total_cnt;
	// uint64_t status_4_total_cnt;
	// uint64_t status_5_total_cnt;
	// double goodness_total;

	int gc_cnt_before_update_thre;
  	int write_hotness_thre;
  	// 当前的窗口计数
	int cnt_window;
	// 记录历史goodness
	double goodness[4];
	// 记录这些阈值的间隔
	int gap[4];
	// 记录这个周期的goodness
	double cur_goodness;
	double cur_goodness1;

	// 记录当前统计读写频率的窗口
	int wr_ratio_cnt_window;
	int write_req_cnt;
	int read_req_cnt;
	double cur_wr_ratio;

	// 记录当前是否处于观察期
	int observe_flag;
	// 观察前记录的goodness
	double pre_goodeness;


	// comboftl相关参数
	int combo_write_thre;
	double *combo_gc_cnt;
	int combo_gc_cnt_thre;
	int *combo_warm_bit;
	uint64_t combo_write_req_cnt;
	// warm分区的数量
	double status_remain_0_cnt;
	double status_remain_1_cnt;
	double status_remain_2_cnt;
	double status_remain_3_cnt;
	double status_update_0_cnt;
	double status_update_1_cnt;
	double status_update_2_cnt;
	double status_update_3_cnt;
	// warm分区的热度
	double status_remain_0_hotness;
	double status_remain_1_hotness;
	double status_remain_2_hotness;
	double status_remain_3_hotness;
};

void ssd_init(FemuCtrl *n);
int get_slc_ru_id(struct ssd *ssd, int i);
int get_qlc_ru_id(struct ssd *ssd, int i);

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
