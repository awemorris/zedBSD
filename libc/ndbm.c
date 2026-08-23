/*
 * zedBSD ndbm implementation. The .pag format is little-endian ZDBM v1:
 * magic[8], u32 count, then repeated u32 key-size, u32 value-size and bytes.
 * The companion .dir file identifies the format and prevents accidental use
 * of an incompatible host DBM database.
 * SPDX-License-Identifier: Zlib
 */
#include <errno.h>
#include <fcntl.h>
#include <ndbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct dbm_record { unsigned char *key, *value; uint32_t key_size, value_size; };
struct __zedbsd_dbm {
	char *base;
	struct dbm_record *records;
	size_t count, capacity, iteration;
	int writable, error;
};

static const unsigned char magic[8] = { 'Z','D','B','M',1,0,0,0 };

static void put32(unsigned char out[4], uint32_t value)
{ out[0]=(unsigned char)value; out[1]=(unsigned char)(value>>8); out[2]=(unsigned char)(value>>16); out[3]=(unsigned char)(value>>24); }
static uint32_t get32(const unsigned char in[4])
{ return (uint32_t)in[0]|((uint32_t)in[1]<<8)|((uint32_t)in[2]<<16)|((uint32_t)in[3]<<24); }

static char *
suffix_name(const char *base, const char *suffix)
{
	size_t length = strlen(base), extra = strlen(suffix);
	char *name = malloc(length + extra + 1);
	if (name != NULL) { memcpy(name, base, length); memcpy(name + length, suffix, extra + 1); }
	return name;
}

static int
find_record(DBM *database, datum key)
{
	size_t index;
	if (key.dsize < 0 || (key.dsize != 0 && key.dptr == NULL)) return -1;
	for (index = 0; index < database->count; index++)
		if (database->records[index].key_size == (uint32_t)key.dsize &&
		    !memcmp(database->records[index].key, key.dptr, (size_t)key.dsize))
			return (int)index;
	return -1;
}

static int
write_database(DBM *database)
{
	char *page = suffix_name(database->base, ".pag");
	char *temporary = suffix_name(database->base, ".pag.new");
	FILE *file;
	unsigned char word[4];
	size_t index;
	if (page == NULL || temporary == NULL) goto no_memory;
	file = fopen(temporary, "wb");
	if (file == NULL) goto failed;
	if (fwrite(magic, 1, 8, file) != 8) goto io_failed;
	put32(word, (uint32_t)database->count);
	if (fwrite(word, 1, 4, file) != 4) goto io_failed;
	for (index = 0; index < database->count; index++) {
		struct dbm_record *record = &database->records[index];
		put32(word, record->key_size); if (fwrite(word,1,4,file)!=4) goto io_failed;
		put32(word, record->value_size); if (fwrite(word,1,4,file)!=4) goto io_failed;
		if (fwrite(record->key,1,record->key_size,file)!=record->key_size ||
		    fwrite(record->value,1,record->value_size,file)!=record->value_size) goto io_failed;
	}
	if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
		(void)fclose(file);
		goto close_failed;
	}
	if (fclose(file) != 0) goto close_failed;
	if (rename(temporary, page) != 0) goto failed;
	free(page); free(temporary); return 0;
io_failed:
	database->error = 1; (void)fclose(file); goto failed;
close_failed:
	database->error = 1;
failed:
	free(page); free(temporary); return -1;
no_memory:
	free(page); free(temporary); errno = ENOMEM; database->error = 1; return -1;
}

static int
load_database(DBM *database, FILE *file)
{
	unsigned char header[12], words[8];
	uint32_t count, index;
	if (fread(header,1,sizeof(header),file) != sizeof(header) || memcmp(header,magic,8)) { errno=EIO; return -1; }
	count = get32(header + 8);
	if (count > 1048576U) { errno=EFBIG; return -1; }
	if (count != 0) {
		database->records = calloc(count, sizeof(*database->records));
		if (database->records == NULL) return -1;
		database->capacity = count;
	}
	for (index = 0; index < count; index++) {
		struct dbm_record *record = &database->records[index];
		if (fread(words,1,8,file) != 8) goto corrupt;
		record->key_size=get32(words); record->value_size=get32(words+4);
		if (record->key_size > 16777216U || record->value_size > 16777216U) goto corrupt;
		record->key=malloc(record->key_size ? record->key_size : 1);
		record->value=malloc(record->value_size ? record->value_size : 1);
		if (record->key==NULL || record->value==NULL) return -1;
		if (fread(record->key,1,record->key_size,file)!=record->key_size ||
		    fread(record->value,1,record->value_size,file)!=record->value_size) goto corrupt;
		database->count++;
	}
	return 0;
corrupt:
	errno=EIO; return -1;
}

DBM *
dbm_open(const char *name, int flags, mode_t mode)
{
	DBM *database;
	char *page, *directory;
	FILE *file;
	(void)mode;
	if (name == NULL) { errno=EINVAL; return NULL; }
	database=calloc(1,sizeof(*database));
	if (database==NULL) return NULL;
	database->base=strdup(name);
	page=suffix_name(name,".pag"); directory=suffix_name(name,".dir");
	if (database->base==NULL || page==NULL || directory==NULL) goto failed;
	database->writable=(flags & (O_WRONLY|O_RDWR)) != 0;
	file=(flags&O_TRUNC)&&database->writable ? NULL :
	    fopen(page,database->writable ? "r+b" : "rb");
	if (file==NULL && (flags & O_CREAT)) {
		file=fopen(page,"w+b");
		if (file!=NULL && fwrite(magic,1,8,file)==8) {
			unsigned char zero[4]={0}; (void)fwrite(zero,1,4,file); (void)fflush(file);
			{ FILE *marker=fopen(directory,"wb"); if(marker){(void)fwrite(magic,1,8,marker);(void)fclose(marker);} }
			(void)fseek(file,0,SEEK_SET);
		}
	}
	if (file==NULL || load_database(database,file)!=0) { if(file)fclose(file); goto failed; }
	fclose(file); free(page); free(directory); return database;
failed:
	free(page); free(directory); dbm_close(database); return NULL;
}

void
dbm_close(DBM *database)
{
	size_t index;
	if (database==NULL) return;
	for(index=0;index<database->count;index++){free(database->records[index].key);free(database->records[index].value);}
	free(database->records); free(database->base); free(database);
}

datum
dbm_fetch(DBM *database, datum key)
{
	datum result={NULL,0}; int index;
	if(database==NULL){errno=EINVAL;return result;}
	index=find_record(database,key);
	if(index>=0){result.dptr=(char*)database->records[index].value;result.dsize=(int)database->records[index].value_size;}
	return result;
}

int
dbm_store(DBM *database, datum key, datum value, int flags)
{
	int index; unsigned char *new_key=NULL,*new_value;
	if(database==NULL || !database->writable || key.dsize<0 || value.dsize<0 ||
	    (key.dsize && !key.dptr) || (value.dsize && !value.dptr)){errno=EINVAL;return -1;}
	index=find_record(database,key);
	if(index>=0 && flags==DBM_INSERT) return 1;
	new_value=malloc(value.dsize ? (size_t)value.dsize : 1);
	if(new_value==NULL) return -1;
	memcpy(new_value,value.dptr,(size_t)value.dsize);
	if(index<0){
		if(database->count==database->capacity){size_t cap=database->capacity?database->capacity*2:8;void*p=reallocarray(database->records,cap,sizeof(*database->records));if(!p){free(new_value);return -1;}database->records=p;database->capacity=cap;}
		new_key=malloc(key.dsize?(size_t)key.dsize:1);if(!new_key){free(new_value);return -1;}memcpy(new_key,key.dptr,(size_t)key.dsize);
		index=(int)database->count++;database->records[index].key=new_key;database->records[index].key_size=(uint32_t)key.dsize;database->records[index].value=NULL;
	}
	free(database->records[index].value);database->records[index].value=new_value;database->records[index].value_size=(uint32_t)value.dsize;
	return write_database(database);
}

int
dbm_delete(DBM *database, datum key)
{
	int index;
	if(database==NULL || !database->writable){errno=EINVAL;return -1;}
	index=find_record(database,key);if(index<0)return -1;
	free(database->records[index].key);free(database->records[index].value);
	if((size_t)index+1<database->count)memmove(&database->records[index],&database->records[index+1],(database->count-(size_t)index-1)*sizeof(*database->records));
	database->count--;database->iteration=0;return write_database(database);
}

datum dbm_firstkey(DBM *database){if(database)database->iteration=0;return dbm_nextkey(database);}
datum dbm_nextkey(DBM *database){datum result={NULL,0};if(database&&database->iteration<database->count){struct dbm_record*r=&database->records[database->iteration++];result.dptr=(char*)r->key;result.dsize=(int)r->key_size;}return result;}
int dbm_error(DBM *database){return database==NULL?1:database->error;}
int dbm_clearerr(DBM *database){if(database)database->error=0;return 0;}
