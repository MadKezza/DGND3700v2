#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
//#include <linux/mtd/mtd.h>
#include <sys/file.h>
#include "nvram.h"
#include <stdarg.h>
#include "nvram_lock.h"
#define BASE_PATH "/tmp/nv/"
#define BASE_PATH_LEN 8  // strlen(BASE_PATH) 

struct nv_entry{
	char *name;
	char *value;
	char *path;
	struct nv_entry *next;
};

static struct nv_entry *nv_list=NULL;

struct nv_entry *find_nv_entry(const char *name , const char *path)
{
	struct nv_entry *p=nv_list;


	if(!name || !p || !path)
		return NULL;

	while(p){
		if( strcmp(p->name,name)==0 && strcmp(p->path,path)==0)
			break;
		p=p->next;		
	}
	return p;
}
int add_nv_entry(const char *name,char *value , char * path)
{
	struct nv_entry *p;

	if(!value)
		return -1;

	p=find_nv_entry(name, path);

	if(!name || ! path){
		free(value);
		return -1;
	}
	// exist
	if(p){
		if(p->value)
			free(p->value);
		p->value = value;
	}else{
		struct nv_entry *new_entry;
		new_entry = malloc(sizeof(struct nv_entry));
		if(!new_entry){
			free(value);
			return -1;
		}
		new_entry->name = strdup(name);
		if(!new_entry->name){
			free(value);
			free(new_entry);
			return -1;
		}

		new_entry->path = strdup(path);
		if( !new_entry->path){
			free(value);
			free(new_entry->name);
			free(new_entry);
			return -1;
		}


		new_entry->value = value;
		new_entry->next=nv_list;
		nv_list = new_entry;
	}
	return 0;	
}

static int readFileBin_unlock(char *path, char **data) {
	int total;
	int fd=0;
	if((fd=open(path, O_RDONLY)) < 0)
		return -1;
	total=lseek(fd,0,SEEK_END);
	lseek(fd,0,0);
	
	if((*data=malloc(total))==NULL){
		close(fd);
		return -1;
	}
	if(read(fd,*data,total)<0){ 
		free(*data);
		close(fd);
		return -1;
	}
	close(fd);
   	return total;
}

int readFileBin(char *path, char **data) {
	int lock;
	int rc;

	lock = nvram_lock(path);
	rc = readFileBin_unlock(path, data);
	nvram_unlock(lock);
	return rc;
}

static void writeFileBin_unlock(char *path, char *data, int len) {
   int fd;

	if((fd=open(path, O_CREAT|O_WRONLY|O_TRUNC,S_IRUSR|S_IWUSR)) < 0)
   		return;
	write(fd, data, len);
	close(fd);
}


void writeFileBin(char *path, char *data, int len) {
	int lock;

	lock = nvram_lock(path);
	writeFileBin_unlock(path, data, len);
	nvram_unlock(lock);
	return;
}
    
static unsigned long crc32(char *data, int length)
{
	unsigned long crc, poly;
	long crcTable[256];
	int i, j;
	poly = 0xEDB88320L;
	for (i=0; i<256; i++) {
		crc = i;
		for (j=8; j>0; j--) {
			if (crc&1) {
				crc = (crc >> 1) ^ poly;
			} else {
				crc >>= 1;
			}
		}
		crcTable[i] = crc;
	}
	crc = 0xFFFFFFFF;

	while( length-- > 0) {
		crc = ((crc>>8) & 0x00FFFFFF) ^ crcTable[ (crc^((char)*(data++))) & 0xFF ];
	}
	
	return crc^0xFFFFFFFF;
}

static int nvram_load_unlock(void)
{
	unsigned long crc;
	int fd1 = -1;
	char *data = NULL;
	int err = 0;

	nvram_header_t header;

	fd1=open(NVRAM_PATH,O_RDONLY);
	if(fd1<0) {
		err = NVRAM_FLASH_ERR;
		goto _err_out;
	}
	read(fd1, &header,sizeof(nvram_header_t));

	/*seek to header end*/
	lseek(fd1,NVRAM_HEADER_SIZE,0);
	if(header.magic!=NVRAM_MAGIC)
	{
		err = NVRAM_MAGIC_ERR;
		goto _err_out;
	}

	data=malloc(header.len+1);
	read(fd1, data, header.len+1);		

	crc=crc32(data, header.len);
	if(crc!=header.crc)
	{
#ifdef TEST 
		printf("CRC Error!!\n");
		printf("header.crc=%lx\tcrc=%lx\n",header.crc,crc);
#endif	
		err  = NVRAM_CRC_ERR;
		goto _err_out;
	}

	writeFileBin_unlock(NVRAM_TMP_PATH, data, header.len);

	free(data);
	close(fd1);	

	return NVRAM_SUCCESS;

_err_out:
	if (data)
		free(data);
	if (fd1 >= 0)
		close(fd1);
	return err;
}

int nvram_load(void)
{
	int tmp_lock = -1, mtd_lock = -1;
	int err = 0;

	tmp_lock = nvram_lock(NVRAM_TMP_PATH);
	mtd_lock = nvram_lock(NVRAM_PATH);
	err = nvram_load_unlock();
	nvram_unlock(mtd_lock);
	nvram_unlock(tmp_lock);

	return err;
}

static int nvram_commit_unlock(void)
{
	int fd1 = -1;
	char *data = NULL;
	int len;
	int err = 0;

	nvram_header_t header;
	
	if((len=readFileBin_unlock(NVRAM_TMP_PATH, &data))<=0) {
		err =  NVRAM_SHADOW_ERR;
		goto _err_out;
	}

	header.magic=NVRAM_MAGIC;
	header.crc=crc32(data, len);
	header.len=len;

	if((fd1=open(NVRAM_PATH,O_WRONLY | O_CREAT))<0) {
		err =  NVRAM_FLASH_ERR;
		goto _err_out;
	}
	write(fd1, &header,sizeof(nvram_header_t));
	lseek(fd1,NVRAM_HEADER_SIZE,0);
	write(fd1, data, len);

// check data 
	lseek(fd1,NVRAM_HEADER_SIZE,0);
	read(fd1, data,len);
	if(header.crc!=crc32(data, len)){
		err = NVRAM_FLASH_ERR;
		goto _err_out;
	}			

	close(fd1);
	free(data);

	/* support tr069 notification */	
	system("/usr/bin/killall -USR2 cwmp > /dev/null 2>&1");	
	return NVRAM_SUCCESS;

_err_out:
	if (data)
		free(data);
	if (fd1 >= 0)
		close(fd1);

	return err;
}

int nvram_commit(void)
{
	int tmp_lock = -1, mtd_lock = -1;
	int err;

	tmp_lock = nvram_lock(NVRAM_TMP_PATH);
	mtd_lock = nvram_lock(NVRAM_PATH);
	err = nvram_commit_unlock();
	nvram_unlock(mtd_lock);
	nvram_unlock(tmp_lock);

	return err;
}

static char* nvram_get_func_unlock(const char *name,char *path)
{
	char *bufspace;
	int size;
	char *s,*sp;
	
	if((size=readFileBin_unlock(path, &bufspace))<0) 
		return NULL;

	for (s = bufspace; *s; s++) {
		if (!strncmp(s, name, strlen(name)) && *(s+strlen(name))=='=') {
			sp=malloc(strlen(s)-strlen(name));
			memcpy(sp,(s+strlen(name)+1),(strlen(s)-strlen(name)));
			free(bufspace);

			add_nv_entry(name,sp, & path[BASE_PATH_LEN]);

			return sp;
		}
		while(*(++s));
	}
	free(bufspace);
	return NULL;
}

char* nvram_get_func(const char *name,char *path)
{
	char *value;
	int lock;

	lock = nvram_lock(path);
	value = nvram_get_func_unlock(name, path);
	nvram_unlock(lock);

	return value;
}

char* nvram_get(const char *name)
{	
	char *pt;
	if((pt=nvram_get_func(name,NVRAM_TMP_PATH))==NULL){
			if((pt=nvram_get_func(name,NVRAM_DEFAULT))!=NULL)
				nvram_set(name,pt);
			else
				return NULL;
	}

	// [bcm_nvram] s
	if ( pt && !strncmp( pt, "*DEL*", 5 ) ) // check if this is the deleted var
	{
		pt = NULL;
	}
	// [bcm_nvram] e
	
	return pt;
}


static int nvram_unset_func_unlock(const char* name,char *path)
{
	char *bufspace, *targetspace;
	int size;
	char *sp, *s;
	int found=0;

	if((size=readFileBin_unlock(path, &bufspace))>0)
	    targetspace=malloc(size);
	else
		return NVRAM_SUCCESS;
		
	sp=targetspace;
	if(size > 0) {
		for (s = bufspace; *s; s++) {
			if (!strncmp(s, name, strlen(name)) && *(s+strlen(name))=='=') {
				found=1;
				while (*(++s));
			}
			else{
				while(*s) *(sp++)=*(s++);
			    	*(sp++)=END_SYMBOL;
			}
		}

		free(bufspace);
	}
	if(!found){
		free(targetspace);
		return NVRAM_SUCCESS;
	}
	
	*(sp) = END_SYMBOL;
	
	writeFileBin_unlock(path, targetspace, (sp-targetspace)+1);
	
	free(targetspace);

	return NVRAM_SUCCESS;
}

int nvram_unset_func(const char* name,char *path)
{
	int lock;
	int err;

	lock = nvram_lock(path);
	err = nvram_unset_func_unlock(name, path);
	nvram_unlock(lock);

	return err;
}

static int nvram_set_func_unlock(const char* name,const char* value,char *path)
{
	char *bufspace, *targetspace;
	int size;
	char *sp, *s;
	int found=0;

	if((size=readFileBin_unlock(path, &bufspace))>0) {
	    targetspace=malloc(size+strlen(name)+strlen(value)+2);
	}
	else {
	    targetspace=malloc(strlen(name)+strlen(value)+2);
	}

	sp=targetspace;
	if(size > 0) {
	   for (s = bufspace; *s; s++) {
		if (!strncmp(s, name, strlen(name)) && *(s+strlen(name))=='=') {
			found=1;
  			strcpy(sp, name);
			sp+=strlen(name);
        		*(sp++) = '=';
       			strcpy(sp, value);
			sp+=strlen(value);		
			while (*(++s));
		}
		while(*s) *(sp++)=*(s++);
	        *(sp++)=END_SYMBOL;
	    }
	
		free(bufspace);
	}
	if(!found){
		strcpy(sp, name);
		sp+=strlen(name);
        	*(sp++) = '=';
	        strcpy(sp, value);
		sp+=strlen(value);
	        *(sp++) = END_SYMBOL;
	}

	*(sp) = END_SYMBOL;

	writeFileBin_unlock(path, targetspace, (sp-targetspace)+1);
	free(targetspace);

	return NVRAM_SUCCESS;
}

int nvram_set_func(const char* name,const char* value,char *path)
{
	int lock;
	int err;
	
	lock = nvram_lock(path);
	err = nvram_set_func_unlock(name, value, path);
	nvram_unlock(lock);
	
	return err;
}


int nvram_set(const char* name,const char* value)
{
	return nvram_set_func(name,value,NVRAM_TMP_PATH);	
}
int nvram_bcm_set(const char* name,const char* value)
{
	return nvram_set_func(name,value,NVRAM_BCM_PATH);	
}
int nvram_bcm_unset(const char* name)
{
	return nvram_unset_func(name,NVRAM_BCM_PATH);
}


/*
	get or set the value of a key
*/
int nv_set(const char* category, const char* key, int key_idx, const char* value)
{
	char name[256];
	char path[256];

	snprintf( name, 256, "%s%d", key, key_idx);
	snprintf( path, 256, "%s%s", BASE_PATH, category);

	return nvram_set_func( name, value, path);
}


int nv_setFX(const char* category, const char* key, int key_idx, const char *value_format, ...)
{
	char name[256];
	char path[256];
	char value[4096];
	va_list arg;
	
	snprintf( name, 256, "%s%d", key, key_idx);
	snprintf( path, 256, "%s%s", BASE_PATH, category);

	value[0]=0;
	va_start(arg, value_format);
	vsnprintf(value,4096, value_format, arg);
	va_end(arg);
	


	return nvram_set_func( name, value, path);
}

int nv_set_int(const char* category, const char* key, int key_idx, int value)
{
	char value_str[256];
	
	snprintf( value_str, 256, "%d", value);

	return nv_set(category, key, key_idx, value_str);
}

char* nv_get(const char* category, const char* key, int key_idx)
{
	char *pt;
	char name[256];
	char path[256];

	snprintf( name, 256, "%s%d", key, key_idx);
	snprintf( path, 256, "%s%s", BASE_PATH, category);


	if((pt=nvram_get_func(name, path))==NULL)
		return NULL;

	// [bcm_nvram] s
	if ( pt && !strncmp( pt, "*DEL*", 5 ) ) // check if this is the deleted var
	{
		//free(pt);
		pt = NULL;
	}
	// [bcm_nvram] e
	
	return pt;
}


int nv_get_int(const char* category, const char* key, int key_idx)
{
	char* pt;	
	int ret=-1;
	pt = nv_get(category, key, key_idx);

	if(pt)
	{
		ret = atoi(pt);
		//free(pt);
	}

	return ret;
}

