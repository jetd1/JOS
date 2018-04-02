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

#define CMDBUF_SIZE    80    // enough for one VGA text line


struct Command
{
    const char *name;
    const char *desc;

    // return -1 to force monitor to exit
    int (*func)(int argc, char **argv, struct Trapframe *tf);
};

static struct Command commands[] = {
        {"help",      "Display this list of commands",        mon_help},
        {"kerninfo",  "Display information about the kernel", mon_kerninfo},
        {"backtrace", "Display stack backtrace",              mon_backtrace},
        {"shutdown",  "Shutdown the kernel",                  mon_shutdown},
        {"restart",   "Restart the kernel",                   mon_restart}
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
    uint32_t *ebp = (uint32_t *) read_ebp();
    cprintf("Stack backtrace:\n");
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
    }
    return 0;
}

int mon_shutdown(int argc, char **argv, struct Trapframe *tf)
{
    asm volatile ("cli");

    // (phony) ACPI shutdown (http://forum.osdev.org/viewtopic.php?t=16990)
    // Works for qemu and bochs.
    outw (0xB004, 0x0 | 0x2000);

    // Magic shutdown code for bochs and qemu.
//    for (const char *s = "Shutdown"; *s; ++s)
//        outb (0x8900, *s);

//    // Magic code for VMWare. Also a hard lock.
//    asm volatile ("cli; hlt");

    // Should never get here;
//    panic("Shutdown failed!");
    asm volatile ("int3");
    return 0;
}

int mon_restart(int argc, char **argv, struct Trapframe *tf)
{
    outb(0x64, 0xFE);

    // Should never get here;
    panic("Restart failed!");
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
