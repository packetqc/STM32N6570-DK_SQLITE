#pragma once

#include "app_filex.h"

// Should be called prior to any other access
void sqlite3_azure_init(FX_MEDIA* media_ptr   // pointer to already opened media
		, sqlite3_int64 (*dattime64)(void)    // pointer to a function that returns Julian Day multiplied by 86400000, may be NULL
		, int (*random_generator)(void)       // pointer to a random number generator, may be NULL
		);
