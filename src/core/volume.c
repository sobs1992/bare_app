#include "volume.h"

#include <math.h>

#include "util.h"

/* 60 dB per decade of the normalised value: its cube is the amplitude */
#define CURVE 6000.0

/* The normalised value of the bottom of the range, mapped to 0 % */
static double floor_of(int db_min, int db_max)
{
	return pow(10.0, (db_min - db_max) / CURVE);
}

int volume_db_to_percent(int db, int db_min, int db_max)
{
	double f;

	if (db_max <= db_min || db >= db_max)
		return 100;
	if (db <= db_min)
		return 0;
	f = floor_of(db_min, db_max);
	return (int)lround((pow(10.0, (db - db_max) / CURVE) - f) / (1 - f) * 100);
}

int volume_percent_to_db(int percent, int db_min, int db_max)
{
	double f;

	if (db_max <= db_min || percent >= 100)
		return db_max;
	if (percent <= 0)
		return db_min;
	f = floor_of(db_min, db_max);
	return CLAMP((int)lround(CURVE * log10(percent / 100.0 * (1 - f) + f)) + db_max, db_min,
		     db_max);
}
