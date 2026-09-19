# kernel/Makefile
# 核心模块 GDT / IDT / IO / 串口
obj-y := asm/io.o desc/gdt.o desc/idt.o desc/exceptions.o desc/apic.o print/serial.o print/print.o asm/idt_handler.o asm/exc_handlers.o asm/irq_handlers.o desc/irq.o\
		 delay.o rtc.o acpi/acpi.o sched/task.o sched/kthread.o sched/utask.o asm/syscall.o syscall.o fork.o exec.o futex.o pipe.o signal.o
