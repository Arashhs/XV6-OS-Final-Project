#include "types.h"
#include "user.h"

void runFunc(){
	printf(1, "Created thread id: %d\n", getThreadID());
    printf(1, "Created thread parent process id: %d\n", getpid());
    exitThread();
}
void runFunc2(){

}

void runFunc3(){

}

int main()
{
    printf(1, "Main process id: %d\n", getpid());
    printf(1, "Main thread id: %d\n", getThreadID());
	void* stack = (void*)malloc(4000);
	void(*func)();
	func = (void*) runFunc;
	int tid1 = createThread(func, stack);
//    int tid2 = createThread(func, stack);
    int ret = joinThread(tid1);
//    joinThread(tid2);
    printf(1, "tid: %d, joinThread(tid) returned: %d\n", tid1, ret);
	exit();
}