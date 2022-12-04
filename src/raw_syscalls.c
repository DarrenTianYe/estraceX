#include "common.h"
#include <stdbool.h>

struct syscall_data_t {
    u32 pid;
    u32 tid;
    u32 type;
    u32 syscall_id;
    u64 lr;
    u64 sp;
    u64 pc;
    u64 ret;
    u64 arg_index;
    u64 args[6];
    char comm[16];
    char arg_str[1024];
};

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
} syscall_events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, u32);
    __type(value, struct syscall_data_t);
    __uint(max_entries, 1);
} syscall_data_buffer_heap SEC(".maps");

// 用于指明哪些参数是string类型的mask
struct arg_mask_t {
    u32 mask;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u32);
    __type(value, struct arg_mask_t);
    __uint(max_entries, 512);
} arg_mask_map SEC(".maps");

// 用于设置过滤配置
struct filter_t {
    u32 uid;
    u32 pid;
    u32 is_32bit;
    u32 try_bypass;
    u32 tid_blacklist_mask;
    u32 tid_blacklist[MAX_COUNT];
    u32 syscall_mask;
    u32 syscall[MAX_COUNT];
    u32 syscall_blacklist_mask;
    u32 syscall_blacklist[MAX_COUNT];
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u32);
    __type(value, struct filter_t);
    __uint(max_entries, 1);
} filter_map SEC(".maps");

static long inline send_data(struct bpf_raw_tracepoint_args* ctx, struct syscall_data_t* data, u64 addr) {
    u32 filter_key = 0;
    struct filter_t* filter = bpf_map_lookup_elem(&filter_map, &filter_key);
    if (filter == NULL) {
        return 0;
    }
    if (filter->try_bypass) {
        char target[3][12] = {
            "/dev/.magisk",
            "which su",
            "mount",
        };
        #pragma unroll
        for (int i = 0; i < 3; i++) {
            bool need_override = true;
            #pragma unroll
            for (int j = 0; j < 12; j++) {
                if (target[i][j] == 0) break;
                if (data->arg_str[j] != target[i][j]) {
                    need_override = false;
                    break;
                }
            }
            if (need_override) {
                // char fmt0[] = "hit rule, lets bypass it, uid:%s\n";
                // bpf_trace_printk(fmt0, sizeof(fmt0), data->arg_str);
                char placeholder[] = "/estrace/is/watching/you";
                bpf_probe_write_user((void*)addr, placeholder, sizeof(placeholder));
            }
        }
    }

    long status = bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));
    return status;
}

SEC("raw_tracepoint/sys_enter")
int raw_syscalls_sys_enter(struct bpf_raw_tracepoint_args* ctx) {

    u32 filter_key = 0;
    struct filter_t* filter = bpf_map_lookup_elem(&filter_map, &filter_key);
    if (filter == NULL) {
        return 0;
    }

    // 获取信息用于过滤
    u64 current_uid_gid = bpf_get_current_uid_gid();
    u32 uid = current_uid_gid >> 32;
    u64 current_pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = current_pid_tgid >> 32;
    u32 tid = current_pid_tgid & 0xffffffff;
    // uid 过滤
    if (filter->uid != 0 && filter->uid != uid) {
        return 0;
    }
    // pid 过滤
    if (filter->pid != 0 && filter->pid != pid) {
        return 0;
    }
    // tid 黑名单过滤
    #pragma unroll
    for (int i = 0; i < MAX_COUNT; i++) {
        if ((filter->tid_blacklist_mask & (1 << i))) {
            if (filter->tid_blacklist[i] == tid) {
                // 在tid黑名单直接结束跳过
                return 0;
            }
        } else {
            // 减少不必要的循环
            break;
        }
    }

    // syscall 白名单过滤
    bool has_find = false;
    #pragma unroll
    for (int i = 0; i < MAX_COUNT; i++) {
        if ((filter->syscall_mask & (1 << i))) {
            if (filter->syscall[i] == ctx->args[1]) {
                has_find = true;
                break;
            }
        } else {
            if (i == 0) {
                // 如果没有设置白名单 则将 has_find 置为 true
                has_find = true;
            }
            // 减少不必要的循环
            break;
        }
    }
    // 不满足白名单规则 则跳过
    if (!has_find) {
        return 0;
    }

    // syscall 黑名单过滤
    #pragma unroll
    for (int i = 0; i < MAX_COUNT; i++) {
        if ((filter->syscall_blacklist_mask & (1 << i))) {
            if (filter->syscall_blacklist[i] == ctx->args[1]) {
                // 在syscall黑名单直接结束跳过
                return 0;
            }
        } else {
            // 减少不必要的循环
            break;
        }
    }

    // 读取参数 字符串类型的根据预设mask读取并分组发送
    struct pt_regs *regs = (struct pt_regs*)(ctx->args[0]);

    u32 zero = 0;
    struct syscall_data_t* data = bpf_map_lookup_elem(&syscall_data_buffer_heap, &zero);
    if (data == NULL) {
        return 0;
    }

    // 获取线程名
    __builtin_memset(&data->comm, 0, sizeof(data->comm));
    bpf_get_current_comm(&data->comm, sizeof(data->comm));

    // 线程名过滤？后面考虑有没有必要

    // 基本信息
    data->pid = pid;
    data->tid = tid;
    data->syscall_id = ctx->args[1];

    // 先获取 lr sp pc 并发送 这样可以尽早计算调用来源情况
    if(filter->is_32bit) {
        bpf_probe_read_kernel(&data->lr, sizeof(data->lr), &regs->regs[14]);
    }
    else {
        bpf_probe_read_kernel(&data->lr, sizeof(data->lr), &regs->regs[30]);
    }
    bpf_probe_read_kernel(&data->pc, sizeof(data->pc), &regs->pc);
    bpf_probe_read_kernel(&data->sp, sizeof(data->sp), &regs->sp);
    __builtin_memset(&data->arg_str, 0, sizeof(data->arg_str));
    data->type = 1;
    long status = bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));

    // 获取参数
    data->type = 2;
    // 获取字符串参数类型配置
    struct arg_mask_t* arg_mask = bpf_map_lookup_elem(&arg_mask_map, &data->syscall_id);
    if ((filter->is_32bit && data->syscall_id == 11) || (!filter->is_32bit && data->syscall_id == 221)) {
        // execve 3个参数
        // const char *filename char *const argv[] char *const envp[]
        // 下面的写法是基于已知参数类型构成为前提
        #pragma unroll
        for (int j = 0; j < 3; j++) {
            data->arg_index = j;
            bpf_probe_read_kernel(&data->args[j], sizeof(u64), &regs->regs[j]);
            if (data->args[j] == 0) continue;
            if (j == 0) {
                __builtin_memset(&data->arg_str, 0, sizeof(data->arg_str));
                bpf_probe_read_user(data->arg_str, sizeof(data->arg_str), (void*)data->args[j]);
                // bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));
                send_data(ctx, data, data->args[j]);
            } else {
                // 最多遍历得到6个子参数
                for (int i = 0; i < 6; i++) {
                    __builtin_memset(&data->arg_str, 0, sizeof(data->arg_str));
                    void* ptr = (void*)(data->args[j] + 8 * i);
                    u64 addr = 0x0;
                    // 这里应该用 bpf_probe_read_user 而不是 bpf_probe_read_kernel
                    bpf_probe_read_user(&addr, sizeof(u64), ptr);
                    if (addr != 0) {
                        bpf_probe_read_user(data->arg_str, sizeof(data->arg_str), (void*)addr);
                        // bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));
                        send_data(ctx, data, addr);
                    } else {
                        // 遇到为NULL的 直接结束内部遍历
                        break;
                    }
                }
            }
        }
    } else if ((filter->is_32bit && data->syscall_id == 387) || (!filter->is_32bit && data->syscall_id == 281)) {
        // int execveat(int dirfd, const char *pathname, const char *const argv[], const char *const envp[], int flags);
        #pragma unroll
        for (int j = 0; j < 5; j++) {
            data->arg_index = j;
            bpf_probe_read_kernel(&data->args[j], sizeof(u64), &regs->regs[j]);
            if (data->args[j] == 0) continue;
            if (arg_mask && !(arg_mask->mask & (1 << j))) continue;
            if (j == 1) {
                __builtin_memset(&data->arg_str, 0, sizeof(data->arg_str));
                bpf_probe_read_user(data->arg_str, sizeof(data->arg_str), (void*)data->args[j]);
                // bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));
                send_data(ctx, data, data->args[j]);
            } else {
                for (int i = 0; i < 6; i++) {
                    __builtin_memset(&data->arg_str, 0, sizeof(data->arg_str));
                    void* ptr = (void*)(data->args[j] + 8 * i);
                    u64 addr = 0x0;
                    bpf_probe_read_user(&addr, sizeof(u64), ptr);
                    if (addr != 0) {
                        bpf_probe_read_user(data->arg_str, sizeof(data->arg_str), (void*)addr);
                        // bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));
                        send_data(ctx, data, addr);
                    } else {
                        break;
                    }
                }
            }
        }
    } else {
        // 展开循环
        #pragma unroll
        for (int i = 0; i < 6; i++) {
            // 栈空间大小限制 分组发送
            if (arg_mask && arg_mask->mask & (1 << i)) {
                data->arg_index = i;
                bpf_probe_read_kernel(&data->args[i], sizeof(u64), &regs->regs[i]);
                __builtin_memset(&data->arg_str, 0, sizeof(data->arg_str));
                // bpf_probe_read_str 读取出来有的内容部分是空 结果中不会有NUL
                // bpf_probe_read_user 读取出来有的内容极少是空 但许多字符串含有NUL
                // bpf_probe_read_user_str 读取出来有的内容部分是空 结果中不会有NUL
                // 综合测试使用 bpf_probe_read_user 最合理 在前端处理 NUL
                // 不过仍然有部分结果是空 调整大小又能读到 原因未知
                bpf_probe_read_user(data->arg_str, sizeof(data->arg_str), (void*)data->args[i]);
                // long status = bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));
                send_data(ctx, data, data->args[i]);
            }
        }
    }

    return 0;
}

SEC("raw_tracepoint/sys_exit")
int raw_syscalls_sys_exit(struct bpf_raw_tracepoint_args* ctx) {
    u32 filter_key = 0;
    struct filter_t* filter = bpf_map_lookup_elem(&filter_map, &filter_key);
    if (filter == NULL) {
        return 0;
    }

    // 获取信息用于过滤
    u64 current_uid_gid = bpf_get_current_uid_gid();
    u32 uid = current_uid_gid >> 32;
    u64 current_pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = current_pid_tgid >> 32;
    u32 tid = current_pid_tgid & 0xffffffff;
    // uid过滤
    if (filter->uid != 0 && filter->uid != uid) {
        return 0;
    }
    // pid过滤
    if (filter->pid != 0 && filter->pid != pid) {
        return 0;
    }
    // tid 黑名单过滤
    #pragma unroll
    for (int i = 0; i < MAX_COUNT; i++) {
        if ((filter->tid_blacklist_mask & (1 << i))) {
            if (filter->tid_blacklist[i] == tid) {
                // 在tid黑名单直接结束跳过
                return 0;
            }
        } else {
            // 减少不必要的循环
            break;
        }
    }

    struct pt_regs *regs = (struct pt_regs*)(ctx->args[0]);

    u32 zero = 0;
    struct syscall_data_t* data = bpf_map_lookup_elem(&syscall_data_buffer_heap, &zero);
    if (data == NULL) {
        return 0;
    }

    if(filter->is_32bit) {
        bpf_probe_read_kernel(&data->syscall_id, sizeof(data->syscall_id), &regs->regs[7]);
    }
    else {
        bpf_probe_read_kernel(&data->syscall_id, sizeof(data->syscall_id), &regs->regs[8]);
    }

    // syscall 白名单过滤
    bool has_find = false;
    #pragma unroll
    for (int i = 0; i < MAX_COUNT; i++) {
        if ((filter->syscall_mask & (1 << i))) {
            if (filter->syscall[i] == data->syscall_id) {
                has_find = true;
                break;
            }
        } else {
            if (i == 0) {
                // 如果没有设置白名单 则将 has_find 置为 true
                has_find = true;
            }
            // 减少不必要的循环
            break;
        }
    }
    // 不满足白名单规则 则跳过
    if (!has_find) {
        return 0;
    }

    // syscall 黑名单过滤
    #pragma unroll
    for (int i = 0; i < MAX_COUNT; i++) {
        if ((filter->syscall_blacklist_mask & (1 << i))) {
            if (filter->syscall_blacklist[i] == data->syscall_id) {
                // 在syscall黑名单直接结束跳过
                return 0;
            }
        } else {
            // 减少不必要的循环
            break;
        }
    }

    data->type = 3;
    data->ret = ctx->args[1];
    data->pid = pid;
    data->tid = tid;
    // 获取线程名
    bpf_get_current_comm(&data->comm, sizeof(data->comm));
    // 发送数据
    long status = bpf_perf_event_output(ctx, &syscall_events, BPF_F_CURRENT_CPU, data, sizeof(struct syscall_data_t));
    return 0;
}