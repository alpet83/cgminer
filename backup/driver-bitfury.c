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

#define GOLDEN_BACKLOG 5

struct device_drv bitfury_drv;

// Forward declarations
static void bitfury_disable(struct thr_info* thr);
static bool bitfury_prepare(struct thr_info *thr);
int calc_stat(time_t * stat_ts, time_t stat, struct timeval now);
double shares_to_ghashes(int shares, int seconds);

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

static int64_t bitfury_scanHash(struct thr_info *thr)
{
	static struct bitfury_device *devices; // TODO Move somewhere to appropriate place
	int chip_n;
	int chip;
	uint64_t hashes = 0;
	struct timeval now;
	unsigned char line[2048];
	int short_stat = 10;
	static time_t short_out_t;
	int long_stat = 600;
	static time_t long_out_t;
	int long_long_stat = 60 * 30;
	static time_t long_long_out_t;
	static first = 0; //TODO Move to detect()
	int i;

	devices = thr->cgpu->devices;
	chip_n = thr->cgpu->chip_n;

	if (!first) {
		for (i = 0; i < chip_n; i++) {
			if(i==9 || i==38) // manual chip tuning :)
				devices[i].osc6_bits = 53;
			else
				devices[i].osc6_bits = 54;
		}
		for (i = 0; i < chip_n; i++) {
			send_reinit(devices[i].slot, devices[i].fasync, devices[i].osc6_bits);
		}
	}
	first = 1;

	for (chip = 0; chip < chip_n; chip++) {
		devices[chip].job_switched = 0;
		if(!devices[chip].work) {
			devices[chip].work = get_queued(thr->cgpu);
			if (devices[chip].work == NULL) {
				return 0;
			}
			work_to_payload(&(devices[chip].payload), devices[chip].work);
		}
	}

	libbitfury_sendHashData(devices, chip_n);
	nmsleep(5);

	cgtime(&now);
	chip = 0;
	for (;chip < chip_n; chip++) {
		if (devices[chip].job_switched) {
			int i,j;
			int *res = devices[chip].results;
			struct work *work = devices[chip].work;
			struct work *owork = devices[chip].owork;
			struct work *o2work = devices[chip].o2work;
			i = devices[chip].results_n;
			for (j = i - 1; j >= 0; j--) {
				if (owork) {
					submit_nonce(thr, owork, bswap_32(res[j]));
					devices[chip].stat_ts[devices[chip].stat_counter++] =
						now.tv_sec;
					if (devices[chip].stat_counter == BITFURY_STAT_N) {
						devices[chip].stat_counter = 0;
					}
				}
				if (o2work) {
					// TEST
					//submit_nonce(thr, owork, bswap_32(res[j]));
				}
			}
			devices[chip].results_n = 0;
			devices[chip].job_switched = 0;
			if (devices[chip].old_nonce && o2work) {
					submit_nonce(thr, o2work, bswap_32(devices[chip].old_nonce));
					i++;
			}
			if (devices[chip].future_nonce) {
					submit_nonce(thr, work, bswap_32(devices[chip].future_nonce));
					i++;
			}

			if (o2work)
				work_completed(thr->cgpu, o2work);

			devices[chip].o2work = devices[chip].owork;
			devices[chip].owork = devices[chip].work;
			devices[chip].work = NULL;
			hashes += 0xffffffffull * i;
			devices[chip].matching_work += i;
		}
	}
	if (now.tv_sec - short_out_t > short_stat) {
		int shares_first = 0, shares_last = 0, shares_total = 0;
		char stat_lines[BITFURY_MAXBANKS][256] = {0};
		int len, k;
		double gh[BITFURY_MAXBANKS][BITFURY_BANKCHIPS] = {0};
		double ghsum = 0, gh1h = 0, gh2h = 0;
		unsigned strange_counter = 0;

		for (chip = 0; chip < chip_n; chip++) {
			int shares_found = calc_stat(devices[chip].stat_ts, short_stat, now);
			double ghash;
			len = strlen(stat_lines[devices[chip].slot]);
			ghash = shares_to_ghashes(shares_found, short_stat);
			gh[devices[chip].slot][chip % BITFURY_BANKCHIPS] = ghash;
			snprintf(stat_lines[devices[chip].slot] + len, 256 - len, "%.1f-%3.0f ", ghash, devices[chip].mhz);

			if(short_out_t && ghash < 0.5) {
				//applog(LOG_WARNING, "Chip_id %d FREQ CHANGE", chip);
				send_freq(devices[chip].slot, devices[chip].fasync, devices[chip].osc6_bits - 1);
				nmsleep(1);
				send_freq(devices[chip].slot, devices[chip].fasync, devices[chip].osc6_bits);
			}
			shares_total += shares_found;
			shares_first += chip < BITFURY_BANKCHIPS/2 ? shares_found : 0;
			shares_last += chip >= BITFURY_BANKCHIPS/2 ? shares_found : 0;
			strange_counter += devices[chip].strange_counter;
			//devices[chip].strange_counter = 0;
		}
#ifdef BITFURY_ENABLE_SHORT_STAT
		sprintf(line, "vvvvwww SHORT stat %ds: wwwvvvv", short_stat);
		applog(LOG_WARNING, line);
		//sprintf(line, "stranges: %u", strange_counter);
		applog(LOG_WARNING, line);
		for(i = 0; i < BITFURY_MAXBANKS; i++)
			if(strlen(stat_lines[i])) {
				len = strlen(stat_lines[i]);
				ghsum = 0;
				gh1h = 0;
				gh2h = 0;
				for(k = 0; k < BITFURY_BANKCHIPS; k++) {
					gh1h += gh[i][k];
					gh2h += gh[i][k + BITFURY_BANKCHIPS/2];
					ghsum += gh[i][k] + gh[i][k + BITFURY_BANKCHIPS/2];
				}
				snprintf(stat_lines[i] + len, 256 - len, "- %2.1f + %2.1f = %2.1f slot %i ", gh1h, gh2h, ghsum, i);
				applog(LOG_WARNING, stat_lines[i]);
			}
#endif
		short_out_t = now.tv_sec;
	}
#ifdef BITFURY_ENABLE_LONG_STAT
	if (now.tv_sec - long_out_t > long_stat) {
		int shares_first = 0, shares_last = 0, shares_total = 0;
		char stat_lines[BITFURY_MAXBANKS][256] = {0};
		int len, k;
		double gh[BITFURY_MAXBANKS][BITFURY_BANKCHIPS] = {0};
		double ghsum = 0, gh1h = 0, gh2h = 0;

		for (chip = 0; chip < chip_n; chip++) {
			int shares_found = calc_stat(devices[chip].stat_ts, long_stat, now);
			double ghash;
			len = strlen(stat_lines[devices[chip].slot]);
			ghash = shares_to_ghashes(shares_found, long_stat);
			gh[devices[chip].slot][chip % BITFURY_BANKCHIPS] = ghash;
			snprintf(stat_lines[devices[chip].slot] + len, 256 - len, "%.1f-%3.0f ", ghash, devices[chip].mhz);
			shares_total += shares_found;
			shares_first += chip < BITFURY_BANKCHIPS/2 ? shares_found : 0;
			shares_last += chip >= BITFURY_BANKCHIPS/2 ? shares_found : 0;
		}
		sprintf(line, "!!!_________ LONG stat %ds: ___________!!!", long_stat);
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
				snprintf(stat_lines[i] + len, 256 - len, "- %2.1f + %2.1f = %2.1f slot %i ", gh1h, gh2h, ghsum, i);
				applog(LOG_WARNING, stat_lines[i]);
			}
		long_out_t = now.tv_sec;
	}
#endif
	return hashes;
}

double shares_to_ghashes(int shares, int seconds) {
	return (double)shares / (double)seconds * 4.84387;  //orig: 4.77628
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
		sprintf(mcw, "strange_count_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_uint(root, mcw, &(devices[i].strange_counter), false);
	}
	for (i = 0; i < cgpu->chip_n; i++) {
		shares_found = calc_stat(devices[i].stat_ts, BITFURY_API_STATS, now);
		ghash = shares_to_ghashes(shares_found, BITFURY_API_STATS);
		ghash_sum += ghash;
		sprintf(mcw, "ghash_%d_%d", devices[i].slot, devices[i].fasync);
		root = api_add_double(root, mcw, &(ghash), true);
	}
	api_add_double(root, "ghash_total", &(ghash_sum), true);
	ghash_sum /= cgpu->chip_n;
	api_add_double(root, "ghash_avg", &(ghash_sum), true);

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

