// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>
#include "pmap.h"
#include "env.h"

#define CMDBUF_SIZE    80    // enough for one VGA text line


struct Command
{
    const char *name;
    const char *desc;

    // return -1 to force monitor to exit
    int (*func)(int argc, char **argv, struct Trapframe *tf);
};

static struct Command commands[] = {
        {"help",         "Display this list of commands",   mon_help},
        {"kerninfo",     "Display information about the kernel",
                                                            mon_kerninfo},
        {"backtrace",    "Display stack backtrace",         mon_backtrace},
        {"shutdown",     "Shutdown the kernel",             mon_shutdown},
        {"restart",      "Restart the kernel",              mon_restart},
        {"showmappings", "Show memory mappings",            mon_showmappings},
        {"setperm",      "Set permission for memory mappings",
                                                            mon_setperm},
        {"dump",         "Dump a range of memory",          mon_dump},
        {"c",            "Continue the execution (for debug)",
                                                            mon_continue},
        {"s",            "Continue the execution by step (for debug)",
                                                            mon_step},
};


/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(commands); i++)
        cprintf("%s - %s\n", commands[i].name, commands[i].desc);
    return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
    extern char _start[], entry[], etext[], edata[], end[];

    cprintf("Special kernel symbols:\n");
    cprintf("  _start                  %08x (phys)\n", _start);
    cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
    cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
    cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
    cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
    cprintf("Kernel executable memory footprint: %dKB\n",
            ROUNDUP(end - entry, 1024) / 1024);
    return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
    register uint32_t *ebp = (uint32_t *) read_ebp();
    cprintf("Stack backtrace:\n");

    register bool user_flag = false;

    cprintf("[INFO] curenv %08x\n", curenv->env_id);

    while (ebp)
    {
        cprintf("  ebp %x  eip %x  args %08x %08x %08x %08x %08x\n",
                ebp,
                *(ebp + 1),
                *(ebp + 2),
                *(ebp + 3),
                *(ebp + 4),
                *(ebp + 5),
                *(ebp + 6)
        );

        uint32_t eip = *(ebp + 1);
        struct Eipdebuginfo debuginfo;
        debuginfo_eip(eip, &debuginfo);
        cprintf("         %s:%d: %.*s+%d\n",
                debuginfo.eip_file,
                debuginfo.eip_line,
                debuginfo.eip_fn_namelen, debuginfo.eip_fn_name,
                eip - debuginfo.eip_fn_addr);

        ebp = (uint32_t *) *ebp;

        if (curenv != NULL && !user_flag && (uint32_t)ebp < KSTACKTOP - PTSIZE)
        {
            cprintf("  ebp %x  (user env)\n", ebp);
            break;
//            user_flag = true;
//            lcr3(PADDR(curenv->env_pgdir));
        }
    }
//    if (user_flag)
//        lcr3(PADDR(kern_pgdir));

    return 0;
}

int mon_shutdown(int argc, char **argv, struct Trapframe *tf)
{
    asm volatile ("cli");

    // (phony) ACPI shutdown (http://forum.osdev.org/viewtopic.php?t=16990)
    // Works for qemu and bochs.
    outw (0xB004, 0x0 | 0x2000);

    asm volatile ("int3");
    return 0;
}

int mon_restart(int argc, char **argv, struct Trapframe *tf)
{
    outb(0x64, 0xFE);

//     Should never get here;
    panic("Restart failed!");
}

static int __mon_showmappings3(uintptr_t start, uintptr_t end)
{
    pte_t* pte_ptr;

    cprintf("   START         END        PHYS    PERM\n");

    while (start < end)
    {
        cprintf("0x%08x - 0x%08x: ",start, start + PGSIZE);

        pte_ptr = pgdir_walk(kern_pgdir, (void *)start, 0);
        if (pte_ptr == NULL)
            cprintf("Not mapped ----\n");
        else
        {
            pte_t pte = *pte_ptr;

            cprintf("0x%08x ", PTE_ADDR(pte));
            if (pte & PTE_U)
                cprintf("U");
            else
                cprintf("-");
            cprintf("R");
            if (pte & PTE_W)
                cprintf("W");
            else
                cprintf("-");
            if (pte & PTE_P)
                cprintf("P");
            else
                cprintf("-");

            cprintf("\n");
        }
        start += PGSIZE;
    }

    return 0;
}

int mon_showmappings(int argc, char **argv, struct Trapframe *tf)
{
    uintptr_t start, end;
    if (argc == 2 || argc == 3)
    {
        start = strtol(argv[1], NULL, 0);
        end = (argc == 2) ? start + PGSIZE : strtol(argv[2], NULL, 0);

        if (start != ROUNDUP(start, PGSIZE)
            || end != ROUNDUP(end, PGSIZE)
            || start >= end)
        {
            cprintf("Invalid address!\n");
            return 0;
        }
        return __mon_showmappings3(start, end);
    }
    else
    {
        cprintf("Usage: showmappings START_ADDR [END_ADDR]\n\n");
        cprintf("Both addresses should be properly aligned.\n");
        cprintf("You can omit END_ADDR to show only one page.\n");
        return 0;
    }
}

static int __mon_setperm4(uintptr_t start, uintptr_t end, const char *perm)
{
    pte_t* pte_ptr;

    while (start < end)
    {
        pte_ptr = pgdir_walk(kern_pgdir, (void *)start, 0);
        if (pte_ptr == NULL)
            cprintf("Address 0x%08x not mapped! Skipping...\n", start);
        else
        {
            if (perm[0] == 'U')
                *pte_ptr |= PTE_U;
            else
                *pte_ptr &= (~PTE_U);

            if (perm[1] == 'W')
                *pte_ptr |= PTE_W;
            else
                *pte_ptr &= (~PTE_W);
        }
        start += PGSIZE;
    }

    return 0;
}

int mon_setperm(int argc, char **argv, struct Trapframe *tf)
{
    if (argc != 3 && argc != 4)
    {
        cprintf("Usage: setperm PERM START_ADDR [END_ADDR]\n\n");
        cprintf("Both addresses should be properly aligned.\n");
        cprintf("You can omit END_ADDR to set for only one page.\n");
        cprintf("PERM should be one of --, -W, U-, UW.\n");
        return 0;
    }

    uintptr_t start, end;
    start = strtol(argv[2], NULL, 0);
    end = (argc == 3) ? start + PGSIZE : strtol(argv[3], NULL, 0);

    if (start != ROUNDUP(start, PGSIZE)
        || end != ROUNDUP(end, PGSIZE)
        || start >= end)
    {
        cprintf("Invalid address!\n");
        return 0;
    }

    char *perm = argv[1];
    if ((perm[0] != '-' && perm[0] != 'U')
        || (perm[1] != '-' && perm[1] != 'W'))
    {
        cprintf("Invalid permission!\n");
        return 0;
    }

    return __mon_setperm4(start, end, perm);
}

static int __mon_dump4p(physaddr_t start, physaddr_t end)
{
    uintptr_t vstart = (uintptr_t)KADDR(start);
    uintptr_t vend = (uintptr_t)KADDR(end);

    for (int i = vstart; i < vend; i += 1)
    {
        if(!(i % 16))
            cprintf("\n0x%08x: ", PADDR((void *)i));
        cprintf("%02x ", *((uint8_t *)i));
    }
    cprintf("\n");

    return 0;
}

static int __mon_dump4v(uintptr_t start, uintptr_t end)
{
    for (int i = start; i < end; i += 1)
    {
        if(!(i % 16))
            cprintf("\n0x%08x: ", i);
        cprintf("%02x ", *((uint8_t *)i));
    }
    cprintf("\n");

    return 0;
}

int mon_dump(int argc, char **argv, struct Trapframe *tf)
{
    if (argc != 3 && argc != 4)
    {
        cprintf("Usage: dump TYPE START_ADDR [END_ADDR]\n\n");
        cprintf("TYPE should be V (Virtual) or P (Physical).\n");
        cprintf("You can omit END_ADDR to dump one page.\n");
        return 0;
    }

    uintptr_t start, end;
    start = strtol(argv[2], NULL, 0);
    end = (argc == 3) ? start + PGSIZE : strtol(argv[3], NULL, 0);

    if (argv[1][0] == 'V')
        return __mon_dump4v(start, end);
    else if  (argv[1][0] == 'P')
        return __mon_dump4p(start, end);
    else
    {
        cprintf("Invalid address type!\n");
        return 0;
    }
}

int mon_continue(int argc, char **argv, struct Trapframe *tf)
{
    if (tf == NULL)
    {
        cprintf("%k04No trapframe found.\n");
        return 0;
    }

    tf->tf_eflags &= ~FL_TF;
    return -1;
}

int mon_step(int argc, char **argv, struct Trapframe *tf)
{
    if (tf == NULL)
    {
        cprintf("%k04No trapframe found.\n");
        return 0;
    }

    tf->tf_eflags |= FL_TF;
    return -1;
}


/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
    int argc;
    char *argv[MAXARGS];
    int i;

    // Parse the command buffer into whitespace-separated arguments
    argc = 0;
    argv[argc] = 0;
    while (1)
    {
        // gobble whitespace
        while (*buf && strchr(WHITESPACE, *buf))
            *buf++ = 0;
        if (*buf == 0)
            break;

        // save and scan past next arg
        if (argc == MAXARGS - 1)
        {
            cprintf("Too many arguments (max %d)\n", MAXARGS);
            return 0;
        }
        argv[argc++] = buf;
        while (*buf && !strchr(WHITESPACE, *buf))
            buf++;
    }
    argv[argc] = 0;

    // Lookup and invoke the command
    if (argc == 0)
        return 0;
    for (i = 0; i < ARRAY_SIZE(commands); i++)
    {
        if (strcmp(argv[0], commands[i].name) == 0)
            return commands[i].func(argc, argv, tf);
    }
    cprintf("Unknown command '%s'\n", argv[0]);
    return 0;
}

// Use this if you want to grade using `make grade`
/*
void
monitor(struct Trapframe *tf)
{
    char *buf;

    cprintf("Welcome to the JOS kernel monitor!\n");
    cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

    while (1)
    {
        buf = readline("K> ");
        if (buf != NULL)
            if (runcmd(buf, tf) < 0)
                break;
    }
}
*/

void
monitor(struct Trapframe *tf)
{
    char *buf;
    bool debug_flag = tf && (tf->tf_eflags & FL_TF);

    if (debug_flag)
    {
        cprintf("Single step debugging...\n");
        cprintf("Type 'c' to continue, 's' to step.\n");
    }
    else
    {
        cprintf("Welcome to the JOS kernel monitor!\n");
        cprintf("Type 'help' for a list of commands.\n");
    }

    if (tf != NULL)
        print_trapframe(tf);

    while (1)
    {
        if (debug_flag)
            cprintf("[%08x] Debug", curenv->env_id);
        else
            cprintf("K");
        buf = readline("> ");
        if (buf != NULL)
            if (runcmd(buf, tf) < 0)
                break;
    }
}
