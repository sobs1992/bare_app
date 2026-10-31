/*
 * Volume curve for mixers with a dB scale, the one alsamixer uses: the
 * percent is the cube root of the amplitude, so equal steps sound about
 * equally loud. dB values are in 0.01 dB, like ALSA's TLV data.
 */
#pragma once

int volume_db_to_percent(int db, int db_min, int db_max);
int volume_percent_to_db(int percent, int db_min, int db_max);
