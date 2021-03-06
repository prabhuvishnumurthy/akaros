#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <parlib/arch/arch.h>
#include <parlib/ros_debug.h>
#include <unistd.h>
#include <gelf.h>
#include <errno.h>
#include <libelf.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <ros/syscall.h>
#include <sys/mman.h>
#include <vmm/vmm.h>
#include <vmm/acpi/acpi.h>
#include <ros/arch/mmu.h>
#include <ros/arch/membar.h>
#include <ros/vmm.h>
#include <parlib/uthread.h>
#include <vmm/linux_bootparam.h>
#include <getopt.h>

#include <vmm/virtio.h>
#include <vmm/virtio_blk.h>
#include <vmm/virtio_mmio.h>
#include <vmm/virtio_ids.h>
#include <vmm/virtio_config.h>
#include <vmm/virtio_console.h>
#include <vmm/virtio_net.h>
#include <vmm/virtio_lguest_console.h>

#include <vmm/sched.h>
#include <vmm/net.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <parlib/opts.h>

struct virtual_machine local_vm, *vm = &local_vm;
struct vmm_gpcore_init *gpcis;

void vapic_status_dump(FILE *f, void *vapic);

#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 1)
#error "Get a gcc newer than 4.4.0"
#else
#define BITOP_ADDR(x) "+m" (*(volatile long *) (x))
#endif

void *timer_thread(void *arg)
{
	uint8_t vector;
	uint32_t initial_count;
	while (1) {
		for (int i = 0; i < vm->nr_gpcs; i++) {
			vector = ((uint32_t *)gpcis[i].vapic_addr)[0x32] & 0xff;
			initial_count = ((uint32_t *)gpcis[i].vapic_addr)[0x38];
			if (vector && initial_count)
				vmm_interrupt_guest(vm, i, vector);
		}
		uthread_usleep(1000);
	}
	fprintf(stderr, "SENDING TIMER\n");
	return 0;
}


static void virtio_poke_guest(uint8_t vec, uint32_t dest)
{
	if (dest < vm->nr_gpcs) {
		vmm_interrupt_guest(vm, dest, vec);
		return;
	}
	if (dest != 0xffffffff)
		panic("INVALID DESTINATION: 0x%02x\n", dest);

	for (int i = 0; i < vm->nr_gpcs; i++)
		vmm_interrupt_guest(vm, i, vec);
}

static struct virtio_mmio_dev cons_mmio_dev = {
	.poke_guest = virtio_poke_guest,
};

static struct virtio_console_config cons_cfg;
static struct virtio_console_config cons_cfg_d;

static struct virtio_vq_dev cons_vqdev = {
	.name = "console",
	.dev_id = VIRTIO_ID_CONSOLE,
	.dev_feat =
	(1ULL << VIRTIO_F_VERSION_1) | (1 << VIRTIO_RING_F_INDIRECT_DESC),
	.num_vqs = 2,
	.cfg = &cons_cfg,
	.cfg_d = &cons_cfg_d,
	.cfg_sz = sizeof(struct virtio_console_config),
	.transport_dev = &cons_mmio_dev,
	.vqs = {
		{
			.name = "cons_receiveq",
			.qnum_max = 64,
			.srv_fn = cons_receiveq_fn,
			.vqdev = &cons_vqdev
		},
		{
			.name = "cons_transmitq",
			.qnum_max = 64,
			.srv_fn = cons_transmitq_fn,
			.vqdev = &cons_vqdev
		},
	}
};

static struct virtio_mmio_dev net_mmio_dev = {
	.poke_guest = virtio_poke_guest,
};

static struct virtio_net_config net_cfg = {
	.max_virtqueue_pairs = 1
};
static struct virtio_net_config net_cfg_d = {
	.max_virtqueue_pairs = 1
};

static struct virtio_vq_dev net_vqdev = {
	.name = "network",
	.dev_id = VIRTIO_ID_NET,
	.dev_feat = (1ULL << VIRTIO_F_VERSION_1 | 1 << VIRTIO_NET_F_MAC),

	.num_vqs = 2,
	.cfg = &net_cfg,
	.cfg_d = &net_cfg_d,
	.cfg_sz = sizeof(struct virtio_net_config),
	.transport_dev = &net_mmio_dev,
	.vqs = {
		{
			.name = "net_receiveq",
			.qnum_max = 64,
			.srv_fn = net_receiveq_fn,
			.vqdev = &net_vqdev
		},
		{
			.name = "net_transmitq",
			.qnum_max = 64,
			.srv_fn = net_transmitq_fn,
			.vqdev = &net_vqdev
		},
	}
};

static struct virtio_mmio_dev blk_mmio_dev = {
	.poke_guest = virtio_poke_guest,
};

static struct virtio_blk_config blk_cfg = {
};

static struct virtio_blk_config blk_cfg_d = {
};

static struct virtio_vq_dev blk_vqdev = {
	.name = "block",
	.dev_id = VIRTIO_ID_BLOCK,
	.dev_feat = (1ULL << VIRTIO_F_VERSION_1),

	.num_vqs = 1,
	.cfg = &blk_cfg,
	.cfg_d = &blk_cfg_d,
	.cfg_sz = sizeof(struct virtio_blk_config),
	.transport_dev = &blk_mmio_dev,
	.vqs = {
		{
			.name = "blk_request",
			.qnum_max = 64,
			.srv_fn = blk_request,
			.vqdev = &blk_vqdev
		},
	}
};

/* Parse func: given a line of text, it sets any vnet options */
static void __parse_vnet_opts(char *_line)
{
	char *eq, *spc;

	/* Check all bools first */
	if (!strcmp(_line, "snoop")) {
		vnet_snoop = TRUE;
		return;
	}
	if (!strcmp(_line, "map_diagnostics")) {
		vnet_map_diagnostics = TRUE;
		return;
	}
	if (!strcmp(_line, "real_address")) {
		vnet_real_ip_addrs = TRUE;
		return;
	}
	/* Numeric fields, must have an = */
	eq = strchr(_line, '=');
	if (!eq)
		return;
	*eq++ = 0;
	/* Drop spaces before =.  atoi trims any spaces after =. */
	while ((spc = strrchr(_line, ' ')))
		*spc = 0;
	if (!strcmp(_line, "nat_timeout")) {
		vnet_nat_timeout = atoi(eq);
		return;
	}
}

static void set_vnet_opts(char *net_opts)
{
	if (parse_opts_file(net_opts, __parse_vnet_opts))
		perror("parse opts file");
}

/* Parse func: given a line of text, it builds any vnet port forwardings. */
static void __parse_vnet_port_fwds(char *_line)
{
	char *tok, *tok_save = 0;
	char *proto, *host_port, *guest_port;

	tok = strtok_r(_line, ":", &tok_save);
	if (!tok)
		return;
	if (strcmp(tok, "port"))
		return;
	tok = strtok_r(NULL, ":", &tok_save);
	if (!tok) {
		fprintf(stderr, "%s, port with no proto!", __func__);
		return;
	}
	proto = tok;
	tok = strtok_r(NULL, ":", &tok_save);
	if (!tok) {
		fprintf(stderr, "%s, port with no host port!", __func__);
		return;
	}
	host_port = tok;
	tok = strtok_r(NULL, ":", &tok_save);
	if (!tok) {
		fprintf(stderr, "%s, port with no guest port!", __func__);
		return;
	}
	guest_port = tok;
	vnet_port_forward(proto, host_port, guest_port);
}

static void set_vnet_port_fwds(char *net_opts)
{
	if (parse_opts_file(net_opts, __parse_vnet_port_fwds))
		perror("parse opts file");
}

/* We map the APIC-access page, the per core Virtual APIC page and the
 * per core Posted Interrupt Descriptors.
 * Note: check if the PID/PIR needs to be a 4k page. */
void alloc_intr_pages(void)
{
	void *a_page;
	void *pages, *pir;

	a_page = mmap((void *)APIC_GPA, PGSIZE, PROT_READ | PROT_WRITE,
	              MAP_POPULATE | MAP_ANONYMOUS, -1, 0);
	fprintf(stderr, "a_page mmap pointer %p\n", a_page);

	if (a_page != (void *)APIC_GPA) {
		perror("Could not mmap APIC");
		exit(1);
	}
	/* The VM should never actually read from this page. */
	for (int i = 0; i < PGSIZE/4; i++)
		((uint32_t *)a_page)[i] = 0xDEADBEEF;

	/* Allocate VAPIC and PIR pages. */
	pages = mmap((void*)0, vm->nr_gpcs * 2 * PGSIZE, PROT_READ | PROT_WRITE,
	             MAP_POPULATE | MAP_ANONYMOUS, -1, 0);
	if (pages == MAP_FAILED) {
		perror("Unable to map VAPIC and PIR pages.");
		exit(1);
	}

	/* We use the first vm->nr_gpcs pages for the VAPIC, and the second set
	 * for the PIRs. Each VAPIC and PIR gets its own 4k page. */
	pir = pages + (vm->nr_gpcs * PGSIZE);

	/* Set the addresses in the gpcis. */
	for (int i = 0; i < vm->nr_gpcs; i++) {
		gpcis[i].posted_irq_desc = pir + (PGSIZE * i);
		gpcis[i].vapic_addr = pages + (PGSIZE * i);
		gpcis[i].apic_addr = a_page;

		/* Set APIC ID. */
		((uint32_t *)gpcis[i].vapic_addr)[0x20/4] = i;
		/* Set APIC VERSION. */
		((uint32_t *)gpcis[i].vapic_addr)[0x30/4] = 0x01060015;
		/* Set LOGICAL APIC ID. */
		((uint32_t *)gpcis[i].vapic_addr)[0xD0/4] = 1 << i;
	}
}

int main(int argc, char **argv)
{
	void *cr3;
	int debug = 0;
	unsigned long long memsize = GiB;
	uintptr_t memstart = MinMemory;
	uintptr_t stack;
	struct boot_params *bp;
	char cmdline_default[512] = {0};
	char *cmdline_extra = "\0";
	char *cmdline;
	void *a = (void *)0xe0000;
	int vmmflags = 0; // Disabled probably forever. VMM_VMCALL_PRINTF;
	uint64_t entry = 0;
	int ret;
	struct vm_trapframe *vm_tf;
	uint64_t tsc_freq_khz;
	char *cmdlinep;
	int cmdlinesz, len, cmdline_fd;
	char *disk_image_file = NULL;
	int c;
	struct stat stat_result;
	int num_read;
	int option_index;
	char *smbiostable = NULL;
	char *net_opts = NULL;
	uint64_t num_pcs = 1;

	static struct option long_options[] = {
		{"debug",         no_argument,       0, 'd'},
		{"vmm_vmcall",    no_argument,       0, 'v'},
		{"maxresume",     required_argument, 0, 'R'},
		{"memsize",       required_argument, 0, 'm'},
		{"memstart",      required_argument, 0, 'M'},
		{"stack",         required_argument, 0, 'S'},
		{"cmdline_extra", required_argument, 0, 'c'},
		{"greedy",        no_argument,       0, 'g'},
		{"scp",           no_argument,       0, 's'},
		{"image_file",    required_argument, 0, 'f'},
		{"cmdline",       required_argument, 0, 'k'},
		{"net",           required_argument, 0, 'n'},
		{"num_cores",     required_argument, 0, 'N'},
		{"smbiostable",   required_argument, 0, 't'},
		{"help",          no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	fprintf(stderr, "%p %p %p %p\n", PGSIZE, PGSHIFT, PML1_SHIFT,
	        PML1_PTE_REACH);

	if ((uintptr_t)__procinfo.program_end >= MinMemory) {
		fprintf(stderr,
		        "Panic: vmrunkernel binary extends into guest memory\n");
		exit(1);
	}

	vm->low4k = malloc(PGSIZE);
	memset(vm->low4k, 0xff, PGSIZE);
	vm->low4k[0x40e] = 0;
	vm->low4k[0x40f] = 0;
	// Why is this here? Because the static initializer is getting
	// set to 1.  Yes, 1. This might be part of the weirdness
	// Barrett is reporting with linker sets. So let's leave it
	// here until we trust our toolchain.
	if (memsize != GiB)
		fprintf(stderr, "static initializers are broken\n");
	memsize = GiB;

	while ((c = getopt_long(argc, argv, "dvm:M:S:c:gsf:k:N:n:t:hR:",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'd':
			debug++;
			break;
		case 'v':
			vmmflags |= VMM_VMCALL_PRINTF;
			break;
		case 'm':
			memsize = strtoull(optarg, 0, 0);
			break;
		case 'M':
			memstart = strtoull(optarg, 0, 0);
			break;
		case 'S':
			stack = strtoull(optarg, 0, 0);
			break;
		case 'c':
			cmdline_extra = optarg;
		case 'g':	/* greedy */
			parlib_never_yield = TRUE;
			break;
		case 's':	/* scp */
			parlib_wants_to_be_mcp = FALSE;
			break;
		case 'f':	/* file to pass to blk_init */
			disk_image_file = optarg;
			break;
		case 'k':	/* specify file to get cmdline args from */
			cmdline_fd = open(optarg, O_RDONLY);
			if (cmdline_fd < 0) {
				fprintf(stderr, "failed to open file: %s\n", optarg);
				exit(1);
			}
			if (stat(optarg, &stat_result) == -1) {
				fprintf(stderr, "stat of %s failed\n", optarg);
				exit(1);
			}
			len = stat_result.st_size;
			if (len > 512) {
				fprintf(stderr, "command line options exceed 512 bytes!");
				exit(1);
			}
			num_read = read(cmdline_fd, cmdline_default, len);
			if (num_read != len) {
				fprintf(stderr, "read failed len was : %d, num_read was: %d\n",
				        len, num_read);
				exit(1);
			}
			close(cmdline_fd);
			break;
		case 't':
			smbiostable = optarg;
			break;
		case 'n':
			net_opts = optarg;
			break;
		case 'N':
			num_pcs = strtoull(optarg, 0, 0);
			break;
		case 'h':
		default:
			// Sadly, the getopt_long struct does
			// not have a pointer to help text.
			for (int i = 0;
			     i < sizeof(long_options)/sizeof(long_options[0]) - 1;
			     i++) {
				struct option *l = &long_options[i];

				fprintf(stderr, "%s or %c%s\n", l->name, l->val,
				        l->has_arg ? " <arg>" : "");
			}
			exit(0);
		}
	}

	if (strlen(cmdline_default) == 0) {
		fprintf(stderr, "WARNING: No command line parameter file specified.\n");
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		fprintf(stderr, "Usage: %s vmimage [-n (no vmcall printf)]\n", argv[0]);
		exit(1);
	}

	// Set vm->nr_gpcs before it's referenced in the struct setups below.
	vm->nr_gpcs = num_pcs;
	fprintf(stderr, "NUM PCS: %d\n", num_pcs);
	gpcis = (struct vmm_gpcore_init *)
	                malloc(num_pcs * sizeof(struct vmm_gpcore_init));
	vm->gpcis = gpcis;

	alloc_intr_pages();

	if ((uintptr_t)(memstart + memsize) >= (uintptr_t)BRK_START) {
		fprintf(stderr,
		        "memstart 0x%llx memsize 0x%llx -> 0x%llx is too large; overlaps BRK_START at %p\n",
		        memstart, memsize, memstart + memsize, BRK_START);
		exit(1);
	}

	mmap_memory(memstart, memsize);

	entry = load_elf(argv[0]);
	if (entry == 0) {
		fprintf(stderr, "Unable to load kernel %s\n", argv[0]);
		exit(1);
	}

	a = setup_biostables(vm, a, smbiostable);

	bp = a;
	a = init_e820map(bp, memstart, memsize);

	/* The MMIO address of the console device is really the address of an
	 * unbacked EPT page: accesses to this page will cause a page fault that
	 * traps to the host, which will examine the fault, see it was for the
	 * known MMIO address, and fulfill the MMIO read or write on the guest's
	 * behalf accordingly. We place the virtio space at 512 GB higher than the
	 * guest physical memory to avoid a full page table walk. */
	uint64_t virtio_mmio_base_addr;

	virtio_mmio_base_addr = ROUNDUP((bp->e820_map[bp->e820_entries - 1].addr +
	                                 bp->e820_map[bp->e820_entries - 1].size),
	                                 512ULL * GiB);
	cons_mmio_dev.addr =
		virtio_mmio_base_addr + PGSIZE * VIRTIO_MMIO_CONSOLE_DEV;
	cons_mmio_dev.vqdev = &cons_vqdev;
	vm->virtio_mmio_devices[VIRTIO_MMIO_CONSOLE_DEV] = &cons_mmio_dev;

	net_mmio_dev.addr =
		virtio_mmio_base_addr + PGSIZE * VIRTIO_MMIO_NETWORK_DEV;
	net_mmio_dev.vqdev = &net_vqdev;
	vm->virtio_mmio_devices[VIRTIO_MMIO_NETWORK_DEV] = &net_mmio_dev;

	if (disk_image_file != NULL) {
		blk_mmio_dev.addr =
			virtio_mmio_base_addr + PGSIZE * VIRTIO_MMIO_BLOCK_DEV;
		blk_mmio_dev.vqdev = &blk_vqdev;
		vm->virtio_mmio_devices[VIRTIO_MMIO_BLOCK_DEV] = &blk_mmio_dev;
		blk_init_fn(&blk_vqdev, disk_image_file);
	}

	set_vnet_opts(net_opts);
	vnet_init(vm, &net_vqdev);
	set_vnet_port_fwds(net_opts);

	/* Set the kernel command line parameters */
	a += 4096;
	cmdline = a;
	a += 4096;

	bp->hdr.cmd_line_ptr = (uintptr_t) cmdline;

	tsc_freq_khz = get_tsc_freq()/1000;
	len = snprintf(cmdline, 4096, "%s tscfreq=%lld %s", cmdline_default,
	               tsc_freq_khz, cmdline_extra);

	cmdlinesz = 4096 - len;
	cmdlinep = cmdline + len;

	for (int i = 0; i < VIRTIO_MMIO_MAX_NUM_DEV; i++) {
		if (vm->virtio_mmio_devices[i] == NULL)
			continue;

		/* Append all the virtio mmio base addresses. */

		/* Since the lower number irqs are no longer being used, the irqs
		 * can now be assigned starting from 0.
		 */
		vm->virtio_mmio_devices[i]->irq = i;
		len = snprintf(cmdlinep, cmdlinesz,
		               "\n virtio_mmio.device=1K@0x%llx:%lld",
		               vm->virtio_mmio_devices[i]->addr,
		               vm->virtio_mmio_devices[i]->irq);
		if (len >= cmdlinesz) {
			fprintf(stderr, "Too many arguments to the linux command line.");
			exit(1);
		}
		cmdlinesz -= len;
		cmdlinep += len;
	}

	/* Set maxcpus to the number of cores we're giving the guest. */
	len = snprintf(cmdlinep, cmdlinesz,
	               "\n maxcpus=%lld", vm->nr_gpcs);
	if (len >= cmdlinesz) {
		fprintf(stderr, "Too many arguments to the linux command line.");
		exit(1);
	}
	cmdlinesz -= len;
	cmdlinep += len;

	ret = vmm_init(vm, vmmflags);
	assert(!ret);

	cr3 = setup_paging(memstart, memsize, debug);
	vmm_run_task(vm, timer_thread, 0);

	vm_tf = gth_to_vmtf(vm->gths[0]);
	vm_tf->tf_cr3 = (uint64_t) cr3;
	vm_tf->tf_rip = entry;
	vm_tf->tf_rsp = stack;
	vm_tf->tf_rsi = (uint64_t) bp;
	vm->up_gpcs = 1;
	fprintf(stderr, "Start guest: cr3 %p rip %p stack %p\n",
	        cr3, entry, stack);
	start_guest_thread(vm->gths[0]);

	uthread_sleep_forever();
	return 0;
}
