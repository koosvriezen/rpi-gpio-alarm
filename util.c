/*
 * Copyright (c) 2013, Koos Vriezen <koos.vriezen@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

void alarm_array_init(struct AlarmArray *array)
{
    array->size = array->capacity = 0;
}

void alarm_array_append(struct AlarmArray *array, void *v)
{
    if (array->size == array->capacity) {
        if (!array->size)
            array->data = (void**)malloc(++array->capacity * sizeof (void*));
        else
            array->data = (void**)realloc(array->data, ++array->capacity * sizeof (void*));
    }
    array->data[array->size++] = v;
}

void alarm_array_remove(struct AlarmArray *array, int i)
{
    if ( i < --array->size )
        memmove(array->data + i, array->data + i + 1, (array->size - i) * sizeof (void*));
}

void alarm_array_clear(struct AlarmArray *array)
{
    if (array->capacity)
        free(array->data);
    array->size = array->capacity = 0;
}

/* based on http://williams.best.vwh.net/sunrise_sunset_algorithm.htm */
/* TODO make this configurable */
#define longitude       (0.00000)
#define latitude        (0.0000)

double norm(double high, double d) {
    while (d < 0)
        d += high;
    while (d > high)
        d -= high;
    return d;
}

static void rise_set(const struct tm* lt, int rise, int *hour, int *minute) {
    const double zenith = 90 + 50./60;
    double N, N1, N2, N3, lngHour, t, M, L, RA, Lquadrant, RAquadrant, sinDec, cosDec, cosH, H, T, UT;

    N1 = floor(275 * (lt->tm_mon +1) / 9);
    N2 = floor(((lt->tm_mon +1) + 9) / 12);
    N3 = (1 + floor(((lt->tm_year+1900) - 4 * floor((lt->tm_year+1900) / 4) + 2) / 3));
    N = N1 - (N2 * N3) + lt->tm_mday - 30;
    lngHour = longitude / 15;
    if (rise)
        t = N + ((6 - lngHour) / 24); /*rising*/
    else
        t = N + ((18 - lngHour) / 24); /*setting*/
    M = (0.9856 * t) - 3.289;
    L = norm(360, M + (1.916 * sin(M*M_PI/180)) + (0.020 * sin(2 * M*M_PI/180)) + 282.634);
    RA = norm(360, atan(0.91764 * tan(L*M_PI/180))*180/M_PI);
    Lquadrant  = floor( L/90) * 90;
    RAquadrant = floor(RA/90) * 90;
    RA = RA + (Lquadrant - RAquadrant);
    RA = RA / 15;
    sinDec = 0.39782 * sin(L*M_PI/180);
    cosDec = cos(asin(sinDec));
    cosH = (cos(zenith*M_PI/180) - (sinDec * sin(latitude*M_PI/180))) / (cosDec * cos(latitude*M_PI/180));
    if (cosH > 1)
        return;
    if (cosH < -1)
        return;
    if (rise)
        H = 360 - acos(cosH)*180/M_PI; /*rising*/
    else
        H =       acos(cosH)*180/M_PI; /*setting*/
    H = H / 15;
    T = H + RA - (0.06571 * t) - 6.622;
    UT = norm(24, T - lngHour);
    *hour = lt->tm_gmtoff/3600 + (int)UT;
    *minute = (int)((UT-(int)UT)*60);
}

void sunrise_sunset(const struct tm* lt, int *sethour, int *setminute, int *risehour, int *riseminute) {
    rise_set(lt, 1, risehour, riseminute);
    rise_set(lt, 0, sethour, setminute);
}
