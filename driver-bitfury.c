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
#include <curses.h>

#define GOLDEN_BACKLOG 5

struct device_drv bitfury_drv;

// Forward declarations
static void bitfury_disable(struct thr_info* thr);
static bool bitfury_prepare(struct thr_info *thr);
int calc_stat(time_t * stat_ts, time_t stat, struct timeval now);
int calc_stat_f(double * stat_ts, double elapse, double now_mcs);
double shares_to_ghashes(int shares, int seconds);

inline double tv2mcs(struct timeval *tv) {
    return (double)tv->tv_sec * 1e6 + (double)tv->tv_usec;
}


static void bitfury_detect(void)
{
    int chip_n;
    int i;
    struct cgpu_info *bitfury_info;

    bitfury_info = calloc(1, sizeof(struct cgpu_info));
    bitfury_info->drv = &bitfury_drv;
    bitfury_info->threads = 1;

    applog(LOG_INFO, "INFO: bitfury_detect");
    chip_n = libbitfury_detectChips(bitfury_info->devices);
    if (!chip_n) {
        applog(LOG_WARNING, "No Bitfury chips detected!");
        return;
    } else {
        applog(LOG_WARNING, "BITFURY: %d chips detected!", chip_n);
    }

    bitfury_info->chip_n = chip_n;
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


int bitfury_findChip(struct bitfury_device *devices, int chip_n, int slot, int fs) {
    int n;
    for (n = 0; n < chip_n; n++) {
        if ( (devices[n].slot == slot) && (devices[n].fasync == fs) )
            return n;
    }
    return -1;
}

void bitfury_setChipClk(struct bitfury_device *devices, int chip_n, int slot, int fs, int osc_bits) {
    int n = bitfury_findChip(devices, chip_n, slot, fs);
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

void bitfury_setSlotClk(struct bitfury_device *devices, int chip_n, int slot, int *fs_list) {

    int n;
    for ( n = 0; ( fs_list[n] >= 0 ) && ( n < BITFURY_BANKCHIPS ); n ++ ) {
        int fs = fs_list[n];
        int osc_bits = fs & 0xFF; // low 8 bits
        fs = fs >> 8; // high 24 bits is slot
        bitfury_setChipClk (devices, chip_n, slot, fs, osc_bits);
    }
}


double tv_diff(PTIMEVAL a, PTIMEVAL b) {
    double diff = tv2mcs(a) - tv2mcs(b);
    if (diff < 0) diff += 24.0 *3600.0 * 1e6; // add one day
    return diff;
}


inline void test_reclock(PBITFURY_DEVICE dev) {

    if ( dev->osc6_bits != dev->osc6_bits_upd ) {
         dev->osc6_bits = dev->osc6_bits_upd;
         send_freq( dev->slot, dev->fasync, dev->osc6_bits );
         applog(LOG_WARNING, " for slot 0x%X chip 0x%X, osc6_bits changed to %d ", dev->slot, dev->fasync, dev->osc6_bits );
    }
}

void init_devices (struct bitfury_device *devices, int chip_n) {
    int i;

    for (i = 0; i < chip_n; i++) {
            devices[i].osc6_bits = 54;
            devices[i].osc6_bits_upd = 54;
        }

    if (1) { // alpet: подстройка моих чипов (известные оптимумы)
        // overclocking/downclocking
        // 0x036, 0x136, 0x236, 0x336, 0x436, 0x536, 0x636
        int slot_0 [] = { 0x037, 0x437, 0x536, 0x636, 0x737, -1 };
        int slot_1 [] = { 0x036, 0x235, 0x635, 0x737, -1 };
        int slot_2 [] = { 0x037, 0x236, 0x635, 0x437, -1 };
        int slot_3 [] = { 0x036, 0x236, 0x436, 0x536, 0x637, -1 };
        int slot_4 [] = { 0x137, 0x236, 0x335, 0x536, 0x636, 0x737, -1 };
        int slot_5 [] = { 0x036, 0x136, 0x235, 0x336, 0x435, 0x536, 0x636, 0x736, -1 };
        int slot_6 [] = { 0x035, 0x136, 0x336, 0x436, 0x636,  -1 };
        int slot_7 [] = { 0x036, 0x136, 0x336, 0x336, 0x436, 0x636, 0x737, -1 };
        int slot_8 [] = { 0x136, 0x235, 0x336, 0x437, 0x537, 0x636, 0x736, -1 };
        int slot_9 [] = { 0x036, 0x136, 0x235, 0x336, 0x436, 0x537, 0x637, 0x736 -1 };
        int slot_A [] = { 0x036, 0x136, 0x236, 0x337, 0x436, 0x636, 0x736, -1 }; //
        int slot_B [] = { -1 }; // absent
        int slot_C [] = { 0x536, 0x636, 0x736, -1 };
        int slot_D [] = { 0x236, 0x336, 0x536, -1 };
        int slot_E [] = { 0x035, 0x136, 0x236, 0x336, 0x736, -1 };
        int slot_F [] = { 0x136, 0x236, 0x336, 0x436, 0x536, 0x636, 0x736, -1 };

        int *all_slots[] = { slot_0, slot_1, slot_2, slot_3, slot_4, slot_5, slot_6, slot_7, slot_8, slot_9, slot_A, slot_B, slot_C, slot_D, slot_E, slot_F, NULL };

        for (i = 0; ( i < BITFURY_MAXBANKS ) && all_slots[i]; i ++)
             bitfury_setSlotClk(devices, chip_n, i, all_slots[i] );

        /*
        bitfury_setSlotClk(devices, chip_n, 0x01, slot_1, 55);

        bitfury_setSlotClk(devices, chip_n, 0x03, slot_3, 55);

        bitfury_setSlotClk(devices, chip_n, 0x05, slot_5, 55);
        bitfury_setSlotClk(devices, chip_n, 0x06, slot_6, 55);
        bitfury_setSlotClk(devices, chip_n, 0x07, slot_7, 55);
        bitfury_setSlotClk(devices, chip_n, 0x08, slot_8, 55);
        bitfury_setSlotClk(devices, chip_n, 0x09, slot_9, 55);
        bitfury_setSlotClk(devices, chip_n, 0x0A, slot_A, 55);
        bitfury_setSlotClk(devices, chip_n, 0x0C, slot_C, 55);
        bitfury_setSlotClk(devices, chip_n, 0x0D, slot_D, 55);
        bitfury_setSlotClk(devices, chip_n, 0x0E, slot_E, 54);
        bitfury_setSlotClk(devices, chip_n, 0x0F, slot_F, 55); // */

        // bitfury_setSlotClk(devices, chip_n, 0x08, slot_8, 55);

        // downclocking
        // for (i = 0; i < 8; i++)  bitfury_setChipClk(devices, chip_n, 14, i, 53); // полный слот неудачников (перегрев?)
    }

    for (i = 0; i < chip_n; i++) {
        send_reinit(devices[i].slot, devices[i].fasync, devices[i].osc6_bits);
    }
}


inline uint64_t works_receive(struct thr_info *thr, struct bitfury_device *devices, int chip_n) {

    uint64_t hashes = 0;
    struct timeval now;
    cgtime(&now);

    int chip;

    for (chip = 0;chip < chip_n; chip++) {
        int nonces_cnt = 0;
        struct bitfury_device *dev = &devices[chip];
        if (dev->job_switched) {
            int j;
            int *res = dev->results;
            struct work *work = dev->work;
            struct work *owork = dev->owork;
            struct work *o2work = dev->o2work;
            for (j = dev->results_n-1; j >= 0; j--) {
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
        }
    }
    return hashes;
}

inline int work_send(struct thr_info *thr, PBITFURY_DEVICE dev) {
    dev->job_switched = 0;
    if ( dev->work == NULL )
    {
        test_reclock(dev); // думаю здесь самое лучшее место, чтобы чип перенастроить на другую частоту
        dev->work = get_queued(thr->cgpu);
        if (dev->work == NULL) return 0; // cannot get work

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

static unsigned loops_count = 0;


static int64_t bitfury_scanHash(struct thr_info *thr)
{
    static struct bitfury_device *devices, *dev; // TODO Move somewhere to appropriate place
    int chip_n;
    int chip;
    static no_work = 0;
    uint64_t hashes = 0;
    struct timeval now;
    unsigned char line[2048];
    int short_stat = 10;
    static time_t short_out_t;
    static double short_out_tf;
    int long_stat = 600;
    static time_t long_out_t = 0;
    int long_long_stat = 60 * 30;
    static time_t long_long_out_t;
    double elps_mcs = 0;
    double now_mcs = 0;

    static vc0_median[BITFURY_MAXBANKS];
    static vc1_median[BITFURY_MAXBANKS];
    static double ghs_median[BITFURY_MAXBANKS];

    static char CL_RESET[]     = "\033[0m";
    static char CL_LT_RED[]    = "\033[1;31m";
    static char CL_LT_GREEN[]  = "\033[1;32m";
    static char CL_LT_YELLOW[] = "\033[1;33m";
    static char CL_LT_BLUE[]   = "\033[1;34m";
    static char CL_LT_CYAN[]   = "\033[1;36m";
    static char CL_LT_WHITE[]  = "\033[1;37m";


    int i;
    static stat_dumps = 0;

    loops_count ++;

    devices = thr->cgpu->devices;
    chip_n = thr->cgpu->chip_n;
    cgtime(&now);

    if ( loops_count == 1 )
         init_devices(devices, chip_n);

    for (chip = 0; chip < chip_n; chip++) {

       int delay = 1100 / chip_n; // вписать планировку всех чипов в среднее время на задание

       if ( loops_count < 10 ) nmsleep(delay);


       if ( 0 == work_send(thr, &devices[chip]) )
       {
           no_work ++;
           // nmsleep(1); // процессору отбой
           return 0;
       }
    }



    libbitfury_sendHashData(thr, devices, chip_n);
    nmsleep(4);
    hashes = works_receive(thr, devices, chip_n);

    cgtime(&now);
    now_mcs = tv2mcs (&now);

    if (short_out_t == 0) {
        short_out_t = now.tv_sec;
        short_out_tf = now_mcs;
    }

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


        for (chip = 0; chip < chip_n; chip++) {
            dev = &devices[chip];
            int shares_found = calc_stat_f(dev->stat_tsf, elps_mcs, now_mcs);
            int i_chip = dev->fasync;
            int n_slot = dev->slot;
            double ghash;
            double alt_gh;

            // if slot changed
            if (n_slot != last_slot) {
                float slot_temp = tm_i2c_gettemp(n_slot) * 0.1;
                float slot_vc0 = tm_i2c_getcore0(n_slot) * 1000;
                float slot_vc1 = tm_i2c_getcore1(n_slot) * 1000;

                if (stat_dumps > 1) {
                    // checking anomaly extremums
                    if (slot_vc0 < vc0_median[n_slot] - 100) slot_vc0 = vc0_median[n_slot];
                    if (slot_vc1 < vc1_median[n_slot] - 100) slot_vc1 = vc1_median[n_slot];
                    slot_vc0 = vc0_median[n_slot] * 0.95 + slot_vc0 * 0.05;
                    slot_vc1 = vc1_median[n_slot] * 0.95 + slot_vc1 * 0.05;
                }

                vc0_median[n_slot] = slot_vc0;
                vc1_median[n_slot] = slot_vc1;

                sprintf(stat_lines[n_slot], "[%X] T:%3.0f | V: %.0f %.0f | ", n_slot, slot_temp, slot_vc0, slot_vc1);
                last_slot = n_slot;
            }

            len = strlen(stat_lines[n_slot]);
            ghash = shares_to_ghashes(shares_found, short_stat);
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
               dev->hw_rate = dev->hw_rate * 0.9 + hw_errs * 0.1; // EMA 10

            int ridx = dev->osc6_bits - 53;

            // сбор статистки по хэш-рейту на клок
            if ( ridx >= 0 ) {
                float prev = dev->rbc_stat[ridx] * 0.92;
                if ( dev->rbc_stat[ridx] > 0 ) //
                    dev->rbc_stat[ridx]  = dev->rbc_stat[ridx] * 0.93 + alt_gh * 0.07; // 20 loops for stat
                else
                     dev->rbc_stat[ridx] = alt_gh;
            }

            char *cl_tag = CL_RESET;
            if ( ghash < 1 )  cl_tag = CL_LT_RED;
            if ( ghash >= 2 ) cl_tag = CL_LT_YELLOW;
            if ( ghash >= 3 ) cl_tag = CL_LT_GREEN;



            if ( maskv < 15 ) {
                if ( ( maskv > 13 ) && ( dev->work_median > 0 ) )
                    snprintf(stat_lines[n_slot] + len, 256 - len, "%3.0f @%5.2f%%| ", alt_gh * 10, 100 * dev->work_wait / dev->work_median ); // speed from work-time, wait time
                else
                    snprintf(stat_lines[n_slot] + len, 256 - len, "%3.0f - %4.1f | ", ghash * 10, dev->hw_rate ); // speed and errors
            }
            else {
                char selected[4] = { 32, 32, 32, 32 };

                selected[ridx] = 0x3E;

                char s53 = selected[0];
                char s54 = selected[1];
                char s55 = selected[2];
                char s56 = selected[3];

                float h53 = dev->rbc_stat[0] * 10;
                float h54 = dev->rbc_stat[1] * 10;
                float h55 = dev->rbc_stat[2] * 10;
                float h56 = dev->rbc_stat[3] * 10;

                snprintf( stat_lines[n_slot] + len, 256 - len, "%c%2.0f%c%2.0f%c%2.0f%c%2.0f|", s53, h53, s54, h54, s55, h55, s56, h56 ); // intermediate dump clock
            }



            if ( ( stat_dumps > 16 ) && ( maskv == 15 ) && !dev->fixed_clk ) {
                // переключение клока принудительно

                int new_clk = 1;
                if ( stat_dumps < 128 ) new_clk = ( ridx + 0 ) & 3; // masked enum


                float best = 3; // extremum Ghz for 54 clk

                if ( stat_dumps > 128 ) {
                    // поиск наилучшего, для работы с заданным клоком.
                    best = dev->rbc_stat[ridx] + 0.1;
                    // if ( stat_dumps > 150 ) best = 3;

                    for (ridx = 0; ridx < 4; ridx ++) {
                        if ( best >= dev->rbc_stat [ridx] ) continue;
                        best = dev->rbc_stat [ridx];
                        new_clk = ridx; // optimus
                    } // for
                }

                new_clk = new_clk + 53;

                if ( dev->osc6_bits_upd != new_clk ) {
                     dev->osc6_bits_upd = new_clk;
                    // if ( dev->osc6_bits > 52 ) send_freq(n_slot, i_chip, dev->osc6_bits);
                }


                /*
                if ( ( dev->hw_rate < 10 ) && ( dev->osc6_bits < 56 ) ) {
                }
                if ( ( dev->hw_rate > 15.0 ) && ( dev->osc6_bits > 54 ) ){
                    dev->osc6_bits --; // downclock
                    applog(LOG_WARNING, " %d-DUMP: for slot 0x%X chip 0x%X, osc6_bits reduced to %d ", maskv, n_slot, i_chip, dev->osc6_bits );
                    send_freq(n_slot, i_chip, dev->osc6_bits);
                } else //
                if ( ( dev->hw_rate > 50.0 ) && ( dev->osc6_bits > 53 ) ){
                    dev->osc6_bits --; // downclock
                    applog(LOG_WARNING, " %d-DUMP: for slot 0x%X chip 0x%X, osc6_bits reduced to %d ", maskv, n_slot, i_chip, dev->osc6_bits );
                    send_freq(n_slot, i_chip, dev->osc6_bits);
                } // */
            }


            // snprintf(stat_lines[dev->slot] + len, 256 - len, "%.1f-%3.0f ", ghash, dev->mhz);

            if (ghash == 0) dev->alerts ++; else dev->alerts = 0;

            if ( 3 < dev->alerts ) {
                applog(LOG_WARNING, "Chip_id %d FREQ CHANGE-RESTORE", chip);
                send_freq(n_slot, i_chip, 54);
                nmsleep(100);
                send_reinit(n_slot, i_chip, 53); // fail-safe
            }
            shares_total += shares_found;
            shares_first += chip < BITFURY_BANKCHIPS/2 ? shares_found : 0;
            shares_last += chip >= BITFURY_BANKCHIPS/2 ? shares_found : 0;
            strange_counter += dev->hw_errors;

            dev->hw_errors = 0;

            //dev->strange_counter = 0;
        }
#ifdef BITFURY_ENABLE_SHORT_STAT
        // sprintf(line, "vvvvwww SHORT stat %ds: wwwvvvv", short_stat);
        sprintf(line, "  ================== SHORT stat, elapsed %.3fs, no_work = 5%d, dump %d =================== ", elps_mcs / 1e6, no_work, stat_dumps);
        applog(LOG_WARNING, line);
        //sprintf(line, "stranges: %u", strange_counter);
        double ghsm_saldo = 0;
        no_work = 0;

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

                if (stat_dumps > 4) {
                    if (stat_dumps > 50 )
                        ghmed = ghs_median[i] * 0.99 + ghsum * 0.01; // EMA 100
                    else
                        ghmed = ghs_median[i] * 0.9 + ghsum * 0.1; // EMA 10
                }

                snprintf(stat_lines[i] + len, 256 - len, " S: %4.1f + %4.1f = %4.1f  (%4.1f) [%X]", gh1h, gh2h, ghsum, ghmed, i);

                ghs_median[i] = ghmed;
                ghsm_saldo += ghmed;
                applog(LOG_WARNING, stat_lines[i]);
            }

        elapsed = now.tv_sec - long_out_t;
        applog(LOG_WARNING, "Median hash-rate saldo = %4.1f, seconds to long stat %5d ", ghsm_saldo, long_stat - elapsed );
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

        for (chip = 0; chip < chip_n; chip++) {
            dev = &devices[chip];
            int shares_found = calc_stat(dev->stat_ts, elapsed, now);
            double ghash;
            len = strlen(stat_lines[dev->slot]);
            ghash = shares_to_ghashes(shares_found, long_stat);
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

double shares_to_ghashes(int shares, int seconds) {
    return ( (double)shares * 4.294967296 ) / ( (double)seconds );

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
    int chip_n;
    int i;

    chip_n = thr->cgpu->chip_n;

    applog(LOG_INFO, "INFO bitfury_shutdown");
    libbitfury_shutdownChips(thr->cgpu->devices, chip_n);
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

    for(i=0; i<cgpu->chip_n; i++)
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
                    chip = bitfury_findChip(cgpu->devices, cgpu->chip_n, slot, fs);
                    if(chip > 0 && chip < cgpu->chip_n && bits >= 48 && bits <= 56) {
                        cgpu->devices[chip].osc6_bits_setpoint = bits;
                        applog(LOG_INFO, "Set clockbits: slot=%d chip=%d bits=%d", slot, fs, bits);
                    }
                }
            } else {
                def_bits = atoi(buf);
                if(def_bits >= 48 && def_bits <= 56) {
                    for(i=0; i<cgpu->chip_n; i++)
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
    root = api_add_int(root, "chip_n", &(cgpu->chip_n),false);
    cgtime(&now);

    for (i = 0; i < cgpu->chip_n; i++) {
        sprintf(mcw, "clock_bits_%d_%d", devices[i].slot, devices[i].fasync);
        osc_bits = (unsigned int)devices[i].osc6_bits;
        root = api_add_int(root, mcw, &(devices[i].osc6_bits), false);
    }
    for (i = 0; i < cgpu->chip_n; i++) {
        sprintf(mcw, "match_work_count_%d_%d", devices[i].slot, devices[i].fasync);
        root = api_add_uint(root, mcw, &(devices[i].matching_work), false);
    }
    for (i = 0; i < cgpu->chip_n; i++) {
        sprintf(mcw, "hw_errors_%d_%d", devices[i].slot, devices[i].fasync);
        root = api_add_uint(root, mcw, &(devices[i].hw_errors), false);
        total_hw += devices[i].hw_errors;
    }
    for (i = 0; i < cgpu->chip_n; i++) {
        shares_found = calc_stat(devices[i].stat_ts, BITFURY_API_STATS, now);
        ghash = shares_to_ghashes(shares_found, BITFURY_API_STATS);
        ghash_sum += ghash;
        sprintf(mcw, "ghash_%d_%d", devices[i].slot, devices[i].fasync);
        root = api_add_double(root, mcw, &(ghash), true);
    }
    api_add_uint64(root, "total_hw", &(total_hw), false);
    api_add_double(root, "total_gh", &(ghash_sum), true);
    ghash_sum /= cgpu->chip_n;
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
    .get_api_stats = bitfury_api_stats,
};

