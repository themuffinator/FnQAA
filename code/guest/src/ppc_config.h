/*
 * The runtime's ppc_config.h. XenonRecomp's ppc_context.h requires one;
 * the generated modules have their own. The PPC_CONFIG_* switches must
 * match the modules' (checked at load through ModuleDesc::config_flags).
 * The image macros only feed XenonRecomp's default indirect-call lookup,
 * which neither the runtime nor the modules use (see ppc_detail.h).
 */
#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED

#define PPC_IMAGE_BASE 0ull
#define PPC_IMAGE_SIZE 0ull
#define PPC_CODE_BASE 0ull
#define PPC_CODE_SIZE 0ull

#endif
