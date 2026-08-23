/*
 * System V IPC compatibility backed by zedBSD's kernel POSIX IPC objects.
 * Stable integer IDs map to private names, so unrelated processes using the
 * same key reach the same kernel object while permissions remain kernel-owned.
 * SPDX-License-Identifier: Zlib
 */
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>

static int
ipc_id(key_t key)
{
	uint32_t value;
	if(key==IPC_PRIVATE){do value=arc4random()&0x3fffffffU;while(value==0);}
	else value=((uint32_t)key&0x3fffffffU)+1U;
	return(int)value;
}

static void ipc_name(char *output,size_t size,const char *kind,int id,int member)
{ if(member<0)snprintf(output,size,"/sysv-%s-%08x",kind,(unsigned)id);else snprintf(output,size,"/sysv-%s-%08x-%04x",kind,(unsigned)id,(unsigned)member); }

static sem_t *
msg_lock_open(int id, int create)
{
	char name[64];
	ipc_name(name, sizeof(name), "msgl", id, -1);
	return create ? sem_open(name, O_CREAT, 0600, 1U) : sem_open(name, 0);
}

int
msgget(key_t key,int flags)
{
	char name[64];struct mq_attr attr={0,16,256,0};int id=ipc_id(key),open_flags=O_RDWR;
	ipc_name(name,sizeof(name),"msg",id,-1);
	if((flags&IPC_CREAT)||key==IPC_PRIVATE)open_flags|=O_CREAT;
	if(flags&IPC_EXCL)open_flags|=O_EXCL;
	{
		mqd_t queue=mq_open(name,open_flags,(mode_t)(flags&0777),&attr);
		if (queue < 0)
			return -1;
		sem_t *lock = msg_lock_open(id, 1);
		if (lock == SEM_FAILED) {
			mq_close(queue);
			return -1;
		}
		sem_close(lock);
		mq_close(queue);
		return id;
	}
}

int
msgsnd(int id,const void *message,size_t size,int flags)
{
	char name[64];mqd_t queue;sem_t *lock;unsigned char *wire;long type;int result;
	if(message==NULL){errno=EFAULT;return -1;}memcpy(&type,message,sizeof(type));if(type<=0){errno=EINVAL;return -1;}
	if(size>256-sizeof(int64_t)){errno=EMSGSIZE;return -1;}
	wire=malloc(size+sizeof(int64_t));if(!wire)return -1;
	{int64_t encoded=type;memcpy(wire,&encoded,sizeof(encoded));memcpy(wire+sizeof(encoded),(const char*)message+sizeof(long),size);}
	ipc_name(name,sizeof(name),"msg",id,-1);queue=mq_open(name,O_WRONLY|O_NONBLOCK);
	if(queue<0){free(wire);return -1;}
	lock=msg_lock_open(id,0);if(lock==SEM_FAILED){mq_close(queue);free(wire);return -1;}
	for(;;){
		int saved;
		if(sem_wait(lock)!=0){result=-1;break;}
		result=mq_send(queue,(char*)wire,size+sizeof(int64_t),0);
		saved=errno;(void)sem_post(lock);errno=saved;
		if(result==0||(flags&IPC_NOWAIT)||errno!=EAGAIN)break;
		(void)usleep(10000U);
	}
	sem_close(lock);mq_close(queue);free(wire);return result;
}

ssize_t
msgrcv(int id,void *message,size_t capacity,long selector,int flags)
{
	char name[64];mqd_t queue;sem_t *lock;struct mq_attr attr;unsigned char *messages;ssize_t *lengths;unsigned *priorities;size_t count,index,chosen;ssize_t result;
	if(message==NULL){errno=EFAULT;return -1;}ipc_name(name,sizeof(name),"msg",id,-1);
	queue=mq_open(name,O_RDWR|O_NONBLOCK);if(queue<0)return -1;if(mq_getattr(queue,&attr)!=0){mq_close(queue);return -1;}
	lock=msg_lock_open(id,0);if(lock==SEM_FAILED){mq_close(queue);return -1;}
	messages=malloc((size_t)attr.mq_maxmsg*(size_t)attr.mq_msgsize);lengths=malloc((size_t)attr.mq_maxmsg*sizeof(*lengths));priorities=malloc((size_t)attr.mq_maxmsg*sizeof(*priorities));
	if(!messages||!lengths||!priorities){free(messages);free(lengths);free(priorities);sem_close(lock);mq_close(queue);return -1;}
	for(;;){
		int operation_error=0;
		chosen=(size_t)-1;result=-1;count=0;
		if(sem_wait(lock)!=0)break;
		while(count<(size_t)attr.mq_maxmsg){
			lengths[count]=mq_receive(queue,(char*)messages+count*(size_t)attr.mq_msgsize,(size_t)attr.mq_msgsize,&priorities[count]);
			if(lengths[count]<0){if(errno!=EAGAIN)operation_error=errno;break;}count++;
		}
		for(index=0;index<count;index++){
			int64_t type;memcpy(&type,messages+index*(size_t)attr.mq_msgsize,sizeof(type));
			if(selector==0 || (selector>0 && ((flags&MSG_EXCEPT)?type!=selector:type==selector)) ||
			    (selector<0 && type<=-selector)){
				if(chosen==(size_t)-1)chosen=index;else if(selector<0){int64_t old;memcpy(&old,messages+chosen*(size_t)attr.mq_msgsize,sizeof(old));if(type<old)chosen=index;}
			}
		}
		if(chosen!=(size_t)-1){size_t payload=(size_t)lengths[chosen]-sizeof(int64_t);int64_t type;
			if(payload>capacity&&!(flags&MSG_NOERROR)){operation_error=E2BIG;}else{if(payload>capacity)payload=capacity;memcpy(&type,messages+chosen*(size_t)attr.mq_msgsize,sizeof(type));*(long*)message=(long)type;memcpy((char*)message+sizeof(long),messages+chosen*(size_t)attr.mq_msgsize+sizeof(type),payload);result=(ssize_t)payload;}
		}
		for(index=0;index<count;index++)if(index!=chosen || result<0)if(mq_send(queue,(char*)messages+index*(size_t)attr.mq_msgsize,(size_t)lengths[index],priorities[index])!=0&&operation_error==0)operation_error=errno;
		(void)sem_post(lock);
		if(result>=0)break;
		if(operation_error!=0){errno=operation_error;break;}
		if(flags&IPC_NOWAIT){errno=ENOMSG;break;}
		(void)usleep(10000U);
	}
	free(messages);free(lengths);free(priorities);sem_close(lock);mq_close(queue);return result;
}

int
msgctl(int id,int command,struct msqid_ds *status)
{
	char name[64],lock_name[64];mqd_t queue;struct mq_attr attr;ipc_name(name,sizeof(name),"msg",id,-1);
	if(command==IPC_RMID){int result=mq_unlink(name),saved=errno;ipc_name(lock_name,sizeof(lock_name),"msgl",id,-1);(void)sem_unlink(lock_name);errno=saved;return result;}
	queue=mq_open(name,O_RDWR);if(queue<0)return -1;
	if(command==IPC_STAT){if(status==NULL){mq_close(queue);errno=EFAULT;return -1;}if(mq_getattr(queue,&attr)!=0){mq_close(queue);return -1;}memset(status,0,sizeof(*status));status->msg_qnum=(size_t)attr.mq_curmsgs;status->msg_qbytes=(size_t)attr.mq_maxmsg*(size_t)attr.mq_msgsize;mq_close(queue);return 0;}
	mq_close(queue);if(command==IPC_SET)return 0;errno=EINVAL;return -1;
}

int
semget(key_t key,int count,int flags)
{
	int id=ipc_id(key),index;char name[64];sem_t *semaphore;
	if(count<0||count>256){errno=EINVAL;return -1;}if(count==0)count=1;
	for(index=0;index<count;index++){int open_flags=0;ipc_name(name,sizeof(name),"sem",id,index);if((flags&IPC_CREAT)||key==IPC_PRIVATE)open_flags|=O_CREAT;if(flags&IPC_EXCL)open_flags|=O_EXCL;semaphore=sem_open(name,open_flags,(mode_t)(flags&0777),0U);if(semaphore==SEM_FAILED)return -1;sem_close(semaphore);}
	return id;
}

static sem_t *open_sem(int id,int number,char name[64])
{ipc_name(name,64,"sem",id,number);return sem_open(name,0);}

int
semop(int id,struct sembuf *operations,size_t count)
{
	size_t index,done=0;if(operations==NULL&&count){errno=EFAULT;return -1;}
	for(index=0;index<count;index++){char name[64];sem_t*s=open_sem(id,operations[index].sem_num,name);int step=0,error=0;if(s==SEM_FAILED)goto rollback;
		if(operations[index].sem_op<0){for(step=0;step< -operations[index].sem_op;step++)if(((operations[index].sem_flg&IPC_NOWAIT)?sem_trywait(s):sem_wait(s))!=0){error=errno;break;}}
		else if(operations[index].sem_op>0){for(step=0;step<operations[index].sem_op;step++)if(sem_post(s)!=0){error=errno;break;}}
		else {int value;if(sem_getvalue(s,&value)!=0||value!=0)error=EAGAIN;}
		sem_close(s);if(error){errno=error;goto rollback;}done=index+1;
	}
	return 0;
rollback:
	while(done){char name[64];sem_t*s;struct sembuf*op=&operations[--done];s=open_sem(id,op->sem_num,name);if(s==SEM_FAILED)continue;if(op->sem_op<0){int n;for(n=0;n< -op->sem_op;n++)sem_post(s);}else if(op->sem_op>0){int n;for(n=0;n<op->sem_op;n++)sem_trywait(s);}sem_close(s);}return -1;
}

int
semctl(int id,int number,int command,...)
{
	va_list arguments;char name[64];sem_t*s;int value,result=0;unsigned short *array;
	if(command==IPC_RMID){for(number=0;number<256;number++){ipc_name(name,sizeof(name),"sem",id,number);if(sem_unlink(name)!=0&&number==0)return -1;}return 0;}
	s=open_sem(id,number,name);if(s==SEM_FAILED)return -1;
	va_start(arguments,command);
	switch(command){case GETVAL:if(sem_getvalue(s,&value)!=0)result=-1;else result=value;break;case SETVAL:value=va_arg(arguments,int);while(sem_trywait(s)==0);errno=0;while(value-->0)if(sem_post(s)!=0){result=-1;break;}break;case GETPID:result=0;break;case GETNCNT:case GETZCNT:result=0;break;case IPC_STAT:{struct semid_ds*st=va_arg(arguments,struct semid_ds*);if(!st){errno=EFAULT;result=-1;}else{memset(st,0,sizeof(*st));st->sem_nsems=1;}}break;case GETALL:array=va_arg(arguments,unsigned short*);if(!array||sem_getvalue(s,&value)){errno=EFAULT;result=-1;}else array[0]=(unsigned short)value;break;case SETALL:array=va_arg(arguments,unsigned short*);if(!array){errno=EFAULT;result=-1;}else{while(sem_trywait(s)==0);value=array[0];while(value-->0)sem_post(s);}break;case IPC_SET:break;default:errno=EINVAL;result=-1;}
	va_end(arguments);sem_close(s);return result;
}

struct attachment { void *address; size_t size; };
static struct attachment attachments[64];

int
shmget(key_t key,size_t size,int flags)
{
	int id=ipc_id(key),open_flags=O_RDWR;char name[64];int fd;struct stat status;
	if(size==0&&key==IPC_PRIVATE){errno=EINVAL;return -1;}ipc_name(name,sizeof(name),"shm",id,-1);
	if ((flags & IPC_CREAT) || key == IPC_PRIVATE)
		open_flags |= O_CREAT;
	if (flags & IPC_EXCL)
		open_flags |= O_EXCL;
	fd=shm_open(name,open_flags,(mode_t)(flags&0777));if(fd<0)return -1;
	if(fstat(fd,&status)!=0){close(fd);return -1;}if(status.st_size==0&&size!=0){if(ftruncate(fd,(off_t)size)!=0){close(fd);return -1;}}else if(size>(size_t)status.st_size){close(fd);errno=EINVAL;return -1;}
	close(fd);return id;
}

void *
shmat(int id,const void *requested,int flags)
{
	char name[64];int fd,prot=PROT_READ;struct stat status;void*address;unsigned index;
	if (!(flags & SHM_RDONLY))
		prot |= PROT_WRITE;
	if ((flags & SHM_RND) && requested != NULL)
		requested = (const void *)((uintptr_t)requested & ~(uintptr_t)(SHMLBA - 1));
	ipc_name(name, sizeof(name), "shm", id, -1);
	fd = shm_open(name, (flags & SHM_RDONLY) ? O_RDONLY : O_RDWR, 0);
	if (fd < 0)
		return (void *)-1;
	if(fstat(fd,&status)!=0){close(fd);return(void*)-1;}address=mmap((void*)requested,(size_t)status.st_size,prot,MAP_SHARED|(requested?MAP_FIXED_NOREPLACE:0),fd,0);close(fd);if(address==MAP_FAILED)return(void*)-1;
	for(index=0;index<64;index++)if(attachments[index].address==NULL){attachments[index].address=address;attachments[index].size=(size_t)status.st_size;return address;}
	munmap(address,(size_t)status.st_size);errno=EMFILE;return(void*)-1;
}

int
shmdt(const void *address)
{unsigned index;for(index=0;index<64;index++)if(attachments[index].address==address){int result=munmap(attachments[index].address,attachments[index].size);if(result==0)attachments[index].address=NULL;return result;}errno=EINVAL;return -1;}

int
shmctl(int id,int command,struct shmid_ds *status)
{
	char name[64];int fd;struct stat st;ipc_name(name,sizeof(name),"shm",id,-1);if(command==IPC_RMID)return shm_unlink(name);
	fd=shm_open(name,O_RDWR,0);if(fd<0)return -1;if(command==IPC_STAT){if(!status){close(fd);errno=EFAULT;return -1;}if(fstat(fd,&st)!=0){close(fd);return -1;}memset(status,0,sizeof(*status));status->shm_segsz=(size_t)st.st_size;close(fd);return 0;}close(fd);if(command==IPC_SET)return 0;errno=EINVAL;return -1;
}
