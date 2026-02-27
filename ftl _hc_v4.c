#include "ftl.h"

//#define FEMU_DEBUG_FTL

/* ======= Forward Declarations (함수 원형 선언) ======= */

/* Hot/Cold 분류 & UID 통계 관련 */
static inline int uid_interval_to_bin(uint64_t delta);
static inline void update_lpn_stats_on_write(struct ssd *ssd, uint64_t lpn);

/* 새로 추가: 주소 체크 / free line / 파라미터 체크 */
static inline void check_addr(int a, int max);
static struct line *get_next_free_line_hot(struct ssd *ssd);
static struct line *get_next_free_line_cold(struct ssd *ssd);
static void check_params(struct ssdparams *spp);

/* FTL I/O Path */
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req);
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req);
static uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req);

/* GC & 라인 관리 */
static int do_gc(struct ssd *ssd, bool force);
// static bool ensure_free_line_hot(struct ssd *ssd);
// static bool ensure_free_line_cold(struct ssd *ssd);
static int do_gc_hot(struct ssd *ssd, bool force);
static int do_gc_cold(struct ssd *ssd, bool force);
/* 통계 출력 */
/* print_waf_stats는 ftl.h에 선언돼 있으므로 여기선 선언 X */

/* FTL 메인 쓰레드 */
static void *ftl_thread(void *arg);

/* ===== Emergency GC / Borrowing Thresholds ===== */

#define EMERGENCY_HOT_FREE_LINES        1   /* Hot 라인이 이 이하로 떨어지면 긴급 대응 */
#define EMERGENCY_COLD_FREE_LINES       1   /* Cold 라인이 이 이하로 떨어지면 긴급 대응 */
#define EMERGENCY_TOTAL_FREE_LINES      4   /* 전체 free line이 이 값 이하이면 "최악 상황"으로 간주 */

#define MAX_EMERGENCY_GC_LOOPS          8   /* 무한 루프 방지용 안전장치 */
#define MAX_CLASS_GC_LOOPS              4   /* 특정 클래스(H/C)만 대상으로 돌리는 GC 최대 횟수 */

/* ===================================================== */

static inline int total_free_lines(struct ssd *ssd)
{
    return ssd->lm.hot_free_line_cnt + ssd->lm.cold_free_line_cnt;
}

static inline bool should_gc(struct ssd *ssd)
{
    return (total_free_lines(ssd) <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (total_free_lines(ssd) <= ssd->sp.gc_thres_lines_high);
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

/* === Hot / Cold victim score 계산 함수  === */

/* Hot: invalid pages 개수(ipc)만으로 Greedy */
static inline uint64_t hot_line_score(struct ssd *ssd, struct line *line)
{
    (void)ssd;
    /* ipc가 클수록 GC 효율 ↑ */
    return (uint64_t)line->ipc;
}

/* Cold: Score = Age × ipc
 *  - Age  = (현재 host_writes - line->last_update_seq)
 *  - ipc  = invalid page count
 */
/* Cost-Benefit: Age × ipc / (vpc + ipc) */
static inline uint64_t cold_line_score(struct ssd *ssd, struct line *line)
{
    if (line->cls != LINE_CLASS_COLD) return 0;
    if (line->ipc == 0) return 0;
    if (line->last_update_seq == 0) return 0;

    uint64_t seq = ssd->host_writes;
    uint64_t age = (seq > line->last_update_seq) ? (seq - line->last_update_seq) : 0;

    if (age == 0) {
        line->cold_score = 0.0;
        return 0;
    }

    /* ⭐ Cost-Benefit 공식: Age × (1 - Utilization) */
    int total_pages = ssd->sp.pgs_per_line;
    double utilization = (double)line->vpc / total_pages;
    double benefit = 1.0 - utilization;  // Invalid 비율

    /* Score = Age × Benefit × 1000 (정수 변환) */
    uint64_t score = (uint64_t)(age * benefit * 1000.0);

    line->cold_score = (double)score;
    return score;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    /* 레거시 리스트 초기화 (통계용) */
    QTAILQ_INIT(&lm->free_line_list);
    QTAILQ_INIT(&lm->full_line_list);
    lm->free_line_cnt = 0;
    lm->full_line_cnt = 0;

    /* Hot/Cold 리스트 초기화 */
    QTAILQ_INIT(&lm->hot_free_line_list);
    QTAILQ_INIT(&lm->cold_free_line_list);

    lm->hot_free_line_cnt = 0;
    lm->cold_free_line_cnt = 0;
    lm->hot_victim_line_cnt = 0;
    lm->cold_victim_line_cnt = 0;

    /* Hot/Cold 비율: 10% Hot, 90% Cold */
    int hot_lines = (lm->tt_lines * 2) / 10;

    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->last_update_seq = 0;
        line->cold_score = 0.0;

        if (i < hot_lines) {
            line->cls = LINE_CLASS_HOT;
            QTAILQ_INSERT_TAIL(&lm->hot_free_line_list, line, entry);
            lm->hot_free_line_cnt++;
        } else {
            line->cls = LINE_CLASS_COLD;
            QTAILQ_INSERT_TAIL(&lm->cold_free_line_list, line, entry);
            lm->cold_free_line_cnt++;
        }
    }

    ftl_log("Line pool initialized: Hot=%d (%.1f%%), Cold=%d (%.1f%%)\n",
            hot_lines, (double)hot_lines / lm->tt_lines * 100.0,
            lm->tt_lines - hot_lines, 
            (double)(lm->tt_lines - hot_lines) / lm->tt_lines * 100.0);
}

static void ssd_init_one_write_pointer(struct ssd *ssd,
                                       struct write_pointer *wpp,
                                       struct line *curline)
{
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(curline);

    wpp->curline = curline;
    wpp->ch  = 0;
    wpp->lun = 0;
    wpp->pg  = 0;
    wpp->blk = curline->id;
    wpp->pl  = 0;

    /* 라인이 새로 활성화되는 시점에 Age 기준 시퀀스 기록 */
    curline->last_update_seq = ssd->host_writes;
    curline->cold_score = 0.0;

    check_addr(wpp->blk, spp->blks_per_pl);
}

static void ssd_init_write_pointers(struct ssd *ssd)
{
    struct line *hot_line  = get_next_free_line_hot(ssd);
    struct line *cold_line = get_next_free_line_cold(ssd);

    if (!hot_line || !cold_line) {
        ftl_err("Failed to initialize write pointers: not enough lines\n");
        abort();
    }

    ssd_init_one_write_pointer(ssd, &ssd->wp_hot, hot_line);
    ssd_init_one_write_pointer(ssd, &ssd->wp_cold, cold_line);
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

// static struct line *get_next_free_line(struct ssd *ssd)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     struct line *curline = NULL;

//     curline = QTAILQ_FIRST(&lm->free_line_list);
//     if (!curline) {
//         ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
//         return NULL;
//     }

//     QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
//     lm->free_line_cnt--;
//     return curline;
// }

/* Hot/Cold에서 라인 “빌려오기” (클래스 재배치) */

// /* Cold -> Hot 임시 빌려쓰기 (시나리오 A, 전략 2) */
// static struct line *borrow_line_from_cold(struct ssd *ssd)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     struct line *line = QTAILQ_FIRST(&lm->cold_free_line_list);

//     if (!line) {
//         return NULL;
//     }

//     /* Cold free 리스트에서 빼고 Hot 용도로 전환 */
//     QTAILQ_REMOVE(&lm->cold_free_line_list, line, entry);
//     lm->cold_free_line_cnt--;

//     line->cls = LINE_CLASS_HOT;

//     /*
//      * 여기서 hot_free_line_list에 다시 넣지 않고
//      * get_next_free_line_hot()의 호출자에게 바로 반환할 것이므로
//      * hot_free_line_cnt를 올리지 않음.
//      * (실제 할당은 곧바로 이 라인에서 이뤄짐)
//      */
//     ftl_log("[POOL] Borrow line %d from COLD -> HOT (cold_free=%d hot_free=%d)\n",
//             line->id, lm->cold_free_line_cnt, lm->hot_free_line_cnt);

//     return line;
// }

// /* Hot -> Cold 강등 (시나리오 B, 전략 2) */
// static struct line *borrow_line_from_hot(struct ssd *ssd)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     struct line *line = QTAILQ_FIRST(&lm->hot_free_line_list);

//     if (!line) {
//         return NULL;
//     }

//     /* Hot free 리스트에서 빼고 Cold 용도로 전환 */
//     QTAILQ_REMOVE(&lm->hot_free_line_list, line, entry);
//     lm->hot_free_line_cnt--;

//     line->cls = LINE_CLASS_COLD;

//     ftl_log("[POOL] Borrow line %d from HOT -> COLD (hot_free=%d cold_free=%d)\n",
//             line->id, lm->hot_free_line_cnt, lm->cold_free_line_cnt);

//     return line;
// }



static struct line *get_next_free_line_hot(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    /* Hot Pool에서 먼저 시도 */
    curline = QTAILQ_FIRST(&lm->hot_free_line_list);
    if (curline) {
        QTAILQ_REMOVE(&lm->hot_free_line_list, curline, entry);
        lm->hot_free_line_cnt--;
        return curline;
    }

    /* Cold에서 빌려오기 (최소 3개만 예약) */
    int min_cold_reserve = 3;
    
    if (lm->cold_free_line_cnt > min_cold_reserve) {
        curline = QTAILQ_FIRST(&lm->cold_free_line_list);
        if (curline) {
            QTAILQ_REMOVE(&lm->cold_free_line_list, curline, entry);
            lm->cold_free_line_cnt--;
            curline->cls = LINE_CLASS_HOT;
            
            ftl_log("[POOL] Borrow line %d from COLD -> HOT (cold_free=%d hot_free=%d)\n",
                    curline->id, lm->cold_free_line_cnt, lm->hot_free_line_cnt);
            
            return curline;
        }
    }

    /* Emergency GC */
    ftl_err("No free lines for HOT! Triggering emergency GC...\n");
    
    if (do_gc_hot(ssd, true) == 0) {
        curline = QTAILQ_FIRST(&lm->hot_free_line_list);
        if (curline) {
            QTAILQ_REMOVE(&lm->hot_free_line_list, curline, entry);
            lm->hot_free_line_cnt--;
            return curline;
        }
    }

    ftl_err("CRITICAL: No free lines available for HOT!\n");
    return NULL;
}

static struct line *get_next_free_line_cold(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    /* Cold Pool에서 먼저 시도 */
    curline = QTAILQ_FIRST(&lm->cold_free_line_list);
    if (curline) {
        QTAILQ_REMOVE(&lm->cold_free_line_list, curline, entry);
        lm->cold_free_line_cnt--;
        return curline;
    }

    /* Hot에서 빌려오기 (최소 3개만 예약) */
    int min_hot_reserve = 3;
    
    if (lm->hot_free_line_cnt > min_hot_reserve) {
        curline = QTAILQ_FIRST(&lm->hot_free_line_list);
        if (curline) {
            QTAILQ_REMOVE(&lm->hot_free_line_list, curline, entry);
            lm->hot_free_line_cnt--;
            curline->cls = LINE_CLASS_COLD;
            
            ftl_log("[POOL] Borrow line %d from HOT -> COLD (hot_free=%d cold_free=%d)\n",
                    curline->id, lm->hot_free_line_cnt, lm->cold_free_line_cnt);
            
            return curline;
        }
    }

    /* Emergency GC */
    ftl_err("No free lines for COLD! Triggering emergency GC...\n");
    
    if (do_gc_cold(ssd, true) == 0) {
        curline = QTAILQ_FIRST(&lm->cold_free_line_list);
        if (curline) {
            QTAILQ_REMOVE(&lm->cold_free_line_list, curline, entry);
            lm->cold_free_line_cnt--;
            return curline;
        }
    }

    ftl_err("CRITICAL: No free lines available for COLD!\n");
    return NULL;
}

/* 시나리오 C까지 포함해서 Hot 쪽 라인을 확보하려는 GC 전략 */
// static bool ensure_free_line_hot(struct ssd *ssd)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     int loops = 0;

//     /* 이미 충분하면 아무 것도 안 함 */
//     if (lm->hot_free_line_cnt > 0) {
//         return true;
//     }

//     /* 1단계: Hot 전용 GC를 몇 번 돌려서 Hot free 라인 확보 시도 */
//     while (lm->hot_free_line_cnt == 0 &&
//            loops < MAX_CLASS_GC_LOOPS) {
//         if (do_gc_hot(ssd, true) != 0) {
//             break;  /* 더 이상 Hot victim 없음 */
//         }
//         loops++;
//     }

//     if (lm->hot_free_line_cnt > 0) {
//         return true;
//     }

//     /* 2단계: Hot만으로 안 되면 "최악 상황"인지 체크해서 양쪽 모두 Aggressive GC */
//     loops = 0;
//     while (total_free_lines(ssd) <= EMERGENCY_TOTAL_FREE_LINES &&
//            loops < MAX_EMERGENCY_GC_LOOPS) {

//         int r1 = do_gc_hot(ssd, true);
//         int r2 = do_gc_cold(ssd, true);

//         if (r1 != 0 && r2 != 0) {
//             /* 양쪽 모두 더 이상 victim을 못 고르면 break */
//             break;
//         }
//         loops++;
//     }

//     return (lm->hot_free_line_cnt > 0);
// }

// /* Cold 쪽 라인을 확보하려는 GC 전략 */

// static bool ensure_free_line_cold(struct ssd *ssd)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     int loops = 0;

//     if (lm->cold_free_line_cnt > 0) {
//         return true;
//     }

//     /* 1단계: Cold 전용 GC 시도 (비효율적일 수 있지만 우선 시도) */
//     while (lm->cold_free_line_cnt == 0 &&
//            loops < MAX_CLASS_GC_LOOPS) {
//         if (do_gc_cold(ssd, true) != 0) {
//             break;
//         }
//         loops++;
//     }

//     if (lm->cold_free_line_cnt > 0) {
//         return true;
//     }

//     /* 2단계: Cold만으로 안 되면 최악 상황으로 보고 양쪽 Aggressive GC */
//     loops = 0;
//     while (total_free_lines(ssd) <= EMERGENCY_TOTAL_FREE_LINES &&
//            loops < MAX_EMERGENCY_GC_LOOPS) {

//         int r1 = do_gc_cold(ssd, true);
//         int r2 = do_gc_hot(ssd, true);

//         if (r1 != 0 && r2 != 0) {
//             break;
//         }
//         loops++;
//     }

//     return (lm->cold_free_line_cnt > 0);
// }

static void ssd_advance_write_pointer_class(struct ssd *ssd,
                                            struct write_pointer *wpp,
                                            line_class_t cls)
{
    struct ssdparams *spp = &ssd->sp;
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
                struct line *curline = wpp->curline;

                wpp->pg = 0;

                /* move current line to {victim,full} line list */
                if (curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, curline, entry);
                    lm->full_line_cnt++;
                } 
                else {
                    ftl_assert(curline->vpc >= 0 && curline->vpc < spp->pgs_per_line);
                    ftl_assert(curline->ipc > 0);
                    }

                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                if (cls == LINE_CLASS_HOT) {
                    wpp->curline = get_next_free_line_hot(ssd);
                } else {
                    wpp->curline = get_next_free_line_cold(ssd);
                }

                if (!wpp->curline) {
                    ftl_err("No free lines left for class=%d in [%s]\n",
                            cls, ssd->ssdname);
                    abort();    /* TODO: 나중에 Cold→Hot 빌려 쓰기 로직 추가 가능 */
                }

                /* 새로 활성화된 라인의 Age 기준점 설정 */
                wpp->curline->last_update_seq = ssd->host_writes;
                wpp->curline->cold_score = 0.0;
                
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);

                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static void ssd_advance_write_pointer_hot(struct ssd *ssd)
{
    ssd_advance_write_pointer_class(ssd, &ssd->wp_hot, LINE_CLASS_HOT);
}

static void ssd_advance_write_pointer_cold(struct ssd *ssd)
{
    ssd_advance_write_pointer_class(ssd, &ssd->wp_cold, LINE_CLASS_COLD);
}

static struct ppa get_new_page_from_wp(struct ssd *ssd,
                                       struct write_pointer *wpp)
{
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch  = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg  = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl  = wpp->pl;
    ftl_assert(ppa.g.pl == 0);
    return ppa;
}

static struct ppa get_new_page_hot(struct ssd *ssd)
{
    return get_new_page_from_wp(ssd, &ssd->wp_hot);
}

static struct ppa get_new_page_cold(struct ssd *ssd)
{
    return get_new_page_from_wp(ssd, &ssd->wp_cold);
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

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    spp->gc_thres_lines = (int)(spp->gc_thres_pcent * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    spp->gc_thres_lines_high = (int)( spp->gc_thres_pcent_high * spp->tt_lines);
    spp->enable_gc_delay = true;


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

    /* WAF 통계 초기화 */
    ssd->host_writes = 0;
    ssd->nand_writes = 0;
    ssd->gc_writes = 0;

    /* ===== LPN Hot/Cold 분류용 메타데이터 초기화 ===== */
    ssd->lpn_state          = g_malloc0(sizeof(lpn_state_t) * spp->tt_pgs);
    ssd->lpn_access_cnt     = g_malloc0(sizeof(uint32_t)    * spp->tt_pgs);
    ssd->lpn_last_write_seq = g_malloc0(sizeof(uint64_t)    * spp->tt_pgs);
    ssd->lpn_short_int_cnt  = g_malloc0(sizeof(uint8_t)     * spp->tt_pgs);
    ssd->hot_cold_last_decay_seq = 0;
    memset(ssd->uid_hist, 0, sizeof(ssd->uid_hist));
    /*  - g_malloc0 이라서 lpn_state 전부 0(COLD)로 초기화됨
     *  - access_cnt / last_write_seq / short_int_cnt 도 전부 0
     */

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointers(ssd);

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
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);

    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }

    /* === Victim 카운트 추적 (최적화) === */
    if (line->ipc == 0) {
        /* 첫 invalid 발생: victim 후보로 추가 */
        if (line->cls == LINE_CLASS_HOT) {
            lm->hot_victim_line_cnt++;
        } else {
            lm->cold_victim_line_cnt++;
        }
    }

    line->ipc++;
    line->vpc--;

    /* Full line이었다면 리스트 이동 */
    if (was_full_line) {
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;

    line->last_update_seq = ssd->host_writes;

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
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    bool is_hot;

    ftl_assert(valid_lpn(ssd, lpn));

    /* 이 LPN이 현재 Hot인지 보고 GC 이후에도 같은 class에 써줌 */
    is_hot = ftl_is_lpn_hot(ssd, lpn);

    if (is_hot) {
        new_ppa = get_new_page_hot(ssd);
    } else {
        new_ppa = get_new_page_cold(ssd);
    }

    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* GC로 인한 실제 NAND 쓰기 카운트 */
    ssd->nand_writes++;
    ssd->gc_writes++;

    /* Hot/Cold에 맞게 write pointer 진행 */
    if (is_hot) {
        ssd_advance_write_pointer_hot(ssd);
    } else {
        ssd_advance_write_pointer_cold(ssd);
    }

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
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

    /* === Victim 카운트 감소 === */
    if (line->ipc > 0) {
        if (line->cls == LINE_CLASS_HOT) {
            ftl_assert(lm->hot_victim_line_cnt > 0);
            lm->hot_victim_line_cnt--;
        } else {
            ftl_assert(lm->cold_victim_line_cnt > 0);
            lm->cold_victim_line_cnt--;
        }
    }

    line->ipc = 0;
    line->vpc = 0;
    line->last_update_seq = 0;
    line->cold_score = 0.0;

    /* Free list로 복귀 */
    if (line->cls == LINE_CLASS_HOT) {
        QTAILQ_INSERT_TAIL(&lm->hot_free_line_list, line, entry);
        lm->hot_free_line_cnt++;
    } else {
        QTAILQ_INSERT_TAIL(&lm->cold_free_line_list, line, entry);
        lm->cold_free_line_cnt++;
    }
}

/* Hot victim 선택: invalid가 충분히 많은 HOT 라인만 골라서 GC */
static struct line *select_victim_line_hot(struct ssd *ssd, bool force)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *best = NULL;
    uint64_t best_score = 0;

    /* 최소 invalid 페이지 수: 라인 전체의 1/8 이상 */
    int min_ipc = spp->pgs_per_line / 8;

    for (int i = 0; i < lm->tt_lines; i++) {
        struct line *line = &lm->lines[i];

        /* 현재 WP가 사용 중인 라인은 victim에서 제외 */
        if (line == ssd->wp_hot.curline || line == ssd->wp_cold.curline) {
            continue;
        }

        /* HOT 라인만 대상 */
        if (line->cls != LINE_CLASS_HOT)
            continue;

        /* invalid 하나도 없으면 스킵 */
        if (line->ipc == 0)
            continue;

        /* 강제 GC가 아닐 때는 invalid가 너무 적은 라인은 건너뜀 */
        if (!force && line->ipc < min_ipc) {
            continue;
        }

        uint64_t score = hot_line_score(ssd, line);
        if (score > best_score) {
            best_score = score;
            best = line;
        }
    }

    return best;
}

/* Cold victim 선택: Age × ipc 기반 + 최소 invalid 비율 조건 */
static struct line *select_victim_line_cold(struct ssd *ssd, bool force)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim = NULL;
    uint64_t max_score = 0;
    uint64_t current_seq = ssd->host_writes;

    /* 최소 Invalid 비율: 25% (force=true), 30% (force=false) */
    double min_invalid_ratio = force ? 0.25 : 0.30;
    uint64_t min_age_threshold = force ? 0 : (HOT_DECAY_WINDOW_PAGES / 4);

    for (int i = 0; i < lm->tt_lines; i++) {
        struct line *line = &lm->lines[i];

        if (line->cls != LINE_CLASS_COLD) continue;
        if (line->vpc == spp->pgs_per_line) continue;
        if (line->ipc == 0) continue;
        if (line->last_update_seq == 0) continue;

        /* ⭐ Invalid 비율 체크 */
        double invalid_ratio = (double)line->ipc / spp->pgs_per_line;
        if (invalid_ratio < min_invalid_ratio) continue;

        /* Age 계산 */
        uint64_t age = (current_seq > line->last_update_seq) 
                       ? (current_seq - line->last_update_seq) : 0;

        if (!force && age < min_age_threshold) continue;

        /* Cost-Benefit: Score = Age × Invalid_Ratio */
        uint64_t score = (uint64_t)(age * invalid_ratio * 1000);

        /* 조기 종료 (매우 좋은 victim) */
        if (invalid_ratio >= 0.7 && age > HOT_DECAY_WINDOW_PAGES * 5) {
            line->cold_score = (double)score;
            return line;
        }

        if (score > max_score) {
            max_score = score;
            victim = line;
        }
    }

    if (victim) {
        victim->cold_score = (double)max_score;
    }

    return victim;
}

static int do_gc_for_line(struct ssd *ssd, struct line *victim_line)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,hot_victim=%d,cold_victim=%d,"
              "full=%d,free_total=%d\n",
              ppa.g.blk, victim_line->ipc,
              ssd->lm.hot_victim_line_cnt,
              ssd->lm.cold_victim_line_cnt,
              ssd->lm.full_line_cnt,
              total_free_lines(ssd));

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
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status (Hot/Cold free list로 복귀) */
    mark_line_free(ssd, &ppa);

    return 0;
}

static int do_gc_hot(struct ssd *ssd, bool force)
{
    struct line *victim_line = select_victim_line_hot(ssd, force);
    if (!victim_line) {
        return -1;
    }
    ftl_log("[GC] HOT: victim line=%d ipc=%d vpc=%d\n", victim_line->id, victim_line->ipc, victim_line->vpc);
    return do_gc_for_line(ssd, victim_line);
}

static int do_gc_cold(struct ssd *ssd, bool force)
{
    struct line *victim_line = select_victim_line_cold(ssd, force);
    if (!victim_line) {
        return -1;
    }
    ftl_log("[GC] COLD: victim line=%d ipc=%d vpc=%d\n", victim_line->id, victim_line->ipc, victim_line->vpc);
    return do_gc_for_line(ssd, victim_line);
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;

    int hot_free  = lm->hot_free_line_cnt;
    int cold_free = lm->cold_free_line_cnt;

    /* 아주 단순한 정책:
     *  - Hot 풀이 더 위험하게 부족하면 Hot GC 우선
     *  - 아니면 Cold GC 실행
     *  (나중에: 둘 다 위험하면 기준 낮춰 양쪽 다 GC 같은 정책을 여기에.)
     */
    if (hot_free <= cold_free) {
        if (do_gc_hot(ssd, force) == 0) {
            return 0;
        }
        return do_gc_cold(ssd, force);
    } else {
        if (do_gc_cold(ssd, force) == 0) {
            return 0;
        }
        return do_gc_hot(ssd, force);
    }
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (start_lpn >= spp->tt_pgs) {
    ftl_err("IO beyond device capacity: start_lpn=%"PRIu64", tt_pgs=%d\n",
            start_lpn, spp->tt_pgs);
    return 0;
    }

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("Clamping IO: start_lpn=%"PRIu64", end_lpn=%"PRIu64", tt_pgs=%d\n",
                start_lpn, end_lpn, spp->tt_pgs);
        end_lpn = spp->tt_pgs - 1;
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
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    
    if (start_lpn >= spp->tt_pgs) {
    ftl_err("IO beyond device capacity: start_lpn=%"PRIu64", tt_pgs=%d\n",
            start_lpn, spp->tt_pgs);
    return 0;
    }

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("Clamping IO: start_lpn=%"PRIu64", end_lpn=%"PRIu64", tt_pgs=%d\n",
                start_lpn, end_lpn, spp->tt_pgs);
        end_lpn = spp->tt_pgs - 1;
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        /* ==== 논리 쓰기 시퀀스 증가 & decay 체크 ==== */
        ssd->host_writes++;                 // LPN 하나당 host write 1페이지
        ftl_maybe_decay_lpn_stats(ssd);     // 필요하면 access_cnt decay

        /* ==== LPN Hot/Cold 메타데이터 업데이트 ==== */
        ftl_update_lpn_on_write(ssd, lpn);
        /*  - 여기서 lpn_state[lpn]이 HOT 또는 COLD로 정리됨
         *  - 다음 단계에서 Hot/Cold 라인 풀로 라우팅할 때 사용할 수 있음
         */

        bool is_hot = ftl_is_lpn_hot(ssd, lpn);

        /* ==== 기존 FTL 동작 (물리 페이지 할당/갱신) ==== */
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write: Hot/Cold 전용 write pointer에서 페이지 할당 */
        if (is_hot) {
            ppa = get_new_page_hot(ssd);
        } else {
            ppa = get_new_page_cold(ssd);
        }

        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* NAND 쓰기 카운트 (host_writes는 위에서 이미 증가됨) */
        ssd->nand_writes++;

        /* write pointer 진행: Hot/Cold에 따라 다른 포인터 */
        if (is_hot) {
            ssd_advance_write_pointer_hot(ssd);
        } else {
            ssd_advance_write_pointer_cold(ssd);
        }

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    // uint32_t attributes = req->dsm_attributes;
    
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;
    int total_out_of_bounds = 0;
    
    if (!ranges || nr_ranges <= 0) {
        printf("TRIM: Invalid ranges or count\n");
        return 0;
    }
    
    // printf("TRIM: Processing %d ranges (attributes=0x%x)\n", nr_ranges, attributes);
    
    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t slba = le64_to_cpu(ranges[range_idx].slba);
        uint32_t nlb = le32_to_cpu(ranges[range_idx].nlb);
        // uint32_t cattr = le32_to_cpu(ranges[range_idx].cattr);
        
        uint64_t start_lpn = slba / spp->secs_per_pg;
        uint64_t end_lpn = (slba + nlb - 1) / spp->secs_per_pg;
        uint64_t lpn;
        struct ppa ppa;
        int trimmed_pages = 0;
        int already_invalid = 0;

        // ftl_debug("TRIM Range %d: LBA %lu + %u sectors, LPN range %lu-%lu (%lu pages), cattr=0x%x\n", 
        //        range_idx, slba, nlb, start_lpn, end_lpn, end_lpn - start_lpn + 1, cattr);

        // Boundary check
        if (end_lpn >= spp->tt_pgs) {
            ftl_err("TRIM: Range %d exceeds FTL capacity - end_lpn=%lu, tt_pgs=%d\n", 
                   range_idx, end_lpn, spp->tt_pgs);
            total_out_of_bounds++;
            continue;  // Skip this range, continue with others
        }

        // Process each LPN in this range
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            
            // Skip already unmapped/invalid pages
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                already_invalid++;
                continue;
            }

            // Invalidate the existing mapped page
            mark_page_invalid(ssd, &ppa);
            
            // Clear reverse mapping
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            
            // Set mapping table entry as unmapped
            ppa.ppa = UNMAPPED_PPA;
            set_maptbl_ent(ssd, lpn, &ppa);
            
            trimmed_pages++;
        }
        
        total_trimmed_pages += trimmed_pages;
        total_already_invalid += already_invalid;
        
        // ftl_debug("TRIM Range %d: %d pages trimmed, %d already invalid\n", 
        //        range_idx, trimmed_pages, already_invalid);
    }

    // ftl_debug("TRIM: Completed - %d pages trimmed, %d already invalid, %d out of bounds across %d ranges\n", 
    //        total_trimmed_pages, total_already_invalid, total_out_of_bounds, nr_ranges);

    // Free the ranges array
    g_free(ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;

    return 0;  // Assume TRIM operations have no NAND latency
}

void print_waf_stats(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    double waf = 0.0;
    double gc_overhead = 0.0;

    // 페이지당 바이트 수 = 섹터 크기 * 섹터 수
    double page_bytes = (double)spp->secsz * (double)spp->secs_per_pg;

    double host_gib = 0.0;
    double nand_gib = 0.0;
    double gc_gib   = 0.0;

    int free_total = ssd->lm.hot_free_line_cnt + ssd->lm.cold_free_line_cnt;

    if (ssd->host_writes > 0) {
        waf = (double)ssd->nand_writes / (double)ssd->host_writes;
        gc_overhead = (double)ssd->gc_writes / (double)ssd->host_writes * 100.0;
    }

    host_gib = (ssd->host_writes * page_bytes) / (1024.0 * 1024.0 * 1024.0);
    nand_gib = (ssd->nand_writes * page_bytes) / (1024.0 * 1024.0 * 1024.0);
    gc_gib   = (ssd->gc_writes   * page_bytes) / (1024.0 * 1024.0 * 1024.0);

    ftl_log("========== WAF Statistics ==========\n");
    ftl_log("Host Writes:  %lu pages (%.2f GiB)\n",
            ssd->host_writes, host_gib);
    ftl_log("NAND Writes:  %lu pages (%.2f GiB)\n",
            ssd->nand_writes, nand_gib);
    ftl_log("GC Writes:    %lu pages (%.2f GiB)\n",
            ssd->gc_writes, gc_gib);
    ftl_log("WAF:          %.4f\n", waf);
    ftl_log("GC Overhead:  %.2f%%\n", gc_overhead);
    ftl_log("Free Lines:   %d / %d (%.1f%%) [hot=%d, cold=%d]\n",
        free_total, ssd->lm.tt_lines,
        (double)free_total / ssd->lm.tt_lines * 100.0,
        ssd->lm.hot_free_line_cnt,
        ssd->lm.cold_free_line_cnt);
    ftl_log("====================================\n");
}

static inline int uid_interval_to_bin(uint64_t delta)
{
    if (delta == 0 || delta == UINT64_MAX) {
        return -1;  // 첫 write거나 비정상 값
    }

    int b = 0;
    while (delta > 1 && b < UID_HIST_BINS - 1) {
        delta >>= 1;
        b++;
    }
    return b;
}

/* 한 번의 host write가 발생할 때 LPN별 통계/UID 히스토그램 업데이트 */
void ftl_maybe_decay_lpn_stats(struct ssd *ssd)
{
    if (ssd->host_writes - ssd->hot_cold_last_decay_seq <
        HOT_DECAY_WINDOW_PAGES) {
        return;
    }

    struct ssdparams *spp = &ssd->sp;
    for (int i = 0; i < spp->tt_pgs; i++) {
        /* 접근 횟수는 절반으로 줄이기 (0으로 가도록) */
        ssd->lpn_access_cnt[i] >>= 1;

        /* 짧은 interval 연속 카운트도 서서히 줄이기 */
        if (ssd->lpn_short_int_cnt[i] > 0) {
            ssd->lpn_short_int_cnt[i] >>= 1;
        }
    }

    ssd->hot_cold_last_decay_seq = ssd->host_writes;
}

static inline void update_lpn_stats_on_write(struct ssd *ssd, uint64_t lpn)
{
    uint64_t seq  = ssd->host_writes;              // 현재 host write 시퀀스
    uint64_t last = ssd->lpn_last_write_seq[lpn];
    uint64_t delta = (last == 0) ? UINT64_MAX : (seq - last);

    /* UID 히스토그램: update interval 분포 분석용 (연구/디버깅 용도) */
    if (delta != UINT64_MAX) {
        int bin = uid_interval_to_bin(delta);
        if (bin >= 0) {
            ssd->uid_hist[bin]++;
        }
    }

    /* 접근 횟수 증가 (최근 window 기준) */
    if (ssd->lpn_access_cnt[lpn] < UINT32_MAX) {
        ssd->lpn_access_cnt[lpn]++;
    }

    /* 짧은 interval(= 자주 덮어씀)이면 연속 카운트↑, 아니면 리셋 */
    if (delta <= HOT_INTERVAL_THRESHOLD_PAGES) {
        if (ssd->lpn_short_int_cnt[lpn] < 255) {
            ssd->lpn_short_int_cnt[lpn]++;
        }
    } else {
        ssd->lpn_short_int_cnt[lpn] = 0;
    }

    /* 마지막 쓰기 시점 갱신 */
    ssd->lpn_last_write_seq[lpn] = seq;

    /* ==== Hot/Cold 상태 전이 ==== */
    lpn_state_t st = ssd->lpn_state[lpn];

    if (st == LPN_STATE_COLD) {
        /* 많이 쓰이고, 짧은 간격 패턴이 여러 번 나온 애는 Hot로 승격 */
        if (ssd->lpn_access_cnt[lpn] >= HOT_ACCESS_THRESHOLD &&
            ssd->lpn_short_int_cnt[lpn] >= HOT_INTERVAL_CONFIRM_COUNT) {
            ssd->lpn_state[lpn] = LPN_STATE_HOT;
        }
    } else { /* 현재 HOT인 LPN */
        /*
         * 1) 최근 window에서 access_cnt가 충분히 크지 않거나
         * 2) 너무 오랫동안 안 쓰였으면 (delta가 매우 큼)
         *    → 식었다고 보고 Cold로 강등
         */
        if (ssd->lpn_access_cnt[lpn] < HOT_ACCESS_THRESHOLD ||
            delta > HOT_INTERVAL_THRESHOLD_PAGES * 4) {
            ssd->lpn_state[lpn] = LPN_STATE_COLD;
            ssd->lpn_short_int_cnt[lpn] = 0;
        }
    }
}

/* 외부에서 쓰기 편하게 wrapper 함수 제공 (ftl.h에 프로토타입 있다고 가정) */
bool ftl_is_lpn_hot(struct ssd *ssd, uint64_t lpn)
{
    return ssd->lpn_state[lpn] == LPN_STATE_HOT;
}

void ftl_update_lpn_on_write(struct ssd *ssd, uint64_t lpn)
{
    update_lpn_stats_on_write(ssd, lpn);
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    /* WAF 출력을 위한 카운터 */
    uint64_t last_print_host_writes = 0;
    const uint64_t PRINT_DATA_INTERVAL = 16384; // 1GB 쓰기마다

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
                if (req->dsm_ranges && req->dsm_nr_ranges > 0) {
                    lat = ssd_trim(ssd, req);
                }
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* 주기적으로 WAF 출력 */
            if ((ssd->host_writes - last_print_host_writes) >= PRINT_DATA_INTERVAL) {
                print_waf_stats(ssd);
                last_print_host_writes = ssd->host_writes;
            }
            
            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}

