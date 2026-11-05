#include <string.h>

#include "cia_patch.h"
#include "sha256.h"

#define FWD_MAGIC_LEN 8
#define FWD_PATH_MAX 256

#define NCCH_PARTITION_ID 0x108
#define NCCH_PROGRAM_ID 0x118
#define NCCH_EXHEADER_HASH 0x160
#define NCCH_EXHEADER_SIZE 0x180
#define NCCH_FLAGS 0x188
#define NCCH_EXEFS_OFF 0x1A0
#define NCCH_EXEFS_HASH 0x1C0

#define TMD_TITLE_ID 0x4C
#define TMD_TITLE_VERSION 0x9C
#define TMD_CONTENT_COUNT 0x9E
#define TMD_INFO_RECORDS_HASH 0xA4
#define TMD_INFO_RECORDS 0xC4
#define TMD_INFO_RECORDS_SIZE (64 * 0x24)

#define EXH_ACI_PROGRAM_ID 0x200
#define EXH_ACCESSDESC_ACI_PROGRAM_ID 0x600

static uint32_t rd32le(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t rd64be(const uint8_t* p)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++) v = (v << 8) | p[i];
	return v;
}
static void wr64le(uint8_t* p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8)); }
static void wr64be(uint8_t* p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - i * 8)); }

static size_t align64(size_t v) { return (v + 63) & ~(size_t)63; }

static size_t sig_size(uint32_t type)
{
	switch (type)
	{
		case 0x010000: case 0x010003: return 0x23C;
		case 0x010001: case 0x010004: return 0x13C;
		case 0x010002: case 0x010005: return 0x7C;
		default: return 0;
	}
}

// ExeFS sections: a 0x200 header, then the data.
static int name_is(const char* field, const char* want)
{
	size_t n = strlen(want);
	if (n > 8) return 0;
	if (memcmp(field, want, n) != 0) return 0;
	for (; n < 8; n++)
		if (field[n] != 0) return 0;
	return 1;
}

static int exefs_entry(const cia_t* cia, int index, const char** name, size_t* off, size_t* size)
{
	const uint8_t* e = cia->data + cia->exefs + index * 0x10;
	if (e[0] == 0) return 0;
	*name = (const char*)e;
	*off = cia->exefs + 0x200 + rd32le(e + 8);
	*size = rd32le(e + 12);
	return 1;
}

// Section hashes sit in reverse order at the end of the ExeFS header.
static size_t exefs_hash_slot(const cia_t* cia, int index)
{
	return cia->exefs + 0x200 - (size_t)(index + 1) * 0x20;
}

int cia_open(cia_t* cia, uint8_t* data, size_t size)
{
	uint32_t hdr_size, cert_sz, tik_sz, tmd_sz;
	uint64_t content_sz;
	size_t cert_off, tik_off, tmd_off;
	uint32_t exefs_off, exefs_hash;

	if (size < 0x2040) return CIA_ERR_FORMAT;

	memset(cia, 0, sizeof(*cia));
	cia->data = data;
	cia->size = size;

	hdr_size = rd32le(data);
	cert_sz = rd32le(data + 0x08);
	tik_sz = rd32le(data + 0x0C);
	tmd_sz = rd32le(data + 0x10);
	content_sz = (uint64_t)rd32le(data + 0x18) | ((uint64_t)rd32le(data + 0x1C) << 32);

	cert_off = align64(hdr_size);
	tik_off = cert_off + align64(cert_sz);
	tmd_off = tik_off + align64(tik_sz);
	cia->content_off = tmd_off + align64(tmd_sz);

	{
		size_t tik_sig = sig_size(rd32be(data + tik_off));
		size_t tmd_sig = sig_size(rd32be(data + tmd_off));
		if (!tik_sig || !tmd_sig) return CIA_ERR_FORMAT;
		cia->tik_body = tik_off + 4 + tik_sig;
		cia->tmd_body = tmd_off + 4 + tmd_sig;
	}

	if (cia->content_off + content_sz > size) return CIA_ERR_FORMAT;

	cia->ncch = cia->content_off;
	if (memcmp(data + cia->ncch + 0x100, "NCCH", 4) != 0) return CIA_ERR_FORMAT;

	cia->exheader = cia->ncch + 0x200;
	cia->exheader_size = rd32le(data + cia->ncch + NCCH_EXHEADER_SIZE);

	{
		uint32_t unit = 0x200u << data[cia->ncch + NCCH_FLAGS + 6];
		exefs_off = rd32le(data + cia->ncch + NCCH_EXEFS_OFF);
		exefs_hash = rd32le(data + cia->ncch + NCCH_EXEFS_OFF + 8);
		cia->exefs = cia->ncch + (size_t)exefs_off * unit;
		cia->exefs_hash_size = (size_t)exefs_hash * unit;
	}
	return CIA_OK;
}

int cia_set_field(cia_t* cia, const char* magic, const char* path)
{
	const char* name;
	size_t off, size, i;
	size_t len = strlen(path);
	int index;

	if (len >= FWD_PATH_MAX) return CIA_ERR_TOO_LONG;

	for (index = 0; index < 10; index++)
	{
		if (!exefs_entry(cia, index, &name, &off, &size)) continue;
		if (!name_is(name, ".code")) continue;

		for (i = 0; i + FWD_MAGIC_LEN <= size; i++)
		{
			if (memcmp(cia->data + off + i, magic, FWD_MAGIC_LEN) != 0) continue;
			memset(cia->data + off + i + FWD_MAGIC_LEN, 0, FWD_PATH_MAX);
			memcpy(cia->data + off + i + FWD_MAGIC_LEN, path, len);
			return CIA_OK;
		}
	}
	return CIA_ERR_MARKER;
}

int cia_set_icon(cia_t* cia, const uint8_t* smdh, size_t len)
{
	const char* name;
	size_t off, size;
	int index;

	for (index = 0; index < 10; index++)
	{
		if (!exefs_entry(cia, index, &name, &off, &size)) continue;
		if (!name_is(name, "icon")) continue;
		if (size != len) return CIA_ERR_SIZE;
		memcpy(cia->data + off, smdh, len);
		return CIA_OK;
	}
	return CIA_ERR_FORMAT;
}

void cia_set_title_id(cia_t* cia, uint64_t tid)
{
	wr64le(cia->data + cia->ncch + NCCH_PARTITION_ID, tid);
	wr64le(cia->data + cia->ncch + NCCH_PROGRAM_ID, tid);
	wr64le(cia->data + cia->exheader + EXH_ACI_PROGRAM_ID, tid);
	wr64le(cia->data + cia->exheader + EXH_ACCESSDESC_ACI_PROGRAM_ID, tid);
	wr64be(cia->data + cia->tik_body + 0x9C, tid);
	wr64be(cia->data + cia->tmd_body + TMD_TITLE_ID, tid);
}

void cia_set_process_name(cia_t* cia, const char* name)
{
	size_t len = strlen(name);
	if (len > 8) len = 8;
	memset(cia->data + cia->exheader, 0, 8);
	memcpy(cia->data + cia->exheader, name, len);
}

void cia_reseal(cia_t* cia)
{
	const char* name;
	size_t off, size, chunks, content, first_info;
	int index, i, count, covered;

	for (index = 0; index < 10; index++)
		if (exefs_entry(cia, index, &name, &off, &size))
			sha256(cia->data + off, size, cia->data + exefs_hash_slot(cia, index));

	sha256(cia->data + cia->exefs, cia->exefs_hash_size, cia->data + cia->ncch + NCCH_EXEFS_HASH);
	sha256(cia->data + cia->exheader, cia->exheader_size, cia->data + cia->ncch + NCCH_EXHEADER_HASH);

	count = rd16be(cia->data + cia->tmd_body + TMD_CONTENT_COUNT);
	first_info = cia->tmd_body + TMD_INFO_RECORDS;
	chunks = first_info + TMD_INFO_RECORDS_SIZE;
	content = cia->content_off;

	for (i = 0; i < count; i++)
	{
		size_t rec = chunks + (size_t)i * 0x30;
		uint64_t csize = rd64be(cia->data + rec + 0x08);
		sha256(cia->data + content, (size_t)csize, cia->data + rec + 0x10);
		content += (size_t)csize;
	}

	covered = rd16be(cia->data + first_info + 0x02);
	sha256(cia->data + chunks, (size_t)covered * 0x30, cia->data + first_info + 0x04);
	sha256(cia->data + first_info, TMD_INFO_RECORDS_SIZE, cia->data + cia->tmd_body + TMD_INFO_RECORDS_HASH);
}

// Rewrites the SMDH titles in place. Simpler than rebuilding a whole SMDH:
// the template icon is kept as is.
static void put_utf16(uint8_t* dst, size_t chars, const char* text)
{
	size_t i;
	memset(dst, 0, chars * 2);
	for (i = 0; text[i] && i + 1 < chars; i++)
	{
		// File names are taken as they come: ASCII is enough here, and any
		// non-ASCII byte is replaced rather than yielding invalid UTF-16.
		unsigned char c = (unsigned char)text[i];
		dst[i * 2] = (c < 0x80) ? c : '?';
		dst[i * 2 + 1] = 0;
	}
}

int cia_set_titles(cia_t* cia, const char* name, const char* publisher)
{
	const char* entry_name;
	size_t off, size;
	int index, lang;

	for (index = 0; index < 10; index++)
	{
		if (!exefs_entry(cia, index, &entry_name, &off, &size)) continue;
		if (!name_is(entry_name, "icon")) continue;
		if (memcmp(cia->data + off, "SMDH", 4) != 0) return CIA_ERR_FORMAT;

		for (lang = 0; lang < 16; lang++)
		{
			uint8_t* t = cia->data + off + 0x08 + (size_t)lang * 0x200;
			put_utf16(t, 0x40, name);             // short title
			put_utf16(t + 0x80, 0x80, name);      // long title
			put_utf16(t + 0x180, 0x40, publisher);
		}
		return CIA_OK;
	}
	return CIA_ERR_FORMAT;
}

int cia_set_rom_path(cia_t* cia, const char* path)
{
	return cia_set_field(cia, "FWD0ROM!", path);
}

int cia_set_core_path(cia_t* cia, const char* path)
{
	return cia_set_field(cia, "FWD0CRE!", path);
}

uint8_t* cia_icon_data(cia_t* cia)
{
	const char* name;
	size_t off, size;
	int index;

	for (index = 0; index < 10; index++)
		if (exefs_entry(cia, index, &name, &off, &size) && name_is(name, "icon"))
			return cia->data + off;
	return NULL;
}

uint8_t* cia_banner_data(cia_t* cia, size_t* reserved)
{
	const char* name;
	size_t off, size;
	int index;

	for (index = 0; index < 10; index++)
		if (exefs_entry(cia, index, &name, &off, &size) && name_is(name, "banner"))
		{
			if (reserved) *reserved = size;
			return cia->data + off;
		}
	return NULL;
}

int cia_set_banner(cia_t* cia, const uint8_t* blob, size_t len)
{
	const char* name;
	size_t off, size;
	int index;

	for (index = 0; index < 10; index++)
	{
		if (!exefs_entry(cia, index, &name, &off, &size)) continue;
		if (!name_is(name, "banner")) continue;
		if (len > size) return CIA_ERR_SIZE;

		memcpy(cia->data + off, blob, len);
		memset(cia->data + off + len, 0, size - len);
		// Declare the new size: the reserved room downstream does not move.
		{
			uint8_t* field = cia->data + cia->exefs + index * 0x10 + 12;
			field[0] = (uint8_t)len; field[1] = (uint8_t)(len >> 8);
			field[2] = (uint8_t)(len >> 16); field[3] = (uint8_t)(len >> 24);
		}
		return CIA_OK;
	}
	return CIA_ERR_FORMAT;
}

uint16_t cia_version(const cia_t* cia)
{
	const uint8_t* p = cia->data + cia->tmd_body + TMD_TITLE_VERSION;
	return (uint16_t)((p[0] << 8) | p[1]);
}

void cia_set_version(cia_t* cia, uint16_t version)
{
	uint8_t* tmd = cia->data + cia->tmd_body + TMD_TITLE_VERSION;
	uint8_t* tik = cia->data + cia->tik_body + 0xA6;
	tmd[0] = (uint8_t)(version >> 8); tmd[1] = (uint8_t)version;
	tik[0] = (uint8_t)(version >> 8); tik[1] = (uint8_t)version;
}
