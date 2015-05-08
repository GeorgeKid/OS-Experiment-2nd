#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include "job.h"

int jobid=0;
int siginfo=1;
int fifo;
int globalfd;

//struct waitqueue *head=NULL;

/*优先级由高到低的3个队列，优先级 1 > 2 > 3*/
struct waitqueue *head_1st = NULL;
struct waitqueue *head_2nd = NULL;
struct waitqueue *head_3rd = NULL;

struct waitqueue *next=NULL,*current =NULL;

int time_delay = 1;

void Que_enq(struct waitqueue *newjob,int priorder/*优先级1、2、3*/)
{
	struct waitqueue **head = NULL;
	
	if (priorder == 0)
	{
		head = &(head_1st);
		newjob->job->defpri = 1;
		newjob->job->curpri = 1;
	} 
	else if (priorder == 1)
		head = &(head_1st);
	else if (priorder == 2)
		head = &(head_2nd);
	else if (priorder == 3)
		head = &(head_3rd);
	else
	{
		printf("ERROR : unknown que in Func.Que_enq. %d\n",priorder);
		exit(1);
	}

	if ((*head) == NULL)
		(*head) = newjob;
	else
	{
		struct waitqueue *p = (*head);
		
		while (p->next != NULL)
			p = p->next;
		p->next = newjob;
	}
}
struct waitqueue* Que_deq(struct waitqueue **head)
{
	struct waitqueue *return_value = NULL;

	if ((*head) == NULL)
	{
		printf("Q is Empty!");
		return_value =  NULL;
	}
	else
	{
		return_value = (*head);
		(*head) = (*head)->next;
		return_value->next = NULL;/*单节拆出，切断关联*/
	}

	return return_value;
}
void Que_moveto(struct waitqueue **head, struct waitqueue *obj, int dest)
{
	struct waitqueue *p = NULL;

	if (obj == (*head))
		(*head) = (*head)->next;
	else
	{
		p = (*head);
		while (p->next != obj)
			p = p->next;
		p->next = obj->next;
	}
	obj->next = NULL;
	Que_enq(obj, dest);
}
void Que_freeall(struct waitqueue *p)
{
	int i;

	for (i = 0; (p->job->cmdarg)[i] != NULL; i++){
		free((p->job->cmdarg)[i]);
		(p->job->cmdarg)[i] = NULL;
	}
	free(p->job->cmdarg);
	free(p->job);
	free(p);
}
void Que_search_delete(int target_id)
{
	struct waitqueue *p = NULL;

	p = head_1st;
	if (p == NULL)
		;
	else if (p->job->jid == target_id)
	{
		head_1st = head_1st->next;
		p->next = NULL;

		Que_freeall(p);
	}
	else
	{
		for (; p->next != NULL; p = p->next)
		if (p->next->job->jid == target_id)
			break;
		Que_freeall(p);
		return;
	}

	p = head_2nd;
	if (p == NULL)
		;
	else if (p->job->jid == target_id)
	{
		head_2nd = head_2nd->next;
		p->next = NULL;

		Que_freeall(p);
		return;
	}
	else
	{
		for (; p->next != NULL; p = p->next)
		if (p->next->job->jid == target_id)
			break;
		Que_freeall(p);
		return;
	}

	p = head_3rd;
	if (p == NULL)
		;
	else if (p->job->jid == target_id)
	{
		head_3rd = head_3rd->next;
		p->next = NULL;

		Que_freeall(p);
		return;
	}
	else
	{
		for (; p->next != NULL; p = p->next)
		if (p->next->job->jid == target_id)
			break;
		Que_freeall(p);
		return;
	}

	printf("Search faild.\n");
}
void Que_print(struct waitqueue *head)
{
	struct waitqueue *p = NULL;
	char timebuf[BUFLEN];

	for (p = head; p != NULL; p = p->next){
		strcpy(timebuf, ctime(&(p->job->create_time)));
		timebuf[strlen(timebuf) - 1] = '\0';
		printf("%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
			p->job->jid,
			p->job->pid,
			p->job->ownerid,
			p->job->run_time,
			p->job->wait_time,
			timebuf,
			"READY");
	}
}
void Que_print_all()
{
	printf("First Queue:\n");
	Que_print(head_1st);
	printf("Second Queue:\n");
	Que_print(head_2nd);
	printf("Third Queue:\n");
	Que_print(head_3rd);
}

/* 调度程序 */
void scheduler()
{
	struct jobinfo *newjob=NULL;
	struct jobcmd cmd;
	int  count = 0;
	bzero(&cmd,DATALEN);
	if((count=read(fifo,&cmd,DATALEN))<0)
		error_sys("read fifo failed");
#ifdef DEBUG

	if(count){
		printf("cmd cmdtype\t%d\ncmd defpri\t%d\ncmd data\t%s\n",cmd.type,cmd.defpri,cmd.data);
	}
	else
		printf("no data read\n");
#endif

         /* 更新之前*/                  do_stat(cmd);

	/* 更新等待队列中的作业 */
	updateall();
                                        
        /* 更新之后*/                   do_stat(cmd);

	switch(cmd.type){
	case ENQ:
		do_enq(newjob,cmd);
		break;
	case DEQ:
		do_deq(cmd);
		break;
	case STAT:
		do_stat(cmd);
		break;
	default:
		break;
	}

	/* 选择高优先级作业 */
	next=jobselect();
	/* 作业切换 */
	jobswitch();
}

int allocjid()
{
	return ++jobid;
}

void updateall()
{
	struct waitqueue *p;
	int time_period_ms = 1000;
	
	if (current != NULL)
	{
		if (current->job->curpri == 1)
			time_period_ms = 1000;
		else if (current->job->curpri == 2)
			time_period_ms = 2000;
		else if (current->job->curpri == 3)
			time_period_ms = 5000;
		else
		{
			printf("ERROR : in Func.updateall,time count error.\n");
			exit(1);
		}

	}

	/* 更新作业运行时间 */
	if (current)
		current->job->run_time += (time_period_ms/1000); /* 单位为s？,加1代表1000ms */

	/* 更新作业等待时间及优先级 */
	for (p = head_1st; p != NULL; p = p->next)
	{
		p->job->wait_time += time_period_ms;
	}

	for (p = head_2nd; p != NULL; p = p->next)
	{
		p->job->wait_time += time_period_ms;
		if (p->job->wait_time >= 10000 && p->job->curpri > 1)
		{
			p->job->curpri -= 1;
			Que_moveto((&head_2nd), p, p->job->curpri);
			printf ("Yui : move pid-%d from que-%d to que-%d.\n",p->job->pid,p->job->curpri+1,p->job->curpri);
			p->job->wait_time = 0;
		}
	}
	for (p = head_3rd; p != NULL; p = p->next)
	{
		p->job->wait_time += time_period_ms;
		if (p->job->wait_time >= 10000 && p->job->curpri > 1)
		{
			p->job->curpri -= 1;
			Que_moveto((&head_3rd), p, p->job->curpri);
			printf ("Yui : move pid-%d from que-%d to que-%d.\n",p->job->pid,p->job->curpri+1,p->job->curpri);
			p->job->wait_time = 0;	
		}
	}
}
struct waitqueue* jobselect()
{
	struct waitqueue **head = NULL;
	
	if (head_1st != NULL)
	{
		head = &head_1st;
	}
	else
	{
		if (head_2nd != NULL)
			head = &head_2nd;
			
		else
		{
			if (head_3rd != NULL)
				head = &head_3rd;
			else
				head = NULL;
		}
	}

	if (head != NULL)
		return Que_deq(head);
	else
		return NULL;
}
void jobswitch()
{
	struct waitqueue *p;
	int i;

	if (current && current->job->state == DONE){ /* 当前作业完成 */
		/* 作业完成，删除它 */
		for (i = 0; (current->job->cmdarg)[i] != NULL; i++){
			free((current->job->cmdarg)[i]);
			(current->job->cmdarg)[i] = NULL;
		}
		/* 释放空间 */
		free(current->job->cmdarg);
		free(current->job);
		free(current);

		current = NULL;
	}
	
	if (next == NULL && current == NULL) /* 没有作业要运行 */
		return;
	else if (next != NULL && current == NULL){ /* 开始新的作业 */

		printf("begin start new job\n");
		current = next;
		next = NULL;
		current->job->state = RUNNING;
		kill(current->job->pid, SIGCONT);
		return;
	}
	else if (next != NULL && current != NULL){ /* 切换作业 */

		printf("switch to Pid: %d\n", next->job->pid);
		kill(current->job->pid, SIGSTOP);
		//current->job->curpri = current->job->defpri;//当前优先级置为初始值
		current->job->wait_time = 0;
		current->job->state = READY;

		/* 放回等待队列 */
		Que_enq(current, current->job->curpri);//放回时依照原优先级置于相应队尾

		current = next;
		next = NULL;
		current->job->state = RUNNING;
		current->job->wait_time = 0;
		kill(current->job->pid, SIGCONT);
		return;
	}
	else{ /* next == NULL且current != NULL，不切换 */
		return;
	}
}


void sig_handler(int sig,siginfo_t *info,void *notused)
{
	int status;
	int ret;
			
	switch (sig) {
case SIGVTALRM: /* 到达计时器所设置的计时间隔 */
	time_delay--;	
	if (time_delay == 0)
	{
		scheduler();
		if (current != NULL && current->job->curpri == 2)
			time_delay = 2;
		else if (current != NULL && current->job->curpri == 3)
			time_delay = 5;
		else
			time_delay = 1;
	}
	
	return;
case SIGCHLD: /* 子进程结束时传送给父进程的信号 */
	ret = waitpid(-1,&status,WNOHANG);
	if (ret == 0)
		return;
	if(WIFEXITED(status)){
		current->job->state = DONE;
		printf("normal termation, exit status = %d\n",WEXITSTATUS(status));
	}else if (WIFSIGNALED(status)){
		printf("abnormal termation, signal number = %d\n",WTERMSIG(status));
	}else if (WIFSTOPPED(status)){
		printf("child stopped, signal number = %d\n",WSTOPSIG(status));
	}
	return;
	default:
		return;
	}
}

void do_deq(struct jobcmd deqcmd)
{
	int deqid, i;
	deqid = atoi(deqcmd.data);

#ifdef DEBUG
	printf("Task-7 : Before deq.\n");//任务7
	Que_print_all();
	
	printf("deq jid %d\n", deqid);
#endif

	/*current jodid==deqid,终止当前作业*/
	if (current && current->job->jid == deqid){
		printf("teminate current job\n");
		kill(current->job->pid, SIGKILL);
		for (i = 0; (current->job->cmdarg)[i] != NULL; i++){
			free((current->job->cmdarg)[i]);
			(current->job->cmdarg)[i] = NULL;
		}
		free(current->job->cmdarg);
		free(current->job);
		free(current);
		current = NULL;
	}
	else /* 或者在等待队列中查找deqid */
		Que_search_delete(deqid);	

#ifdef DEBUG
	printf("Task-7 : After deq.\n");//任务7
	Que_print_all();
#endif
}

void do_enq(struct jobinfo *newjob, struct jobcmd enqcmd)
{
#ifdef DEBUG
	printf("Task-7 : Before enq.\n");//任务7
	Que_print_all();
#endif

	struct waitqueue *newnode, *p;
	int i = 0, pid;
	char *offset, *argvec, *q;
	char **arglist;
	sigset_t zeromask;

	sigemptyset(&zeromask);

	/* 封装jobinfo数据结构 */
	newjob = (struct jobinfo *)malloc(sizeof(struct jobinfo));
	newjob->jid = allocjid();
	newjob->defpri = enqcmd.defpri;
	newjob->curpri = enqcmd.defpri;
	newjob->ownerid = enqcmd.owner;
	newjob->state = READY;
	newjob->create_time = time(NULL);
	newjob->wait_time = 0;
	newjob->run_time = 0;
	arglist = (char**)malloc(sizeof(char*)*(enqcmd.argnum + 1));
	newjob->cmdarg = arglist;
	offset = enqcmd.data;
	argvec = enqcmd.data;
	while (i < enqcmd.argnum){
		if (*offset == ':'){
			*offset++ = '\0';
			q = (char*)malloc(offset - argvec);
			strcpy(q, argvec);
			arglist[i++] = q;
			argvec = offset;
		}
		else
			offset++;
	}

	arglist[i] = NULL;

#ifdef DEBUG

	printf("enqcmd argnum %d\n", enqcmd.argnum);
	for (i = 0; i < enqcmd.argnum; i++)
		printf("parse enqcmd:%s\n", arglist[i]);

#endif

	/*向等待队列中增加新的作业*/
	newnode = (struct waitqueue*)malloc(sizeof(struct waitqueue));
	newnode->next = NULL;
	newnode->job = newjob;

	Que_enq(newnode,newnode->job->curpri);

	/*为作业创建进程*/
	if ((pid = fork())<0)
		error_sys("enq fork failed");

	if (pid == 0){
		newjob->pid = getpid();
		/*阻塞子进程,等等执行*/
		raise(SIGSTOP);
#ifdef DEBUG
		printf("Task-7 : After enq.\n");//任务7
		Que_print_all();

		printf("begin running\n");
		for (i = 0; arglist[i] != NULL; i++)
			printf("arglist %s\n", arglist[i]);
#endif

		/*复制文件描述符到标准输出*/
		dup2(globalfd, 1);
		/* 执行命令 */
		if (execv(arglist[0], arglist)<0)
			printf("exec failed\n");
		exit(1);
	}
	else{
		newjob->pid = pid;
	}
}

void do_stat(struct jobcmd statcmd)
{
	struct waitqueue *p;
	char timebuf[BUFLEN];
	/*
	*打印所有作业的统计信息:
	*1.作业ID
	*2.进程ID
	*3.作业所有者
	*4.作业运行时间
	*5.作业等待时间
	*6.作业创建时间
	*7.作业状态
	*/

	/* 打印信息头部 */
	printf("JOBID\tPID\tOWNER\tRUNTIME\tWAITTIME\tCREATTIME\t\tSTATE\n");
	if(current){
		strcpy(timebuf,ctime(&(current->job->create_time)));
		timebuf[strlen(timebuf)-1]='\0';
		printf("%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
			current->job->jid,
			current->job->pid,
			current->job->ownerid,
			current->job->run_time,
			current->job->wait_time,
			timebuf,"RUNNING");
	}
	
	printf ("First Queue:\n");
	Que_print(head_1st);
	printf ("Second Queue:\n");
	Que_print(head_2nd);
	printf ("Third Queue:\n");
	Que_print(head_3rd);
}

int main()
{
	struct stat statbuf;
	struct sigaction newact,oldact1,oldact2;

	struct timeval interval;
	struct itimerval new,old;

	if(stat("/tmp/server",&statbuf)==0){
		/* 如果FIFO文件存在,删掉 */
		if(remove("/tmp/server")<0)
			error_sys("remove failed");
	}

	if(mkfifo("/tmp/server",0666)<0)
		error_sys("mkfifo failed");
	/* 在非阻塞模式下打开FIFO */
	if((fifo=open("/tmp/server",O_RDONLY|O_NONBLOCK))<0)
		error_sys("open fifo failed");

	/* 建立信号处理函数 */
	newact.sa_sigaction=sig_handler;
	sigemptyset(&newact.sa_mask);
	newact.sa_flags=SA_SIGINFO;
	sigaction(SIGCHLD,&newact,&oldact1);
	sigaction(SIGVTALRM,&newact,&oldact2);

	/* 设置时间间隔为1000毫秒 */
	interval.tv_sec=1;
	interval.tv_usec=0;

	new.it_interval=interval;
	new.it_value=interval;
	
	setitimer(ITIMER_VIRTUAL,&new,&old);

	while(siginfo==1);

	close(fifo);
	close(globalfd);
	return 0;
}
