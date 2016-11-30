#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libelf.h>
#include <gelf.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <poll.h>
#include <ctype.h>
#include "libbpf.h"
#include "bpf_helpers.h"

#define DEBUGFS "/sys/kernel/debug/tracing/"

static char license[128];
static int kern_version;
static bool processed_sec[128];
int map_fd[32];
int prog_fd[32];
int event_fd[32];
int prog_cnt;
int prog_array_fd = -1;

static int load_and_attach(const char *event, struct bpf_insn *prog, int size)
{
	bool is_kprobe = strncmp(event, "kprobe/", 7) == 0;
	bool is_kretprobe = strncmp(event, "kretprobe/", 10) == 0;
	bool is_tracepoint = strncmp(event, "tracepoint/", 11) == 0;
	bool is_xdp = strncmp(event, "xdp", 3) == 0;
	bool is_perf_event = strncmp(event, "perf_event", 10) == 0;
	enum bpf_prog_type prog_type;
	char buf[256];
	int fd, efd, err, id;
	struct perf_event_attr attr = {};

	attr.type = PERF_TYPE_TRACEPOINT;
	attr.sample_type = PERF_SAMPLE_RAW;
	attr.sample_period = 1;
	attr.wakeup_events = 1;

	if (is_kprobe || is_kretprobe) {
		prog_type = BPF_PROG_TYPE_KPROBE;
	} else {
		printf("Unknown event '%s'\n", event);
		return -1;
	}

	fd = bpf_prog_load(prog_type, prog, size, license, kern_version);
	if (fd < 0 && errno == EPERM) {
		/*
		 * When EPERM is returned, two reasons are possible:
		 * 1. user has no permissions for bpf()
		 * 2. user has insufficent rlimit for locked memory
		 * Unfortunately, there is no api to inspect the current usage of locked
		 * mem for the user, so an accurate calculation of how much memory to lock
		 * for this new program is difficult to calculate. As a hack, bump the limit
		 * to unlimited. If program load fails again, return the error.
		 */
		printf("bpf_prog_load() err=%d\n%s\nset rlimit to RLIM_INFINITY and try again\n", errno, bpf_log_buf);
		struct rlimit rl = {};
		if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
			rl.rlim_max = RLIM_INFINITY;
			rl.rlim_cur = rl.rlim_max;
			if (setrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
				fd = bpf_prog_load(prog_type, prog, size, license, kern_version);
			}
			else {
				printf("setrlimit() failed with errno=%d\n", errno);
				return -1;
			}
		}
	}
	/* This checks the second try as well as the case fd < 0 && errno != EPERM */
	if (fd < 0) {
		printf("bpf_prog_load() err=%d\n%s", errno, bpf_log_buf);
		return -1;
	}

	prog_fd[prog_cnt++] = fd;

	if (is_xdp || is_perf_event)
		return 0;

	if (is_kprobe || is_kretprobe) {
		if (is_kprobe)
			event += 7;
		else
			event += 10;

		if (*event == 0) {
			printf("event name cannot be empty\n");
			return -1;
		}

		snprintf(buf, sizeof(buf),
			 "echo '%c:%c%s %s' >> /sys/kernel/debug/tracing/kprobe_events",
			 is_kprobe ? 'p' : 'r',
			 is_kprobe ? 'p' : 'r',
			 event, event);
		err = system(buf);
		if (err < 0) {
			printf("failed to create kprobe '%s' error '%s'\n",
			       event, strerror(errno));
			return -1;
		}

		strcpy(buf, DEBUGFS);
		strcat(buf, "events/kprobes/");
		if (is_kprobe)
			strcat(buf, "p");
		else
			strcat(buf, "r");
		strcat(buf, event);
		strcat(buf, "/id");
	} else if (is_tracepoint) {
		event += 11;

		if (*event == 0) {
			printf("event name cannot be empty\n");
			return -1;
		}
		strcpy(buf, DEBUGFS);
		strcat(buf, "events/");
		strcat(buf, event);
		strcat(buf, "/id");
	}

	efd = open(buf, O_RDONLY, 0);
	if (efd < 0) {
		printf("failed to open event %s\n", event);
		return -1;
	}

	err = read(efd, buf, sizeof(buf));
	if (err < 0 || err >= sizeof(buf)) {
		printf("read from '%s' failed '%s'\n", event, strerror(errno));
		return -1;
	}

	close(efd);

	buf[err] = 0;
	id = atoi(buf);
	attr.config = id;

	efd = perf_event_open(&attr, -1/*pid*/, 0/*cpu*/, -1/*group_fd*/, 0);
	if (efd < 0) {
		printf("event %d fd %d err %s\n", id, efd, strerror(errno));
		return -1;
	}
	event_fd[prog_cnt - 1] = efd;
	ioctl(efd, PERF_EVENT_IOC_ENABLE, 0);
	ioctl(efd, PERF_EVENT_IOC_SET_BPF, fd);

	return 0;
}

static int load_maps(struct bpf_map_def *maps, int len)
{
	int i;

	for (i = 0; i < len / sizeof(struct bpf_map_def); i++) {

		map_fd[i] = bpf_create_map(maps[i].type,
					   maps[i].key_size,
					   maps[i].value_size,
					   maps[i].max_entries,
					   maps[i].map_flags);
		if (map_fd[i] < 0) {
			printf("failed to create a map: %d %s\n",
			       errno, strerror(errno));
			return 1;
		}

		if (maps[i].type == BPF_MAP_TYPE_PROG_ARRAY)
			prog_array_fd = map_fd[i];
	}
	return 0;
}

static int get_sec(Elf *elf, int i, GElf_Ehdr *ehdr, char **shname,
		   GElf_Shdr *shdr, Elf_Data **data)
{
	Elf_Scn *scn;

	scn = elf_getscn(elf, i);
	if (!scn)
		return 1;

	if (gelf_getshdr(scn, shdr) != shdr)
		return 2;

	*shname = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
	if (!*shname || !shdr->sh_size)
		return 3;

	*data = elf_getdata(scn, 0);
	if (!*data || elf_getdata(scn, *data) != NULL)
		return 4;

	return 0;
}

static int parse_relo_and_apply(Elf_Data *data, Elf_Data *symbols,
				GElf_Shdr *shdr, struct bpf_insn *insn)
{
	int i, nrels;

	nrels = shdr->sh_size / shdr->sh_entsize;

	for (i = 0; i < nrels; i++) {
		GElf_Sym sym;
		GElf_Rel rel;
		unsigned int insn_idx;

		gelf_getrel(data, i, &rel);

		insn_idx = rel.r_offset / sizeof(struct bpf_insn);

		gelf_getsym(symbols, GELF_R_SYM(rel.r_info), &sym);

		if (insn[insn_idx].code != (BPF_LD | BPF_IMM | BPF_DW)) {
			printf("invalid relo for insn[%d].code 0x%x\n",
			       insn_idx, insn[insn_idx].code);
			return 1;
		}
		insn[insn_idx].src_reg = BPF_PSEUDO_MAP_FD;
		insn[insn_idx].imm = map_fd[sym.st_value / sizeof(struct bpf_map_def)];
	}

	return 0;
}

int load_bpf_file(char *path)
{
	int fd, i;
	Elf *elf;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr, shdr_prog;
	Elf_Data *data, *data_prog, *symbols = NULL;
	char *shname, *shname_prog;

	if (elf_version(EV_CURRENT) == EV_NONE)
		return 1;

	fd = open(path, O_RDONLY, 0);
	if (fd < 0)
		return 1;

	elf = elf_begin(fd, ELF_C_READ, NULL);

	if (!elf)
		return 1;

	if (gelf_getehdr(elf, &ehdr) != &ehdr)
		return 1;

	/* clear all kprobes */
	i = system("echo \"\" > /sys/kernel/debug/tracing/kprobe_events");

	/* scan over all elf sections to get license and map info */
	for (i = 1; i < ehdr.e_shnum; i++) {

		if (get_sec(elf, i, &ehdr, &shname, &shdr, &data))
			continue;

		if (0) /* helpful for llvm debugging */
			printf("section %d:%s data %p size %zd link %d flags %d\n",
			       i, shname, data->d_buf, data->d_size,
			       shdr.sh_link, (int) shdr.sh_flags);

		if (strcmp(shname, "license") == 0) {
			processed_sec[i] = true;
			memcpy(license, data->d_buf, data->d_size);
		} else if (strcmp(shname, "version") == 0) {
			processed_sec[i] = true;
			if (data->d_size != sizeof(int)) {
				printf("invalid size of version section %zd\n",
				       data->d_size);
				return 1;
			}
			memcpy(&kern_version, data->d_buf, sizeof(int));
		} else if (strcmp(shname, "maps") == 0) {
			processed_sec[i] = true;
			if (load_maps(data->d_buf, data->d_size))
				return 1;
		} else if (shdr.sh_type == SHT_SYMTAB) {
			symbols = data;
		}
	}

	/* load programs that need map fixup (relocations) */
	for (i = 1; i < ehdr.e_shnum; i++) {

		if (get_sec(elf, i, &ehdr, &shname, &shdr, &data))
			continue;
		if (shdr.sh_type == SHT_REL) {
			struct bpf_insn *insns;

			if (get_sec(elf, shdr.sh_info, &ehdr, &shname_prog,
				    &shdr_prog, &data_prog))
				continue;

			insns = (struct bpf_insn *) data_prog->d_buf;

			processed_sec[shdr.sh_info] = true;
			processed_sec[i] = true;

			if (parse_relo_and_apply(data, symbols, &shdr, insns))
				continue;

			if (memcmp(shname_prog, "kprobe/", 7) == 0 ||
			    memcmp(shname_prog, "kretprobe/", 10) == 0 ||
			    memcmp(shname_prog, "tracepoint/", 11) == 0 ||
			    memcmp(shname_prog, "xdp", 3) == 0 ||
			    memcmp(shname_prog, "perf_event", 10) == 0 ||
			    memcmp(shname_prog, "socket", 6) == 0)
				load_and_attach(shname_prog, insns, data_prog->d_size);
		}
	}

	/* load programs that don't use maps */
	for (i = 1; i < ehdr.e_shnum; i++) {

		if (processed_sec[i])
			continue;

		if (get_sec(elf, i, &ehdr, &shname, &shdr, &data))
			continue;

		if (memcmp(shname, "kprobe/", 7) == 0 ||
		    memcmp(shname, "kretprobe/", 10) == 0 ||
		    memcmp(shname, "tracepoint/", 11) == 0 ||
		    memcmp(shname, "xdp", 3) == 0 ||
		    memcmp(shname, "perf_event", 10) == 0 ||
		    memcmp(shname, "socket", 6) == 0)
			load_and_attach(shname, data->d_buf, data->d_size);
	}

	close(fd);
	return 0;
}
