/*

   maxxc - maximise cross country flights
   Copyright (C) 2008  Tom Payne

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "maxxc.h"

    void
trkpt_to_wpt(const trkpt_t *trkpt, wpt_t *wpt)
{
    wpt->lat = trkpt->lat;
    wpt->lon = trkpt->lon;
    wpt->time = trkpt->time;
    wpt->ele = trkpt->ele;
    wpt->name = 0;
    wpt->val = trkpt->val;
}

__attribute__ ((nonnull(1, 2))) __attribute__ ((pure))
    static inline double
coord_delta(const coord_t *coord1, const coord_t *coord2)
{
    double x = coord1->sin_lat * coord2->sin_lat + coord1->cos_lat * coord2->cos_lat * cos(coord1->lon - coord2->lon);
    return x < 1.0 ? acos(x) : 0.0;

}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline double
track_delta(const track_t *track, int i, int j)
{
    return coord_delta(track->coords + i, track->coords + j);
}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline int
track_forward(const track_t *track, int i, double d)
{
    int step = (int) (d / track->max_delta);
    return step > 0 ? i + step : ++i;
}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline int
track_fast_forward(const track_t *track, int i, double d)
{
    double target = track->sigma_delta[i] + d;
    i = track_forward(track, i, d);
    if (i >= track->ntrkpts)
        return i;
    while (1) {
        double error = target - track->sigma_delta[i];
        if (error <= 0.0)
            return i;
        i = track_forward(track, i, error);
        if (i >= track->ntrkpts)
            return i;
    }
}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline int
track_backward(const track_t *track, int i, double d)
{
    int step = (int) (d / track->max_delta);
    return step > 0 ? i - step : --i;
}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline int
track_fast_backward(const track_t *track, int i, double d)
{
    double target = track->sigma_delta[i] - d;
    i = track_backward(track, i, d);
    if (i < 0)
        return i;
    while (1) {
        double error = track->sigma_delta[i] - target;
        if (error <= 0.0)
            return i;
        i = track_backward(track, i, error);
        if (i < 0)
            return i;
    }
}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline int
track_furthest_from(const track_t *track, int i, int begin, int end, double bound, double *out)
{
    int result = -1;
    for (int j = begin; j < end; ) {
        double d = track_delta(track, i, j);
        if (d > bound) {
            bound = *out = d;
            result = j;
            ++j;
        } else {
            j = track_fast_forward(track, j, bound - d);
        }
    }
    return result;
}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline int
track_nearest_to(const track_t *track, int i, int begin, int end, double bound, double *out)
{
    int result = -1;
    for (int j = begin; j < end; ) {
        double d = track_delta(track, i, j);
        if (d < bound) {
            result = j;
            bound = *out = d;
            ++j;
        } else {
            j = track_fast_forward(track, j, d - bound);
        }
    }
    return result;
}

    static inline int
__attribute__ ((nonnull(1))) __attribute__ ((pure))
track_furthest_from2(const track_t *track, int i, int j, int begin, int end, double bound, double *out)
{
    int result = -1;
    for (int k = begin; k < end; ) {
        double d = track_delta(track, i, k) + track_delta(track, k, j);
        if (d > bound) {
            result = k;
            bound = *out = d;
            ++k;
        } else {
            k = track_fast_forward(track, k, (bound - d) / 2.0);
        }
    }
    return result;
}

    static inline int
__attribute__ ((nonnull(1))) __attribute__ ((pure))
track_first_at_least(const track_t *track, int i, int begin, int end, double bound)
{
    for (int j = begin; j < end; ) {
        double d = track_delta(track, i, j);
        if (d > bound)
            return j;
        j = track_fast_forward(track, j, bound - d);
    }
    return -1;
}

__attribute__ ((nonnull(1))) __attribute__ ((pure))
    static inline int
track_last_at_least(const track_t *track, int i, int begin, int end, double bound)
{
    for (int j = end - 1; j >= begin; ) {
        double d = track_delta(track, i, j);
        if (d > bound)
            return j;
        j = track_fast_backward(track, j, bound - d);
    }
    return -1;
}

__attribute__ ((nonnull(1, 2))) __attribute__ ((pure))
    static inline int
track_first_inside(const track_t *track, const coord_t *coord, double radius, int begin, int end)
{
    for (int i = begin; i < end; ) {
        double d = coord_delta(coord, track->coords + i);
        if (d <= radius)
            return i;
        i = track_forward(track, i, d - radius);
    }
    return -1;
}

__attribute__ ((nonnull(1, 2))) __attribute__ ((pure))
    static inline int
track_first_outside(const track_t *track, const coord_t *coord, double radius, int begin, int end)
{
    for (int i = begin; i < end; ) {
        double d = coord_delta(coord, track->coords + i);
        if (d > radius)
            return i;
        i = track_forward(track, i, d - radius);
    }
    return -1;
}

    static void
track_initialize(track_t *track)
{
    track->coords = alloc(track->ntrkpts * sizeof(coord_t));
#pragma omp parallel for schedule(static)
    for (int i = 0; i < track->ntrkpts; ++i) {
        double lat = M_PI * track->trkpts[i].lat / (180 * 60000);
        double lon = M_PI * track->trkpts[i].lon / (180 * 60000);
        track->coords[i].sin_lat = sin(lat);
        track->coords[i].cos_lat = cos(lat);
        track->coords[i].lon = lon;
    }
    track->max_delta = 0.0;
    track->sigma_delta = alloc(track->ntrkpts * sizeof(double));
    track->sigma_delta[0] = 0.0;
    for (int i = 1; i < track->ntrkpts; ++i) {
        double delta = track_delta(track, i - 1, i);
        track->sigma_delta[i] = track->sigma_delta[i - 1] + delta;
        if (delta > track->max_delta)
            track->max_delta = delta;
    }
#pragma omp parallel sections
    {
#pragma omp section
        {
#pragma omp critical
            track->before = alloc(track->ntrkpts * sizeof(limit_t));
            track->before[0].index = 0;
            track->before[0].distance = 0.0;
            for (int i = 1; i < track->ntrkpts; ++i)
                track->before[i].index = track_furthest_from(track, i, 0, i, track->before[i - 1].distance - track->max_delta, &track->before[i].distance);
        }
#pragma omp section
        {
#pragma omp critical
            track->after = alloc(track->ntrkpts * sizeof(limit_t));
            track->after[0].index = track_furthest_from(track, 0, 1, track->ntrkpts, 0.0, &track->after[0].distance);
            for (int i = 1; i < track->ntrkpts - 1; ++i)
                track->after[i].index = track_furthest_from(track, i, i + 1, track->ntrkpts, track->after[i - 1].distance - track->max_delta, &track->after[i].distance);
            track->after[track->ntrkpts - 1].index = track->ntrkpts - 1;
            track->after[track->ntrkpts - 1].distance = 0.0;
        }
    }
}

    void
track_compute_circuit_tables(track_t *track, double circuit_bound)
{
    track->last_finish = alloc(track->ntrkpts * sizeof(int));
    track->best_start = alloc(track->ntrkpts * sizeof(int));
    int current_best_start = 0, i, j;
    for (i = 0; i < track->ntrkpts; ++i) {
        for (j = track->ntrkpts - 1; j >= i; ) {
            double error = track_delta(track, i, j);
            if (error < circuit_bound) {
                track->last_finish[i] = j;
                break;
            } else {
                j = track_fast_backward(track, j, error - circuit_bound);
            }
        }
        if (track->last_finish[i] > track->last_finish[current_best_start])
            current_best_start = i;
        if (track->last_finish[current_best_start] < i) {
            current_best_start = 0;
            for (j = 1; j <= i; ++j)
                if (track->last_finish[j] > track->last_finish[current_best_start])
                    current_best_start = j;
        }
        track->best_start[i] = current_best_start;
    }
}

    static inline const char *
match_char(const char *p, char c)
{
    if (!p) return 0;
    return *p == c ? ++p : 0;
}

    static inline const char *
match_string(const char *p, const char *s)
{
    if (!p) return 0;
    while (*p && *s && *p == *s) {
        ++p;
        ++s;
    }
    return *s ? 0 : p;
}

    static inline const char *
match_unsigned(const char *p, int n, int *result)
{
    if (!p) return 0;
    *result = 0;
    for (; n > 0; --n) {
        if ('0' <= *p && *p <= '9') {
            *result = 10 * *result + *p - '0';
            ++p;
        } else {
            return 0;
        }
    }
    return p;
}

    static inline const char *
match_one_of(const char *p, const char *s, char *result)
{
    if (!p) return 0;
    for (; *s; ++s)
        if (*p == *s) {
            *result = *p;
            return ++p;
        }
    return 0;
}

    static inline const char *
match_capture_until(const char *p, char c, char **result)
{
    if (!p) return 0;
    const char *start = p;
    while (*p && *p != c)
        ++p;
    if (!p) return 0;
    *result = alloc(p - start + 1);
    memcpy(*result, start, p - start);
    (*result)[p - start] = '\0';
    return p;
}

    static inline const char *
match_until_eol(const char *p)
{
    if (!p) return 0;
    while (*p && *p != '\r')
        ++p;
    if (*p != '\r') return 0;
    ++p;
    return *p == '\n' ? ++p : 0;
}

    static const char *
match_b_record(const char *p, struct tm *tm, trkpt_t *trkpt)
{
    p = match_char(p, 'B');
    if (!p) return 0;

    int hour = 0, min = 0, sec = 0;
    p = match_unsigned(p, 2, &hour);
    p = match_unsigned(p, 2, &min);
    p = match_unsigned(p, 2, &sec);
    if (!p) return 0;

    int lat_deg = 0, lat_mmin = 0;
    char lat_hemi = 0;
    p = match_unsigned(p, 2, &lat_deg);
    p = match_unsigned(p, 5, &lat_mmin);
    p = match_one_of(p, "NS", &lat_hemi);
    int lat = 60000 * lat_deg + lat_mmin;
    if (lat_hemi == 'S') lat *= -1;

    int lon_deg = 0, lon_mmin = 0;
    char lon_hemi = 0;
    p = match_unsigned(p, 3, &lon_deg);
    p = match_unsigned(p, 5, &lon_mmin);
    p = match_one_of(p, "EW", &lon_hemi);
    int lon = 60000 * lon_deg + lon_mmin;
    if (lon_hemi == 'W') lon *= -1;

    char val = 0;
    p = match_one_of(p, "AV", &val);

    int alt = 0, ele = 0;
    p = match_unsigned(p, 5, &alt);
    p = match_unsigned(p, 5, &ele);

    p = match_until_eol(p);
    if (!p) return 0;

    tm->tm_hour = hour;
    tm->tm_min = min;
    tm->tm_sec = sec;
    time_t time;
    time = mktime(tm);
    if (time == (time_t) -1)
        DIE("mktime", errno);
    trkpt->time = time;
    trkpt->lat = lat;
    trkpt->lon = lon;
    trkpt->val = val;
    trkpt->alt = alt;
    trkpt->ele = ele;

    return p;
}

    static const char *
match_c_record(const char *p, wpt_t *wpt)
{
    p = match_char(p, 'C');
    if (!p) return 0;

    int lat_deg = 0, lat_mmin = 0;
    char lat_hemi = 0;
    p = match_unsigned(p, 2, &lat_deg);
    p = match_unsigned(p, 5, &lat_mmin);
    p = match_one_of(p, "NS", &lat_hemi);
    int lat = 60000 * lat_deg + lat_mmin;
    if (lat_hemi == 'S') lat *= -1;

    int lon_deg = 0, lon_mmin = 0;
    char lon_hemi = 0;
    p = match_unsigned(p, 3, &lon_deg);
    p = match_unsigned(p, 5, &lon_mmin);
    p = match_one_of(p, "EW", &lon_hemi);
    int lon = 60000 * lon_deg + lon_mmin;
    if (lon_hemi == 'W') lon *= -1;

    char *name = 0;
    p = match_capture_until(p, '\r', &name);

    p = match_until_eol(p);
    if (!p) return 0;

    wpt->time = (time_t) -1;
    wpt->lat = lat;
    wpt->lon = lon;
    wpt->val = 'V';
    wpt->ele = 0;
    wpt->name = name;

    return p;
}

    static const char *
match_hfdte_record(const char *p, struct tm *tm)
{
    int mday = 0, mon = 0, year = 0;
    p = match_string(p, "HFDTE");
    if (!p) return 0;
    p = match_unsigned(p, 2, &mday);
    p = match_unsigned(p, 2, &mon);
    p = match_unsigned(p, 2, &year);
    p = match_string(p, "\r\n");
    if (!p) return 0;
    tm->tm_year = year + 2000 - 1900;
    tm->tm_mon = mon - 1;
    tm->tm_mday = mday;
    return p;
}

    static void
track_push_trkpt(track_t *track, const trkpt_t *trkpt)
{
    if (track->ntrkpts == track->trkpts_capacity) {
        track->trkpts_capacity = track->trkpts_capacity ? 2 * track->trkpts_capacity : 16384;
        track->trkpts = realloc(track->trkpts, track->trkpts_capacity * sizeof(trkpt_t));
        if (!track->trkpts)
            DIE("realloc", errno);
    }
    track->trkpts[track->ntrkpts] = *trkpt;
    ++track->ntrkpts;
}

    static void
track_push_task_wpt(track_t *track, const wpt_t *task_wpt)
{
    if (track->ntask_wpts == track->task_wpts_capacity) {
        track->task_wpts_capacity = track->task_wpts_capacity ? 2 * track->task_wpts_capacity : 16;
        track->task_wpts = realloc(track->task_wpts, track->task_wpts_capacity * sizeof(wpt_t));
        if (!track->task_wpts)
            DIE("realloc", errno);
    }
    track->task_wpts[track->ntask_wpts] = *task_wpt;
    ++track->ntask_wpts;
}

    track_t *
track_new_from_igc(const char *filename, FILE *file)
{
    track_t *track = alloc(sizeof(track_t));
    track->filename = filename;

    struct tm tm;
    memset(&tm, 0, sizeof tm);
    trkpt_t trkpt;
    memset(&trkpt, 0, sizeof trkpt);
    wpt_t wpt;
    memset(&wpt, 0, sizeof wpt);
    char record[1024];
    while (fgets(record, sizeof record, file)) {
        int n = strlen(record);
        if (track->igc_size + n > track->igc_capacity) {
            track->igc_capacity = track->igc_capacity ? 2 * track->igc_capacity : 131072;
            track->igc = realloc(track->igc, track->igc_capacity);
            if (!track->igc)
                DIE("realloc", errno);
        }
        memcpy(track->igc + track->igc_size, record, n);
        track->igc_size += n;
        switch (record[0]) {
            case 'B':
                if (match_b_record(record, &tm, &trkpt))
                    track_push_trkpt(track, &trkpt);
                break;
            case 'C':
                if (match_c_record(record, &wpt))
                    track_push_task_wpt(track, &wpt);
                break;
            case 'H':
                match_hfdte_record(record, &tm);
                break;
        }
    }
    track_initialize(track);
    return track;
}

    void
track_delete(track_t *track)
{
    if (track) {
        free(track->trkpts);
        if (track->task_wpts) {
            for (int i = 0; i < track->ntask_wpts; ++i)
                free(track->task_wpts[i].name);
            free(track->task_wpts);
        }
        free(track->coords);
        free(track->sigma_delta);
        free(track->before);
        free(track->after);
        free(track->best_start);
        free(track->last_finish);
        free(track->igc);
        free(track);
    }
}

    static double
track_open_distance(const track_t *track, double bound, int *indexes)
{
    indexes[0] = indexes[1] = -1;
    for (int start = 0; start < track->ntrkpts - 1; ++start) {
        int finish = track_furthest_from(track, start, start + 1, track->ntrkpts, bound, &bound);
        if (finish != -1) {
            indexes[0] = start;
            indexes[1] = finish;
        }
    }
    return bound;
}

    static double
track_open_distance1(const track_t *track, double bound, int *indexes)
{
    indexes[0] = indexes[1] = indexes[2] = -1;
    for (int tp1 = 1; tp1 < track->ntrkpts - 1; ) {
        double total = track->before[tp1].distance + track->after[tp1].distance;
        if (total > bound) {
            indexes[0] = track->before[tp1].index;
            indexes[1] = tp1;
            indexes[2] = track->after[tp1].index;
            bound = total;
            ++tp1;
        } else {
            tp1 = track_fast_forward(track, tp1, 0.5 * (bound - total));
        }
    }
    return bound;
}

    static double
track_open_distance2(const track_t *track, double bound, int *indexes)
{
    indexes[0] = indexes[1] = indexes[2] = indexes[3] = -1;
#pragma omp parallel for schedule(dynamic)
    for (int tp1 = 1; tp1 < track->ntrkpts - 2; ++tp1) {
        double leg1 = track->before[tp1].distance;
        for (int tp2 = tp1 + 1; tp2 < track->ntrkpts - 1; ) {
            double distance = leg1 + track_delta(track, tp1, tp2) + track->after[tp2].distance;
            int new_bound = 0;
#pragma omp critical
            if (distance > bound) {
                new_bound = 1;
                bound = distance;
                indexes[0] = track->before[tp1].index;
                indexes[1] = tp1;
                indexes[2] = tp2;
                indexes[3] = track->after[tp2].index;
            }
            if (new_bound)
                ++tp2;
            else
                tp2 = track_fast_forward(track, tp2, 0.5 * (bound - distance));
        }
    }
    return bound;
}

    static double
track_open_distance3(const track_t *track, double bound, int *indexes)
{
    indexes[0] = indexes[1] = indexes[2] = indexes[3] = indexes[4] = -1;
#pragma omp parallel for schedule(dynamic)
    for (int tp1 = 1; tp1 < track->ntrkpts - 3; ++tp1) {
        double leg1 = track->before[tp1].distance;
        for (int tp2 = tp1 + 1; tp2 < track->ntrkpts - 2; ++tp2) {
            double leg2 = track_delta(track, tp1, tp2);
            for (int tp3 = tp2 + 1; tp3 < track->ntrkpts - 1; ) {
                double distance = leg1 + leg2 + track_delta(track, tp2, tp3) + track->after[tp3].distance;
                int new_bound = 0;
#pragma omp critical
                if (distance > bound) {
                    new_bound = 1;
                    bound = distance;
                    indexes[0] = track->before[tp1].index;
                    indexes[1] = tp1;
                    indexes[2] = tp2;
                    indexes[3] = tp3;
                    indexes[4] = track->after[tp3].index;
                }
                if (new_bound)
                    ++tp3;
                else
                    tp3 = track_fast_forward(track, tp3, 0.5 * (bound - distance));
            }
        }
    }
    return bound;
}

    static double
track_frcfd_aller_retour(const track_t *track, double bound, int *indexes)
{
    bound /= 2.0;
    indexes[0] = indexes[1] = indexes[2] = indexes[3] = -1;
#pragma omp parallel for schedule(dynamic)
    for (int tp1 = 0; tp1 < track->ntrkpts - 2; ++tp1) {
        int start = track->best_start[tp1];
        int finish = track->last_finish[start];
        if (finish < 0)
            continue;
        double distance = 0.0;
        double local_bound;
#pragma omp critical
        local_bound = bound;
        int tp2 = track_furthest_from(track, tp1, tp1 + 1, finish + 1, local_bound, &distance);
        if (tp2 >= 0) {
#pragma omp critical
            if (distance > bound) {
                bound = distance;
                indexes[0] = start;
                indexes[1] = tp1;
                indexes[2] = tp2;
                indexes[3] = finish;
            }
        }
    }
    return 2.0 * bound;
}

    static double
track_frcfd_triangle_fai(const track_t *track, double bound, int *indexes)
{
    indexes[0] = indexes[1] = indexes[2] = indexes[3] = indexes[4] = -1;
    double legbound = 0.28 * bound;
    int tp1;
    for (tp1 = 0; tp1 < track->ntrkpts - 2; ++tp1) {
        int start = track->best_start[tp1];
        int finish = track->last_finish[start];
        if (finish < 0)
            continue;
        int tp3first = track_first_at_least(track, tp1, tp1 + 2, finish + 1, legbound);
        if (tp3first < 0)
            continue;
        int tp3last = track_last_at_least(track, tp1, tp3first, finish + 1, legbound);
        if (tp3last < 0)
            continue;
        int tp3;
        for (tp3 = tp3last; tp3 >= tp3first; ) {
            double leg3 = track_delta(track, tp3, tp1);
            if (leg3 < legbound) {
                tp3 = track_fast_backward(track, tp3, legbound - leg3);
                continue;
            }
            double shortestlegbound = 0.28 * leg3 / 0.44;
            int tp2first = track_first_at_least(track, tp1, tp1 + 1, tp3 - 1, shortestlegbound);
            if (tp2first < 0) {
                --tp3;
                continue;
            }
            int tp2last = track_last_at_least(track, tp3, tp2first, tp3, shortestlegbound);
            if (tp2last < 0) {
                --tp3;
                continue;
            }
            double longestlegbound = 0.44 * leg3 / 0.28;
            int tp2;
            for (tp2 = tp2first; tp2 <= tp2last; ) {
                double d = 0.0;
                double leg1 = track_delta(track, tp1, tp2);
                if (leg1 < shortestlegbound)
                    d = shortestlegbound - leg1;
                if (leg1 > longestlegbound && leg1 - longestlegbound > d)
                    d = leg1 - longestlegbound;
                double leg2 = track_delta(track, tp2, tp3);
                if (leg2 < shortestlegbound && shortestlegbound - leg2 > d)
                    d = shortestlegbound - leg2;
                if (leg2 > longestlegbound && leg2 - longestlegbound > d)
                    d = leg2 - longestlegbound;
                if (d > 0.0) {
                    tp2 = track_fast_forward(track, tp2, d);
                    continue;
                }
                double total = leg1 + leg2 + leg3;
                double thislegbound = 0.28 * total;
                if (leg1 < thislegbound)
                    d = thislegbound - leg1;
                if (leg2 < thislegbound && thislegbound - leg2 > d)
                    d = thislegbound - leg2;
                if (leg3 < thislegbound && thislegbound - leg3 > d)
                    d = thislegbound - leg3;
                if (d > 0.0) {
                    tp2 = track_fast_forward(track, tp2, 0.5 * d);
                    continue;
                }
                if (total < bound) {
                    tp2 = track_fast_forward(track, tp2, 0.5 * (bound - total));
                    continue;
                }
                bound = total;
                legbound = thislegbound;
                indexes[0] = start;
                indexes[1] = tp1;
                indexes[2] = tp2;
                indexes[3] = tp3;
                indexes[4] = finish;
                ++tp2;
            }
            --tp3;
        }
    }
    return bound;
}

    static double
track_frcfd_triangle_plat(const track_t *track, double bound, int *indexes)
{
    indexes[0] = indexes[1] = indexes[2] = indexes[3] = indexes[4] = -1;
    for (int tp1 = 0; tp1 < track->ntrkpts - 1; ++tp1) {
        if (track->sigma_delta[track->ntrkpts - 1] - track->sigma_delta[tp1] < bound)
            break;
        int start = track->best_start[tp1];
        int finish = track->last_finish[start];
        if (finish < 0 || track->sigma_delta[finish] - track->sigma_delta[tp1] < bound)
            continue;
        for (int tp3 = finish; tp3 > tp1 + 1; --tp3) {
            double leg31 = track_delta(track, tp3, tp1);
            double bound123 = bound - leg31;
            double legs123 = 0.0;
            int tp2 = track_furthest_from2(track, tp1, tp3, tp1 + 1, tp3, bound123, &legs123);
            if (tp2 > 0) {
                bound = leg31 + legs123;
                indexes[0] = start;
                indexes[1] = tp1;
                indexes[2] = tp2;
                indexes[3] = tp3;
                indexes[4] = finish;
            }
        }
    }
    return bound;
}

    static double
track_frcfd_circuit_distance(const track_t *track, int n, int *indexes)
{
    double distance = track_delta(track, indexes[n - 2], indexes[1]);
    for (int i = 1; i < n - 2; ++i)
        distance += track_delta(track, indexes[i], indexes[i + 1]);
    return R * distance;
}

    result_t *
track_optimize_frcfd(track_t *track, int complexity, const declaration_t *declaration)
{
    static const char *league = "Coupe F\303\251d\303\251rale de Distance (France)";
    result_t *result = result_new();

    int indexes[6];
    double bound;

    bound = track_open_distance(track, 0.0, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "distance libre sans point de contournement", R * bound, 1.0, 0, 0);
        const char *names[] = { "BD", "BA" };
        route_push_trkpts(route, track->trkpts, 2, indexes, names);
    }

    if (complexity != -1 && complexity < 1)
        return result;

    bound = track_open_distance1(track, bound, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "distance libre avec un point de contournement", R * bound, 1.0, 0, 0);
        const char *names[] = { "BD", "B1", "BA" };
        route_push_trkpts(route, track->trkpts, 3, indexes, names);
    }

    if (complexity != -1 && complexity < 2)
        return result;

    bound = track_open_distance2(track, bound, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "distance libre avec deux points de contournement", R * bound, 1.0, 0, 0);
        const char *names[] = { "BD", "B1", "B2", "BA" };
        route_push_trkpts(route, track->trkpts, 4, indexes, names);
    }

    track_compute_circuit_tables(track, 3.0 / R);

    bound = track_frcfd_aller_retour(track, 15.0 / R, indexes);
    if (indexes[0] != -1) {
        double distance = track_frcfd_circuit_distance(track, 4, indexes);
        route_t *route = result_push_new_route(result, league, "parcours en aller-retour", distance, 1.2, 1, 0);
        static const char *names[] = { "BD", "B1", "B2", "BA" };
        route_push_trkpts(route, track->trkpts, 4, indexes, names);
    }

    if (complexity != -1 && complexity < 3)
        return result;

    bound = track_frcfd_triangle_fai(track, bound, indexes);
    if (indexes[0] != -1) {
        double distance = track_frcfd_circuit_distance(track, 5, indexes);
        route_t *route = result_push_new_route(result, league, "triangle FAI", distance, 1.4, 1, 0);
        static const char *names[] = { "BD", "B1", "B2", "B3", "BA" };
        route_push_trkpts(route, track->trkpts, 5, indexes, names);
    }

    bound = track_frcfd_triangle_plat(track, bound, indexes);
    if (indexes[0] != -1) {
        double distance = track_frcfd_circuit_distance(track, 5, indexes);
        route_t *route = result_push_new_route(result, league, "triangle plat", distance, 1.2, 1, 0);
        static const char *names[] = { "BD", "B1", "B2", "B3", "BA" };
        route_push_trkpts(route, track->trkpts, 5, indexes, names);
    }

    /* TODO track_frcfd_quadrilatere */

    return result;
}

    result_t *
track_optimize_uknxcl(track_t *track, int complexity, const declaration_t *declaration)
{
    static const char *league = "UK National XC League";
    result_t *result = result_new();

    int indexes[6];
    double bound;

    bound = track_open_distance(track, 0.0, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "open distance", R * bound, 1.0, 0, 0);
        const char *names[] = { "Start", "Finish" };
        route_push_trkpts(route, track->trkpts, 2, indexes, names);
    }

    if (complexity != -1 && complexity < 1)
        return result;

    bound = track_open_distance1(track, bound, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "open distance via a turnpoint", R * bound, 1.0, 0, 0);
        const char *names[] = { "Start", "TP1", "Finish" };
        route_push_trkpts(route, track->trkpts, 3, indexes, names);
    }

    if (complexity != -1 && complexity < 2)
        return result;

    bound = track_open_distance2(track, bound, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "open distance via two turnpoints", R * bound, 1.0, 0, 0);
        const char *names[] = { "Start", "TP1", "TP2", "Finish" };
        route_push_trkpts(route, track->trkpts, 4, indexes, names);
    }

    track_compute_circuit_tables(track, 0.4 / R);

    bound = track_frcfd_aller_retour(track, 15.0 / R, indexes);
    if (indexes[0] != -1) {
        double distance = track_frcfd_circuit_distance(track, 4, indexes);
        route_t *route = result_push_new_route(result, league, "out and return via a turnpoint", distance, 2.0, 1, 0);
        static const char *names[] = { "Start", "TP1", "TP2", "Finish" };
        route_push_trkpts(route, track->trkpts, 4, indexes, names);
    }

    if (complexity != -1 && complexity < 3)
        return result;

    bound = track_frcfd_triangle_fai(track, bound, indexes);
    if (indexes[0] != -1) {
        double distance = track_frcfd_circuit_distance(track, 5, indexes);
        route_t *route = result_push_new_route(result, league, "FAI triangle", distance, 2.5, 1, 0);
        static const char *names[] = { "Start", "TP1", "TP2", "TP3", "Finish" };
        route_push_trkpts(route, track->trkpts, 5, indexes, names);
    }

    bound = track_frcfd_triangle_plat(track, bound, indexes);
    if (indexes[0] != -1) {
        double distance = track_frcfd_circuit_distance(track, 5, indexes);
        route_t *route = result_push_new_route(result, league, "out and return via two turnpoints", distance, 2.0, 1, 0);
        static const char *names[] = { "Start", "TP1", "TP2", "TP3", "Finish" };
        route_push_trkpts(route, track->trkpts, 5, indexes, names);
    }

    return result;
}

    result_t *
track_optimize_ukxcl(track_t *track, int complexity, const declaration_t *declaration)
{
    static const char *league = "Cross Country League (United Kingdom)";
    result_t *result = result_new();

    int indexes[6];
    double bound;

    bound = track_open_distance(track, 10.0 / R, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "open distance", R * bound, 1.0, 0, 0);
        const char *names[] = { "Start", "Finish" };
        route_push_trkpts(route, track->trkpts, 2, indexes, names);
    }

    if (complexity != -1 && complexity < 3)
        return result;

    if (bound < 15.0 / R)
        bound = 15.0 / R;
    bound = track_open_distance3(track, bound, indexes);
    if (indexes[0] != -1) {
        route_t *route = result_push_new_route(result, league, "turnpoint flight", R * bound, 1.0, 0, 0);
        const char *names[] = { "Start", "TP1", "TP2", "TP3", "Finish" };
        route_push_trkpts(route, track->trkpts, 5, indexes, names);
    }

#if 0
    track_compute_circuit_tables(track, 0.4 / R);
#endif

    return result;
}
