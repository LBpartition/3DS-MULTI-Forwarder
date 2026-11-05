#pragma once
#include <stddef.h>
#include <stdint.h>

// C port of cia_format.py. Deliberately free of 3DS dependencies so it can be
// compiled on a PC and compared against the Python reference.

typedef struct
{
	uint8_t* data;
	size_t size;
	size_t tik_body;
	size_t tmd_body;
	size_t ncch;
	size_t exheader;
	size_t exheader_size;
	size_t exefs;
	size_t exefs_hash_size;
	size_t content_off;
} cia_t;

#define CIA_OK 0
#define CIA_ERR_FORMAT -1
#define CIA_ERR_MARKER -2
#define CIA_ERR_TOO_LONG -3
#define CIA_ERR_SIZE -4

int cia_open(cia_t* cia, uint8_t* data, size_t size);
int cia_set_field(cia_t* cia, const char* magic, const char* path);
int cia_set_rom_path(cia_t* cia, const char* path);
int cia_set_core_path(cia_t* cia, const char* path);
int cia_set_icon(cia_t* cia, const uint8_t* smdh, size_t len);
void cia_set_title_id(cia_t* cia, uint64_t tid);
void cia_set_process_name(cia_t* cia, const char* name);
void cia_reseal(cia_t* cia);
int cia_set_titles(cia_t* cia, const char* name, const char* publisher);

// Pointer to the SMDH inside the ExeFS, to patch the icon in place.
uint8_t* cia_icon_data(cia_t* cia);

// Returns a pointer to the banner and the size reserved for it.
uint8_t* cia_banner_data(cia_t* cia, size_t* reserved);

// Writes a banner into the reserved room and declares its size.
int cia_set_banner(cia_t* cia, const uint8_t* blob, size_t len);

// The HOME menu caches icons: without a version change, reinstalling keeps
// the old one.
uint16_t cia_version(const cia_t* cia);
void cia_set_version(cia_t* cia, uint16_t version);
