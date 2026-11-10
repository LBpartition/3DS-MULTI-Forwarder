#ifndef MULTI_FORWARDER_NDS_BRIDGE_H
#define MULTI_FORWARDER_NDS_BRIDGE_H

#include <3ds/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int multi_nds_initialize(char *err, int errlen);
int multi_nds_build_install(const char *rom_path, const char *custom_title,
                            char *err, int errlen);
u32 multi_nds_title_count(void);
int multi_nds_title_ids(u64 *ids, u32 capacity, u32 *count);
int multi_nds_sd_title_ids(u64 *ids, u32 capacity, u32 *count);

#ifdef __cplusplus
}
#endif

#endif
