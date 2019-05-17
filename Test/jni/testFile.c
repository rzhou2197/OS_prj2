/*
 * StudentNumber:516021910576
 * Name:ZhouRong
 */

#include <stdio.h>				
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>				
#include <string.h>
#include <sched.h>              
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <ctype.h>

char *getSchedulerName(int policy){
	switch(policy){
		case 0:
			return "NORMAL";
		case 1:
			return "FIFO";
		case 2:
			return "RR";
		case 6:
			return "WRR";
	}
	return ;
}

int checkInput(int policy){
	switch(policy){
		case 0:
			return 0;
		case 1:
			return 0;
		case 2:
			return 0;
		case 6:
			return 0;
	}
	return -1;
}

int main(int argc, char *argv[])
{
	
    printf ("Current policy for this testfile is %s\n", getSchedulerName(sched_getscheduler(getpid())));
    printf ("PID for this testfile is %d\n", getpid());
    
   
    struct sched_param param;
    int prioPolicy;
    int policy;
    struct timespec tp;
    printf("Please input the choice of scheduling algorithms:0-NORMAL,1-FIFO,2-RR,6-WRR: ");
    scanf("%d",&policy);
    if(checkInput(policy)==-1){
    	perror("Reading input scheduling algorithm error: out of range!\n");
        exit(1);
    }


    
    int targetPID;
    prioPolicy = sched_get_priority_max(policy);


    printf("Please enter the id of testprocess: ");
    scanf("%d",&targetPID);

    int pre_policy=sched_getscheduler(targetPID);
    printf ("Previous scheduling algorithm for testprocess is %s\n", getSchedulerName(pre_policy));


    printf("Set testprocess's priority(1-99): ");
    scanf("%d",&prioPolicy);
    if(prioPolicy<1 || prioPolicy>99){
    	perror("Priority setting error: out of range!\n");
        exit(1);
    }
    printf("Current scheduler priority for testprocess is %d\n",prioPolicy);
    param.sched_priority = prioPolicy;

    if(sched_setscheduler(targetPID, policy, &param) == -1){
        perror("sched_setscheduler() error!\n");
        exit(1);
    }

    if(sched_rr_get_interval(targetPID, &tp) == -1){
        perror("sched_rr_get_interval() error!\n");
        exit(1);
    }
    printf ("pre scheduler: %s\ncur scheduler: %s\ntime slice is %.2lf", getSchedulerName(pre_policy), getSchedulerName(sched_getscheduler(targetPID)),tp.tv_nsec/(1000000.0f));
    return 0;
}


