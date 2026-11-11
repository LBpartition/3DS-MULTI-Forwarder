#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "builder.hpp"
#include "error.hpp"
#include "lang.hpp"
#include "settings.hpp"
#include "nds_forwarder_bridge.h"

#define LOGPATH "sdmc:/3ds/MULTI-Forwarder"

Lang gLang;
static Builder builder;
static bool builder_ready;

bool Lang::isReady()
{
   return false;
}

void Lang::loadStrings(u8)
{
}

std::string Lang::getString(std::string key)
{
   return key;
}

std::string Lang::parseString(std::string key, ...)
{
   return key;
}

static void set_error(char *err, int errlen, const std::string &message, u32 code)
{
   if (!err || errlen <= 0)
      return;
   if (message.empty())
      std::snprintf(err, errlen, "NDS forwarder failed (%08lX).", (unsigned long)code);
   else
      std::snprintf(err, errlen, "%s", message.c_str());
}

int multi_nds_initialize(char *err, int errlen)
{
   ReturnResult *result = builder.initialize();
   if (!result)
   {
      set_error(err, errlen, std::string(), 0xFFFFFFFF);
      return -1;
   }
   if (!result->isSuccess())
   {
      set_error(err, errlen, result->message, result->code);
      delete result;
      return -1;
   }
   delete result;
   result = builder.loadTemplate("sdcard");
   if (!result)
   {
      set_error(err, errlen, std::string(), 0xFFFFFFFF);
      return -1;
   }
   if (!result->isSuccess())
   {
      set_error(err, errlen, result->message, result->code);
      delete result;
      return -1;
   }
   delete result;
   builder_ready = true;
   return 0;
}

int multi_nds_build_install(const char *rom_path, const char *custom_title,
                            char *err, int errlen)
{
   ReturnResult *result;
   if (!builder_ready && multi_nds_initialize(err, errlen))
      return -1;
   // Invalid extended banner CRCs fall back to a static v1 banner; base CRC
   // and header validation remain strict in the builder.
   result = builder.buildCIA(rom_path, false,
                             custom_title ? custom_title : "", true);
   if (!result)
   {
      set_error(err, errlen, std::string(), 0xFFFFFFFF);
      return -1;
   }
   if (!result->isSuccess())
   {
      set_error(err, errlen, result->message, result->code);
      delete result;
      return -1;
   }
   delete result;
   return 0;
}

static bool is_user_twl_title(u64 tid)
{
   u16 category = (u16)(tid >> 32);
   static const u64 system_ids[] = {
      0x00048005484E4441ULL, 0x0004800542383841ULL,
      0x0004800F484E4841ULL, 0x0004800F484E4C41ULL,
      0x00048005484E4443ULL, 0x00048005484E444BULL
   };
   size_t i;
   if (category != 0x8004 && category != 0x8005 &&
       category != 0x800F && category != 0x8015)
      return false;
   for (i = 0; i < sizeof(system_ids) / sizeof(system_ids[0]); i++)
      if (tid == system_ids[i])
         return false;
   return true;
}

static int multi_nds_title_ids_for_media(FS_MediaType media, u64 *ids,
                                         u32 capacity, u32 *count)
{
   u32 total = 0, read = 0, found = 0, i;
   u64 *all;
   Result result;
   if (!count)
      return -1;
   *count = 0;
   result = AM_GetTitleCount(media, &total);
   if (R_FAILED(result))
      return -1;
   if (!total)
      return 0;
   all = new u64[total];
   if (!all)
      return -1;
   result = AM_GetTitleList(&read, media, total, all);
   if (R_FAILED(result))
   {
      delete[] all;
      return -1;
   }
   for (i = 0; i < read; i++)
      if (is_user_twl_title(all[i]))
      {
         if (ids && found < capacity)
            ids[found] = all[i];
         found++;
      }
   delete[] all;
   *count = found;
   return 0;
}

int multi_nds_title_ids(u64 *ids, u32 capacity, u32 *count)
{
   return multi_nds_title_ids_for_media(MEDIATYPE_NAND, ids, capacity, count);
}

int multi_nds_sd_title_ids(u64 *ids, u32 capacity, u32 *count)
{
   return multi_nds_title_ids_for_media(MEDIATYPE_SD, ids, capacity, count);
}

u32 multi_nds_title_count(void)
{
   u32 count = 0;
   return multi_nds_title_ids(NULL, 0, &count) ? 0xFFFFFFFF : count;
}
