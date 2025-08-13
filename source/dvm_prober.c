// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include <dvm.h>
#include "dvm_debug.h"

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define le16(x) (x)
#define le32(x) (x)
#else
#define le16(x) __builtin_bswap16(x)
#define le32(x) __builtin_bswap32(x)
#endif

typedef struct DvmMbrPartEntry {
	uint8_t status;
	uint8_t _pad_0x01[3];
	uint8_t type;
	uint8_t _pad_0x05[3];
	uint8_t start_lba_le[4];
	uint8_t num_sectors_le[4];
} DvmMbrPartEntry;

static inline bool _dvmIsPo2(unsigned x)
{
	return (x & (x-1)) == 0;
}

static inline unsigned _dvmRead8(const void* buf, unsigned offset)
{
	return *((const uint8_t*)buf + offset);
}

static inline unsigned _dvmRead16(const void* buf, unsigned offset)
{
	uint16_t ret;
	memcpy(&ret, (const uint8_t*)buf + offset, sizeof(ret));
	return le16(ret);
}

static inline unsigned _dvmRead32(const void* buf, unsigned offset)
{
	uint32_t ret;
	memcpy(&ret, (const uint8_t*)buf + offset, sizeof(ret));
	return le32(ret);
}

static const char* _dvmIdentMbrVbr(const void* buf)
{
	unsigned jmp = _dvmRead8(buf, 0);
	bool has_signature = _dvmRead16(buf, 0x1fe) == 0xaa55;

	// Check for a valid Microsoft VBR
	if (has_signature && (jmp == 0xeb || jmp == 0xe9 || jmp == 0xe8)) {
		// Check for NTFS and exFAT
		const char* fsname = (const char*)buf + 3;
		if (memcmp(fsname, "NTFS    ", 8) == 0) {
			return "ntfs";
		} else if (memcmp(fsname, "EXFAT   ", 8) == 0) {
			return "exfat";
		}

		// Check for FAT32
		fsname = (const char*)buf + 0x052;
		if (memcmp(fsname, "FAT32   ", 8) == 0) {
			return "vfat";
		}

		// Check for FAT12/FAT16
		unsigned bytesPerSector    = _dvmRead16(buf, 0x00b);
		unsigned sectorsPerCluster = _dvmRead8 (buf, 0x00d);
		unsigned numRsvdSectors    = _dvmRead16(buf, 0x00e);
		unsigned numFats           = _dvmRead8 (buf, 0x010);
		unsigned numRootEntries    = _dvmRead16(buf, 0x011);
		unsigned totalSectors16    = _dvmRead16(buf, 0x013);
		unsigned sectorsPerFat     = _dvmRead16(buf, 0x016);
		unsigned totalSectors32    = _dvmRead32(buf, 0x020);
		if (
			_dvmIsPo2(bytesPerSector) &&
			_dvmIsPo2(sectorsPerCluster) &&
			numRsvdSectors > 0 &&
			(numFats == 1 || numFats == 2) &&
			numRootEntries > 0 &&
			(totalSectors16 >= 0x80 || totalSectors32 >= 0x10000) &&
			sectorsPerFat > 0
		) {
			return "vfat";
		}
	}

	// Otherwise: assume MBR if signature is present
	return has_signature ? "" : NULL;
}

static unsigned _dvmReadPartitionTable(DvmDisc* disc, DvmPartInfo* out, unsigned max_partitions, unsigned flags, void* buf)
{
	if (!disc->vt->read_sectors(disc, buf, 0, 1)) {
		dvmDebug("Disc read error\n");
		return 0;
	}

	const char* ident = _dvmIdentMbrVbr(buf);
	if (ident) {
		if (*ident) {
			dvmDebug("Found VBR\n");
			out->index = 0;
			out->type = 0;
			out->fstype = ident;
			out->start_sector = 0;
			out->num_sectors = disc->num_sectors;
			return 1;
		} else {
			dvmDebug("Found MBR\n");
		}
	} else {
		dvmDebug("Cannot find MBR or VBR\n");
		return 0;
	}

	DvmMbrPartEntry* mbr_part = (DvmMbrPartEntry*)((char*)buf + 0x1be);
	sec_t total_used_sectors = 0;
	unsigned num_parts = 0;
	for (unsigned i = 0; i < 4 && num_parts < max_partitions; i ++) {
		unsigned status = mbr_part[i].status;
		unsigned type   = mbr_part[i].type;

		// Validate partition status
		if (status != 0x80 && status != 0x00) {
			dvmDebug("Malformed MBR\n");
			return 0;
		}

		// Skip unpopulated/extended partitions
		if (type == 0x00 || type == 0x05 || type == 0x0f) {
			continue;
		}

		DvmPartInfo* part = &out[num_parts++];
		part->index = i;
		part->type = type;
		part->fstype = NULL;
		part->start_sector = _dvmRead32(mbr_part[i].start_lba_le, 0);
		part->num_sectors = _dvmRead32(mbr_part[i].num_sectors_le, 0);

		sec_t part_end = part->start_sector + part->num_sectors;
		if (part_end > total_used_sectors) {
			total_used_sectors = part_end;
		}
	}

	// Validate disc size
	dvmDebug("Disc size 0x%lx\n", disc->num_sectors);
	dvmDebug("Det  size 0x%lx\n", total_used_sectors);
	if (~disc->num_sectors == 0) {
		disc->num_sectors = total_used_sectors;
	} else if (total_used_sectors > disc->num_sectors) {
		dvmDebug("Out of bound partitions\n");
		return 0;
	}

	// Identify fstype for each partition if needed
	if (flags & DVM_IDENT_FSTYPE) {
		for (unsigned i = 0; i < num_parts; i ++) {
			dvmDebug("[%u:%.2X] 0x%lx 0x%lx\n", out[i].index, out[i].type, out[i].start_sector, out[i].num_sectors);
			if (!disc->vt->read_sectors(disc, buf, out[i].start_sector, 1)) {
				dvmDebug("Disc read error\n");
				return 0;
			}

			ident = _dvmIdentMbrVbr(buf);
			if (ident && *ident) {
				dvmDebug("  fstype %s\n", ident);
				out[i].fstype = ident;
			}
		}
	}

	return num_parts;
}

unsigned dvmReadPartitionTable(DvmDisc* disc, DvmPartInfo* out, unsigned max_partitions, unsigned flags)
{
	if (!disc || !out || !max_partitions) {
		return 0;
	}

	unsigned num_parts = 0;
	void* buf = aligned_alloc(LIBDVM_BUFFER_ALIGN, 1U << disc->sector_shift);
	if (buf) {
		num_parts = _dvmReadPartitionTable(disc, out, max_partitions, flags, buf);
		free(buf);
	}

	return num_parts;
}

unsigned dvmProbeMountDisc(const char* basename, DvmDisc* disc)
{
	DvmPartInfo partinfo[4];
	unsigned num_parts = dvmReadPartitionTable(disc, partinfo, 4, DVM_IDENT_FSTYPE);
	if (!num_parts) {
		return 0;
	}

	dvmDebug("Loaded %u partitions\n", num_parts);
	char volname[16];
	size_t basenamelen = strnlen(basename, sizeof(volname)-2);
	memcpy(volname, basename, basenamelen);

	// Try to mount partitions
	unsigned num_mounted = 0;
	for (unsigned i = 0; i < num_parts; i ++) {
		DvmPartInfo* part = &partinfo[i];
		if (!part->fstype) {
			continue;
		}

		volname[basenamelen+0] = part->index ? ('1' + part->index) : 0;
		volname[basenamelen+1] = 0;

		if (dvmMountVolume(volname, disc, part->start_sector, part->fstype)) {
			num_mounted ++;
		}
	}

	return num_mounted;
}

unsigned dvmProbeMountDiscIface(const char* basename, const DISC_INTERFACE* iface, unsigned cache_pages, unsigned sectors_per_page)
{
	bool num_mounted = 0;
	DvmDisc* disc = NULL;

	if (iface) {
		disc = dvmDiscCreate(iface);
	}

	if (disc && cache_pages != 0) {
		disc = dvmDiscCacheCreate(disc, cache_pages, sectors_per_page);
	}

	if (disc) {
		num_mounted = dvmProbeMountDisc(basename, disc);
	}

	if (!num_mounted && disc) {
		disc->vt->destroy(disc);
	}

	return num_mounted;
}
