/* ppc_config.h for the synthetic test module (XenonRecomp generates one per module). */
#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED

#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x20000ull
#define PPC_CODE_BASE 0x82010000ull
#define PPC_CODE_SIZE 0x1000ull

#ifdef PPC_INCLUDE_DETAIL
#include "ppc_detail.h"
#endif

#endif
