/*
 * Copyright (C) 2015 The PASTA Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdint.h>
#include <string.h>
#include "decryption.h"
#include "console.h"
#include "draw.h"
#include "lang.h"
#include "hid.h"
#include "fs.h"
#include "crypto.h"
#include "stdio.h"
 #include "screenshot.h"

#define TITLES (uint8_t*)0x22000000
#define BUFFER_ADDR ((uint8_t*)0x21000000)
#define BLOCK_SIZE  (8*1024*1024)

char str[100];


int DecryptTitleKey(uint8_t *titleid, uint8_t *key, uint32_t index) {
	const size_t blockSize = 16;
	static struct {
		uint8_t key[blockSize];
		uint32_t pad;
	} *keyYList = NULL;
	uint8_t ctr[blockSize] __attribute__((aligned(32)));
	uint8_t keyY[blockSize] __attribute__((aligned(32)));
	uint8_t titleId[8] __attribute__((aligned(32)));
	uintptr_t p;

	memcpy(titleId, titleid, 8);
	memset(ctr, 0, blockSize);
	memcpy(ctr, titleId, 8);
	set_ctr(AES_BIG_INPUT | AES_NORMAL_INPUT, ctr);

	if (keyYList == NULL) {
		p = 0x08080000;
		while (((uint8_t *)p)[0] != 0xD0 || ((uint8_t *)p)[1] != 0x7B) {
			p++;
			if (p >= 0x080A0000)
				return 1;
		}

		keyYList = (void *)p;
	}

	memcpy(keyY, keyYList[index].key, sizeof(keyY));
	setup_aeskey(0x3D, AES_BIG_INPUT | AES_NORMAL_INPUT, keyY);
	use_aeskey(0x3D);
	aes_decrypt(key, key, ctr, 1, AES_CBC_DECRYPT_MODE);
	return 0;
}


int getTitleKey(uint8_t *TitleKey, uint32_t low, uint32_t high, int drive) {
	File tick;
	uint32_t tid_low = ((low >> 24) & 0xff) | ((low << 8) & 0xff0000) | ((low >> 8) & 0xff00) | ((low << 24) & 0xff000000);
	uint32_t tid_high = ((high >> 24) & 0xff) | ((high << 8) & 0xff0000) | ((high >> 8) & 0xff00) | ((high << 24) & 0xff000000);
	uint32_t tick_size = 0x200;     //Chunk size

	wchar_t path[_MAX_LFN] = {0};
	int r;

	swprintf(path, _MAX_LFN, L"%d:dbs/ticket.db", drive);

	if (FileOpen(&tick, path, 0)) {
		uint8_t *buf = TITLES;
		int pos = 0;
		for (;;) {
			int rb = FileRead(&tick, buf, tick_size, pos);
			if (rb == 0) { break; } /* error or eof */
			pos += rb;
			if (buf[0] == 'T' && buf[1] == 'I' && buf[2] == 'C' && buf[3] == 'K') {
				tick_size = 0xD0000;
				continue;
			}
			for (int j = 0; j < tick_size; j++) {
				if (!strcmp((char *)buf + j, "Root-CA00000003-XS0000000c")) {
					uint8_t *titleid = buf + j + 0x9C;
					uint32_t kindex = *(buf + j + 0xB1);
					uint8_t Key[16]; memcpy(Key, buf + j + 0x7F, 16);
					if (*((uint32_t *)titleid) == tid_low && *((uint32_t *)(titleid + 4)) == tid_high) {
						r = DecryptTitleKey(titleid, Key, kindex);
						if (!r)
							memcpy(TitleKey, Key, 16);
						FileClose(&tick);
						return r;
					}
				}
			}
		}
		FileClose(&tick);
	}
	return 1;
}

static FRESULT seekRead(FIL *fp, DWORD ofs, void *buff, UINT btr)
{
	FRESULT r;
	UINT br;

	r = f_lseek(fp, ofs);
	if (r != FR_OK)
		return r;

	r = f_read(fp, buff, btr, &br);
	return br < btr ? (r == FR_OK ? EOF : r) : FR_OK;
}

#define CETK_MEMBER_SIZE(member) (sizeof(((TicketHdr *)NULL)->member))
#define CETK_READ_MEMBER(fp, member, buff)	\
	(seekRead((fp), 0x140 + offsetof(TicketHdr, member), buff,	\
		CETK_MEMBER_SIZE(member)))

int getTitleKeyWithCetk(uint8_t dst[16], const TCHAR *path)
{
	uint8_t id[CETK_MEMBER_SIZE(titleId)];
	uint8_t index;
	FRESULT r;
	FIL f;

	r = f_open(&f, path, FA_READ);
	if (r != FR_OK)
		return r;

	r = CETK_READ_MEMBER(&f, titleKey, dst);
	if (r != FR_OK) {
		f_close(&f);
		return r;
	}

	r = CETK_READ_MEMBER(&f, titleId, id);
	if (r != FR_OK) {
		f_close(&f);
		return r;
	}

	r = CETK_READ_MEMBER(&f, keyIndex, &index);
	if (r != FR_OK) {
		f_close(&f);
		return r;
	}

	f_close(&f);
	return DecryptTitleKey(id, dst, index);
}

uint32_t DecryptPartition(PartitionInfo* info){
	if(info->keyY != NULL)
		setup_aeskey(info->keyslot, AES_BIG_INPUT|AES_NORMAL_INPUT, info->keyY);
	use_aeskey(info->keyslot);

	uint8_t ctr[16] __attribute__((aligned(32)));
	memcpy(ctr, info->ctr, 16);

	uint32_t size_bytes = info->size;
	for (uint32_t i = 0; i < size_bytes; i += BLOCK_SIZE) {
		uint32_t j;
		for (j = 0; (j < BLOCK_SIZE) && (i+j < size_bytes); j+= 16) {
			set_ctr(AES_BIG_INPUT|AES_NORMAL_INPUT, ctr);
			aes_decrypt((void*)info->buffer+j, (void*)info->buffer+j, ctr, 1, AES_CTR_MODE);
			add_ctr(ctr, 1);
			TryScreenShot(); //Putting it here allows us to take screenshots at any decryption point, since everyting loops in this
		}
	}
	return 0;
}

void ProcessExeFS(PartitionInfo* info){ //We expect Exefs to take just a block. Why? No exefs right now reached 8MB.
	if(info->keyslot == 0x2C){
		DecryptPartition(info);
	}else if(info->keyslot == 0x25){  //The new keyX is a bit tricky, 'couse only .code is encrypted with it
		PartitionInfo myInfo;
		memcpy((void*)&myInfo, (void*)info, sizeof(PartitionInfo));
		uint8_t OriginalCTR[16]; memcpy(OriginalCTR, info->ctr, 16);
		myInfo.keyslot = 0x2C; myInfo.size = 0x200;
		DecryptPartition(&myInfo); add_ctr(myInfo.ctr, 0x200 / 16);
		if(myInfo.buffer[0] == '.' && myInfo.buffer[1] == 'c' && myInfo.buffer[2] == 'o' && myInfo.buffer[3] == 'd' && myInfo.buffer[4] == 'e'){
			//The 7.xKey encrypted .code partition
			uint32_t codeSize = *((unsigned int*)(myInfo.buffer + 0x0C));
			uint32_t nextSection = *((unsigned int*)(myInfo.buffer + 0x18)) + 0x200;
			myInfo.buffer += 0x200; myInfo.size = codeSize; myInfo.keyslot = 0x25;
			DecryptPartition(&myInfo);
			//The rest is normally encrypted
			memcpy((void*)&myInfo, (void*)info, sizeof(PartitionInfo));
			myInfo.buffer += nextSection; myInfo.size -= nextSection; myInfo.keyslot = 0x2C;
			myInfo.ctr = OriginalCTR;
			add_ctr(myInfo.ctr, nextSection/16);
			DecryptPartition(&myInfo);
		}else{
			myInfo.size = info->size-0x200;
			myInfo.buffer += 0x200;
			DecryptPartition(&myInfo);
		}
	}
}
