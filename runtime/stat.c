/* -*- linux-c -*-
 * Statistics Aggregation
 * Copyright (C) 2005-2016 Red Hat Inc.
 * Copyright (C) 2006 Intel Corporation
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 */
#ifndef _STAT_C_
#define _STAT_C_

/** @file stat.c
 * @brief Statistics Aggregation
 */
/** @addtogroup stat Statistics Aggregation
 * The Statistics aggregations keep per-cpu statistics. You
 * must create all aggregations at probe initialization and it is
 * best to not read them until probe exit. If you must read them
 * while probes are running, the values may be slightly off due
 * to a probe updating the statistics of one cpu while another cpu attempts
 * to read the same data. This will also negatively impact performance.
 *
 * Stats keep track of count, sum, min, max, avg, and variance.  Bit-shift
 * can be optionally specified, scaling the numbers, in order to improve the
 * accuracy of the integer arithmetics.
 *
 * Histograms are optional. If you want a histogram, you must set "type"
 * to HIST_LOG or HIST_LINEAR when you call _stp_stat_init().
 *
 * @{
 */

#include "stat-common.c"


/** Initialize a Stat.
 * Call this during probe initialization to create a Stat.
 *
 * @param type (KEY_HIST_TYPE and associated parameters) 
 *
 * For HIST_LOG, the following additional parametrs are required:
 * @param buckets - An integer specifying the number of buckets.
 *
 * For HIST_LINEAR, the following additional parametrs are required:
 * @param start - An integer. The start of the histogram.
 * @param stop - An integer. The stopping value. Should be > start.
 * @param interval - An integer. The interval.
 *
 * @param stat_ops (STAT_OP_* and associated parameter bit_shift for STAT_OP_VARIANCE)
 */
static Stat _stp_stat_init (int first_arg, ...)
{
	int size, buckets=0, start=0, stop=0, interval=0, bit_shift=0;
	int stat_ops=0, htype=0;
	int arg = first_arg;
	Stat st;
	va_list ap;

	va_start (ap, first_arg);
	do {
		switch (arg) {
		case KEY_HIST_TYPE:
			htype = va_arg(ap, int);
			if (htype == HIST_LINEAR) {
				start = va_arg(ap, int);
				stop = va_arg(ap, int);
				interval = va_arg(ap, int);

				buckets = _stp_stat_calc_buckets(stop, start, interval);
				if (!buckets) {
					va_end (ap);
					return NULL;
				}
			}
			if (htype == HIST_LOG)
				buckets = HIST_LOG_BUCKETS;
                        break;
		case STAT_OP_COUNT:
			stat_ops |= STAT_OP_COUNT;
			break;
		case STAT_OP_SUM:
			stat_ops |= STAT_OP_SUM;
			break;
		case STAT_OP_MIN:
			stat_ops |= STAT_OP_MIN;
			break;
		case STAT_OP_MAX:
			stat_ops |= STAT_OP_MAX;
			break;
		case STAT_OP_AVG:
			stat_ops |= STAT_OP_AVG;
			break;
		case STAT_OP_VARIANCE:
			stat_ops |= STAT_OP_VARIANCE;
			bit_shift = va_arg(ap, int);
			break;
		default:
			_stp_warn ("Unknown argument %d\n", arg);
		}
		arg = va_arg(ap, int);
	} while (arg);
	va_end (ap);

	size = buckets * sizeof(int64_t) + sizeof(stat_data);
	st = _stp_stat_alloc (size);
	if (st == NULL)
		return NULL;

	st->hist.type = htype;
	st->hist.start = start;
	st->hist.stop = stop;
	st->hist.interval = interval;
	st->hist.buckets = buckets;
	st->hist.bit_shift = bit_shift;
	st->hist.stat_ops = stat_ops;
	return st;
}

/** Delete Stat.
 * Call this to free all memory allocated during initialization.
 *
 * @param st Stat
 */
static void _stp_stat_del (Stat st)
{
	if (st)
		_stp_stat_free(st);
}

/** Add to a Stat.
 * Add an int64 to a Stat.
 *
 * @param st Stat
 * @param val Value to add
 */
static void _stp_stat_add (Stat st, int64_t val)
{
	stat_data *sd = _stp_stat_per_cpu_ptr (st, STAT_GET_CPU());
	STAT_LOCK(sd);
	__stp_stat_add (&st->hist, sd, val);
	STAT_UNLOCK(sd);
	STAT_PUT_CPU();
}


static void _stp_stat_clear_data (Stat st, stat_data *sd)
{
        int j;
        sd->count = sd->sum = sd->min = sd->max = 0;
        sd->avg = sd->avg_s = sd->variance = sd->variance_s = 0;

        if (st->hist.type != HIST_NONE) {
                for (j = 0; j < st->hist.buckets; j++)
                        sd->histogram[j] = 0;
        }
}

/** Get Stats.
 * Gets the aggregated Stats for all CPUs.
 *
 * @param st Stat
 * @param clear Set if you want the data cleared after the read. Useful
 * for polling.
 * @returns A pointer to a stat.
 */
static stat_data *_stp_stat_get (Stat st, int clear)
{
	int i, j;
	int64_t S1, S2;
	stat_data *agg = _stp_stat_get_agg(st);
	stat_data *sd;
	STAT_LOCK(agg);
	_stp_stat_clear_data (st, agg);
	S1 = S2 = 0;

	for_each_possible_cpu(i) {
		stat_data *sd = _stp_stat_per_cpu_ptr (st, i);
		STAT_LOCK(sd);
		if (sd->count) {
			agg->shift = sd->shift;
			if (agg->count == 0) {
				agg->min = sd->min;
				agg->max = sd->max;
			}
			agg->count += sd->count;
			agg->sum += sd->sum;
			if (sd->max > agg->max)
				agg->max = sd->max;
			if (sd->min < agg->min)
				agg->min = sd->min;
			if (st->hist.type != HIST_NONE) {
				for (j = 0; j < st->hist.buckets; j++)
					agg->histogram[j] += sd->histogram[j];
			}
		}
		STAT_UNLOCK(sd);
	}

	agg->avg_s = _stp_div64(NULL, agg->sum << agg->shift, agg->count);

	/*
	 * For aggregating variance over available CPUs, the Total Variance
	 * formula is being used.  This formula is mentioned in following
	 * paper: Niranjan Kamat, Arnab Nandi: A Closer Look at Variance
	 * Implementations In Modern Database Systems: SIGMOD Record 2015.
	 * Available at: http://web.cse.ohio-state.edu/~kamatn/variance.pdf
	 */
	for_each_possible_cpu(i) {
		sd = _stp_stat_per_cpu_ptr (st, i);
		STAT_LOCK(sd);
		if (sd->count) {
			S1 += sd->count * (sd->avg_s - agg->avg_s) * (sd->avg_s - agg->avg_s);
			S2 += (sd->count - 1) * sd->variance_s;
		}
		if (clear)
			_stp_stat_clear_data (st, sd);
		STAT_UNLOCK(sd);
	}

	agg->variance_s = _stp_div64(NULL, (S1 + S2), (agg->count - 1));

	/*
	 * Setting agg->avg = agg->avg_s >> agg->shift; below would
	 * introduce slight rounding errors.
	 */
	agg->avg = _stp_div64(NULL, agg->sum, agg->count);
	agg->variance = agg->variance_s >> (2 * agg->shift);

	/*
	 * Originally this function returned the aggregate still
	 * locked and it was the caller's responsibility to unlock the
	 * aggregate. However the translator generated code that called
	 * this function wasn't unlocking it...
	 *
	 * But, the translator generates its own locks for global
	 * variables (like stats), so we don't need to return the
	 * aggregate still locked.
	 *
	 * It is possible we could even skip locking the aggregate in
	 * this function, but to be a bit paranoid lets keep the
	 * locking.
	 */
	STAT_UNLOCK(agg);
	return agg;
}


/** Clear Stats.
 * Clears the Stats.
 *
 * @param st Stat
 */
static void _stp_stat_clear (Stat st)
{
	int i;

	for_each_possible_cpu(i) {
		stat_data *sd = _stp_stat_per_cpu_ptr (st, i);
		STAT_LOCK(sd);
		_stp_stat_clear_data (st, sd);
		STAT_UNLOCK(sd);
	}
}
/** @} */
#endif /* _STAT_C_ */
