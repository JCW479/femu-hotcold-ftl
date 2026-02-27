#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"
#include <stdint.h>
#include <stdbool.h> 

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
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


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* ========= Hot / Cold 분류 관련 매크로 ========= */
/*
 * HOT_ACCESS_THRESHOLD:
 *   - 최근 윈도우 내에서 최소 몇 번 이상 쓰여야 "자주 쓰인다"로 볼지
 *
 * HOT_INTERVAL_THRESHOLD_PAGES:
 *   - host_writes 기준으로 delta가 이 값 이하이면 "짧은 interval"로 판정
 *
 * HOT_INTERVAL_CONFIRM_COUNT:
 *   - 짧은 interval이 연속 몇 번 이상 나와야 진짜 Hot이라고 확신할지
 *
 * HOT_DECAY_WINDOW_PAGES:
 *   - 이만큼 host_writes가 지나갈 때마다 access_cnt를 decay해서
 *     예전 히스토리가 너무 오래 영향 주지 않도록 함
 *
 * 값은 일단 보수적으로 잡아두고, 나중에 Zipf 분석 결과로 튜닝.
 */
#define UID_HIST_BINS                   32U   /* UID histogram bins */

/* 대략 1GiB(= 262,144 pages * 4KiB/page)마다 한 번 decay */
#define HOT_DECAY_WINDOW_PAGES   (ssd->sp.tt_pgs / 10)

/* "짧은 interval" 을 정의하는 host write 간격 (pages 단위) */
#define HOT_INTERVAL_THRESHOLD_PAGES    (64ULL)

/* window 내 최소 접근 횟수 */
#define HOT_ACCESS_THRESHOLD            (3U)

/* 짧은 interval 패턴이 몇 번 이상 반복되면 HOT 확정 */
#define HOT_INTERVAL_CONFIRM_COUNT      (2U)

/* LPN state: 최대한 단순하게 Hot / Cold 두 상태만 사용 */
typedef enum {
    LPN_STATE_COLD = 0,
    LPN_STATE_HOT  = 1,
} lpn_state_t;

/* LPN Hot/Cold와 별개로, 라인이 어떤 풀에 속하는지 */
typedef enum {
    LINE_CLASS_COLD = 0,
    LINE_CLASS_HOT  = 1,
} line_class_t;

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

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    size_t pos;
    line_class_t cls;   /* 이 라인이 Hot 풀인지 Cold 풀인지 */

    /* --- Cold Cost-Benefit GC용 메타데이터 (옵션) --- */
    uint64_t last_update_seq; /* 이 라인에 마지막으로 write가 들어온 host_writes 시퀀스 */
    double   cold_score;      /* Age × (1 - util) 형태로 계산한 점수 (Cold victim 선택용) */
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
    
    /* 레거시 리스트 (통계/호환성 유지용) */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    
    /* Hot/Cold 전용 Free Line 리스트 추가 */
    QTAILQ_HEAD(hot_free_line_list, line) hot_free_line_list;
    QTAILQ_HEAD(cold_free_line_list, line) cold_free_line_list;
    
    int tt_lines;
    int free_line_cnt;          // 레거시 (사용 안 함)
    int full_line_cnt;
    
    /* Hot/Cold 전용 카운터 */
    int hot_free_line_cnt;
    int cold_free_line_cnt;
    int hot_victim_line_cnt;    // 최적화용 (ipc > 0인 Hot 라인 수)
    int cold_victim_line_cnt;   // 최적화용 (ipc > 0인 Cold 라인 수)
};
struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;

    struct write_pointer wp_hot;  /* Hot 전용 쓰기 포인터 */
    struct write_pointer wp_cold; /* Cold 전용 쓰기 포인터 */
    
    struct line_mgmt lm;

    /* WAF 측정을 위한 통계 변수 */
    uint64_t host_writes;      // 호스트로부터 받은 쓰기 요청 (페이지 단위)
    uint64_t nand_writes;      // 실제 NAND에 쓴 페이지 수 (GC 포함)
    uint64_t gc_writes;        // GC로 인한 쓰기

    /* ===== LPN 단위 Hot/Cold 분류를 위한 메타데이터 ===== */

    /*
     * lpn_state[lpn]:
     *   - 해당 LPN이 현재 Hot인지 Cold인지
     *   - 0: COLD, 1: HOT
     */
    lpn_state_t *lpn_state;

    /*
     * lpn_access_cnt[lpn]:
     *   - 최근 window 내 접근 횟수 (decay 적용)
     *   - HOT_ACCESS_THRESHOLD 이상이면 "자주 쓰임" 후보
     */
    uint32_t *lpn_access_cnt;

    /*
     * lpn_last_write_seq[lpn]:
     *   - 마지막으로 쓰였을 때의 host_writes 값
     *   - delta = host_writes - last_seq 로 update interval 계산
     */
    uint64_t *lpn_last_write_seq;

    /*
     * lpn_short_int_cnt[lpn]:
     *   - "짧은 interval(delta <= HOT_INTERVAL_THRESHOLD_PAGES)"가
     *     연속해서 몇 번 나왔는지
     *   - HOT_INTERVAL_CONFIRM_COUNT 이상이면 진짜 Hot으로 승격하는 데 사용
     */
    uint8_t *lpn_short_int_cnt;

    /*
     * hot_cold_last_decay_seq:
     *   - 마지막으로 access_cnt decay를 수행했을 때의 host_writes 값
     *   - host_writes - last_decay_seq >= HOT_DECAY_WINDOW_PAGES 이면
     *     lpn_access_cnt 전체를 decay하는 트리거로 사용
     */
    uint64_t hot_cold_last_decay_seq;

    /*
     * uid_hist[bin]:
     *   - update interval(delta)을 log2 스케일 bin으로 매핑해서
     *     "update interval 분포(UID)"를 근사
     *   - 분석용 (선택적으로 사용할 수 있음)
     */
    uint64_t uid_hist[UID_HIST_BINS];

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
};

void ssd_init(FemuCtrl *n);
void print_waf_stats(struct ssd *ssd);

/* Hot/Cold 관련 helper 함수 프로토타입 (ftl.c에서 구현 예정) */

/* LPN이 현재 Hot인지 확인 */
bool ftl_is_lpn_hot(struct ssd *ssd, uint64_t lpn);

/* 쓰기 발생 시 LPN 메타데이터 업데이트 (access_cnt, interval, state 전이 등) */
void ftl_update_lpn_on_write(struct ssd *ssd, uint64_t lpn);

/* 필요 시 전체 LPN에 대해 access_cnt decay 수행 */
void ftl_maybe_decay_lpn_stats(struct ssd *ssd);

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