#include <mach/mach.h>
#include <libproc.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <string.h>
#include <errno.h>
#include <mach-o/dyld.h>
static void require(int ok,const char *message) {if(!ok){fprintf(stderr,"FAIL metrics guest: %s\n",message);exit(91);}}
int main(void) {
    _Static_assert(TASK_BASIC_INFO==5 && sizeof(task_basic_info_data_t)==40,"x86 task basic info ABI");
    pid_t pid=0;
    require(pid_for_task(mach_task_self(),&pid)==KERN_SUCCESS && pid==getpid(),"guest task PID");
    task_basic_info_data_t task;mach_msg_type_number_t count=TASK_BASIC_INFO_COUNT;
    require(task_info(mach_task_self(),TASK_BASIC_INFO,(task_info_t)&task,&count)==KERN_SUCCESS,"translated task-info flavor");
    require(count==TASK_BASIC_INFO_COUNT && task.virtual_size>0 && task.resident_size>0,"task-info size fields");
    host_t host=mach_host_self();vm_statistics_data_t stats;count=HOST_VM_INFO_COUNT;
    vm_size_t page=0;
    require(host_page_size(host,&page)==KERN_SUCCESS && page==(vm_size_t)getpagesize(),"real host page size ABI");
    require(host_statistics(host,HOST_VM_INFO,(host_info_t)&stats,&count)==KERN_SUCCESS && count==HOST_VM_INFO_COUNT,"host VM info");
    _Static_assert(sizeof(vm_statistics64_data_t)==416,"x86 VM-info64 ABI");
    vm_statistics64_data_t stats64;count=HOST_VM_INFO64_COUNT;
    require(host_statistics64(host,HOST_VM_INFO64,(host_info64_t)&stats64,&count)==KERN_SUCCESS&&count==HOST_VM_INFO64_COUNT,"host VM-info64 packing and count");
    require(mach_port_deallocate(mach_task_self(),host)==KERN_SUCCESS,"owned host port");
    _Static_assert(sizeof(struct statfs)==2168,"x86 statfs64 ABI");
    int mounts=getfsstat(NULL,0,MNT_NOWAIT);require(mounts>0,"filesystem count");
    struct statfs *filesystems=calloc((size_t)mounts+8,sizeof(*filesystems));require(filesystems!=NULL,"filesystem buffer");
    int loaded=getfsstat(filesystems,(mounts+8)*(int)sizeof(*filesystems),MNT_NOWAIT);
    require(loaded>0&&loaded<=mounts+8,"filesystem records");
    int root=0;for(int i=0;i<loaded;i++)if(strcmp(filesystems[i].f_mntonname,"/")==0)root=filesystems[i].f_bsize>0&&filesystems[i].f_blocks>0;
    require(root,"root filesystem fields and mount-point offsets");free(filesystems);
    struct proc_bsdinfo process;
    require(proc_pidinfo(getpid(),PROC_PIDTBSDINFO,0,&process,sizeof(process))==sizeof(process) && process.pbi_pid==(unsigned)getpid(),"Darwin BSD process info");
    struct proc_taskallinfo all;
    require(proc_pidinfo(getpid(),PROC_PIDTASKALLINFO,0,&all,sizeof(all))==sizeof(all)&&all.pbsd.pbi_pid==(unsigned)getpid()&&all.ptinfo.pti_threadnum>0,"combined BSD and task info ABI");
    char executable[PROC_PIDPATHINFO_MAXSIZE],name[32];
    int length=proc_pidpath(getpid(),executable,sizeof(executable));
    require(length>0&&(size_t)length==strlen(executable)&&strcmp(executable,_dyld_get_image_name(0))==0,"process path names the guest executable");
    const char *base=strrchr(executable,'/');base=base?base+1:executable;
    require(proc_name(getpid(),name,sizeof(name))>0&&strncmp(name,base,sizeof(name)-1)==0,"guest process name");
    name[0]='x';errno=0;
    require(proc_pidpath(getpid(),name,sizeof(name))==0&&errno==ENOMEM&&name[0]=='x',"small process-path buffer does not overwrite memory");
    struct rusage_info_v2 extended;
    require(proc_pid_rusage(getpid(),RUSAGE_INFO_V2,(rusage_info_t *)&extended)==0&&extended.ri_resident_size>0&&extended.ri_proc_start_abstime>0,"extended process resource-info-v2 ABI");
    struct proc_vnodepathinfo paths;
    require(proc_pidinfo(getpid(),PROC_PIDVNODEPATHINFO,0,&paths,sizeof(paths))==sizeof(paths)&&paths.pvi_cdir.vip_path[0]=='/'&&
            (paths.pvi_rdir.vip_path[0]==0||strcmp(paths.pvi_rdir.vip_path,"/")==0),"working/root directory path-info ABI");
    struct rusage usage;
    require(getrusage(RUSAGE_SELF,&usage)==0 && usage.ru_utime.tv_sec>=0,"resource usage ABI");
    puts("PASS guest Mach task/host and process metrics ABI");return 0;
}
