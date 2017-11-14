#ifndef DESCRIPTOR_H
#define DESCRIPTOR_H

#define NULL_SELECTOR 0
#define KERNEL_CODE32_SELECTOR    0x08
#define KERNEL_DATA32_SELECTOR    0x10
#define USER_CODE32_SELECTOR      0x18
#define USER_DATA32_SELECTOR      0x20
#define KERNEL_CODE64_SELECTOR    0x28
#define KERNEL_DATA64_SELECTOR    0x30
#define USER_CODE64_SELECTOR      0X38
#define USER_DATA64_SELECTOR      0x40
#define TSS_SELECTOR              0x48

#endif // DESCRIPTOR_H
