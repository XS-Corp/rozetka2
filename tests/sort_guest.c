#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL sort: %s\n",message);exit(91);}}
typedef struct {int key,index;uint64_t guard;} item;
static __thread unsigned calls;
static pthread_t original;
static int nesting,fail;
static int number_compare(const void *left,const void *right){int a=*(const int *)left,b=*(const int *)right;return a<b?INT_MIN:a>b?INT_MAX:0;}
static int compare(const void *left,const void *right)
{
    if(fail)__asm__ volatile("ud2");
    char marker;uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());size_t size=pthread_get_stacksize_np(pthread_self());
    require(pthread_equal(original,pthread_self())&&(uintptr_t)&marker<top&&(uintptr_t)&marker>=top-size,"comparator guest thread and original stack");calls++;
    if(!nesting){nesting=1;int inner[]={7,-4,9,1};qsort(inner,4,sizeof(int),number_compare);require(inner[0]==-4&&inner[3]==9,"nested comparator sort");}
    const item *a=left,*b=right;require(a->guard==UINT64_C(0x1122334455667788)&&b->guard==a->guard,"original element pointers and complete records");return a->key<b->key?INT_MIN:a->key>b->key?INT_MAX:0;
}
static int compare_context(void *opaque,const void *left,const void *right){require(opaque==(void *)42,"Darwin qsort_r userdata first");return number_compare(right,left);}
int main(int argc,char **argv)
{
    original=pthread_self();int data[]={5,-2,8,1};
    if(argc>1){
        if(strcmp(argv[1],"overflow")==0)qsort(data,SIZE_MAX,2,number_compare);
        else if(strcmp(argv[1],"memory")==0)qsort((void *)(uintptr_t)1,2,4,number_compare);
        else if(strcmp(argv[1],"target")==0)qsort(data,4,4,(void *)(uintptr_t)1);
        else {item records[2]={{.key=2,.guard=0x1122334455667788},{.key=1,.guard=0x1122334455667788}};fail=1;qsort(records,2,sizeof(item),compare);}
        return 90;
    }
    item records[257];for(int i=0;i<257;i++)records[i]=(item){.key=(i*103)%67-33,.index=i,.guard=0x1122334455667788};
    qsort(records,257,sizeof(item),compare);unsigned char seen[257]={0};
    for(int i=0;i<257;i++){require(i==0||records[i-1].key<=records[i].key,"signed comparator ordering with duplicates");require(records[i].index>=0&&records[i].index<257&&!seen[records[i].index]++,"sort preserves all records");}
    require(calls>257,"x86 comparator executed and retained TLS");
    qsort_r(data,4,sizeof(int),(void *)42,compare_context);require(data[0]==8&&data[1]==5&&data[2]==1&&data[3]==-2,"contextual descending order");
    for(unsigned i=0;i<4100;i++){int pair[]={2,1};qsort(pair,2,sizeof(int),number_compare);require(pair[0]==1&&pair[1]==2,"synchronous callbacks do not exhaust pinned contexts");}
    unsigned before=calls;qsort(records,0,sizeof(item),compare);require(calls==before,"empty array does not call comparator");
    puts("PASS x86 qsort/qsort_r: records, signed results, nested sorts, original stack, TLS and repeated callbacks");return 0;
}
