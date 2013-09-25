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
#include <math.h>
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
#include "driver-config.h"
#include "memutil.h"

#define GOLDEN_BACKLOG 5


struct device_drv bitfury_drv;
unsigned loops_count = 0;
unsigned call_count = 0;



// Forward declarations
static void bitfury_disable(thr_info_t* thr);
static bool bitfury_prepare(thr_info_t *thr);
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

    bitfury_info = safe_calloc(1, sizeof(struct cgpu_info), "bitfury_info in bitfury_detect");
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

static int bitfury_submitNonce(thr_info_t *thr, bitfury_device_t *device, struct timeval *now, struct work *owork, uint32_t nonce)
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


int bitfury_findChip(bitfury_device_t *devices, int chip_count, int slot, int fs) {
    int n;
    for (n = 0; n < chip_count; n++) {
        if ( (devices[n].slot == slot) && (devices[n].fasync == fs) )
            return n;
    }
    return -1;
}

void bitfury_setChipClk(bitfury_device_t *devices, int chip_count, int slot, int fs, int osc_bits) {
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

void bitfury_setSlotClk(bitfury_device_t *devices, int chip_count, int slot, int *fs_list) {

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


inline void test_reclock(bitfury_device_p dev) {

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

void init_devices (bitfury_device_t *devices, int chip_count) {
    int i;
    bitfury_device_p dev;


    for (i = 0; i < chip_count; i++) {
        dev = &devices[i];

#ifdef FAST_CLOCK1
            dev->osc6_bits = 53;
            if (!dev->osc6_bits_upd) dev->osc6_bits_upd = 53;
#else
            dev->osc6_bits = 54;
            if (!dev->osc6_bits_upd) dev->osc6_bits_upd = 54; // если не задано через опции командной строки
#endif

#ifndef BITFURY_AUTOCLOCK
            dev->fixed_clk = true;
#endif
            dev->rbc_stat[0] = dev->rbc_stat[1] = dev->rbc_stat[2] = dev->rbc_stat[3] = 0;
        }

    if (1) { // alpet: подстройка моих чипов (известные оптимумы)
        // overclocking/downclocking
        // 0x036, 0x136, 0x236, 0x336, 0x436, 0x536, 0x636


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
            mkdir(filename, 0x777);
    }
    else
        strcpy(filename, "");

    strncat(filename, ".cgminer/", PATH_MAX);
    mkdir(filename, 0x777);
    strncat(filename, "bitfury_opt.conf", PATH_MAX);
}


void load_opt_conf (bitfury_device_t *devices, int chip_count) {
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
                if ( i >= 0 ) {
                    bitfury_device_p dev = &devices[i];
                    memcpy( dev->cch_stat, v, sizeof(v) ); // update stat
                    int best = 0;
                    // поправка лучшего битклока по количеству выборов
                    for (i = 0; i < 4; i ++)
                        if ( best < v[i] ) {
                             best = v[i];
                             dev->osc6_bits_upd = BASE_OSC_BITS + i;
                        }
               }
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


void save_opt_conf (bitfury_device_t *devices, int chip_count) {
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
        bitfury_device_p dev = &devices[i];

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


struct timeval* get_cgtime() {
    static struct timeval now;
    cgtime(&now);
    return &now;
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

    struct work* nw = NULL;

    int max_need = cgpu->chip_count / 3 + 1;

    if (max_need > PREFETCH_WORKS)
        max_need = PREFETCH_WORKS;

    int now_need =  ( max_need - works_prefetched(cgpu) );
    ret = ( now_need <= 0 ); // need find optimal values

    if (ret) return ret;
    nw = get_queued (cgpu);
    if (NULL == nw) return false;
    rd_lock(&cgpu->qlock); // don't return before unlock(!)
    for (i = 0; i < PREFETCH_WORKS; i ++)  {
        if ( NULL == cgpu->prefetch [cgpu->w_prefetch] ) {
            nw->debug_stage = 128;
            cgpu->prefetch [cgpu->w_prefetch] = nw;
            now_need --;
            break;
        }
        cgpu->w_prefetch = next_prefetch ( cgpu->w_prefetch );
    }

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


inline uint64_t works_receive(thr_info_t *thr, bitfury_device_t *devices, int chip_count) {

    uint64_t hashes = 0;
    struct timeval now;


    int chip;

    for (chip = 0;chip < chip_count; chip++) {
        int nonces_cnt = 0;
        bitfury_device_p dev = &devices[chip];

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
                o2work->debug_stage = 245;
                work_completed(thr->cgpu, o2work);
                double diff = tv_diff (&now, &dev->work_start);
                dev->work_end = now;

                if (dev->work_median == 0)
                    dev->work_median = diff;
                else
                    dev->work_median = dev->work_median * 0.993 + diff *0.007; // EMA
            }
            // сдвиг миниочереди
            if (dev->o2work) dev->o2work->debug_stage = 193;
            if (dev->owork)  dev->owork->debug_stage = 192;
            if (dev->work)   dev->work->debug_stage = 191;
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

inline int work_push(thr_info_t *thr, bitfury_device_p dev) {
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
        dev->work->debug_stage = 190;
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

void dump_histogram(short *stat, char *buff, size_t buff_sz) {
    int i;
    for (i = 0; i < 50; i ++)
        if ( stat[i] ) {
            int n;
            size_t l = strlen(buff);
            if (l >= buff_sz) break;

            snprintf (buff + l, buff_sz - l, "\t%.1f = ", 0.1 * (float)i );
            for (n = 0; n < buff[i]; n ++)
                strncat(buff, "*", buff_sz);
            strncat (buff, "\t\t\t\t\n\r", buff_sz);
        }
}

void dump_chip_eff (bitfury_device_p dev, int ridx) {
    char filename[PATH_MAX];
    strcpy(filename, "/var/log/bitfury/");
    mkdir(filename, 0x777);
    size_t l = strlen(filename);
    snprintf(filename + l, PATH_MAX - l, "slot%X_chip%X.log", dev->slot, dev->fasync);

    FILE *f = fopen(filename, "a");
    if (!f) {
        applog(LOG_WARNING, "Cannot open file %s for append", filename);
        return;
    }

    char buff[4096];
    format_time ( get_cgtime(), buff );
    fprintf(f, "%s --------------------- \n", buff);

    buff[0] = 0;
    short *stat = dev->big_stat[ridx];
    dump_histogram ( stat, buff, 4096 );


    float median = 0;
    float count = 0;
    for (l = 1; l < 50; l ++) {
        if (stat[l] < 5) continue; // не существенные результаты
        count += (float) stat[l];
        median += 0.1 * (float) ( l * stat[l] );
    }


    fprintf(f, "%s", buff);
    if (count > 0) {
        dev->eff_speed = median / count;
        fprintf(f, "osc6_bits = %d, eff_speed = %.2f Gh/s, hw_rate = %.1f%% \n", BASE_OSC_BITS + ridx, dev->eff_speed, dev->hw_rate);
    }
    fclose(f);

}


static int64_t try_scanHash(thr_info_t *thr)
{

    static bitfury_device_t *devices, *dev; // TODO Move somewhere to appropriate place
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

    short chips_by_rate[50] = { 0 };


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
            #ifdef BITFURY_ENABLE_SHORT_STAT
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
            #endif
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

                float ema_value = 1;
                if ( dev->csw_back < 32 )
                         ema_value = 1; // усреднять ближние циклы

                float prev_part = ( 1 - 1 / ema_value );

                ema_ghash = dev->rbc_stat[ridx] * prev_part + ghash * (1 - prev_part); // up to 300 loops for stat
                // ema_ghash -= dev->hw_rate / 1000;  // для улучшения сортировки

                if ( ema_ghash < 0.1 || elps_eff > elps_mcs ) // на случай аномалии или достаточно продолжительной работы
                     ema_ghash = ghash;

                dev->rbc_stat[ridx] = ema_ghash;
                gh[dev->slot][i_chip] = ema_ghash;
            }

            i = (int) round ( ema_ghash * 10 );
            if ( i >= 49 ) i = 49;
            chips_by_rate [i] ++;
            if (dev->csw_back > 12) // если после переключения прошло много времени, и производительность стабилизировалась.
                dev->big_stat[ridx][i] ++; // для получения детального отчета по чипу


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

                dump_chip_eff (dev, ridx);
                if (dev->eff_speed > 0) {
                    dev->rbc_stat[ridx] = dev->eff_speed; // наиболее честный (?)
                    gh[dev->slot][i_chip] = dev->eff_speed;
                }

                // проверки на слишком маленькую частоту
                if ( dev->csw_back > 50 && dev->eff_speed > 0 && dev->eff_speed < LOW_HASHRATE) dev->fixed_clk = false;

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
            // сброс чипа, если три цикла подряд слишком маленький хэшрейт
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
                    // если произошла смена к удачной конфигурации или было мало регистраций выбора
                    if ( ridx != new_clk || dev->cch_stat[new_clk] < 2 ) dev->cch_stat[new_clk] ++;
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
        // printing histogram
        strcpy(line, "Chips by rate stats:\t\t\t\t\n\r");
        dump_histogram(chips_by_rate, line, 2048);
        applog(LOG_WARNING, "%s", line);

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
        // malloc_stats();
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

static int64_t bitfury_scanHash(thr_info_t *thr) {
     struct timeval now;
     double time_ms;
     cgtime (&now);
     time_ms = tv2mcs (&now) * 0.001;
     int64_t result = try_scanHash(thr);
     cgtime (&now);
     time_ms = tv2mcs (&now) * 0.001 - time_ms; // how elapsed
     if ( 0 == result ) nmsleep ( BITFURY_SCANHASH_DELAY - (int)time_ms ); // strict loop time
     if (time_ms > 500)
         applog(LOG_WARNING, "#PERF: scanHash loop complete in %.1f msec", time_ms);

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


static void bitfury_shutdown(thr_info_t *thr)
{
    int chip_count;
    int i;

    chip_count = thr->cgpu->chip_count;

    applog(LOG_INFO, "INFO bitfury_shutdown");
    libbitfury_shutdownChips(thr->cgpu->devices, chip_count);
}

static void bitfury_disable(thr_info_t *thr)
{
    applog(LOG_INFO, "INFO bitfury_disable");
}


static void get_options(struct cgpu_info *cgpu)
{
    char buf[BUFSIZ+1];
    char *ptr, *comma, *colon, *colon2;
    size_t max = 0;
    int i, slot, fs, bits, chip, def_bits;

    int default_bits = BASE_OSC_BITS + 1;

#ifdef FAST_CLOCK1
    default_bits = 53;
#endif


    for(i=0; i<cgpu->chip_count; i++)
        cgpu->devices[i].osc6_bits_upd = default_bits; // this is default value

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
                        cgpu->devices[chip].osc6_bits_upd = bits;
                        applog(LOG_INFO, "Set clockbits: slot=%d chip=%d bits=%d", slot, fs, bits);
                    }
                }
            } else {
                def_bits = atoi(buf);
                if(def_bits >= 48 && def_bits <= 56) {
                    for(i=0; i<cgpu->chip_count; i++)
                        cgpu->devices[i].osc6_bits_upd = def_bits;
                }
            }
        }
        if(comma != NULL)
            ptr = ++comma;
    } while (comma != NULL);
} // */

static bool bitfury_prepare(thr_info_t *thr)
{
    struct timeval now;
    struct cgpu_info *cgpu = thr->cgpu;

    cgtime(&now);
    get_datestamp(cgpu->init, &now);

    get_options(cgpu);

    applog(LOG_INFO, "INFO bitfury_prepare");
    return true;
}


static struct api_data *bitfury_api_stats(struct cgpu_info *cgpu)
{
    struct api_data *root = NULL;
    static bitfury_device_t *devices;
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

