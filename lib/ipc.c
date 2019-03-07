// User-level IPC library routines

#include <inc/lib.h>

// 通过IPC接收一个值，并且返回它
// 如果pg(实际上就是dstva)是非空的，那么 sender发送的物理页 会 映射到pg上。
// 如果from_env_store是 非空的，那么 把IPC发送方的envid 存进 *from_env_store中
// 如果perm_store是非空的，那么 把IPC发送方 的页权限 存到 *perm_store中
//   (这是非零的 当且仅当 一个物理页 成功地传递给了pg)
// 如果 系统调用失败了，那么 把*fromenv 和 *perm设为0(如果他们都是非空的)，并返回错误
// 否则，返回 发送方的发送的值
// 
// 提示：
//   用'thisenv'来 发现 发送值，以及 发送方
// 	 如果'pg'是空的，那么 给 sys_ipc_recv 传递一个值(UTOP)，使得 调用者 知道 是“no page”
//   (0不是一个 正确的值的选项，因为 它是一个合法的 能够映射 物理页的 虚拟地址的位置。)
int32_t
ipc_recv(envid_t *from_env_store, void *pg, int *perm_store)
{
	// LAB 4: Your code here.
	// panic("ipc_recv not implemented");
	if (pg == NULL){
		pg = (void *)UTOP;//
	}

    int r = sys_ipc_recv(pg);
    int from_env = 0, perm = 0;
    if (r == 0) {
        from_env = thisenv->env_ipc_from;
        perm = thisenv->env_ipc_perm;
        r = thisenv->env_ipc_value;
    } else {
        from_env = 0;
        perm = 0;
    }   

    if (from_env_store){
		*from_env_store = from_env;
	}
    if (perm_store){
		*perm_store = perm;
	}

    return r;
}

// 把值val发送给to_env对应的进程(以及pg(srcva)对应的物理页，权限为perm)
// 这个函数 会一直尝试，直到 成功发送
// 除了-E_IP_NOT_RECV的错误 都要抛出异常panic()
// 
// 提示：
// 	 使用sys_yield()来 变得CPU-friendly
//   如果pg是空的，那么 把pg设为某个值(UTOP) 使得 sys_ipc_try_send知道 意味着no page
//   (0 不是那个正确的值)
void
ipc_send(envid_t to_env, uint32_t val, void *pg, int perm)
{
	// LAB 4: Your code here.
	// panic("ipc_send not implemented");

	if (pg == NULL) pg = (void *)UTOP;

    int ret;
    while ((ret = sys_ipc_try_send(to_env, val, pg, perm))) {// 一直尝试发送 直到成功
        if (ret != -E_IPC_NOT_RECV){
			panic("ipc_send error %e", ret);
		}
        sys_yield();
    }
}

// Find the first environment of the given type.  We'll use this to
// find special environments.
// Returns 0 if no such environment exists.
envid_t
ipc_find_env(enum EnvType type)
{
	int i;
	for (i = 0; i < NENV; i++)
		if (envs[i].env_type == type)
			return envs[i].env_id;
	return 0;
}
