/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 bmax121. All Rights Reserved.
 */

/*   SPDX-License-Identifier: GPL-3.0-only   */
/*
 * Copyright (C) 2024 Nep-Timeline. All Rights Reserved.
 * Copyright (C) 2024 lzghzr. All Rights Reserved.
 */

#include "re_kernel.h"

#include <asm/atomic.h>
#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <taskext.h>

#include "../kpm_utils.h"
#include "re_utils.h"

KPM_NAME("re_kernel");
KPM_VERSION(MYKPM_VERSION);
KPM_LICENSE("GPL v3");
KPM_AUTHOR("Nep-Timeline, lzghzr");
KPM_DESCRIPTION("Re:Kernel, support 4.4 ~ 6.6");

#define NETLINK_REKERNEL_MAX 26
#define NETLINK_REKERNEL_MIN 22
#define USER_PORT 100
#define PACKET_SIZE 256
#define MIN_USERAPP_UID 10000
#define MAX_SYSTEM_UID 2000
#define PARCEL_OFFSET 16
#define INTERFACETOKEN_BUFF_SIZE 140

enum report_type {
  BINDER,
  SIGNAL,
#ifdef CONFIG_NETWORK
  NETWORK,
#endif /* CONFIG_NETWORK */
};
enum binder_type {
  REPLY,
  TRANSACTION,
  OVERFLOW,
};
static const char* binder_type[] = {
    "reply",
    "transaction",
    "free_buffer_full",
};

#define IZERO (1UL << 0x10)
#define UZERO (1UL << 0x20)

// cgroup_freezing, cgroupv1_freeze
static bool (*cgroup_freezing)(struct task_struct* task);
// send_netlink_message
struct sk_buff* kfunc_def(__alloc_skb)(unsigned int size, gfp_t gfp_mask, int flags, int node);
struct nlmsghdr* kfunc_def(__nlmsg_put)(struct sk_buff* skb, u32 portid, u32 seq, int type, int len, int flags);
void kfunc_def(kfree_skb)(struct sk_buff* skb);
int kfunc_def(netlink_unicast)(struct sock* ssk, struct sk_buff* skb, u32 portid, int nonblock);
// netlink_rcv
int kfunc_def(netlink_rcv_skb)(struct sk_buff* skb,
                               int (*cb)(struct sk_buff*, struct nlmsghdr*, struct netlink_ext_ack*));
// start_rekernel_server
static struct net kvar_def(init_net);
struct sock* kfunc_def(__netlink_kernel_create)(struct net* net, int unit, struct module* module,
                                                struct netlink_kernel_cfg* cfg);
void kfunc_def(netlink_kernel_release)(struct sock* sk);
// prco
struct proc_dir_entry* kfunc_def(proc_mkdir)(const char* name, struct proc_dir_entry* parent);
struct proc_dir_entry* kfunc_def(proc_create_data)(const char* name, umode_t mode, struct proc_dir_entry* parent,
                                                   const struct file_operations* proc_fops, void* data);
void kfunc_def(proc_remove)(struct proc_dir_entry* de);
// hook binder_proc_transaction
static int (*binder_proc_transaction)(struct binder_transaction* t, struct binder_proc* proc,
                                      struct binder_thread* thread);
// free the outdated transaction and buffer
static void (*binder_transaction_buffer_release)(struct binder_proc* proc, struct binder_thread* thread,
                                                 struct binder_buffer* buffer, binder_size_t off_end_offset,
                                                 bool is_failure);
static void (*binder_transaction_buffer_release_v6)(struct binder_proc* proc, struct binder_thread* thread,
                                                    struct binder_buffer* buffer, binder_size_t failed_at,
                                                    bool is_failure);
static void (*binder_transaction_buffer_release_v4)(struct binder_proc* proc, struct binder_buffer* buffer,
                                                    binder_size_t failed_at, bool is_failure);
static void (*binder_transaction_buffer_release_v3)(struct binder_proc* proc, struct binder_buffer* buffer,
                                                    binder_size_t* failed_at);
static void (*binder_alloc_free_buf)(struct binder_alloc* alloc, struct binder_buffer* buffer);
void kfunc_def(kfree)(const void* objp);
struct binder_stats kvar_def(binder_stats);
// hook do_send_sig_info
static int (*do_send_sig_info)(int sig, struct siginfo* info, struct task_struct* p, enum pid_type type);
// hook binder_transaction
static void (*binder_transaction)(struct binder_proc* proc, struct binder_thread* thread,
                                  struct binder_transaction_data* tr, int reply, binder_size_t extra_buffers_size);
// copy_from_user
void* kfunc_def(memdup_user)(const void __user* src, size_t len);
void kfunc_def(kvfree)(const void* addr);

#ifdef CONFIG_NETWORK
// netfilter
kuid_t kfunc_def(sock_i_uid)(struct sock* sk);
// hook tcp_rcv
static int (*tcp_v4_rcv)(struct sk_buff* skb);
static int (*tcp_v6_rcv)(struct sk_buff* skb);
static int ipv4_version = 4, ipv6_version = 6;
#endif /* CONFIG_NETWORK */

// _raw_spin_lock && _raw_spin_unlock
void kfunc_def(_raw_spin_lock)(raw_spinlock_t* lock);
void kfunc_def(_raw_spin_unlock)(raw_spinlock_t* lock);
// trace
int kfunc_def(tracepoint_probe_register)(struct tracepoint* tp, void* probe, void* data);
int kfunc_def(tracepoint_probe_unregister)(struct tracepoint* tp, void* probe, void* data);
// trace_binder_transaction
struct tracepoint kvar_def(__tracepoint_binder_transaction);
#ifdef CONFIG_DEBUG_CMDLINE
int kfunc_def(get_cmdline)(struct task_struct* task, char* buffer, int buflen);
#endif /* CONFIG_DEBUG_CMDLINE */

// 最好初始化一个大于 0xFFFFFFFF 的值, 否则编译器优化后, 全局变量可能出错
// 实际上会被编译器优化为 bool
static uint64_t binder_transaction_buffer_release_ver6 = UZERO, binder_transaction_buffer_release_ver5 = UZERO,
                binder_transaction_buffer_release_ver4 = UZERO;

static unsigned long trace = UZERO, ext_tr_offset = UZERO;

#ifndef CONFIG_VMLINUX
struct struct_offset struct_offset = {};
#else
#include "re_offsets.vmlinux.c"
#endif
#include "re_offsets.c"

// binder_node_lock
static inline void binder_node_lock(struct binder_node* node) {
  spinlock_t* node_lock = binder_node_lock_ptr(node);
  spin_lock(node_lock);
}
// binder_node_unlock
static inline void binder_node_unlock(struct binder_node* node) {
  spinlock_t* node_lock = binder_node_lock_ptr(node);
  spin_unlock(node_lock);
}
// binder_inner_proc_lock
static inline void binder_inner_proc_lock(struct binder_proc* proc) {
  spinlock_t* inner_lock = binder_proc_inner_lock(proc);
  spin_lock(inner_lock);
}
// binder_inner_proc_unlock
static inline void binder_inner_proc_unlock(struct binder_proc* proc) {
  spinlock_t* inner_lock = binder_proc_inner_lock(proc);
  spin_unlock(inner_lock);
}

// binder_is_frozen
static inline bool binder_is_frozen(struct binder_proc* proc) {
  bool is_frozen = false;
  if (struct_offset.binder_proc_is_frozen > 0) {
    is_frozen = binder_proc_is_frozen(proc);
  }
  return is_frozen;
}

// cgroupv2_freeze
static inline bool jobctl_frozen(struct task_struct* task) {
  unsigned long jobctl = task_jobctl(task);
  return ((jobctl & JOBCTL_TRAP_FREEZE) != 0);
}
// 判断线程是否进入 frozen 状态
static inline bool frozen_task_group(struct task_struct* task) {
  return (jobctl_frozen(task) || cgroup_freezing(task));
}

// netlink
static int netlink_count = 0;
static struct sock* rekernel_netlink;
static unsigned long rekernel_netlink_unit = UZERO;
static struct proc_dir_entry *rekernel_dir, *rekernel_unit_entry;
static const struct file_operations rekernel_unit_fops = {};
// 发送 netlink 消息
static int send_netlink_message(char* msg) {
  int len = strlen(msg);
  struct sk_buff* skbuffer;
  struct nlmsghdr* nlhdr;

  skbuffer = nlmsg_new(len, GFP_ATOMIC);
  if (!skbuffer) {
    logkm("netlink alloc failure.\n");
    return -ENOMEM;
  }

  nlhdr = nlmsg_put(skbuffer, 0, 0, rekernel_netlink_unit, len, 0);
  if (!nlhdr) {
    logkm("nlmsg_put failaure.\n");
    nlmsg_free(skbuffer);
    return -EMSGSIZE;
  }

  memcpy(nlmsg_data(nlhdr), msg, len);
  return netlink_unicast(rekernel_netlink, skbuffer, USER_PORT, MSG_DONTWAIT);
}
// 接收 netlink 消息
static int netlink_rcv_msg(struct sk_buff* skb, struct nlmsghdr* nlh, struct netlink_ext_ack* extack) {
  char* umsg = nlmsg_data(nlh);
  if (!umsg)
    return -EINVAL;

  netlink_count++;
  char netlink_kmsg[PACKET_SIZE];
  snprintf(netlink_kmsg, sizeof(netlink_kmsg), "Successfully received data packet! %d", netlink_count);
  logkm("kernel recv packet from user: %s\n", umsg);
  return send_netlink_message(netlink_kmsg);
}
static void netlink_rcv(struct sk_buff* skb) { netlink_rcv_skb(skb, &netlink_rcv_msg); }
// 创建 netlink 服务
static int start_rekernel_server(void) {
  if (rekernel_netlink_unit != UZERO)
    return 0;
  struct netlink_kernel_cfg rekernel_cfg = {
      .input = netlink_rcv,
  };

  for (rekernel_netlink_unit = NETLINK_REKERNEL_MAX; rekernel_netlink_unit >= NETLINK_REKERNEL_MIN;
       rekernel_netlink_unit--) {
    rekernel_netlink = netlink_kernel_create(kvar(init_net), rekernel_netlink_unit, &rekernel_cfg);
    if (rekernel_netlink != NULL)
      break;
  }
  if (rekernel_netlink == NULL) {
    rekernel_netlink_unit = UZERO;
    logkm("Failed to create Re:Kernel server!\n");
    return -ENOBUFS;
  }
  logkm("Created Re:Kernel server! NETLINK UNIT: %d\n", rekernel_netlink_unit);

  rekernel_dir = proc_mkdir("rekernel", NULL);
  if (!rekernel_dir) {
    logkm("create /proc/rekernel failed!\n");
  } else {
    char buff[32];
    sprintf(buff, "%d", rekernel_netlink_unit);
    rekernel_unit_entry = proc_create(buff, 0400, rekernel_dir, &rekernel_unit_fops);
    if (!rekernel_unit_entry) {
      logkm("create rekernel unit failed!\n");
    }
  }

  return 0;
}

static void rekernel_report(int reporttype, int type, pid_t src_pid, struct task_struct* src, pid_t dst_pid,
                            struct task_struct* dst, bool oneway) {
  if (start_rekernel_server() != 0)
    return;

#ifdef CONFIG_NETWORK
  if (reporttype == NETWORK) {
    char binder_kmsg[PACKET_SIZE];
    snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Network,target=%d,proto=ipv%d;", dst_pid, src_pid);
#ifdef CONFIG_DEBUG
    logkm("%s\n", binder_kmsg);
#endif /* CONFIG_DEBUG */
    send_netlink_message(binder_kmsg);
    return;
  }
#endif /* CONFIG_NETWORK */

  if (!frozen_task_group(dst))
    return;

  if (task_uid(src).val == task_uid(dst).val)
    return;

  char binder_kmsg[PACKET_SIZE];
  switch (reporttype) {
    case BINDER:
      if (oneway && type == TRANSACTION) {
        if (ext_tr_offset == UZERO)
          return;
        struct task_ext* ext = get_task_ext(current);
        struct binder_transaction_data* tr = *(void**)task_local_ptr(ext, ext_tr_offset);
        if (!tr)
          return;
        // 减少异步消息
        if (tr->code < 29 || tr->code > 32)
          return;

        size_t buf_data_size = tr->data_size > INTERFACETOKEN_BUFF_SIZE ? INTERFACETOKEN_BUFF_SIZE : tr->data_size;
        char* buf_data = memdup_user((char*)tr->data.ptr.buffer, buf_data_size);
        if (IS_ERR(buf_data))
          return;
        char buf[INTERFACETOKEN_BUFF_SIZE] = {0};
        int i = 0;
        int j = PARCEL_OFFSET + 1;
        char* p = buf_data + PARCEL_OFFSET;
        while (i < INTERFACETOKEN_BUFF_SIZE && j < buf_data_size && *p != '\0') {
          buf[i++] = *p;
          j += 2;
          p += 2;
        }
        kvfree(buf_data);
        if (i == INTERFACETOKEN_BUFF_SIZE) {
          buf[i - 1] = '\0';
        }
        snprintf(binder_kmsg, sizeof(binder_kmsg),
                 "type=Binder,bindertype=%s,oneway=%d,from_pid=%d,from=%d,target_pid=%d,target=%d,"
                 "rpc_name=%s,code=%d;",
                 binder_type[type], oneway, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val, buf, tr->code);
      } else {
        snprintf(binder_kmsg, sizeof(binder_kmsg),
                 "type=Binder,bindertype=%s,oneway=%d,from_pid=%d,from=%d,target_pid=%d,target=%d;", binder_type[type],
                 oneway, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val);
      }
      break;
    case SIGNAL:
      snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Signal,signal=%d,killer_pid=%d,killer=%d,dst_pid=%d,dst=%d;",
               type, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val);
      break;
    default:
      return;
  }
#ifdef CONFIG_DEBUG
  logkm("%s\n", binder_kmsg);
  logkm("src_comm=%s,dst_comm=%s\n", get_task_comm(src), get_task_comm(dst));
#endif /* CONFIG_DEBUG */
#ifdef CONFIG_DEBUG_CMDLINE
  char src_cmdline[PATH_MAX], dst_cmdline[PATH_MAX];
  memset(&src_cmdline, 0, PATH_MAX);
  memset(&dst_cmdline, 0, PATH_MAX);
  int res = 0;
  res = get_cmdline(src, src_cmdline, PATH_MAX - 1);
  src_cmdline[res] = '\0';
  res = get_cmdline(dst, dst_cmdline, PATH_MAX - 1);
  dst_cmdline[res] = '\0';
  logkm("src_cmdline=%s,dst_cmdline=%s\n", src_cmdline, dst_cmdline);
#endif /* CONFIG_DEBUG_CMDLINE */
  send_netlink_message(binder_kmsg);
}

static void binder_reply_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst,
                                 bool oneway) {
  if (unlikely(!dst))
    return;
  if (task_uid(dst).val > MAX_SYSTEM_UID || src_pid == dst_pid)
    return;

  // oneway=0
  rekernel_report(BINDER, REPLY, src_pid, src, dst_pid, dst, oneway);
}

static void binder_trans_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst,
                                 bool oneway) {
  if (unlikely(!dst))
    return;
  if ((task_uid(dst).val <= MIN_USERAPP_UID) || src_pid == dst_pid)
    return;

  rekernel_report(BINDER, TRANSACTION, src_pid, src, dst_pid, dst, oneway);
}

static void binder_overflow_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst,
                                    bool oneway) {
  if (unlikely(!dst))
    return;

  // oneway=1
  rekernel_report(BINDER, OVERFLOW, src_pid, src, dst_pid, dst, oneway);
}

static void rekernel_binder_transaction(void* data, bool reply, struct binder_transaction* t,
                                        struct binder_node* target_node) {
  struct binder_proc* to_proc = binder_transaction_to_proc(t);
  if (!to_proc)
    return;
  struct binder_thread* from = binder_transaction_from(t);

  if (reply) {
    binder_reply_handler(task_tgid_nr(current), current, to_proc->pid, to_proc->tsk, false);
  } else if (from) {
    if (from->proc) {
      binder_trans_handler(from->proc->pid, from->proc->tsk, to_proc->pid, to_proc->tsk, false);
    }
  } else {  // oneway=1
    binder_trans_handler(task_tgid_nr(current), current, to_proc->pid, to_proc->tsk, true);

    struct binder_alloc* target_alloc = binder_proc_alloc(to_proc);
    size_t free_async_space = binder_alloc_free_async_space(target_alloc);
    size_t buffer_size = binder_alloc_buffer_size(target_alloc);
    if (free_async_space < (buffer_size / 10 + 0x300)) {
      binder_overflow_handler(task_tgid_nr(current), current, to_proc->pid, to_proc->tsk, true);
    }
  }
}

static bool binder_can_update_transaction(struct binder_transaction* t1, struct binder_transaction* t2) {
  struct binder_proc* t1_to_proc = binder_transaction_to_proc(t1);
  struct binder_buffer* t1_buffer = binder_transaction_buffer(t1);
  unsigned int t1_code = binder_transaction_code(t1);
  unsigned int t1_flags = binder_transaction_flags(t1);
  binder_uintptr_t t1_ptr = binder_node_ptr(t1_buffer->target_node);
  binder_uintptr_t t1_cookie = binder_node_cookie(t1_buffer->target_node);

  struct binder_proc* t2_to_proc = binder_transaction_to_proc(t2);
  struct binder_buffer* t2_buffer = binder_transaction_buffer(t2);
  unsigned int t2_code = binder_transaction_code(t2);
  unsigned int t2_flags = binder_transaction_flags(t2);
  binder_uintptr_t t2_ptr = binder_node_ptr(t2_buffer->target_node);
  binder_uintptr_t t2_cookie = binder_node_cookie(t2_buffer->target_node);

  if ((t1_flags & t2_flags & TF_ONE_WAY) != TF_ONE_WAY || !t1_to_proc || !t2_to_proc)
    return false;
  if (t1_to_proc->tsk == t2_to_proc->tsk && t1_code == t2_code && t1_flags == t2_flags
      && (struct_offset.binder_proc_is_frozen > 0 ? t1_buffer->pid == t2_buffer->pid : true)  // 4.19 以下无此数据
      && t1_ptr == t2_ptr && t1_cookie == t2_cookie)
    return true;
  return false;
}

static struct binder_transaction* binder_find_outdated_transaction_ilocked(struct binder_transaction* t,
                                                                           struct list_head* target_list) {
  struct binder_work* w;
  bool second = false;

  list_for_each_entry(w, target_list, entry) {
    if (w->type != BINDER_WORK_TRANSACTION)
      continue;
    struct binder_transaction* t_queued = container_of(w, struct binder_transaction, work);
    if (binder_can_update_transaction(t_queued, t)) {
      if (second)
        return t_queued;
      else {
        second = true;
      }
    }
  }
  return NULL;
}

static inline void outstanding_txns_dec(struct binder_proc* proc) {
  if (struct_offset.binder_proc_outstanding_txns > 0) {
    int* outstanding_txns = binder_proc_outstanding_txns(proc);
    (*outstanding_txns)--;
  }
}

static inline void binder_release_entire_buffer(struct binder_proc* proc, struct binder_thread* thread,
                                                struct binder_buffer* buffer, bool is_failure) {
  if (binder_transaction_buffer_release_ver6 == IZERO) {
    binder_transaction_buffer_release_v6(proc, thread, buffer, 0, is_failure);
  } else if (binder_transaction_buffer_release_ver5 == IZERO) {
    binder_size_t off_end_offset = ALIGN(buffer->data_size, sizeof(void*));
    off_end_offset += buffer->offsets_size;

    binder_transaction_buffer_release(proc, thread, buffer, off_end_offset, is_failure);
  } else if (binder_transaction_buffer_release_ver4 == IZERO) {
    binder_transaction_buffer_release_v4(proc, buffer, 0, is_failure);
  } else {
    binder_transaction_buffer_release_v3(proc, buffer, NULL);
  }
}

static inline void binder_stats_deleted(enum binder_stat_types type) {
  atomic_t* binder_stats_deleted_addr =
      (atomic_t*)((uintptr_t)kvar(binder_stats) + struct_offset.binder_stats_deleted_transaction);
  atomic_inc(binder_stats_deleted_addr);
}

static void binder_proc_transaction_before(hook_fargs3_t* args, void* udata) {
  struct binder_transaction* t = (struct binder_transaction*)args->arg0;
  struct binder_proc* proc = (struct binder_proc*)args->arg1;

  struct binder_buffer* buffer = binder_transaction_buffer(t);
  struct binder_node* node = buffer->target_node;
  // 兼容不支持 trace 的内核
  if (trace == UZERO) {
    rekernel_binder_transaction(NULL, false, t, NULL);
  }
  unsigned int flags = binder_transaction_flags(t);
  if (!node || !(flags & TF_ONE_WAY))
    return;

  // binder 冻结时不再清理过时消息
  if (binder_is_frozen(proc) || !frozen_task_group(proc->tsk))
    return;

  binder_node_lock(node);
  bool has_async_transaction = binder_node_has_async_transaction(node);
  if (!has_async_transaction) {
    binder_node_unlock(node);
    return;
  }
  binder_inner_proc_lock(proc);

  struct list_head* async_todo = binder_node_async_todo(node);
  struct binder_transaction* t_outdated = binder_find_outdated_transaction_ilocked(t, async_todo);
  if (t_outdated) {
    list_del_init(&t_outdated->work.entry);
    outstanding_txns_dec(proc);
  }

  binder_inner_proc_unlock(proc);
  binder_node_unlock(node);

  if (t_outdated) {
    struct binder_alloc* target_alloc = binder_proc_alloc(proc);
    struct binder_buffer* buffer = binder_transaction_buffer(t_outdated);
#ifdef CONFIG_DEBUG
    logkm("free_outdated pid=%d,uid=%d,data_size=%d\n", proc->pid, task_uid(proc->tsk).val, buffer->data_size);
#endif /* CONFIG_DEBUG */

    *(struct binder_buffer**)((uintptr_t)t_outdated + struct_offset.binder_transaction_buffer) = NULL;
    buffer->transaction = NULL;
    binder_release_entire_buffer(proc, NULL, buffer, false);
    binder_alloc_free_buf(target_alloc, buffer);
    kfree(t_outdated);
    binder_stats_deleted(BINDER_STAT_TRANSACTION);
  }
}

static void binder_transaction_before(hook_fargs5_t* args, void* udata) {
  struct task_ext* ext = get_task_ext(current);
  if (!task_ext_valid(ext))
    return;
  if (ext_tr_offset == UZERO) {
    // reg_task_local 似乎有bug
    // ext_tr_offset = reg_task_local(sizeof(uint64_t));
    ext_tr_offset = task_ext_size + sizeof(uintptr_t);
    // 随缘兼容其他模块
    while (*(uintptr_t**)task_local_ptr(ext, ext_tr_offset)) {
      ext_tr_offset += sizeof(uintptr_t);
    }
  }
  *(uintptr_t*)task_local_ptr(ext, ext_tr_offset) = args->arg2;
}

static void do_send_sig_info_before(hook_fargs4_t* args, void* udata) {
  int sig = (int)args->arg0;
  struct task_struct* dst = (struct task_struct*)args->arg2;

  if (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT || sig == SIGQUIT) {
    rekernel_report(SIGNAL, sig, task_tgid_nr(current), current, task_tgid_nr(dst), dst, false);
  }
}

#ifdef CONFIG_NETWORK
static inline bool sk_fullsock(const struct sock* sk) {
  return (1 << sk->sk_state) & ~(TCPF_TIME_WAIT | TCPF_NEW_SYN_RECV);
}

static void tcp_rcv_before(hook_fargs1_t* args, void* udata) {
  struct sk_buff* skb = (struct sk_buff*)args->arg0;
  struct sock* sk = skb->sk;
  ;
  if (sk == NULL || !sk_fullsock(sk))
    return;

  uid_t uid = sock_i_uid(sk).val;
  if (uid < MIN_USERAPP_UID)
    return;

  int version = *(int*)udata;
  rekernel_report(NETWORK, 0, version, NULL, uid, NULL, true);
}
#endif /* CONFIG_NETWORK */

static long inline_hook_init(const char* args, const char* event, void* __user reserved) {
  lookup_name(cgroup_freezing);

  kfunc_lookup_name(__alloc_skb);
  kfunc_lookup_name(__nlmsg_put);
  kfunc_lookup_name(kfree_skb);
  kfunc_lookup_name(netlink_unicast);
  kfunc_lookup_name(netlink_rcv_skb);

  kvar_lookup_name(init_net);
  kfunc_lookup_name(__netlink_kernel_create);
  kfunc_lookup_name(netlink_kernel_release);

  kfunc_lookup_name(proc_mkdir);
  kfunc_lookup_name(proc_create_data);
  kfunc_lookup_name(proc_remove);

  kfunc_lookup_name(tracepoint_probe_register);
  kfunc_lookup_name(tracepoint_probe_unregister);

  kfunc_lookup_name(_raw_spin_lock);
  kfunc_lookup_name(_raw_spin_unlock);
  kvar_lookup_name(__tracepoint_binder_transaction);

  lookup_name(binder_transaction_buffer_release);
  binder_transaction_buffer_release_v6 =
      (typeof(binder_transaction_buffer_release_v6))binder_transaction_buffer_release;
  binder_transaction_buffer_release_v4 =
      (typeof(binder_transaction_buffer_release_v4))binder_transaction_buffer_release;
  binder_transaction_buffer_release_v3 =
      (typeof(binder_transaction_buffer_release_v3))binder_transaction_buffer_release;
  lookup_name(binder_alloc_free_buf);
  kfunc_lookup_name(kfree);
  kvar_lookup_name(binder_stats);
  kfunc_lookup_name(kvfree);
  kfunc_lookup_name(memdup_user);

  lookup_name(binder_proc_transaction);
  lookup_name(binder_transaction);
  lookup_name(do_send_sig_info);

#ifdef CONFIG_NETWORK
  kfunc_lookup_name(sock_i_uid);

  lookup_name(tcp_v4_rcv);
  lookup_name(tcp_v6_rcv);
#endif /* CONFIG_NETWORK */
#ifdef CONFIG_DEBUG_CMDLINE
  kfunc_lookup_name(get_cmdline);
#endif /* CONFIG_DEBUG_CMDLINE */

  int rc = 0;
  rc = calculate_offsets();
  if (rc < 0)
    return rc;

  rc = tracepoint_probe_register(kvar(__tracepoint_binder_transaction), rekernel_binder_transaction, NULL);
  if (rc == 0) {
    trace = IZERO;
  }

  hook_func(binder_proc_transaction, 3, binder_proc_transaction_before, NULL, NULL);
  hook_func(binder_transaction, 5, binder_transaction_before, NULL, NULL);
  hook_func(do_send_sig_info, 4, do_send_sig_info_before, NULL, NULL);

#ifdef CONFIG_NETWORK
  hook_func(tcp_v4_rcv, 1, tcp_rcv_before, NULL, &ipv4_version);
  hook_func(tcp_v6_rcv, 1, tcp_rcv_before, NULL, &ipv6_version);
#endif /* CONFIG_NETWORK */

  return 0;
}

static long inline_hook_control0(const char* ctl_args, char* __user out_msg, int outlen) {
  char msg[64];
  snprintf(msg, sizeof(msg), "_(._.)_");
  compat_copy_to_user(out_msg, msg, sizeof(msg));
  return 0;
}

static long inline_hook_exit(void* __user reserved) {
  if (rekernel_netlink) {
    netlink_kernel_release(rekernel_netlink);
  }
  if (rekernel_dir) {
    proc_remove(rekernel_dir);
  }

  tracepoint_probe_unregister(kvar(__tracepoint_binder_transaction), rekernel_binder_transaction, NULL);

  unhook_func(binder_proc_transaction);
  unhook_func(binder_transaction);
  unhook_func(do_send_sig_info);

#ifdef CONFIG_NETWORK
  unhook_func(tcp_v4_rcv);
  unhook_func(tcp_v6_rcv);
#endif /* CONFIG_NETWORK */

  return 0;
}

KPM_INIT(inline_hook_init);
KPM_CTL0(inline_hook_control0);
KPM_EXIT(inline_hook_exit);
