/*
 * CANopen main program file for Linux SocketCAN.
 *
 * @file        main
 * @author      Janez Paternoster
 * @copyright   2015 Janez Paternoster
 *
 * This file is part of CANopenNode, an opensource CANopen Stack.
 * Project home page is <http://canopennode.sourceforge.net>.
 * For more information on CANopen see <http://www.can-cia.org/>.
 *
 * CANopenNode is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include "CANopen.h"
#include "main_utils.h"
//#include "eeprom.h"
//#include "CgiLog.h"
//#include "CgiOD.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#include <pthread.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/reboot.h>
#include <sys/reboot.h>


#define TMR_TASK_INTERVAL_NS    (1000000)       /* Interval of taskTmr in nanoseconds */
#define TMR_TASK_INTERVAL_US    (1000)          /* Interval of taskTmr in microseconds */
#define INCREMENT_1MS(var)      (var++)         /* Increment 1ms variable in taskTmr */


/* Global variables and objects */
    volatile uint16_t       CO_timer1ms = 0U;   /* variable increments each millisecond */
    static int              rtPriority = 1;     /* Real time priority, configurable by arguments. */
    static taskMain_t       taskMain;           /* Object for Mainline task */
    static taskTmr_t        taskTmr;            /* Object for Timer interval task */
    static bool_t CANrx_taskTmr_process(int fd);/* Timer interval and CANrx tasks */

    static void* rt_thread(void* arg);          /* Realtime thread */

//    CO_EE_t             CO_EEO;             /* EEprom object */

//    CgiLog_t            CgiLogObj;
//    CgiCli_t            CgiCliObj;
//    CgiLog_t           *CgiLog = &CgiLogObj;
//    CgiCli_t           *CgiCli = &CgiCliObj;
//
//    #define CgiCANLogBufSize    0x80000L
//    #define CgiCliBufSize       0x10000
//    #define CgiCliSDObufSize    0x800
//    uint8_t CgiCANLogBuf0[CgiCANLogBufSize];
//    uint8_t CgiCANLogBuf1[CgiCANLogBufSize];
//    char_t CgiCliBuf[CgiCliBufSize];
//    uint8_t CgiCliSDObuf[CgiCliSDObufSize];


/* CAN Receive thread and locking */
#if 0
    static void* CANrx_thread(void* arg);
    /* After presence of SYNC message on CANopen bus, CANrx should be
     * temporary disabled until all receive PDOs are processed. See
     * also CO_SYNC.h file and CO_SYNC_initCallback() function. */
    static sem_t CANrx_sem;
    volatile static int CANrx_semCnt;
    static int CANrx_semInit(void){
        return sem_init(&CANrx_sem, 0, 1);
    }
    static void CANrx_lockCbSync(bool_t syncReceived){
        /* Callback function is called at SYNC message on CAN bus.
         * It disables CAN receive thread. Tries to lock, nonblocking */
        sem_trywait(&CANrx_sem);
        CANrx_semCnt = 1;
    }
    static void CANrx_unlock(void){
        int sVal;
        /* Function is called from tmrTask after
         * synchronous CANopen RPDOs are processed. */
        sem_getvalue(&CANrx_sem, &sVal);
        if(sVal < 1){
            sem_post(&CANrx_sem);
        }
        CANrx_semCnt = 0;
    }
#endif


/* Signal handler */
    static volatile sig_atomic_t endProgram = 0;
    static void sigHandler(int sig){
        endProgram = 1;
    }


/* Helper functions ***********************************************************/
void CO_errExit(char* msg){
    perror(msg);
    exit(EXIT_FAILURE);
}

static void usageExit(char *progName){
    fprintf(stderr, "Usage: %s <device> -i <Node ID (1..127)> [options]\n", progName);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -p <RT priority>    Realtime priority of rt_thread (default=1).\n");
    fprintf(stderr, "                      If specified as -1, realtime scheduler is not used.\n");
    fprintf(stderr, "  -r                  Enable reboot on CANopen NMT reset_node command. \n");
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

//static void wakeUpTask(uint32_t arg){
//    RTX_Wakeup(arg);
//}

/* main ***********************************************************************/
int main (int argc, char *argv[]){
    CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
    uint8_t powerOn = 1;
    int CANdevice0Index = 0;
    int opt;
    bool_t firstRun = true;
    pthread_t rt_thread_id;

    char* CANdevice = NULL;     /* CAN device, configurable by arguments. */
    int nodeId = -1;            /* Set to 1..127 by arguments */
    bool_t rebootEnable = false;/* Configurable by arguments */

    if(argc < 3 || strcmp(argv[1], "--help") == 0) usageExit(argv[0]);

    /* Get program options */
    while((opt = getopt(argc, argv, "i:p:r")) != -1) {
        switch (opt) {
            case 'i': nodeId = strtol(optarg, NULL, 0);     break;
            case 'p': rtPriority = strtol(optarg, NULL, 0); break;
            case 'r': rebootEnable = true;                  break;
            default:  usageExit(argv[0]);
        }
    }

    if(optind < argc){
        CANdevice = argv[optind];
        CANdevice0Index = if_nametoindex(CANdevice);
    }

    if(nodeId < 1 || nodeId > 127){
        fprintf(stderr, "Wrong node ID (%d)\n", nodeId);
        usageExit(argv[0]);
    }

    if(rtPriority != -1 && (rtPriority < sched_get_priority_min(SCHED_FIFO)
                         || rtPriority > sched_get_priority_max(SCHED_FIFO))){
        fprintf(stderr, "Wrong RT priority (%d)\n", rtPriority);
        usageExit(argv[0]);
    }

    if(CANdevice0Index == 0){
        char s[120];
        snprintf(s, 120, "Can't find CAN device \"%s\"", CANdevice);
        CO_errExit(s);
    }


    printf("%s - starting CANopen device with Node ID %d(0x%02X) ...\n", argv[0], nodeId, nodeId);


    /* Verify, if OD structures have proper alignment of initial values */
    if(CO_OD_RAM.FirstWord != CO_OD_RAM.LastWord) {
        fprintf(stderr, "Program init - %s - Error in CO_OD_RAM.\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if(CO_OD_EEPROM.FirstWord != CO_OD_EEPROM.LastWord) {
        fprintf(stderr, "Program init - %s - Error in CO_OD_EEPROM.\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if(CO_OD_ROM.FirstWord != CO_OD_ROM.LastWord) {
        fprintf(stderr, "Program init - %s - Error in CO_OD_ROM.\n", argv[0]);
        exit(EXIT_FAILURE);
    }


    /* task control (timers, delays) */
    if(taskMain_init(&taskMain, &CO_timer1ms, &OD_performance[ODA_performance_mainCycleMaxTime]) != 0)
        CO_errExit("Program init -taskMain_init failed");

    if(taskTmr_init(&taskTmr, TMR_TASK_INTERVAL_NS, &OD_performance[ODA_performance_timerCycleMaxTime]) != 0)
        CO_errExit("Program init  - taskTmr_init failed");

    OD_performance[ODA_performance_timerCycleTime] = TMR_TASK_INTERVAL_US;


//    /* initialize battery powered SRAM */
//    struct{
//        uint8_t* EEPROMptr;
//        uint32_t EEPROMsize;
//        uint8_t* emcyBufPtr;
//        uint32_t emcyBufSize;
//        uint32_t size;
//    }SRAM;
//
//    SRAM.size = 0;
//    /* Sizes must be divisible by 4. (struct should have uint32_t as last element.) */
//    SRAM.EEPROMsize = sizeof(CO_OD_EEPROM);                   SRAM.size += SRAM.EEPROMsize;
//    SRAM.emcyBufSize = OD_CANopenLog.emcySRAMSize&0xFFFFFFFC; SRAM.size += SRAM.emcyBufSize;
//
//    if(RTX_MemWindow(gSysPublicData.sramAddr, SRAM.size)){
//        SRAM.EEPROMptr = 0;
//        SRAM.emcyBufPtr = 0;
//        printf("\nError: SRAM initialization failed.");
//        return 1;
//    }
//    else{
//        SRAM.EEPROMptr = gSysPublicData.sramAddr;
//        SRAM.emcyBufPtr = SRAM.EEPROMptr + SRAM.EEPROMsize;
//    }


//    /* initialize EEPROM - part 1 */
//    CO_ReturnError_t eeStatus = CO_EE_init_1(&CO_EEO, SRAM.EEPROMptr, (uint8_t*) &CO_OD_EEPROM, sizeof(CO_OD_EEPROM),
//                            (uint8_t*) &CO_OD_ROM, sizeof(CO_OD_ROM));


//    /* initialize CGI interface. These functions are bound with a fixed name and */
//    /* are executed by the Web server task, if a HTTP request with such a fixed */
//    /* name comes in. This mechanism allows dynamic usage of the IPC@CHIP Web server. */
//    OD_CANopenLog.CANLogSize = CgiCANLogBufSize;
//
//    if(   CgiLog_init_1(CgiLog, SRAM.emcyBufPtr, SRAM.emcyBufSize, CgiCANLogBuf0,
//                        CgiCANLogBuf1, CgiCANLogBufSize, OD_CANopenLog.maxDumpFiles)
//       || CgiCli_init_1(CgiCli, CgiCliBuf, CgiCliBufSize, CgiCliSDObuf, CgiCliSDObufSize))
//    {
//        printf("\nError: CGI initialization failed.");
//        exit(EXIT_FAILURE);
//    }

    /* Catch signals SIGINT and SIGTERM */
    if(signal(SIGINT, sigHandler) == SIG_ERR)
        CO_errExit("Program init - SIGINIT handler creation failed");
    if(signal(SIGTERM, sigHandler) == SIG_ERR)
        CO_errExit("Program init - SIGTERM handler creation failed");

    /* increase variable each startup. Variable is stored in eeprom. */
    OD_powerOnCounter++;


    while(reset != CO_RESET_APP && reset != CO_RESET_QUIT && endProgram == 0){
/* CANopen communication reset - initialize CANopen objects *******************/
        CO_ReturnError_t err;

        printf("%s - communication reset ...\n", argv[0]);

        /* Wait rt_thread, then configure CAN */
        CO_LOCK_OD();

        CO_CANsetConfigurationMode(CANdevice0Index);


        /* initialize CANopen */
        err = CO_init(CANdevice0Index, nodeId, 0);
        if(err != CO_ERROR_NO){
            char s[120];
            snprintf(s, 120, "Communication reset - CANopen initialization failed, err=%d", err);
            CO_errExit(s);
        }

        /* set callback functions for task control. */
        CO_SYNC_initCallback(CO->SYNC, NULL);
        /* Prepare function, which will wake this task after CAN SDO response is */
        /* received (inside CAN receive interrupt). */
//        CO->SDO->pFunctSignal = wakeUpTask;    /* will wake from RTX_Sleep_Time() */
//        CO->SDO->functArg = RTX_Get_TaskID();  /* id of this task */

//        /* initialize eeprom - part 2 */
//        CO_EE_init_2(&CO_EEO, eeStatus, CO->SDO, CO->em);


        /* just for info */
        if(powerOn){
            CO_errorReport(CO->em, CO_EM_MICROCONTROLLER_RESET, CO_EMC_NO_ERROR, OD_powerOnCounter);
            powerOn = 0;
        }


        /* start CAN */
        CO_CANsetNormalMode(CO->CANmodule[0]);


        /* Generate realtime thread and set scheduler. */
        if(firstRun){
            firstRun = false;

            if(pthread_create(&rt_thread_id, NULL, rt_thread, NULL) != 0)
                CO_errExit("Program init - rt_thread creation failed");

            if(rtPriority > 0){
                struct sched_param param;

                param.sched_priority = rtPriority;
                if(pthread_setschedparam(rt_thread_id, SCHED_FIFO, &param) != 0)
                    CO_errExit("Program init - rt_thread set scheduler failed");
            }
        }


        CO_UNLOCK_OD();
        reset = CO_RESET_NOT;

        printf("%s - running ...\n", argv[0]);


        while(reset == CO_RESET_NOT && endProgram == 0){
/* loop for normal program execution ******************************************/
            uint16_t timer1msDiff;
            uint16_t timerNext = 50;

            /* Wait for timer to expire */
            if(taskMain_wait(&taskMain, &timer1msDiff) != 0)
                CO_errorReport(CO->em, CO_EM_GENERIC_SOFTWARE_ERROR, CO_EMC_SOFTWARE_INTERNAL, 0x20000000L | errno);

            /* CANopen process */
            reset = CO_process(CO, timer1msDiff, &timerNext);

            /* Nonblocking application code may go here. */
#if 1
        static uint16_t maxIntervalMain = 0;
        if(maxIntervalMain < timer1msDiff){
            maxIntervalMain = timer1msDiff;
            printf("Maximum main interval time: %d milliseconds\n", maxIntervalMain);
        }
#endif

            /* Set delay for next sleep. */
            if(taskMain_setDelay(&taskMain, timerNext) != 0)
                CO_errorReport(CO->em, CO_EM_GENERIC_SOFTWARE_ERROR, CO_EMC_SOFTWARE_INTERNAL, 0x20020000L | errno);


            //            CgiLogEmcyProcess(CgiLog);


            /* Process EEPROM */
//            CO_EE_process(&CO_EEO);
        }
    }


/* program exit ***************************************************************/
    /* join threads */
    endProgram = 1;
    if(pthread_join(rt_thread_id, NULL) != 0){
        CO_errExit("Program end - pthread_join failed");
    }

    /* delete objects from memory */
    CO_delete(CANdevice0Index);

    printf("%s on %s(nodeId=0x%02X) - finished.\n", argv[0], CANdevice, nodeId);

    /* Flush all buffers (and reboot) */
    sync();
    if(rebootEnable && reset == CO_RESET_APP){
        if(reboot(LINUX_REBOOT_CMD_RESTART) != 0){
            CO_errExit("Program end - reboot failed");
        }
    }

    exit(EXIT_SUCCESS);
}


/* Realtime thread for CAN receive and taskTmr ********************************/
static void* rt_thread(void* arg){
    int epfd;
    struct epoll_event ev;

    /* Configure epoll */
    epfd = epoll_create(2);
    if(epfd == -1)
        CO_errExit("rt_thread init - epoll_create failed");

    ev.events = EPOLLIN;
    ev.data.fd = CO->CANmodule[0]->fd;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, CO->CANmodule[0]->fd, &ev) == -1)
        CO_errExit("rt_thread init - epoll_ctl CANmodule failed");

    ev.events = EPOLLIN;
    ev.data.fd = taskTmr.fd;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, taskTmr.fd, &ev) == -1)
        CO_errExit("rt_thread init - epoll_ctl taskTmr failed");

    /* Endless loop */
    while(endProgram == 0){
        int ready;

        ready = epoll_wait(epfd, &ev, 1, -1);

        if(ready != 1){
            if(errno != EINTR){
                CO_errorReport(CO->em, CO_EM_GENERIC_SOFTWARE_ERROR, CO_EMC_SOFTWARE_INTERNAL, 0x30100000L | errno);
            }
        }

        else{
            if(CANrx_taskTmr_process(ev.data.fd) == false){
                /* No file descriptor was processed. */
                CO_errorReport(CO->em, CO_EM_GENERIC_SOFTWARE_ERROR, CO_EMC_SOFTWARE_INTERNAL, 0x30110000L);
            }
        }
    }

    return NULL;
}


/* Process CAN receive and taskTmr (use after epoll) **************************/
static bool_t CANrx_taskTmr_process(int fd){
    bool_t wasProcessed = true;

    /* Get received CAN message. */
    if(fd == CO->CANmodule[0]->fd){
        CO_CANrxWait(CO->CANmodule[0]);
    }


    /* Execute taskTmr */
    else if(fd == taskTmr.fd){
        if(taskTmr_wait(&taskTmr, NULL) != 0)
            CO_errorReport(CO->em, CO_EM_GENERIC_SOFTWARE_ERROR, CO_EMC_SOFTWARE_INTERNAL, 0x30200000L | errno);

        if(*taskTmr.maxTime > (TMR_TASK_INTERVAL_US*3) && rtPriority > 0){
            CO_errorReport(CO->em, CO_EM_ISR_TIMER_OVERFLOW, CO_EMC_SOFTWARE_INTERNAL, 0x30210000L | *taskTmr.maxTime);
        }


#if 1
        static uint16_t maxInterval = 0;
        if(maxInterval < OD_performance[ODA_performance_timerCycleMaxTime]){
            maxInterval = OD_performance[ODA_performance_timerCycleMaxTime];
            printf("Maximum timer interval time: %5d microseconds\n", maxInterval);
        }
#endif

        /* Lock PDOs and OD */
        CO_LOCK_OD();

        INCREMENT_1MS(CO_timer1ms);

        if(CO->CANmodule[0]->CANnormal) {
            bool_t syncWas;

            /* Process Sync and read inputs */
            syncWas = CO_process_SYNC_RPDO(CO, TMR_TASK_INTERVAL_US);

            /* Re-enable CANrx, if it was disabled by SYNC callback */

            /* Further I/O or nonblocking application code may go here. */

            /* Write outputs */
            CO_process_TPDO(CO, syncWas, TMR_TASK_INTERVAL_US);
        }

        /* Unlock */
        CO_UNLOCK_OD();
    }

    else{
        wasProcessed = false;
    }

    return wasProcessed;
}
