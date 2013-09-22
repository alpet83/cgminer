/*
 * device-bitfury.c - device functions for Bitfury chip/board library
 *
 * Copyright (c) 2013 bitfury
 * Copyright (c) 2013 legkodymov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
*/

#include "miner.h"
#include <unistd.h>
#include <sha2.h>
#include "libbitfury.h"
#include "util.h"
#include "tm_i2c.h"
#include <stdio.h>
#include <execinfo.h>
#include <signal.h>
#include <curses.h>
#include "uthash.h"

#define GOLDEN_BACKLOG 5


struct device_drv bitfury_drv;
unsigned loops_count = 0;
unsigned call_count = 0;


// Forward declarations
static void bitfury_disable(struct thr_info* thr);
static bool bitfury_prepare(struct thr_info *thr);
int calc_stat(time_t * stat_ts, time_t stat, struct timeval now);
int calc_stat_f(double * stat_ts, double elapse, double now_mcs);
double shares_to_ghashes(int shares, double seconds);

inline double tv2mcs(struct timeval *tv) {
    return (double)tv->tv_sec * 1e6 + (double)tv->tv_usec;
}


void sig_handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  printf("Error: signal %d, trace-size %d: \n", sig, size);
  backtrace_symbols_fd(array, size, STDOUT_FILENO);
  exit(1);
}

static void bitfury_detect(void)
{
    int chip_count;
    int i;
    struct cgpu_info *bitfury_info;

    signal(SIGSEGV, sig_handler);
    signal(SIGILL, sig_handler);

    bitfury_info = calloc(1, sizeof(struct cgpu_info));
    bitfury_info->drv = &bitfury_drv;
    bitfury_info->threads = 1;

    applog(LOG_INFO, "INFO: bitfury_detect");
    chip_count = libbitfury_detectChips(bitfury_info->devices);
    if (!chip_count) {
        applog(LOG_WARNING, "No Bitfury chips detected!");
        return;
    } else {
        applog(LOG_WARNING, "BITFURY: %d chips detected!", chip_count);
    }

    bitfury_info->chip_count = chip_count;
    add_cgpu(bitfury_info);
}

static uint32_t bitfury_checkNonce(struct work *work, uint32_t nonce)
{
    applog(LOG_INFO, "INFO: bitfury_checkNonce");
}

static int bitfury_submitNonce(struct thr_info *thr, struct bitfury_device *device, struct timeval *now, struct work *owork, uint32_t nonce)
{
    int i;
    int is_dupe = 0;

    for(i=0; i<32; i++) {
        if(device->nonces[i] == nonce) {
            is_dupe = 1;
            break;
        }
    }

    if(!is_dupe) {
        submit_nonce(thr, owork, nonce);
        device->nonces[device->current_nonce++] = nonce;
        if(device->current_nonce > 32)
            device->current_nonce = 0;
        i = device->stat_counter++;
        device->stat_ts [i] = now->tv_sec;
        device->stat_tsf[i] = tv2mcs(now);
        if (device->stat_counter == BITFURY_STAT_N)
            device->stat_counter = 0;
    }

    return(!is_dupe);
}


int bitfury_findChip(struct bitfury_device *devices, int chip_count, int slot, int fs) {
    int n;
    for (n = 0; n < chip_count; n++) {
        if ( (devices[n].slot == slot) && (devices[n].fasync == fs) )
            return n;
    }
    return -1;
}

void bitfury_setChipClk(struct bitfury_device *devices, int chip_count, int slot, int fs, int osc_bits) {
    int n = bitfury_findChip(devices, chip_count, slot, fs);
    if ( n >= 0 ) {
         // devices[n].osc6_bits = osc_bits;
         devices[n].osc6_bits_upd = osc_bits;
         devices[n].fixed_clk = true;
         applog(LOG_WARNING, "INFO: for chip %d assigned osc6_bits = %d", n, osc_bits);
    }
    else {
        applog(LOG_WARNING, "FATAL: chip %d not detected in slot %d", fs, slot);
    }
}

void bitfury_setSlotClk(struct bitfury_device *devices, int chip_count, int slot, int *fs_list) {

    int n;
    for ( n = 0; ( fs_list[n] >= 0 ) && ( n < BITFURY_BANKCHIPS ); n ++ ) {
        int fs = fs_list[n];
        int osc_bits = fs & 0xFF; // low 8 bits
        fs = fs >> 8; // high 24 bits is slot
        bitfury_setChipClk (devices, chip_count, slot, fs, osc_bits);
    }
}


double tv_diff(PTIMEVAL a, PTIMEVAL b) {
    double diff = tv2mcs(a) - tv2mcs(b);
    if (diff < 0) diff += 24.0 *3600.0 * 1e6; // add one day
    return diff;
}


inline void test_reclock(PBITFURY_DEVICE dev) {

    if ( dev->osc6_bits != dev->osc6_bits_upd ) {
        applog(LOG_WARNING, " for slot %X chip %X, osc6_bits changed from %d to %d, csw_count = %3d, cch_stat = { %2d %2d %2d %2d } ",
                               dev->slot, dev->fasync, dev->osc6_bits, dev->osc6_bits_upd, dev->csw_count,
                               dev->cch_stat[0], dev->cch_stat[1], dev->cch_stat[2], dev->cch_stat[3] );
         dev->osc6_bits = dev->osc6_bits_upd;
         send_freq( dev->slot, dev->fasync, dev->osc6_bits );
         cgtime (&dev->rst_time);
         dev->csw_count ++;
         dev->csw_back = 0;


    }
}

void init_devices (struct bitfury_device *devices, int chip_count) {
    int i;
    PBITFURY_DEVICE dev;
// #define FAST_CLOCK


    for (i = 0; i < chip_count; i++) {
#ifdef FAST_CLOCK1
            devices[i].osc6_bits = 53;
            devices[i].osc6_bits_upd = 53;
#else
            devices[i].osc6_bits = 54;
            devices[i].osc6_bits_upd = 54;
#endif
            devices[i].fixed_clk = false;
            devices[i].rbc_stat[0] = 0;
            devices[i].rbc_stat[1] = 0;
            devices[i].rbc_stat[2] = 0;
            devices[i].rbc_stat[3] = 0;
        }

    if (1) { // alpet: подстройка моих чипов (известные оптимумы)
        // overclocking/downclocking
        // 0x036, 0x136, 0x236, 0x336, 0x436, 0x536, 0x636


#ifdef FAST_CLOCK
        #define BASE_OSC_BITS 51
        #define LOW_HASHRATE 2.7
        int slot_0 [] = { 0x035, 0x135, 0x236, 0x435, 0x635, 0x735, -1 }; // 0x035, 0x136, 0x235,
        int slot_1 [] = { 0x336, -1 };  // 0x036, 0x136, 0x235, 0x335, 0x734,
        int slot_2 [] = { 0x335, 0x536, -1 }; // 0x036, 0x135, 0x235, 0x333, 0x436, 0x534, 0x634,
        int slot_3 [] = { 0x535, 0x635, -1 }; // 0x036, 0x136, 0x235, 0x335, 0x734,
        int slot_4 [] = { 0x234, 0x533, -1 }; // 0x035, 0x135, 0x333, 0x535,
        int slot_5 [] = { 0x035, 0x235, 0x335, 0x435, 0x535, 0x635, 0x735, -1 }; // 0x036, 0x433, 0x535, 0x635, 0x734,
        int slot_6 [] = { 0x035, 0x636, -1 }; // 0x034, 0x135, 0x234, 0x335, 0x435, 0x635, 0x735,
        int slot_7 [] = { -1 }; // 0x036, 0x134, 0x234, 0x336, 0x435, 0x536, 0x735,
        int slot_8 [] = { -1 }; // 0x035, 0x134, 0x235, 0x336, 0x433, 0x536, 0x634, 0x733,
        int slot_9 [] = { 0x035, 0x235, 0x335, 0x435, 0x535, 0x635, 0x735, -1 }; // 0x035, 0x134, 0x236, 0x335, 0x435, 0x535, 0x635, 0x736,
        int slot_A [] = { 0x035, 0x135, 0x235, 0x335, 0x435, 0x535, 0x635, 0x735, -1 }; // 0x034, 0x135, 0x234, 0x334, 0x433, 0x634,
        int slot_B [] = { -1 }; // absent
        int slot_C [] = { -1 }; // 0x035, 0x235, 0x335, 0x435, 0x535, 0x735,
        int slot_D [] = { 0x035, -1 }; // 0x035, 0x135, 0x235, 0x335, 0x433, 0x535, 0x734,
        int slot_E [] = { -1 }; // 0x135, 0x535,
        int slot_F [] = { 0x335, 0x536, 0x734, -1 }; // 0x036, 0x135, 0x235, 0x435, 0x535, 0x635, 0x735,

#else
        #define BASE_OSC_BITS 53
        #define LOW_HASHRATE 1.5
        int slot_0 [] = { -1 };
        int slot_1 [] = { -1 };
        int slot_2 [] = { -1 };
        int slot_3 [] = { -1 };
        int slot_4 [] = { -1 };
        int slot_5 [] = { -1 };
        int slot_6 [] = { -1 };
        int slot_7 [] = { -1 };
        int slot_8 [] = { -1 };
        int slot_9 [] = { -1 };
        int slot_A [] = { -1 };
        int slot_B [] = { -1 };
        int slot_C [] = { -1 };
        int slot_D [] = { -1 };
        int slot_E [] = { -1 };
        int slot_F [] = { -1 };

#endif
        int *all_slots[] = { slot_0, slot_1, slot_2, slot_3, slot_4, slot_5, slot_6, slot_7, slot_8, slot_9, slot_A, slot_B, slot_C, slot_D, slot_E, slot_F, NULL };

        for (i = 0; ( i < BITFURY_MAXBANKS ) && all_slots[i]; i ++)
             bitfury_setSlotClk(devices, chip_count, i, all_slots[i] );
    }

    for (i = 0; i < chip_count; i++) {

        dev = &devices[i];
        send_reinit(dev->slot, dev->fasync, dev->osc6_bits);
        cgtime (&dev->rst_time);
    }
}


int next_prefetch(int i) {
    return ( i + 1 ) % PREFETCH_WORKS;
}

void get_opt_filename(char *filename) {
    if ( getenv("HOME") && *getenv("HOME") ) {
            strcpy(filename, getenv("HOME"));
            strcat(filename, "/");
            mkdir(filename, 0777);
    }
    else
        strcpy(filename, "");

    strncat(filename, ".cgminer/", PATH_MAX);
    mkdir(filename, 0777);
    strncat(filename, "bitfury_opt.conf", PATH_MAX);
}


void load_opt_conf (struct bitfury_device *devices, int chip_count) {
    char filename[PATH_MAX];
    get_opt_filename(filename);
    FILE *fcfg = fopen(filename, "r");
    if (!fcfg) return;

    applog(LOG_WARNING, "loading opt configuration from %s ", filename);

    int lcount = 0;

    while ( ! feof(fcfg) ) {
        char line [1024] = { 0 };
        fgets (line, 1024, fcfg);
        char *s = strstr(line, "slot_");
        if ( !s ) continue;
        lcount ++;

        s[4] = 32; // 'slot_XX=' -> 'slot XX='
        char *t = strtok(s, "=");
        int n_slot = 0, n_chip = -1;

        if (!t || strlen(t) < 1 ) {
            applog(LOG_WARNING, "cannot locate = in line %s", s);
            continue;
        }

        applog(LOG_WARNING, "parsing line %d, 1-st token: \t%s", lcount, t);

        char tmp[100];

        if ( sscanf (s, "%s %X", tmp, &n_slot) < 2 ) {
            applog(LOG_WARNING, "parsing error at slot number detect");
            continue;
        }

        t = strtok(NULL, ";");
        while (t && strlen(t) > 10 ) {
            applog(LOG_WARNING, "parsing line %d, next token: %35s", lcount, t);

            int v[4];
            int tc = sscanf(t, "%d:[%d,%d,%d,%d]@{%*.2f,%*.2f,%*.2f,%*.2f}", &n_chip, &v[0], &v[1], &v[2], &v[3]);
            if ( tc >= 5 ) {

                if ( n_chip < 0 ) break;
                int i = bitfury_findChip (devices, chip_count, n_slot, n_chip);
                if ( i >= 0 )
                    memcpy( devices[i].cch_stat, v, sizeof(v) ); // update stat
            }
            else {
                applog(LOG_WARNING, "parsing error for token %s, sscanf returns %d", t, tc);
                break;
            }
            t = strtok(NULL, ";");
        }  // while 2
    } // while 1

    fclose(fcfg);
}


void save_opt_conf (struct bitfury_device *devices, int chip_count) {
    FILE *fcfg;
    char filename[PATH_MAX];
    if (!chip_count) return;

    get_opt_filename(filename);
    applog(LOG_WARNING, "dumping opt configuration to %s ", filename);

    fcfg = fopen(filename, "w");
    int i;
    int last_slot = devices[0].slot;
    char line[1024] = { 0 };

    for (i = 0; i < chip_count; i ++) {
        PBITFURY_DEVICE dev = &devices[i];

        if (dev->slot != last_slot) {
            fprintf(fcfg, "slot_%X=%s\n", last_slot, line);
            last_slot = dev->slot;
            line[0] = 0;
        }

        char dev_stat[128];
        sprintf(dev_stat, "%d:[%d,%d,%d,%d]@", dev->fasync, dev->cch_stat[0], dev->cch_stat[1], dev->cch_stat[2], dev->cch_stat[3]);
        strncat(line, dev_stat, 1024);

        float v0, v1, v2, v3;
        v0 = dev->rbc_stat[0];
        v1 = dev->rbc_stat[1];
        v2 = dev->rbc_stat[2];
        v3 = dev->rbc_stat[3];

        sprintf(dev_stat, "{%.2f,%.2f,%.2f,%.2f}; ", v0, v1, v2, v3);
        strncat(line, dev_stat, 1024);
    }

    fprintf(fcfg, "slot_%X=%s\n", last_slot, line);
    fclose(fcfg);
}




inline int works_prefetched (struct cgpu_info *cgpu) {
    int i, cnt = 0;
    for (i = 0; i < PREFETCH_WORKS; i ++)
        if ( cgpu->prefetch[i] ) cnt ++;
    return cnt;
}

static bool bitfury_fill(struct cgpu_info *cgpu) {
    bool ret;
    int i;

    /*
    struct work* wrk, *tmp;

    HASH_ITER(hh, cgpu->queued_work, wrk, tmp) {
         if (!wrk->queued) cnt ++;
    }

    return ( cnt > 170 ); // */


    struct work* nw = get_queued (cgpu);
    if (!nw) return true;

    rd_lock(&cgpu->qlock); //
    for (i = 0; i < PREFETCH_WORKS; i ++)  {
        if ( NULL == cgpu->prefetch [cgpu->w_prefetch] ) {
            cgpu->prefetch [cgpu->w_prefetch] = nw;
            break;
        }
        cgpu->w_prefetch = next_prefetch ( cgpu->w_prefetch );
    }
    int max_need = cgpu->chip_count / 3 + 1;
    if (max_need > PREFETCH_WORKS)
        max_need = PREFETCH_WORKS;

    ret = ( works_prefetched(cgpu) >= max_need ); // need find optimal values
    rd_unlock(&cgpu->qlock);
    return ret;
    // */
}

struct work* load_prefetch(struct cgpu_info *cgpu){
    int i;
    struct work* result = NULL;

    // rd_lock(&cgpu->qlock);
    // выборка задания из большой очереди
    // TODO: вместо блокировки здесь нужны атомарные операции!
    for (i = 0; i < PREFETCH_WORKS; i ++)  {
        if ( cgpu->prefetch [cgpu->r_prefetch]  ) {
            result = cgpu->prefetch [cgpu->r_prefetch];
            cgpu->prefetch [cgpu->r_prefetch] = NULL; // больше не выдавать
            break;
        }
        cgpu->r_prefetch = next_prefetch ( cgpu->r_prefetch );
    }
    // rd_unlock(&cgpu->qlock); // */
    return result;
}


inline uint64_t works_receive(struct thr_info *thr, struct bitfury_device *devices, int chip_count) {

    uint64_t hashes = 0;
    struct timeval now;


    int chip;

    for (chip = 0;chip < chip_count; chip++) {
        int nonces_cnt = 0;
        struct bitfury_device *dev = &devices[chip];

        if (dev->job_switched && dev->work) {
            int j;
            int *res = dev->results;
            struct work *work = dev->work;
            struct work *owork = dev->owork;
            struct work *o2work = dev->o2work;
            cgtime(&now);

            // новое задание - считается - закончено?
            // work=>>owork=>>o2work

            for (j = dev->results_n - 1; j >= 0; j--) {
                if (owork) {
                    nonces_cnt += bitfury_submitNonce(thr, dev, &now, owork, bswap_32(res[j]));
                }
                if (o2work) {
                    // TEST
                    //submit_nonce(thr, owork, bswap_32(res[j]));
                }
            }
            dev->results_n = 0;
            dev->job_switched = 0;
            if (dev->old_nonce && o2work)
                nonces_cnt += bitfury_submitNonce(thr, dev, &now, o2work, bswap_32(dev->old_nonce));

            if (dev->future_nonce)
                nonces_cnt += bitfury_submitNonce(thr, dev, &now, work, bswap_32(dev->future_nonce));

            if (o2work) {
                work_completed(thr->cgpu, o2work);
                double diff = tv_diff (&now, &dev->work_start);
                dev->work_end = now;

                if (dev->work_median == 0)
                    dev->work_median = diff;
                else
                    dev->work_median = dev->work_median * 0.993 + diff *0.007; // EMA
            }
            // сдвиг миниочереди
            dev->o2work = dev->owork;
            dev->owork = dev->work;
            dev->work = NULL;
            hashes += 0xffffffffull * nonces_cnt;
            dev->matching_work += nonces_cnt;
            test_reclock(dev); // думаю здесь самое лучшее место, чтобы чип перенастроить на другую частоту
        }
    }
    return hashes;
}

inline int work_push(struct thr_info *thr, PBITFURY_DEVICE dev) {
    dev->job_switched = 0;
    if ( dev->work == NULL )
    {
        struct work *qwork  = thr->cgpu->queued_work;

        dev->work = load_prefetch (thr->cgpu);
        if (dev->work == NULL)           // no prefetched
        {
           dev->work = get_queued(thr->cgpu);
           if (NULL == dev->work) return 0;
        }
        cgtime(&dev->work_start);
        work_to_payload(&(dev->payload), dev->work);

        if (dev->work_end.tv_sec > 0) {
            double diff = tv_diff (&dev->work_start, &dev->work_end); // сколько прошло перед работой

            if ( ( diff > 0 ) && ( diff < 1e6 ) ) {
                if (dev->work_wait == 0)
                    dev->work_wait = diff;
                else
                    dev->work_wait = dev->work_wait * 0.993 + diff * 0.007; // EMA
            }
        }

        return 2;
    }
    return 1;
}

static int64_t try_scanHash(struct thr_info *thr)
{

    static struct bitfury_device *devices, *dev; // TODO Move somewhere to appropriate place
    int chip_count;
    int chip;
    static no_work = 0;
    uint64_t hashes = 0;
    static struct timeval now;
    static struct timeval last_call;
    static double call_period = 0;

    unsigned char line[2048];
    int short_stat = 20;
    static time_t short_out_t = 0;
    static double short_out_tf = 0;
    int long_stat = 900;
    static time_t long_out_t = 0;
    int long_long_stat = 60 * 30;
    static time_t long_long_out_t;
    double elps_mcs = 0;
    double now_mcs = 0;

    static vc0_median[BITFURY_MAXBANKS];
    static vc1_median[BITFURY_MAXBANKS];
    static double ghs_median[BITFURY_MAXBANKS];

    static char debug_log[1024];

    static char CL_RESET[]     = "\e[0m";
    static char CL_LT_RED[]    = "\e[1;31m";
    static char CL_LT_GREEN[]  = "\e[1;32m";
    static char CL_LT_YELLOW[] = "\e[1;33m";
    static char CL_LT_BLUE[]   = "\e[1;34m";
    static char CL_LT_CYAN[]   = "\e[1;36m";
    static char CL_LT_WHITE[]  = "\e[1;37m";

    static int last_chip = 0; // для кольцевого обхода по выдаче заданий



    int i;
    static stat_dumps = 0;

    loops_count ++;
    call_count ++;


    devices = thr->cgpu->devices;
    chip_count = thr->cgpu->chip_count;
    cgtime(&now);


    if ( loops_count == 1 ) {
         init_devices  (devices, chip_count);
         load_opt_conf (devices, chip_count);
    }


    if ( loops_count > 2 ) {
        elps_mcs = tv_diff (&now, &last_call); //
        if ( call_period == 0 )
             call_period = elps_mcs;
        else
             call_period = call_period * 0.999 + elps_mcs * 0.001;

    }


    last_call = now;
    int w_pushed = 0;

    hashes += works_receive(thr, devices, chip_count);

    // подготовка заданий для чипов
    for (chip = 0; chip < chip_count; chip++) {


       int code = work_push(thr, &devices[last_chip]);

       if ( 2 == code ) w_pushed ++;

       if ( 0 == code ) {
           char msg[64];
           sprintf(msg, "chip = %3d, lcount = %5d, pcount = %3d | ", chip, loops_count, works_prefetched(thr->cgpu) );
           no_work ++;
           strncat (debug_log, msg, 1023);

           if ( no_work % 10 == 0 || strlen(debug_log) > 800 ) {
               printf(CL_LT_CYAN);
               applog(LOG_WARNING, debug_log);
               printf(CL_RESET);
               debug_log[0] = 0;
           }

           // return 0;
           break;
       }
       last_chip ++;
       if (last_chip >= chip_count)
           last_chip = 0;
    }

    libbitfury_sendHashData(thr, devices, chip_count);
    hashes += works_receive(thr, devices, chip_count);

    // if ( w_pushed == 0 ) nmsleep(5);


    cgtime(&now);
    now_mcs = tv2mcs (&now);

    if (short_out_t == 0) {
        short_out_t = now.tv_sec;
        short_out_tf = now_mcs;
    }

    if ( loops_count < 10 )
         return hashes; // обычно статистика не накапливается

    int elapsed = now.tv_sec - short_out_t;


    if (elapsed >= short_stat) {
        elps_mcs = now_mcs - short_out_tf;
        short_out_tf = now_mcs;
        int shares_first = 0, shares_last = 0, shares_total = 0;
        char stat_lines[BITFURY_MAXBANKS][1024] = {0};
        char color [15];
        int len, k;
        double gh[BITFURY_MAXBANKS][BITFURY_BANKCHIPS] = {0};



        double ghsum = 0, gh1h = 0, gh2h = 0;
        unsigned strange_counter = 0;

        int last_slot = -1;

        stat_dumps ++;

        int maskv = stat_dumps & 15;
        if ( maskv == 15 ) printf("%s\n", CL_LT_WHITE);


        for (chip = 0; chip < chip_count; chip++) {
            dev = &devices[chip];

            // статистику стоит оценивать от последнего сброса устройства, иначе хрень будет.
            double rst_msc = tv2mcs (&dev->rst_time);
            double elps_eff = now_mcs - rst_msc;
            // if (elps_eff > elps_mcs) elps_eff = elps_mcs;

            if (elps_eff > 5e8) elps_eff = 5e8; // 500 seconds limit

            int shares_found = calc_stat_f(dev->stat_tsf, elps_eff, now_mcs);
            int i_chip = dev->fasync;
            int n_slot = dev->slot;
            double ghash;
            double alt_gh;

            // if slot changed
            if (n_slot != last_slot) {
                float slot_temp = tm_i2c_gettemp(n_slot) * 0.1;
                float slot_vc0 = tm_i2c_getcore0(n_slot) * 1000;
                float slot_vc1 = tm_i2c_getcore1(n_slot) * 1000;

                if (stat_dumps > 2) {
                    // checking anomaly extremums 0.2 outbound
                    if (slot_vc0 < 850) slot_vc0 = 850;
                    if (slot_vc1 < 850) slot_vc1 = 850;
                    if (slot_vc0 > 2000) slot_vc0 = 1090;
                    if (slot_vc1 > 2000) slot_vc1 = 1090;

                    slot_vc0 = vc0_median[n_slot] * 0.95 + slot_vc0 * 0.05;
                    slot_vc1 = vc1_median[n_slot] * 0.95 + slot_vc1 * 0.05;
                }

                vc0_median[n_slot] = slot_vc0;
                vc1_median[n_slot] = slot_vc1;

                // sprintf(stat_lines[n_slot], "[%X] T:%3.0f | V: %4.0f %4.0f| ", n_slot, slot_temp, slot_vc0, slot_vc1);
                sprintf(stat_lines[n_slot], "[%X] T:%3.0f | V: %4.2f %4.2f| ", n_slot, slot_temp, slot_vc0 / 1000, slot_vc1 / 1000);
                last_slot = n_slot;
            }

            len = strlen(stat_lines[n_slot]);
            ghash = shares_to_ghashes(shares_found, elps_eff / 1e6 );

            dev->csw_back ++;

            // if ( stat_dumps <= 2 || dev->csw_back <= 2 ) ghash *= 0.5; // из-за заполнения очередей, тут перебор тот ещё


            alt_gh = ghash;
            gh[dev->slot][chip % BITFURY_BANKCHIPS] = ghash;
            float hw_errs = (float) devices[chip].hw_errors;
            float saldo = hw_errs + shares_found; // TODO: проверить, нужно ли добавить режики?


            if ( dev->work_median > 0 )
                 alt_gh = 3e6 / dev->work_median;

            if (saldo > 0)
               hw_errs = 100 * hw_errs / saldo;
            else
               hw_errs = 0;

            if (stat_dumps < 5)
               dev->hw_rate = hw_errs;
            else
               dev->hw_rate = dev->hw_rate * 0.93 + hw_errs * 0.07; // EMA 16

            int ridx = dev->osc6_bits - BASE_OSC_BITS;

            double ema_ghash = ghash;

            // сбор статистки по хэш-рейту на клок
            if ( ( ridx >= 0 ) && ( stat_dumps > 1 )  ) {
                // float prev = dev->rbc_stat[ridx] * 0.92;

                float ema_value = 100;
                if ( dev->csw_back < 32 )
                         ema_value = 32; // усреднять ближние циклы

                float prev_part = ( 1 - 1 / ema_value );

                ema_ghash = dev->rbc_stat[ridx] * prev_part + ghash * (1 - prev_part); // up to 300 loops for stat
                // ema_ghash -= dev->hw_rate / 1000;  // для улучшения сортировки

                if ( ema_ghash < 0.1 || elps_eff > elps_mcs ) // на случай аномалии или достаточно продолжительной работы
                     ema_ghash = ghash;

                dev->rbc_stat[ridx] = ema_ghash;
                gh[dev->slot][chip % BITFURY_BANKCHIPS] = ema_ghash;
            }



            char *cl_tag = " ";
            if ( ema_ghash >= 3 ) cl_tag = " +";
            if ( ema_ghash >= 4 ) cl_tag = "++";


            if ( maskv < 15 ) {

                if ( ( maskv > 13 ) && ( dev->work_median > 0 ) )
                    snprintf(stat_lines[n_slot] + len, 256 - len, "%3.0f @%5.2f%%| ", alt_gh * 10, 100 * dev->work_wait / dev->work_median ); // speed from work-time, wait time
                else
                    snprintf(stat_lines[n_slot] + len, 256 - len, "%2s%2.0f -%5.1f | ", cl_tag, ema_ghash * 10, dev->hw_rate ); // speed and errors
            }
            else {
                char selected[5] = { 32, 32, 32, 32, 32 };

                selected[ridx] = 0x5B;
                selected[ridx + 1] = 0x5D;

                char s0 = selected[0];
                char s1 = selected[1];
                char s2 = selected[2];
                char s3 = selected[3];
                char s4 = selected[4];

                float h0 = dev->rbc_stat[0] * 10;
                float h1 = dev->rbc_stat[1] * 10;
                float h2 = dev->rbc_stat[2] * 10;
                float h3 = dev->rbc_stat[3] * 10;

                snprintf( stat_lines[n_slot] + len, 256 - len, "%c%2.0f%c%2.0f%c%2.0f%c%2.0f|", s0, h0, s1, h1, s2, h2, s3, h3, s4 ); // intermediate dump clock


                // проверка на слишком маленькую частоту

                if ( dev->csw_back > 32 && ema_ghash > 1.0 && ema_ghash < LOW_HASHRATE && !dev->fixed_clk ) {
                    dev->fixed_clk = false;
                    dev->csw_count = 0;
                    printf(CL_LT_RED);
                    applog(LOG_WARNING, "#WARNING: Chip at %x x %x has low median hashrate, auto-clock reset ", dev->fasync, dev->slot );
                    printf(CL_RESET);
                    for (i = 0; i < 3; i ++) dev->rbc_stat[i] = 0; // затереть статистику, типа устарела
                }

            }

            if ( ema_ghash <= 1.0  && dev->csw_back > 31 ) dev->alerts ++; else dev->alerts = 0;

            if ( 3 < dev->alerts ) {
                printf(CL_LT_RED);
                applog(LOG_WARNING, "Chip_id %d FREQ CHANGE-RESTORE", chip);
                printf(CL_RESET);
                // send_freq(n_slot, i_chip, 54);
                send_shutdown(n_slot, i_chip);
                nmsleep(100);
                send_reinit(n_slot, i_chip, 53); // fail-safe
                dev->fixed_clk = false;
                dev->alerts = 0;
                dev->csw_back = 0;
                dev->csw_count ++;
                dev->rst_time = now;
                dev->cch_stat[0] = dev->cch_stat[1] = dev->cch_stat[2] = dev->cch_stat[3] = 0; // полный сброс статистики автоподбора
            }



            if ( ( stat_dumps > 16 ) && ( maskv == 15 ) && !dev->fixed_clk ) {
                // переключение клока принудительно

                int new_clk = ridx;


                float best = 3; // extremum Ghz for 54 clk
                best = dev->rbc_stat[ridx];
                int csum = 0;
                int test_count = 4;
                for (i = 0; i < 4; i ++) csum += dev->cch_stat[i];
                if ( csum > 2 )
                     test_count = 2;

                if ( dev->csw_count < test_count ) {

                    int optimal = 1;
                    if ( csum > 4 ) optimal = dev->cch_stat[ridx]; // probably best choice

                    new_clk = ( ridx + 1 ) & 3; // masked enum
                    while ( csum > 2 && dev->cch_stat[new_clk] < optimal )
                            new_clk = ( new_clk + 1 ) & 3; // дополнительные циклы - пропуск неоптимальных выборов
                }
                else
                if ( best < 4 && dev->csw_count < test_count + 1 ) {
                    // однократный(!) поиск наилучшего, для работы с заданным клоком.
                    // if ( stat_dumps > 150 ) best = 3;
                    int i;
                    for (i = 0; i < 4; i ++) {
                        if ( best >= dev->rbc_stat [i] ) continue;
                        best = dev->rbc_stat [i];
                        new_clk = i; // optimus
                    } // for

                    // подведение итогов соревнования
                    dev->cch_stat[new_clk] ++;
                }




                new_clk = new_clk + BASE_OSC_BITS;

                if ( dev->osc6_bits_upd != new_clk ) {
                     dev->osc6_bits_upd = new_clk;
                     // dev->rbc_stat[ridx] += 0.3; // welcome back

                     test_reclock(dev);
                    // if ( dev->osc6_bits > 52 ) send_freq(n_slot, i_chip, dev->osc6_bits);
                }
            } // handling maskv == 15


            // snprintf(stat_lines[dev->slot] + len, 256 - len, "%.1f-%3.0f ", ghash, dev->mhz);


            shares_total += shares_found;
            shares_first += chip < BITFURY_BANKCHIPS/2 ? shares_found : 0;
            shares_last += chip >= BITFURY_BANKCHIPS/2 ? shares_found : 0;
            strange_counter += dev->hw_errors;

            dev->hw_errors = 0;

            //dev->strange_counter = 0;

        } // for (chip; chip < n-chip; chip++)


        if (maskv == 15)
            save_opt_conf(devices, chip_count);

#ifdef BITFURY_ENABLE_SHORT_STAT
        // sprintf(line, "vvvvwww SHORT stat %ds: wwwvvvv", short_stat);
        sprintf(line, "  ================== SHORT stat, elapsed %.3fs, no_work = %d, dump %d, call period = %.2f ms, count = %5d =================== ",
                                                 elps_mcs / 1e6, no_work, stat_dumps, call_period / 1000, call_count );
        no_work = 0;
        call_count = 0;


        applog(LOG_WARNING, line);
        //sprintf(line, "stranges: %u", strange_counter);
        double ghsm_saldo = 0;


        for(i = 0; i < BITFURY_MAXBANKS; i++)
            if(strlen(stat_lines[i])) {
                len = strlen(stat_lines[i]);
                ghsum = 0;
                gh1h = 0;
                gh2h = 0;


                for(k = 0; k < BITFURY_BANKCHIPS/2; k++) {
                    gh1h += gh[i][k];                       // saldo for 0..3 chip
                    gh2h += gh[i][k + BITFURY_BANKCHIPS/2]; // saldo for 4..7 chip

                }
                // snprintf(stat_lines[i] + len, 256 - len, "- %2.1f + %2.1f = %2.1f slot %i ", gh1h, gh2h, ghsum, i);
                ghsum = gh1h + gh2h;

                double ghmed = ghsum;

                /*

                if (stat_dumps > 4) {
                    if (stat_dumps > 50 )
                        ghmed = ghs_median[i] * 0.95 + ghsum * 0.05; // EMA 20
                    else
                        ghmed = ghs_median[i] * 0.9 + ghsum * 0.1; // EMA 10
                } // */

                snprintf(stat_lines[i] + len, 256 - len, " S: %4.1f + %4.1f = %4.1f  (%4.1f) [%X]", gh1h, gh2h, ghsum, ghmed, i);

                ghs_median[i] = ghmed;
                ghsm_saldo += ghmed;


                if (maskv < 18) {
                    if (i & 1 == 1)
                        printf(CL_LT_GREEN);
                    else
                        printf("\e[0m\r");
                }

                applog(LOG_WARNING, stat_lines[i]);
            }

        elapsed = now.tv_sec - long_out_t;
        printf("\e[37;40m\r");

        rd_lock(&thr->cgpu->qlock);
        int hcount = HASH_COUNT(thr->cgpu->queued_work);
        int pcount = works_prefetched(thr->cgpu);
        rd_unlock(&thr->cgpu->qlock);

        applog(LOG_WARNING, "Median hash-rate saldo = %4.1f, seconds to long stat %5d, prefetched = %3d ", ghsm_saldo, long_stat - elapsed, pcount  );
        applog(LOG_WARNING, line);
#endif
        short_out_t = now.tv_sec;

        if ( maskv == 15 ) printf("%s", CL_RESET);
    }
#ifdef BITFURY_ENABLE_LONG_STAT
    if (elapsed >= long_stat) {
        int shares_first = 0, shares_last = 0, shares_total = 0;
        char stat_lines[BITFURY_MAXBANKS][256] = {0};
        int len, k;
        double gh[BITFURY_MAXBANKS][BITFURY_BANKCHIPS] = {0};
        double ghsum = 0, gh1h = 0, gh2h = 0;

        for (chip = 0; chip < chip_count; chip++) {
            dev = &devices[chip];
            int shares_found = calc_stat(dev->stat_ts, elapsed, now);
            double ghash;
            len = strlen(stat_lines[dev->slot]);
            ghash = shares_to_ghashes(shares_found, (double)long_stat);
            gh[dev->slot][chip % BITFURY_BANKCHIPS] = ghash;
            snprintf(stat_lines[dev->slot] + len, 256 - len, "%.2f-%3.0f ", ghash, dev->mhz);
            shares_total += shares_found;
            shares_first += chip < BITFURY_BANKCHIPS/2 ? shares_found : 0;
            shares_last += chip >= BITFURY_BANKCHIPS/2 ? shares_found : 0;
        }

        sprintf(line, "  !!!_________ LONG stat, elapsed %ds: ___________!!!", elapsed);
        // attron(A_BOLD);
        printf("%s", CL_LT_YELLOW);
        applog(LOG_WARNING, line);
        for(i = 0; i < BITFURY_MAXBANKS; i++)
            if(strlen(stat_lines[i])) {
                len = strlen(stat_lines[i]);
                ghsum = 0;
                gh1h = 0;
                gh2h = 0;
                for(k = 0; k < BITFURY_BANKCHIPS/2; k++) {
                    gh1h += gh[i][k];
                    gh2h += gh[i][k + BITFURY_BANKCHIPS/2];
                    ghsum += gh[i][k] + gh[i][k + BITFURY_BANKCHIPS/2];
                }
                snprintf(stat_lines[i] + len, 256 - len, "- %4.1f + %4.1f = %4.1f Gh/s slot %X ", gh1h, gh2h, ghsum, i);
                applog(LOG_WARNING, stat_lines[i]);
            }
        long_out_t = now.tv_sec;
        printf("%s", CL_RESET);
        // attroff(A_BOLD);
    }
#endif


    return hashes;
}

static int64_t bitfury_scanHash(struct thr_info *thr) {
     int64_t result = try_scanHash(thr);
     if ( 0 == result ) nmsleep(1);
     return result;
}


double shares_to_ghashes(int shares, double seconds) {
    return ( (double)shares * 4.294967296 ) / ( seconds );

}

int calc_stat(time_t * stat_ts, time_t stat, struct timeval now) {
    int j;
    int shares_found = 0;
    for(j = 0; j < BITFURY_STAT_N; j++) {
        if (now.tv_sec - stat_ts[j] < stat) {
            shares_found++;
        }
    }
    return shares_found;
}
int calc_stat_f (double * stat_tsf, double elapsed, double now_mcs) {
    int j;
    int shares_found = 0;
    for(j = 0; j < BITFURY_STAT_N; j++) {
        if (now_mcs - stat_tsf[j] < elapsed) {
            shares_found++;
        }
    }
    return shares_found;
}


static void bitfury_statline_before(char *buf, struct cgpu_info *cgpu)
{
    applog(LOG_INFO, "INFO bitfury_statline_before");
}

static bool bitfury_prepare(struct thr_info *thr)
{
    struct timeval now;
    struct cgpu_info *cgpu = thr->cgpu;

    cgtime(&now);
    get_datestamp(cgpu->init, &now);

    applog(LOG_INFO, "INFO bitfury_prepare");
    return true;
}

static void bitfury_shutdown(struct thr_info *thr)
{
    int chip_count;
    int i;

    chip_count = thr->cgpu->chip_count;

    applog(LOG_INFO, "INFO bitfury_shutdown");
    libbitfury_shutdownChips(thr->cgpu->devices, chip_count);
}

static void bitfury_disable(struct thr_info *thr)
{
    applog(LOG_INFO, "INFO bitfury_disable");
}


static void get_options(struct cgpu_info *cgpu)
{
    char buf[BUFSIZ+1];
    char *ptr, *comma, *colon, *colon2;
    size_t max = 0;
    int i, slot, fs, bits, chip, def_bits;

    for(i=0; i<cgpu->chip_count; i++)
        cgpu->devices[i].osc6_bits_setpoint = 54; // this is default value

    if (opt_bitfury_clockbits == NULL) {
        buf[0] = '\0';
        return;
    }

    ptr = opt_bitfury_clockbits;

    do {
        comma = strchr(ptr, ',');
        if (comma == NULL)
            max = strlen(ptr);
        else
            max = comma - ptr;
        if (max > BUFSIZ)
            max = BUFSIZ;
        strncpy(buf, ptr, max);
        buf[max] = '\0';

        if (*buf) {
            colon = strchr(buf, ':');
            if (colon) {
                *(colon++) = '\0';
                colon2 = strchr(colon, ':');
                if (colon2)
                    *(colon2++) = '\0';
                if (*buf && *colon && *colon2) {
                    slot = atoi(buf);
                    fs = atoi(colon);
                    bits = atoi(colon2);
                    chip = bitfury_findChip(cgpu->devices, cgpu->chip_count, slot, fs);
                    if(chip > 0 && chip < cgpu->chip_count && bits >= 48 && bits <= 56) {
                        cgpu->devices[chip].osc6_bits_setpoint = bits;
                        applog(LOG_INFO, "Set clockbits: slot=%d chip=%d bits=%d", slot, fs, bits);
                    }
                }
            } else {
                def_bits = atoi(buf);
                if(def_bits >= 48 && def_bits <= 56) {
                    for(i=0; i<cgpu->chip_count; i++)
                        cgpu->devices[i].osc6_bits_setpoint = def_bits;
                }
            }
        }
        if(comma != NULL)
            ptr = ++comma;
    } while (comma != NULL);
} // */

static struct api_data *bitfury_api_stats(struct cgpu_info *cgpu)
{
    struct api_data *root = NULL;
    static struct bitfury_device *devices;
    struct timeval now;
    struct bitfury_info *info = cgpu->device_data;
    int shares_found, i;
    double ghash, ghash_sum = 0.0;
    unsigned int osc_bits;
    char mcw[24];
    uint64_t total_hw = 0;

    devices = cgpu->devices;
    root = api_add_int(root, "chip_count", &(cgpu->chip_count),false);
    cgtime(&now);

    for (i = 0; i < cgpu->chip_count; i++) {
        sprintf(mcw, "clock_bits_%d_%d", devices[i].slot, devices[i].fasync);
        osc_bits = (unsigned int)devices[i].osc6_bits;
        root = api_add_int(root, mcw, &(devices[i].osc6_bits), false);
    }
    for (i = 0; i < cgpu->chip_count; i++) {
        sprintf(mcw, "match_work_count_%d_%d", devices[i].slot, devices[i].fasync);
        root = api_add_uint(root, mcw, &(devices[i].matching_work), false);
    }
    for (i = 0; i < cgpu->chip_count; i++) {
        sprintf(mcw, "hw_errors_%d_%d", devices[i].slot, devices[i].fasync);
        root = api_add_uint(root, mcw, &(devices[i].hw_errors), false);
        total_hw += devices[i].hw_errors;
    }
    for (i = 0; i < cgpu->chip_count; i++) {
        shares_found = calc_stat(devices[i].stat_ts, BITFURY_API_STATS, now);
        ghash = shares_to_ghashes(shares_found, (double)BITFURY_API_STATS);
        ghash_sum += ghash;
        sprintf(mcw, "ghash_%d_%d", devices[i].slot, devices[i].fasync);
        root = api_add_double(root, mcw, &(ghash), true);
    }
    api_add_uint64(root, "total_hw", &(total_hw), false);
    api_add_double(root, "total_gh", &(ghash_sum), true);
    ghash_sum /= cgpu->chip_count;
    api_add_double(root, "avg_gh_per_chip", &(ghash_sum), true);

    return root;
}

struct device_drv bitfury_drv = {
    .drv_id = DRIVER_BITFURY,
    .dname = "bitfury",
    .name = "BITFURY",
    .drv_detect = bitfury_detect,
    .get_statline_before = bitfury_statline_before,
    .thread_prepare = bitfury_prepare,
    .scanwork = bitfury_scanHash,
    .thread_shutdown = bitfury_shutdown,
    .hash_work = hash_queued_work,
    .queue_full = bitfury_fill,
    .get_api_stats = bitfury_api_stats,
};

